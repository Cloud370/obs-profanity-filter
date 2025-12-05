#include <obs-module.h>
#include <obs-frontend-api.h>

#include "profanity-filter.hpp"
#include "plugin-config.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-profanity-filter", "en-US")

// --- OBS Callbacks ---

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

MODULE_EXPORT bool obs_module_load(void)
{
    obs_register_source(&profanity_filter_info);
    SetGlobalConfigModule(obs_current_module());
    InitGlobalConfig();
    
    // Register Menu Item
    obs_frontend_add_tools_menu_item("语音脏话屏蔽设置", [](void *){
        OpenGlobalConfigDialog();
    }, nullptr);
    
    return true;
}

MODULE_EXPORT void obs_module_unload(void)
{
    FreeConfigDialog();
    FreeGlobalConfig();
}
