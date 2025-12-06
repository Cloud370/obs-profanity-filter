#pragma once

#include <obs.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <deque>
#include <string>
#include <atomic>

struct FrameData {
    gs_texrender_t *render = nullptr;
    enum gs_color_space space = GS_CS_SRGB;
};

class VideoDelayFilter {
public:
    obs_source_t *context;
    
    // GPU State
    std::deque<FrameData> frames;
    
    uint64_t delay_ns = 0;
    uint64_t interval_ns = 0;
    uint64_t last_render_ts = 0;
    
    uint32_t cx = 0;
    uint32_t cy = 0;
    bool target_valid = false;
    bool processed_frame = false;

    // Status for UI (Thread-safe access)
    std::atomic<double> current_memory_mb{0.0};
    std::atomic<uint64_t> current_delay_ms{0};
    std::atomic<uint32_t> current_frame_count{0};
    
    // Global Status
    static std::atomic<double> total_memory_mb;

    VideoDelayFilter(obs_source_t *ctx);
    ~VideoDelayFilter();
    
    void Tick(float t);
    void Render(gs_effect_t *effect);
    enum gs_color_space GetColorSpace(size_t count, const enum gs_color_space *preferred_spaces);
    
    std::string GetStatusString();

private:
    double last_reported_mb = 0.0;
    
    void FreeTextures();
    void UpdateInterval(uint64_t new_interval_ns);
    void CheckInterval();
    bool CheckSize();
    void DrawFrame();
    void UpdateDelayFromConfig();
    void UpdateStatus();
};
