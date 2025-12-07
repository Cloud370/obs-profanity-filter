#include "profanity-filter.hpp"
#include "plugin-config.hpp"
#include "utils.hpp"
#include "logging-macros.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <cpp-pinyin/G2pglobal.h>

// Dummy function to locate the module handle
static void ModuleLocator() {}

#include <sstream>
#include <cmath>
#include <algorithm>
#include <regex>
#include <filesystem>
#include <windows.h>

using namespace std;

std::set<ProfanityFilter*> ProfanityFilter::instances;
std::mutex ProfanityFilter::instances_mutex;

ProfanityFilter::ProfanityFilter(obs_source_t *ctx) : context(ctx) {
    {
        lock_guard<mutex> lock(instances_mutex);
        instances.insert(this);
    }
    // Initial sync with global config
    GlobalConfig *cfg = GetGlobalConfig();
    lock_guard<mutex> lock(cfg->mutex);
    target_model_path = cfg->model_path;
    cached_delay = cfg->delay_seconds;
}

ProfanityFilter::~ProfanityFilter() {
    {
        lock_guard<mutex> lock(instances_mutex);
        instances.erase(this);
    }
    Stop();
    if (settings) obs_data_release(settings);
    if (stream) {
        SherpaOnnxDestroyOnlineStream(stream);
        stream = nullptr;
    }
    asr_model.reset();
}

std::pair<bool, std::string> ProfanityFilter::GetGlobalModelStatus() {
    std::lock_guard<std::mutex> lock(instances_mutex);
    
    // Check loading first
    for (auto* filter : instances) {
        if (filter->is_loading) {
             std::string path;
             {
                 std::lock_guard<std::mutex> h_lock(filter->history_mutex);
                 path = filter->loading_target_path;
             }
             // Truncate if too long for UI
             if (path.length() > 40) path = "..." + path.substr(path.length() - 37);
             return {true, "🟡 正在加载 " + path};
        }
    }
    
    // Check loaded or error
    for (auto* filter : instances) {
        if (filter->asr_model && filter->asr_model->recognizer) {
             return {false, "🟢 模型运行中"};
        }
        
        // Check error (initialization_error is not strictly mutex protected but only written during load)
        // Since is_loading is false here, it should be safe enough, but to be 100% safe we could use history_mutex for it too if we moved it there.
        // For now, let's assume it's stable when not loading.
        if (!filter->initialization_error.empty()) {
             return {false, "🔴 错误: " + filter->initialization_error};
        }
    }
    
    if (instances.empty()) return {false, "⚪ 无活跃来源 (请添加滤镜)"};
    
    return {false, "⚪ 未初始化"};
}

void ProfanityFilter::LoadModel(const string& path) {
    {
        lock_guard<mutex> lock(history_mutex);
        loading_target_path = path;
    }
    is_loading = true;
    if (stream) {
        SherpaOnnxDestroyOnlineStream(stream);
        stream = nullptr;
    }
    
    if (asr_model) {
        // If this is the last instance, the underlying model will be destroyed here
        BLOG(LOG_INFO, "正在释放旧模型引用..."); 
    }
    asr_model.reset();
    initialization_error = "";
    
    if (path.empty()) {
        loaded_model_path = "";
        
        // Check if it's due to global disable
        GlobalConfig *cfg = GetGlobalConfig();
        bool global_enable = true;
        if (cfg) {
            lock_guard<mutex> lock(cfg->mutex);
            global_enable = cfg->global_enable;
        }
        
        if (global_enable) {
            initialization_error = "未选择模型路径";
            BLOG(LOG_ERROR, "错误: %s", initialization_error.c_str());
        } else {
            // Just unloaded, no error
        }
        is_loading = false;
        return;
    }
    
    string err;
    asr_model = ModelManager::Get(path, err);
    
    if (asr_model && asr_model->recognizer) {
        stream = SherpaOnnxCreateOnlineStream(asr_model->recognizer);
        {
            lock_guard<mutex> lock(history_mutex);
            loaded_model_path = path;
        }
        BLOG(LOG_INFO, "引擎初始化成功");
    } else {
        initialization_error = err.empty() ? "引擎初始化失败" : err;
        BLOG(LOG_ERROR, "错误: %s", initialization_error.c_str());
        
        // IMPORTANT: Even if failed, update loaded_model_path to prevent infinite retry loop in ASRLoop
        {
            lock_guard<mutex> lock(history_mutex);
            loaded_model_path = path;
        }
    }
    is_loading = false;
}

void ProfanityFilter::Start() {
    if (running) return;
    running = true;
    asr_thread = thread(&ProfanityFilter::ASRLoop, this);
}

void ProfanityFilter::Stop() {
    running = false;
    if (asr_thread.joinable()) asr_thread.join();
}

void ProfanityFilter::ASRLoop() {
    uint64_t total_samples_popped_16k = 0;
    
    // Time sync
    uint64_t start_offset_input = 0;
    uint64_t last_feed_offset = 0;
    
    // AGC State
    float current_agc_gain = 1.0f;
    
    uint64_t tw = total_samples_written.load();
    if (tw > 0) {
        size_t q_size;
        {
            lock_guard<mutex> lock(queue_mutex);
            q_size = asr_queue.size();
        }
        double ratio = sample_rate_ratio.load();
        uint64_t backlog_input = (uint64_t)(q_size * ratio);
        if (tw > backlog_input) {
            start_offset_input = tw - backlog_input;
        }
        last_feed_offset = start_offset_input;
    }

    while (running) {
        // Poll Global Config for model path changes and Gain settings
        bool enable_agc = true;
        
        {
            GlobalConfig *cfg = GetGlobalConfig();
            std::lock_guard<std::mutex> lock(cfg->mutex);
            
            std::string desired_path = cfg->global_enable ? cfg->model_path : "";
            enable_agc = cfg->enable_agc;
            
            if (target_model_path != desired_path) {
                std::lock_guard<std::mutex> q_lock(queue_mutex);
                target_model_path = desired_path;
            }
        }

        // 1. Check for Model Change
        {
            string target;
            {
                lock_guard<mutex> lock(queue_mutex);
                target = target_model_path;
            }
            
            if (target != loaded_model_path) {
                LoadModel(target);
                // Reset stream implies resetting timestamp reference
                last_reset_sample_16k = total_samples_popped_16k;

                // Fix: Clear queue and processed matches to prevent latency accumulation and index collision
                {
                    lock_guard<mutex> lock(queue_mutex);
                    asr_queue.clear();
                }
                
                // Re-sync time after clearing queue
                uint64_t tw_now = total_samples_written.load();
                double ratio_now = sample_rate_ratio.load();
                int64_t diff = (int64_t)tw_now - (int64_t)(total_samples_popped_16k * ratio_now);
                if (diff >= 0) {
                    start_offset_input = (uint64_t)diff;
                }
                last_feed_offset = start_offset_input;
                
                processed_matches.clear();
            }
        }
        
        // 2. Process Audio
        vector<float> chunk;
        double current_ratio = sample_rate_ratio.load();
        uint32_t current_sr = sample_rate.load();

        {
            lock_guard<mutex> lock(queue_mutex);
            if (!asr_queue.empty()) {
                // Gap Check: If start_offset_input jumped significantly (e.g. > 0.5s), reset stream
                // This handles cases where filter was disabled/idle for a long time, ensuring fresh context
                // and preventing latency accumulation from stale state.
                if (start_offset_input > last_feed_offset + (uint64_t)(current_sr * 0.5)) {
                        if (asr_model && asr_model->recognizer && stream) {
                            SherpaOnnxDestroyOnlineStream(stream);
                            stream = SherpaOnnxCreateOnlineStream(asr_model->recognizer);
                            last_reset_sample_16k = total_samples_popped_16k;
                            processed_matches.clear();
                            {
                                lock_guard<mutex> h_lock(history_mutex);
                                current_partial_text = "";
                            }
                        }
                }
                last_feed_offset = start_offset_input;

                size_t n = min((size_t)3200, asr_queue.size()); 
                chunk.assign(asr_queue.begin(), asr_queue.begin() + n);
                asr_queue.erase(asr_queue.begin(), asr_queue.begin() + n);
            } else {
                // Re-sync offset to handle gaps (e.g. toggle enabled, queue clear)
                // This ensures timestamps remain accurate even if we dropped samples
                uint64_t tw = total_samples_written.load();
                if (tw > 0) {
                    // Calculate what start_offset_input SHOULD be so that:
                    // current_time ~= start_offset + total_popped * ratio
                    // We assume since queue is empty, current_time == tw
                    
                    // Use signed math to handle potential small drift
                    int64_t diff = (int64_t)tw - (int64_t)(total_samples_popped_16k * current_ratio);
                    // start_offset_input is uint64, assuming positive result
                    if (diff >= 0) {
                        start_offset_input = (uint64_t)diff;
                    }
                }
            }
        }
        
        if (chunk.empty()) {
            this_thread::sleep_for(chrono::milliseconds(10));
            continue;
        }
        
        total_samples_popped_16k += chunk.size();
        
        // --- Gain Processing (AGC) ---
        // Create a copy for model processing so output audio remains original
        vector<float> model_chunk = chunk;
        
        if (enable_agc) {
            // Automatic Gain Control
            float peak = 0.0001f;
            for (float s : model_chunk) {
                float abs_s = fabsf(s);
                if (abs_s > peak) peak = abs_s;
            }
            
            // Target Peak: 0.6 (-4.4 dB)
            float target_peak = 0.6f;
            float desired_gain = target_peak / peak;
            
            // Constraints
            if (desired_gain > 31.6f) desired_gain = 31.6f; // Max +30dB
            if (desired_gain < 0.1f) desired_gain = 0.1f;   // Min -20dB
            
            // Smooth Update
            if (desired_gain < current_agc_gain) {
                // Attack (fast reduction)
                current_agc_gain = current_agc_gain * 0.9f + desired_gain * 0.1f;
            } else {
                // Release (slow increase)
                current_agc_gain = current_agc_gain * 0.99f + desired_gain * 0.01f;
            }
            
            // Apply to model_chunk ONLY
            for (float &s : model_chunk) {
                s *= current_agc_gain;
                if (s > 1.0f) s = 1.0f;
                if (s < -1.0f) s = -1.0f;
            }
        } else {
             current_agc_gain = 1.0f; // Reset if disabled
        }
        // ---------------------------------------

        if (asr_model && asr_model->recognizer && stream) {
            SherpaOnnxOnlineStreamAcceptWaveform(stream, 16000, model_chunk.data(), (int32_t)model_chunk.size());
            while (SherpaOnnxIsOnlineStreamReady(asr_model->recognizer, stream)) {
                SherpaOnnxDecodeOnlineStream(asr_model->recognizer, stream);
            }
            
            const SherpaOnnxOnlineRecognizerResult *result = SherpaOnnxGetOnlineStreamResult(asr_model->recognizer, stream);
            if (result) {
                // Get Patterns and Config from Global
                GlobalConfig *cfg = GetGlobalConfig();
                vector<regex> patterns;
                bool use_pinyin;
                bool comedy_mode;
                int model_offset_ms;
                string current_dirty_words;
                {
                    lock_guard<mutex> lock(cfg->mutex);
                    patterns = cfg->dirty_patterns; // Copy
                    use_pinyin = cfg->use_pinyin;
                    comedy_mode = cfg->comedy_mode;
                    model_offset_ms = cfg->model_offset_ms;
                    current_dirty_words = cfg->dirty_words_str;
                }
                
                if (result->count > 0) {
                    string full_text = "";
                    // Reconstruct text
                    for(int i=0; i<result->count; i++) full_text += result->tokens_arr[i];
                    
                    if (!full_text.empty()) {
                        {
                            lock_guard<mutex> lock(history_mutex);
                            current_partial_text = full_text;
                        }
                    }

                    // Collect Candidates
                    struct MatchCandidate {
                        size_t start_char;
                        uint64_t start_sample;
                        uint64_t end_sample;
                        string log_text;
                        bool is_pinyin;
                    };
                    vector<MatchCandidate> candidates;

                    // 1. Regex Matching
                    for (const auto& pattern : patterns) {
                        sregex_iterator begin(full_text.begin(), full_text.end(), pattern);
                        sregex_iterator end;
                        
                        for (auto i = begin; i != end; ++i) {
                            smatch match = *i;
                            size_t m_start_char = match.position();
                            
                            // Find timestamps
                            float m_start_time = -1.0f;
                            float m_end_time = -1.0f;
                            
                            size_t current_char = 0;
                            for(int t=0; t<result->count; t++) {
                                string tok = result->tokens_arr[t];
                                size_t tok_len = tok.length();
                                float tok_start = result->timestamps[t];
                                float tok_end = (t < result->count - 1) ? result->timestamps[t+1] : (tok_start + 0.2f);
                                
                                // Check overlap
                                size_t m_end_char = m_start_char + match.length();
                                if (current_char + tok_len > m_start_char && current_char < m_end_char) {
                                    if (m_start_time < 0) m_start_time = tok_start;
                                    m_end_time = tok_end;
                                }
                                current_char += tok_len;
                            }
                            
                            if (m_start_time >= 0) {
                                uint64_t start_16k = last_reset_sample_16k + (uint64_t)(m_start_time * 16000.0f);
                                uint64_t end_16k = last_reset_sample_16k + (uint64_t)(m_end_time * 16000.0f);
                                
                                uint64_t start_abs = (uint64_t)(start_16k * current_ratio) + start_offset_input;
                                uint64_t end_abs = (uint64_t)(end_16k * current_ratio) + start_offset_input;
                                
                                // Apply Model Latency Offset
                                int64_t offset_samples = (int64_t)((model_offset_ms / 1000.0) * current_sr);
                                if (offset_samples >= 0) {
                                    start_abs += offset_samples;
                                    end_abs += offset_samples;
                                } else {
                                    uint64_t sub = (uint64_t)(-offset_samples);
                                    start_abs = (start_abs > sub) ? start_abs - sub : 0;
                                    end_abs = (end_abs > sub) ? end_abs - sub : 0;
                                }
                                
                                // Safe margin: 150ms = 0.15 * sr (Reduced from 400ms to avoid false positives)
                                uint32_t margin = (uint32_t)(0.15 * current_sr);
                                start_abs = (start_abs > margin) ? start_abs - margin : 0; 
                                end_abs += margin; 
                                
                                candidates.push_back({m_start_char, start_abs, end_abs, match.str(), false});
                            }
                        }
                    }

                    // 2. Pinyin Matching
                    if (use_pinyin) {
                        if (!pinyin_converter) {
                            // Try to locate dict directory relative to the plugin DLL
                            string dict_path;
                            
                            // Method 1: Try OBS data path (Standard Install)
                            char *obs_data_ptr = obs_module_file("dict");
                            if (obs_data_ptr) {
                                if (filesystem::exists(obs_data_ptr)) {
                                    dict_path = obs_data_ptr;
                                }
                                bfree(obs_data_ptr);
                            }
                            
                            // Method 2: Try next to DLL (Portable / Dev) or Self-contained bundle
                                if (dict_path.empty()) {
                                    HMODULE hMod = nullptr;
                                    MEMORY_BASIC_INFORMATION mbi;
                                    if (VirtualQuery((LPCVOID)&ModuleLocator, &mbi, sizeof(mbi))) {
                                        hMod = (HMODULE)mbi.AllocationBase;
                                    }
                                    
                                    if (hMod) {
                                        char path[MAX_PATH];
                                        if (GetModuleFileNameA(hMod, path, MAX_PATH)) {
                                            filesystem::path p(path);
                                            
                                            // 1. Check next to DLL (e.g. local build: bin/64bit/dict)
                                            filesystem::path p_next = p.parent_path() / "dict";
                                            if (filesystem::exists(p_next)) {
                                                dict_path = p_next.string();
                                            } else {
                                                // 2. Check standard plugin structure (root/data/dict)
                                                filesystem::path p_bundle = p.parent_path().parent_path().parent_path() / "data" / "dict";
                                                if (filesystem::exists(p_bundle)) {
                                                    dict_path = p_bundle.string();
                                                }
                                            }
                                        }
                                    }
                                }

                            if (!dict_path.empty()) {
                                Pinyin::setDictionaryPath(dict_path);
                                pinyin_converter = make_shared<Pinyin::Pinyin>();
                                BLOG(LOG_INFO, "Pinyin Engine Initialized from: %s", dict_path.c_str());
                            } else {
                                static bool logged_error = false;
                                if (!logged_error) {
                                    BLOG(LOG_ERROR, "Error: Could not find 'dict' directory for Pinyin engine.");
                                    logged_error = true;
                                }
                            }
                        }

                        if (pinyin_converter) {
                            // Update patterns if changed
                            if (current_dirty_words != cached_dirty_words_str_for_pinyin) {
                                cached_pinyin_patterns.clear();
                                stringstream ss(current_dirty_words);
                                string item;
                                while (getline(ss, item, ',')) {
                                    item.erase(0, item.find_first_not_of(" \t\n\r"));
                                    item.erase(item.find_last_not_of(" \t\n\r") + 1);
                                    if (!item.empty()) {
                                        auto res = pinyin_converter->hanziToPinyin(item, Pinyin::ManTone::Style::NORMAL, Pinyin::Error::Default, false, false);
                                        vector<string> pat;
                                        for(const auto& r : res) {
                                            if (!r.pinyin.empty() && r.pinyin != " ") {
                                                    pat.push_back(NormalizePinyin(r.pinyin));
                                            }
                                        }
                                        if (!pat.empty()) cached_pinyin_patterns.push_back(pat);
                                    }
                                }
                                cached_dirty_words_str_for_pinyin = current_dirty_words;
                            }

                            // Prepare text pinyin
                            vector<string> text_pinyins;
                            vector<int> pinyin_to_token;
                            
                            for(int t=0; t<result->count; t++) {
                                string tok = result->tokens_arr[t];
                                
                                // Try cache first
                                vector<string> pinyins;
                                auto it = pinyin_cache.find(tok);
                                if (it != pinyin_cache.end()) {
                                    pinyins = it->second;
                                } else {
                                    // Not in cache, convert
                                    auto res = pinyin_converter->hanziToPinyin(tok, Pinyin::ManTone::Style::NORMAL, Pinyin::Error::Default, false, false);
                                    for(const auto& r : res) {
                                            if (!r.pinyin.empty() && r.pinyin != " ") {
                                                pinyins.push_back(NormalizePinyin(r.pinyin));
                                            }
                                    }
                                    // Store in cache (limit size to prevent memory leak)
                                    if (pinyin_cache.size() > 5000) pinyin_cache.clear();
                                    pinyin_cache[tok] = pinyins;
                                }

                                for(const auto& p : pinyins) {
                                        text_pinyins.push_back(p);
                                        pinyin_to_token.push_back(t);
                                }
                            }
                            
                            // Debug Log Pinyin (First 3s only to avoid spam)
                            static int debug_log_count = 0;
                            if (debug_log_count < 3 && !text_pinyins.empty()) {
                                stringstream ss;
                                ss << "DEBUG Pinyin: ";
                                for(auto& p : text_pinyins) ss << p << " ";
                                BLOG(LOG_INFO, "%s", ss.str().c_str());
                                debug_log_count++;
                            }

                            // Match
                            for (const auto& pat : cached_pinyin_patterns) {
                                if (pat.size() > text_pinyins.size()) continue;
                                for (size_t i = 0; i <= text_pinyins.size() - pat.size(); ++i) {
                                    bool match = true;
                                    for (size_t j = 0; j < pat.size(); ++j) {
                                        if (text_pinyins[i+j] != pat[j]) {
                                            match = false;
                                            break;
                                        }
                                    }
                                    
                                    if (match) {
                                        int start_token = pinyin_to_token[i];
                                        int end_token = pinyin_to_token[i + pat.size() - 1];
                                        
                                        // Calculate char pos for processed_matches check
                                        size_t char_pos = 0;
                                        for(int k=0; k<start_token; k++) char_pos += string(result->tokens_arr[k]).length();
                                        
                                        float start_time = result->timestamps[start_token];
                                        float end_time = (end_token < result->count - 1) ? result->timestamps[end_token+1] : (result->timestamps[end_token] + 0.2f);
                                        
                                        uint64_t start_16k = last_reset_sample_16k + (uint64_t)(start_time * 16000.0f);
                                        uint64_t end_16k = last_reset_sample_16k + (uint64_t)(end_time * 16000.0f);
                                        
                                        uint64_t start_abs = (uint64_t)(start_16k * current_ratio) + start_offset_input;
                                        uint64_t end_abs = (uint64_t)(end_16k * current_ratio) + start_offset_input;

                                        // Apply Model Latency Offset
                                        int64_t offset_samples = (int64_t)((model_offset_ms / 1000.0) * current_sr);
                                        if (offset_samples >= 0) {
                                            start_abs += offset_samples;
                                            end_abs += offset_samples;
                                        } else {
                                            uint64_t sub = (uint64_t)(-offset_samples);
                                            start_abs = (start_abs > sub) ? start_abs - sub : 0;
                                            end_abs = (end_abs > sub) ? end_abs - sub : 0;
                                        }
                                        
                                        // Safe margin: 150ms = 0.15 * sr
                                        uint32_t margin = (uint32_t)(0.15 * current_sr);
                                        start_abs = (start_abs > margin) ? start_abs - margin : 0; 
                                        end_abs += margin; 
                                        
                                        stringstream ss;
                                        ss << "已屏蔽(拼音): ";
                                        for(const auto& p : pat) ss << p << " ";
                                        ss << "[匹配源: ";
                                        for(int k=0; k<pat.size(); k++) ss << text_pinyins[i+k] << " ";
                                        ss << "]";
                                        
                                        candidates.push_back({char_pos, start_abs, end_abs, ss.str(), true});
                                    }
                                }
                            }
                        }
                    }

                    // 3. Sort and Apply Candidates
                    if (comedy_mode) {
                        // Comedy Mode: Shortest First
                        sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b){
                            return (a.end_sample - a.start_sample) < (b.end_sample - b.start_sample);
                        });
                    } else {
                        // Normal Mode: Longest First (Cover max area)
                        sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b){
                            return (a.end_sample - a.start_sample) > (b.end_sample - b.start_sample);
                        });
                    }

                    vector<pair<uint64_t, uint64_t>> covered_intervals;
                    for(const auto& m : candidates) {
                        // Skip if already processed in previous frames
                        if (processed_matches.count(m.start_char)) continue;
                        
                        // Check overlap with currently selected candidates in this frame
                        bool overlap = false;
                        for(const auto& interval : covered_intervals) {
                            if (m.start_sample < interval.second && m.end_sample > interval.first) {
                                overlap = true;
                                break;
                            }
                        }
                        
                        if (!overlap) {
                            lock_guard<mutex> b_lock(beep_mutex);
                            pending_beeps.push_back({m.start_sample, m.end_sample, m.start_sample});
                            BLOG(LOG_INFO, "%s", m.log_text.c_str());
                            
                            covered_intervals.push_back({m.start_sample, m.end_sample});
                        }
                        
                        // Always mark as processed to prevent re-evaluation or double-application
                        processed_matches.insert(m.start_char);
                    }
                }
                SherpaOnnxDestroyOnlineRecognizerResult(result);
            }
            
            // Check endpoint or force reset if segment is too long (> 600s = 10min)
            bool force_reset = (total_samples_popped_16k - last_reset_sample_16k) > (16000 * 600);

            if (force_reset || SherpaOnnxOnlineStreamIsEndpoint(asr_model->recognizer, stream)) {
                if (force_reset) {
                    BLOG(LOG_INFO, "Info: Periodic reset of ASR stream (segment > 10min)");
                }
                SherpaOnnxOnlineStreamReset(asr_model->recognizer, stream);
                last_reset_sample_16k = total_samples_popped_16k;
                {
                    lock_guard<mutex> lock(history_mutex);
                    current_partial_text = "";
                }
                processed_matches.clear();
            }
        }
    }
}

struct obs_audio_data *ProfanityFilter::ProcessAudio(struct obs_audio_data *audio) {
    uint32_t frames = audio->frames;
    if (!audio->data[0]) return audio;

    // Sync with Global Config
    GlobalConfig *cfg = GetGlobalConfig();
    double global_delay;
    string global_model;
    int global_effect; // 0=Beep, 1=Silence, 2=Squeaky, 3=Robot
    int global_freq;
    int global_mix;
    bool global_enable;
    
    {
        lock_guard<mutex> lock(cfg->mutex);
        global_delay = cfg->delay_seconds;
        global_model = cfg->model_path;
        global_effect = cfg->audio_effect;
        global_freq = cfg->beep_frequency;
        global_mix = cfg->beep_mix_percent;
        global_enable = cfg->global_enable;
    }
    
    // If disabled globally, pass through and signal unload
    if (!global_enable) {
        {
            lock_guard<mutex> lock(queue_mutex);
            target_model_path = ""; // Signal unload
        }
        return audio;
    }
    
    // Update Filter State
    {
        lock_guard<mutex> lock(queue_mutex);
        target_model_path = global_model;
    }
    cached_delay = global_delay;
    
    // 1. Push to ASR (Only if enabled and model loaded)
    // But we always calculate RMS for status
    const float *input = (const float *)audio->data[0];
    double sum_sq = 0.0;
    for (size_t i = 0; i < frames; i++) sum_sq += input[i] * input[i];
    current_rms = (float)sqrt(sum_sq / frames);
    
    // Update Sample Rate (Dynamic)
    uint32_t current_sr = 0;
    struct obs_audio_info aoi;
    if (obs_get_audio_info(&aoi)) {
        current_sr = aoi.samples_per_sec;
    }
    if (current_sr == 0) current_sr = 48000; // Fallback
    
    // Update state if changed
    if (sample_rate != current_sr) {
        sample_rate = current_sr;
        sample_rate_ratio = (double)current_sr / 16000.0;
        // Reset resampler
        resample_acc = 0.0f;
    }
    
    double current_ratio = sample_rate_ratio.load();

    if (enabled && !target_model_path.empty()) {
        lock_guard<mutex> lock(queue_mutex);

        // Safety: Limit queue size to prevent memory leak if ASR is too slow
        // Limit to ~60 seconds of audio (16000 * 60 = 960,000 samples)
        if (asr_queue.size() > 960000) {
            asr_queue.clear();
        }

        // Dynamic Downsampling (Nearest Neighbor / Accumulator)
        for (size_t i = 0; i < frames; i++) {
                resample_acc += 1.0f;
                if (resample_acc >= current_ratio) {
                    resample_acc -= (float)current_ratio;
                    asr_queue.push_back(input[i]);
                }
        }
    }
    
    // 2. Buffer Logic
    if (channels.empty()) {
        for (size_t c = 0; c < 8; c++) {
            if (audio->data[c]) channels_count++; else break;
        }
        if (channels_count == 0) channels_count = 1;
        channels.resize(channels_count);
        
        size_t buf_size = current_sr * 12; // Max 12s
        for (auto& ch : channels) {
            ch.buffer.resize(buf_size, 0.0f);
            ch.clean_buffer.resize(buf_size, 0.0f);
        }
    }
    
    // Ensure buffer size covers delay
    size_t delay_samples = (size_t)(cached_delay * current_sr);
    size_t current_buf_size = channels[0].buffer.size();
    
    // Resize check (handle sample rate change or large delay)
    if (delay_samples * 2 > current_buf_size || current_buf_size != current_sr * 12) {
            size_t new_size = max((size_t)(current_sr * 12), delay_samples * 2);
            if (new_size != current_buf_size) {
                current_buf_size = new_size;
                for (auto& ch : channels) {
                    vector<float> new_buf(new_size, 0.0f);
                    ch.buffer = new_buf;
                    ch.clean_buffer = new_buf;
                    ch.head = 0;
                }
            }
    }
    
    if (delay_samples >= current_buf_size) delay_samples = current_buf_size - current_sr;
    
    // Write to buffer
    for (size_t c = 0; c < channels_count; c++) {
        if (!audio->data[c]) continue;
        float *data_in = (float *)audio->data[c];
        auto& ch = channels[c];
        for (size_t i = 0; i < frames; i++) {
            ch.buffer[ch.head] = data_in[i];
            ch.clean_buffer[ch.head] = data_in[i];
            ch.head = (ch.head + 1) % current_buf_size;
            ch.total_written++;
        }
    }
    total_samples_written.fetch_add(frames);
    
    // Apply Beeps (Only if enabled)
    if (enabled) {
        lock_guard<mutex> lock(beep_mutex);
        uint64_t current_write_pos = channels[0].total_written;
        uint64_t play_head_pos = 0;
        if (current_write_pos > delay_samples) {
            play_head_pos = current_write_pos - delay_samples;
        }
        
        for (auto it = pending_beeps.begin(); it != pending_beeps.end(); ) {
                // 1. Handling Late Beeps (Latency > Delay)
                if (it->start_sample < play_head_pos) {
                    it->start_sample = play_head_pos;
                }

                uint64_t start = it->start_sample;
                uint64_t end = it->end_sample;
                
                if (start >= end) {
                    size_t dropped = ++dropped_beeps_count;
                    if (dropped <= 5 || dropped % 10 == 0) {
                        BLOG(LOG_WARNING, "Beep dropped! Latency > Delay. Increase delay setting. (Start: %llu, End: %llu, Head: %llu)", 
                            (unsigned long long)start, (unsigned long long)end, (unsigned long long)play_head_pos);
                    }

                    it = pending_beeps.erase(it);
                    continue;
                }
                
                if (end < current_write_pos - current_buf_size) {
                    it = pending_beeps.erase(it);
                    continue;
                }
                
                for (size_t c = 0; c < channels_count; c++) {
                    auto& ch = channels[c];
                    
                    // Minion Effect: Pre-fetch original audio to avoid feedback loop
                    // Only needed for global_effect == 2
                    std::vector<float> temp_source;
                    uint64_t temp_start_idx = 0;
                    if (global_effect == 2) {
                        uint64_t window_size = 2048; // Approx 40ms
                        // Ensure we have enough history for pitch shifter lookback
                        uint64_t safe_start = (start > window_size) ? start - window_size : 0;
                        // Check buffer limits (don't go before what we have)
                        if (safe_start < current_write_pos - current_buf_size) {
                             safe_start = current_write_pos - current_buf_size;
                        }
                        
                        temp_start_idx = safe_start;
                        size_t len = (size_t)(end - safe_start);
                        temp_source.resize(len);
                        
                        for(size_t k=0; k<len; k++) {
                             uint64_t s_abs = safe_start + k;
                             size_t diff = (size_t)(current_write_pos - s_abs);
                             size_t idx = (ch.head + current_buf_size - (diff % current_buf_size)) % current_buf_size;
                             temp_source[k] = ch.clean_buffer[idx];
                        }
                    }

                    for (uint64_t s = start; s < end; s++) {
                        if (s >= current_write_pos) break; 
                        if (s < current_write_pos - current_buf_size) continue;
                        
                        size_t diff = (size_t)(current_write_pos - s);
                        size_t idx = (ch.head + current_buf_size - (diff % current_buf_size)) % current_buf_size;
                        
                        float val = 0.0f;
                        float original = ch.buffer[idx];
                        float mix = (float)global_mix / 100.0f;

                        if (global_effect == 1) { // Silence
                             val = 0.0f;
                        } else if (global_effect == 2) { // Minion (Pitch Shifter)
                             // Barberpole Pitch Shifter
                             // Pitch Ratio: 2.0 (Octave up) for sharper Minion sound
                             double pitch_ratio = 2.0;
                             double window_size = 2048.0; // Must match pre-fetch window approx
                             
                             // Phase runs 0..1
                             double speed = pitch_ratio - 1.0;
                             double phase = (double)(s % (uint64_t)(window_size / speed)) * speed / window_size;
                             phase -= floor(phase);

                             // Delay decreases from window_size to 0 for Pitch Up
                             double delay_A = (1.0 - phase) * window_size;
                             double delay_B = (1.0 - ((phase + 0.5) - floor(phase + 0.5))) * window_size;
                             
                             // Read from temp_source
                             // temp_source[0] corresponds to temp_start_idx
                             // Current time s corresponds to index (s - temp_start_idx)
                             // We want to read at (s - delay)
                             
                             int64_t read_idx_A = (int64_t)(s - temp_start_idx) - (int64_t)delay_A;
                             int64_t read_idx_B = (int64_t)(s - temp_start_idx) - (int64_t)delay_B;
                             
                             float sample_A = 0.0f;
                             float sample_B = 0.0f;
                             
                             if (read_idx_A >= 0 && read_idx_A < (int64_t)temp_source.size()) sample_A = temp_source[read_idx_A];
                             if (read_idx_B >= 0 && read_idx_B < (int64_t)temp_source.size()) sample_B = temp_source[read_idx_B];
                             
                             // Triangle Window
                             float gain_A = 1.0f - 2.0f * (float)fabs(phase - 0.5);
                             float gain_B = 1.0f - 2.0f * (float)fabs(((phase + 0.5) - floor(phase + 0.5)) - 0.5);
                             
                             val = sample_A * gain_A + sample_B * gain_B;
                             mix = 1.0f; // Force wet mix for voice change
                        } else if (global_effect == 3) { // Telegraph (Morse Code Style)
                             double t = (double)s / (double)current_sr;
                             
                             // Carrier: 750Hz Sine Wave (Classic CW tone)
                             double carrier = sin(2.0 * 3.14159265358979323846 * 750.0 * t);
                             
                             // Pseudo-random Morse Pattern Generator
                             // Use sine waves at different prime frequencies to create a non-repeating pattern of "dits" and "dahs"
                             // 8Hz = fast dits, 3Hz = word spacing rhythm
                             double rhythm = sin(2.0 * 3.14159265358979323846 * 8.0 * t) + 
                                             sin(2.0 * 3.14159265358979323846 * 3.0 * t);
                             
                             // Threshold to create on/off keying
                             // If rhythm > 0, tone is ON. Else OFF.
                             float envelope = (rhythm > 0.0) ? 1.0f : 0.0f;
                             
                             val = 0.15f * (float)carrier * envelope;
                             mix = 1.0f; // Force 100% replacement
                        } else { // Default: Beep
                             double cycles = (double)s * (double)global_freq / (double)current_sr;
                             double phase = cycles - floor(cycles);
                             val = 0.1f * (float)sin(2.0 * 3.14159265358979323846 * phase);
                        }
                        
                        ch.buffer[idx] = (val * mix) + (original * (1.0f - mix));
                    }
                }

                if (end > current_write_pos) {
                    if (it->start_sample < current_write_pos) {
                        it->start_sample = current_write_pos;
                    }
                    ++it;
                } else {
                    it = pending_beeps.erase(it);
                }
        }
    }
    
    // Output Delayed
    uint64_t current_written = channels[0].total_written;
    for (size_t c = 0; c < channels_count; c++) {
        if (!audio->data[c]) continue;
        float *data_out = (float *)audio->data[c];
        auto& ch = channels[c];
        
        for (size_t i = 0; i < frames; i++) {
            int64_t target_abs = (int64_t)(current_written - frames + i) - (int64_t)delay_samples;
            if (target_abs < 0) {
                data_out[i] = 0.0f;
            } else {
                size_t diff = (size_t)(current_written - target_abs);
                size_t idx = (ch.head + current_buf_size - (diff % current_buf_size)) % current_buf_size;
                data_out[i] = ch.buffer[idx];
            }
        }
    }
    
    return audio;
}
