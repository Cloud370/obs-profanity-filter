#pragma once

#include <obs.h>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <thread>
#include <set>
#include <map>
#include "sherpa-onnx/c-api/c-api.h"
#include "asr-model.hpp"
#include "cpp-pinyin/Pinyin.h"

class ProfanityFilter {
public:
    obs_source_t *context;
    
    // Local Properties
    bool enabled = true; 
    
    // Global Cache
    std::string target_model_path;
    std::string loaded_model_path;
    double cached_delay = 1.5;
    
    // History
    std::deque<std::string> log_history;
    std::string current_partial_text; 
    std::mutex history_mutex;

    // State
    std::shared_ptr<ASRModel> asr_model; 
    const SherpaOnnxOnlineStream *stream = nullptr;
    
    // Audio Buffer
    struct ChannelBuffer {
        std::vector<float> buffer;
        size_t head = 0; 
        uint64_t total_written = 0;
    };
    
    std::vector<ChannelBuffer> channels;
    std::atomic<uint32_t> sample_rate{48000};
    std::atomic<double> sample_rate_ratio{3.0}; // sample_rate / 16000.0
    std::atomic<uint64_t> total_samples_written{0}; 
    size_t channels_count = 0;
    
    // Resampler state
    float resample_acc = 0.0f;
    
    // ASR Thread
    std::thread asr_thread;
    std::atomic<bool> running{false};
    std::mutex queue_mutex;
    std::deque<float> asr_queue; 
    
    // Beep Map
    struct BeepRange {
        uint64_t start_sample; 
        uint64_t end_sample;
    };
    std::mutex beep_mutex;
    std::vector<BeepRange> pending_beeps;
    
    std::string initialization_error = "";
    float current_rms = 0.0f;
    
    obs_data_t *settings = nullptr;
    uint64_t last_reset_sample_16k = 0;
    std::set<size_t> processed_matches; 
    std::atomic<size_t> dropped_beeps_count{0};
    
    // Pinyin Support
    std::shared_ptr<Pinyin::Pinyin> pinyin_converter;
    std::vector<std::vector<std::string>> cached_pinyin_patterns;
    std::string cached_dirty_words_str_for_pinyin;
    // Cache for single hanzi pinyin to avoid re-conversion
    std::map<std::string, std::vector<std::string>> pinyin_cache;
    
    ProfanityFilter(obs_source_t *ctx);
    ~ProfanityFilter();

    void LogToFile(const std::string& message);
    std::string GetHistoryString();
    void LoadModel(const std::string& path);
    void Start();
    void Stop();
    void ASRLoop();
    
    struct obs_audio_data *ProcessAudio(struct obs_audio_data *audio);
};
