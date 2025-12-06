#include "video-delay.hpp"
#include "plugin-config.hpp"
#include <obs-module.h>
#include <util/util_uint64.h>
#include <util/platform.h>
#include <cmath>
#include <sstream>
#include <iomanip>

static const char *get_tech_name_and_multiplier(enum gs_color_space current_space, enum gs_color_space source_space,
                        float *multiplier)
{
    const char *tech_name = "Draw";
    *multiplier = 1.f;

    switch (source_space) {
    case GS_CS_SRGB:
    case GS_CS_SRGB_16F:
        if (current_space == GS_CS_709_SCRGB) {
            tech_name = "DrawMultiply";
            *multiplier = obs_get_video_sdr_white_level() / 80.0f;
        }
        break;
    case GS_CS_709_EXTENDED:
        switch (current_space) {
        case GS_CS_SRGB:
        case GS_CS_SRGB_16F:
            tech_name = "DrawTonemap";
            break;
        case GS_CS_709_SCRGB:
            tech_name = "DrawMultiply";
            *multiplier = obs_get_video_sdr_white_level() / 80.0f;
            break;
        default:
            break;
        }
        break;
    case GS_CS_709_SCRGB:
        switch (current_space) {
        case GS_CS_SRGB:
        case GS_CS_SRGB_16F:
            tech_name = "DrawMultiplyTonemap";
            *multiplier = 80.0f / obs_get_video_sdr_white_level();
            break;
        case GS_CS_709_EXTENDED:
            tech_name = "DrawMultiply";
            *multiplier = 80.0f / obs_get_video_sdr_white_level();
            break;
        default:
            break;
        }
    }

    return tech_name;
}

std::atomic<double> VideoDelayFilter::total_memory_mb{0.0};

VideoDelayFilter::VideoDelayFilter(obs_source_t *ctx) : context(ctx) {
    last_render_ts = 0; // Start as inactive, wait for first render to allocate
}

VideoDelayFilter::~VideoDelayFilter() {
    FreeTextures();
    
    // Cleanup global memory counter
    double current = last_reported_mb;
    if (current > 0) {
        double old_total = total_memory_mb.load();
        while (!total_memory_mb.compare_exchange_weak(old_total, old_total - current));
    }
}

void VideoDelayFilter::FreeTextures() {
    obs_enter_graphics();
    for (auto &f : frames) {
        if (f.render) gs_texrender_destroy(f.render);
    }
    frames.clear();
    obs_leave_graphics();
    
    UpdateStatus();
}

void VideoDelayFilter::UpdateDelayFromConfig() {
    GlobalConfig *cfg = GetGlobalConfig();
    if (!cfg) return;
    
    // Convert seconds to ns
    uint64_t new_delay = (uint64_t)(cfg->delay_seconds * 1000000000.0);
    
    if (new_delay != delay_ns) {
        delay_ns = new_delay;
        
        // Full reset on delay change
        cx = 0;
        cy = 0;
        interval_ns = 0;
        FreeTextures();
    }
}

void VideoDelayFilter::UpdateInterval(uint64_t new_interval_ns) {
    if (!target_valid) {
        FreeTextures();
        return;
    }
    
    // Prevent division by zero
    if (new_interval_ns == 0) return;

    interval_ns = new_interval_ns;
    size_t num = (size_t)(delay_ns / new_interval_ns);
    
    size_t current_num = frames.size();

    if (num > current_num) {
        obs_enter_graphics();
        for (size_t i = current_num; i < num; i++) {
            FrameData f;
            f.render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
            frames.push_back(f);
        }
        obs_leave_graphics();
    } else if (num < current_num) {
        obs_enter_graphics();
        while (frames.size() > num) {
            FrameData f = frames.front();
            if (f.render) gs_texrender_destroy(f.render);
            frames.pop_front();
        }
        obs_leave_graphics();
    }
    
    UpdateStatus();
}

void VideoDelayFilter::CheckInterval() {
    struct obs_video_info ovi = {0};
    obs_get_video_info(&ovi);
    
    if (ovi.fps_num == 0) return; // Safety check

    uint64_t new_interval = util_mul_div64(ovi.fps_den, 1000000000ULL, ovi.fps_num);

    if (new_interval != interval_ns) {
        UpdateInterval(new_interval);
    }
}

bool VideoDelayFilter::CheckSize() {
    obs_source_t *target = obs_filter_get_target(context);
    target_valid = !!target;
    if (!target_valid) return true;

    uint32_t new_cx = obs_source_get_base_width(target);
    uint32_t new_cy = obs_source_get_base_height(target);

    target_valid = !!new_cx && !!new_cy;
    if (!target_valid) return true;

    if (new_cx != cx || new_cy != cy) {
        cx = new_cx;
        cy = new_cy;
        
        // Reset logic
        interval_ns = 0;
        FreeTextures();
        CheckInterval();
        return true;
    }

    return false;
}

void VideoDelayFilter::Tick(float t) {
    UNUSED_PARAMETER(t);
    
    // Optimize VRAM: Only process if source is actively rendering
    uint64_t now = os_gettime_ns();
    if (now - last_render_ts > 1000000000ULL) { // 1 second timeout
        if (!frames.empty()) {
            FreeTextures();
            interval_ns = 0; // Force re-allocation when active again
        }
        return;
    }

    UpdateDelayFromConfig();

    processed_frame = false;

    if (CheckSize()) return;
    CheckInterval();
}

void VideoDelayFilter::UpdateStatus() {
    // Calculate memory usage: Width * Height * 4 bytes (RGBA) * Frames
    // Note: Mipmaps/Padding might add more, but this is a good lower bound estimate.
    size_t bytes_per_frame = cx * cy * 4;
    size_t total_bytes = bytes_per_frame * frames.size();
    double mb = (double)total_bytes / (1024.0 * 1024.0);
    
    current_memory_mb.store(mb);
    current_delay_ms.store(delay_ns / 1000000);
    current_frame_count.store((uint32_t)frames.size());
    
    // Update global total safely
    double diff = mb - last_reported_mb;
    if (std::abs(diff) > 0.001) { // Only update if change is significant
        double old_total = total_memory_mb.load();
        while (!total_memory_mb.compare_exchange_weak(old_total, old_total + diff));
        last_reported_mb = mb;
    }
}

std::string VideoDelayFilter::GetStatusString() {
    double mb = current_memory_mb.load();
    uint64_t ms = current_delay_ms.load();
    uint32_t count = current_frame_count.load();
    
    std::stringstream ss;
    ss << "同步缓冲: " << ms << " ms (" << count << " 帧)\n";
    ss << "显存占用: " << std::fixed << std::setprecision(1) << mb << " MB";
    
    if (count == 0 && ms > 0) {
        ss << "\n(当前场景未激活，已释放显存)";
    }
    
    if (mb > 1000.0) {
        ss << "\n⚠️ 警告: 显存占用较高!";
    }
    
    return ss.str();
}

void VideoDelayFilter::DrawFrame() {
    if (frames.empty()) return;
    
    FrameData &frame = frames.front(); // peek front

    const enum gs_color_space current_space = gs_get_color_space();
    float multiplier;
    const char *technique = get_tech_name_and_multiplier(current_space, frame.space, &multiplier);

    gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_texture_t *tex = gs_texrender_get_texture(frame.render);
    
    if (tex) {
        const bool previous = gs_framebuffer_srgb_enabled();
        gs_enable_framebuffer_srgb(true);

        gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), tex);
        gs_effect_set_float(gs_effect_get_param_by_name(effect, "multiplier"), multiplier);

        while (gs_effect_loop(effect, technique))
            gs_draw_sprite(tex, 0, cx, cy);

        gs_enable_framebuffer_srgb(previous);
    }
}

void VideoDelayFilter::Render(gs_effect_t *effect) {
    last_render_ts = os_gettime_ns();

    obs_source_t *target = obs_filter_get_target(context);
    obs_source_t *parent = obs_filter_get_parent(context);

    if (!target_valid || !target || !parent || frames.empty()) {
        obs_source_skip_video_filter(context);
        return;
    }

    if (processed_frame) {
        DrawFrame();
        return;
    }

    FrameData frame = frames.front();
    frames.pop_front();

    // Check format/size
    const enum gs_color_space preferred_spaces[] = {
        GS_CS_SRGB,
        GS_CS_SRGB_16F,
        GS_CS_709_EXTENDED,
    };
    const enum gs_color_space space = obs_source_get_color_space(target, 3, preferred_spaces);
    const enum gs_color_format format = gs_get_format_from_space(space);
    
    if (gs_texrender_get_format(frame.render) != format) {
        gs_texrender_destroy(frame.render);
        frame.render = gs_texrender_create(format, GS_ZS_NONE);
        UpdateStatus(); // Format change might imply re-allocation considerations, updating status just in case
    }

    gs_texrender_reset(frame.render);

    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

    if (gs_texrender_begin_with_color_space(frame.render, cx, cy, space)) {
        uint32_t parent_flags = obs_source_get_output_flags(target);
        bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
        bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;
        struct vec4 clear_color;

        vec4_zero(&clear_color);
        gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
        gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

        if (target == parent && !custom_draw && !async)
            obs_source_default_render(target);
        else
            obs_source_video_render(target);

        gs_texrender_end(frame.render);

        frame.space = space;
    }

    gs_blend_state_pop();

    frames.push_back(frame);
    DrawFrame();
    processed_frame = true;

    UNUSED_PARAMETER(effect);
}

enum gs_color_space VideoDelayFilter::GetColorSpace(size_t count, const enum gs_color_space *preferred_spaces) {
    obs_source_t *target = obs_filter_get_target(context);
    obs_source_t *parent = obs_filter_get_parent(context);

    if (!target_valid || !target || !parent || frames.empty()) {
        return (count > 0) ? preferred_spaces[0] : GS_CS_SRGB;
    }

    FrameData &frame = frames.front();
    enum gs_color_space space = frame.space;
    for (size_t i = 0; i < count; ++i) {
        if (preferred_spaces[i] == space)
            return space;
    }

    return space;
}
