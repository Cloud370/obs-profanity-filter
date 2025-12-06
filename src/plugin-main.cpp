#include <obs-module.h>
#include <obs-frontend-api.h>

#include "profanity-filter.hpp"
#include "plugin-config.hpp"
#include "video-delay.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-profanity-filter", "en-US")

// --- Audio Filter Callbacks ---

static const char *get_name(void *unused) { return "语音脏话屏蔽 (全局配置)"; }

static void *create(obs_data_t *settings, obs_source_t *context) {
    ProfanityFilter *filter = new ProfanityFilter(context);
    filter->enabled = obs_data_get_bool(settings, "enabled");
    filter->settings = settings;
    obs_data_addref(settings);
    filter->Start();
    return filter;
}

static void destroy(void *data) {
    ProfanityFilter *filter = (ProfanityFilter *)data;
    delete filter;
}

static void update(void *data, obs_data_t *settings) {
    ProfanityFilter *filter = (ProfanityFilter *)data;
    
    // Safe update to avoid race with refresh_history
    obs_data_addref(settings);
    obs_data_t *old = filter->settings;
    filter->settings = settings;
    if (old) obs_data_release(old);

    filter->enabled = obs_data_get_bool(settings, "enabled");
}

static bool refresh_history(obs_properties_t *props, obs_property_t *p, void *data) {
    ProfanityFilter *filter = (ProfanityFilter *)data;
    if (filter && filter->settings) {
        obs_data_set_string(filter->settings, "history_view", filter->GetHistoryString().c_str());
        return true; 
    }
    return false;
}

static obs_properties_t *get_properties(void *data) {
    obs_properties_t *props = obs_properties_create();
    
    obs_properties_add_group(props, "group_usage", "使用说明", OBS_GROUP_NORMAL, NULL);
    obs_property_t *usage = obs_properties_add_text(props, "usage_info", "", OBS_TEXT_INFO);
    obs_property_set_long_description(usage, 
        "请在 OBS 顶部菜单 【工具】 -> 【语音脏话屏蔽设置】 中配置模型路径、屏蔽词和延迟时间。\n"
        "本滤镜仅提供开关功能，但会始终应用配置的延迟以保持音画同步。");

    obs_properties_add_bool(props, "enabled", "启用脏话过滤 (关闭时仅保留延迟)");
    
    obs_properties_add_button(props, "refresh_btn", "🔄 刷新状态与日志", refresh_history);
    obs_properties_add_text(props, "history_view", "日志输出:", OBS_TEXT_MULTILINE);
    
    if (data) {
        ProfanityFilter *filter = (ProfanityFilter *)data;
        if (filter->settings) {
            obs_data_set_string(filter->settings, "history_view", filter->GetHistoryString().c_str());
        }
    }
    return props;
}

static void get_defaults(obs_data_t *settings) {
    obs_data_set_default_bool(settings, "enabled", true);
    obs_data_set_default_string(settings, "history_view", "点击上方刷新按钮获取最新日志...");
}

static struct obs_audio_data *filter_audio(void *data, struct obs_audio_data *audio) {
    ProfanityFilter *filter = (ProfanityFilter *)data;
    return filter->ProcessAudio(audio);
}

struct obs_source_info profanity_filter_info = {
    .id = "profanity_filter",
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_AUDIO,
    .get_name = get_name,
    .create = create,
    .destroy = destroy,
    .get_defaults = get_defaults,
    .get_properties = get_properties,
    .update = update,
    .filter_audio = filter_audio,
};

// --- Video Delay Filter Callbacks ---

static const char *get_name_video(void *unused) { return "语音屏蔽-视频同步延迟"; }

static void *create_video(obs_data_t *settings, obs_source_t *context) {
    return new VideoDelayFilter(context);
}

static void destroy_video(void *data) {
    delete (VideoDelayFilter *)data;
}

static void video_tick(void *data, float t) {
    ((VideoDelayFilter *)data)->Tick(t);
}

static void video_render(void *data, gs_effect_t *effect) {
    ((VideoDelayFilter *)data)->Render(effect);
}

static enum gs_color_space video_get_color_space(void *data, size_t count, const enum gs_color_space *preferred_spaces) {
    return ((VideoDelayFilter *)data)->GetColorSpace(count, preferred_spaces);
}

static bool refresh_video_status(obs_properties_t *props, obs_property_t *p, void *data) {
    VideoDelayFilter *filter = (VideoDelayFilter *)data;
    if (filter) {
        obs_property_t *status = obs_properties_get(props, "status_info");
        if (status) {
            obs_property_set_long_description(status, filter->GetStatusString().c_str());
            return true;
        }
    }
    return false;
}

static obs_properties_t *get_properties_video(void *data) {
    obs_properties_t *props = obs_properties_create();
    
    obs_properties_add_group(props, "group_usage", "使用说明", OBS_GROUP_NORMAL, NULL);
    obs_property_t *usage = obs_properties_add_text(props, "usage_info", "", OBS_TEXT_INFO);
    obs_property_set_long_description(usage, 
        "此滤镜会自动读取【语音脏话屏蔽设置】中的延迟时间并应用到视频。\n"
        "适用于整个场景或游戏捕获等渲染源。\n"
        "⚠️ 注意：高延迟会导致较大的显存占用。");

    obs_properties_add_button(props, "refresh_btn", "🔄 刷新状态", refresh_video_status);
    
    obs_property_t *status = obs_properties_add_text(props, "status_info", "", OBS_TEXT_INFO);
    if (data) {
        VideoDelayFilter *filter = (VideoDelayFilter *)data;
        obs_property_set_long_description(status, filter->GetStatusString().c_str());
    } else {
        obs_property_set_long_description(status, "请先激活滤镜以查看状态...");
    }

    return props;
}

struct obs_source_info profanity_video_delay_info = {
    .id = "profanity_video_delay",
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_VIDEO,
    .get_name = get_name_video,
    .create = create_video,
    .destroy = destroy_video,
    .get_properties = get_properties_video,
    .video_tick = video_tick,
    .video_render = video_render,
    .video_get_color_space = video_get_color_space,
};

// --- Module Load/Unload ---

static void frontend_event(enum obs_frontend_event event, void *) {
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
        GlobalConfig *cfg = GetGlobalConfig();
        if (cfg && cfg->loaded && cfg->is_first_run) {
            UpdateVideoDelayFiltersState();
            cfg->is_first_run = false;
            cfg->Save();
        }
    }
}

MODULE_EXPORT bool obs_module_load(void)
{
    obs_register_source(&profanity_filter_info);
    obs_register_source(&profanity_video_delay_info);
    
    SetGlobalConfigModule(obs_current_module());
    InitGlobalConfig();
    
    // Register Menu Item
    obs_frontend_add_tools_menu_item("语音脏话屏蔽设置", [](void *){
        OpenGlobalConfigDialog();
    }, nullptr);
    
    obs_frontend_add_event_callback(frontend_event, nullptr);
    
    return true;
}

void UpdateVideoDelayFiltersState() {
    GlobalConfig *cfg = GetGlobalConfig();
    bool enable = cfg->video_delay_enabled;

    struct obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);
    
    for (size_t i = 0; i < scenes.sources.num; i++) {
        obs_source_t *scene_source = scenes.sources.array[i];
        obs_source_t *filter = obs_source_get_filter_by_name(scene_source, "语音屏蔽-音画同步");
        
        if (enable) {
            if (!filter) {
                obs_source_t *new_filter = obs_source_create_private("profanity_video_delay", "语音屏蔽-音画同步", NULL);
                obs_source_filter_add(scene_source, new_filter);
                obs_source_release(new_filter);
            }
        } else {
            // Also remove old named filters for backward compatibility
            obs_source_t *old_filter = obs_source_get_filter_by_name(scene_source, "语音屏蔽-视频同步延迟");
            if (old_filter) {
                obs_source_filter_remove(scene_source, old_filter);
                obs_source_release(old_filter);
            }

            if (filter) {
                obs_source_filter_remove(scene_source, filter);
            }
        }
        
        if (filter) obs_source_release(filter);
    }
    obs_frontend_source_list_free(&scenes);
}

MODULE_EXPORT void obs_module_unload(void)
{
    FreeConfigDialog();
    FreeGlobalConfig();
}
