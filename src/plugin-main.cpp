#include <obs-module.h>
#include <obs.h>
#include <obs-frontend-api.h>
#include <string>
#include <vector>
#include <mutex>
#include <deque>
#include <thread>
#include <atomic>
#include <regex>
#include <cmath>
#include <algorithm>
#include <filesystem>

#include "sherpa-onnx/c-api/c-api.h"
#include "plugin-config.hpp"
#include "cpp-pinyin/Pinyin.h"
#include "cpp-pinyin/G2pglobal.h"
#include "cpp-pinyin/ManTone.h"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <set>
#include <map>
#include <windows.h>

using namespace std;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-profanity-filter", "en-US")

#define BLOG(level, format, ...) blog(level, "[Profanity Filter] " format, ##__VA_ARGS__)

// --- Shared Model Management ---

struct ASRModel {
    const SherpaOnnxOnlineRecognizer *recognizer = nullptr;
    string model_path;
    
    ASRModel(const string& path, string& error_msg) : model_path(path) {
        SherpaOnnxOnlineRecognizerConfig config;
        memset(&config, 0, sizeof(config));
        
        config.feat_config.sample_rate = 16000;
        config.feat_config.feature_dim = 80;
        
        string tokens = model_path + "/tokens.txt";
        string encoder = model_path + "/encoder-epoch-99-avg-1.onnx";
        
        // Check files existence
        FILE *f = fopen(tokens.c_str(), "r");
        if (!f) {
             error_msg = "文件缺失: tokens.txt";
             return;
        }
        fclose(f);

        f = fopen(encoder.c_str(), "r");
        if (f) { fclose(f); } else { 
            encoder = model_path + "/encoder.onnx"; 
            f = fopen(encoder.c_str(), "r");
            if (!f) {
                error_msg = "文件缺失: encoder.onnx (或 epoch-99)";
                return;
            }
            fclose(f);
        }
        
        string decoder = model_path + "/decoder-epoch-99-avg-1.onnx";
        f = fopen(decoder.c_str(), "r");
        if (f) { fclose(f); } else { 
            decoder = model_path + "/decoder.onnx"; 
            f = fopen(decoder.c_str(), "r");
            if (!f) {
                error_msg = "文件缺失: decoder.onnx (或 epoch-99)";
                return;
            }
            fclose(f);
        }
        
        string joiner = model_path + "/joiner-epoch-99-avg-1.onnx";
        f = fopen(joiner.c_str(), "r");
        if (f) { fclose(f); } else { 
            joiner = model_path + "/joiner.onnx"; 
            f = fopen(joiner.c_str(), "r");
            if (!f) {
                error_msg = "文件缺失: joiner.onnx (或 epoch-99)";
                return;
            }
            fclose(f);
        }
        
        config.model_config.transducer.encoder = encoder.c_str();
        config.model_config.transducer.decoder = decoder.c_str();
        config.model_config.transducer.joiner = joiner.c_str();
        config.model_config.tokens = tokens.c_str();
        config.model_config.num_threads = 1;
        config.model_config.provider = "cpu";
        
        // Use modified_beam_search for better accuracy on short phrases
        config.decoding_method = "modified_beam_search"; 
        config.max_active_paths = 4;
        
        // Enable Endpoint detection to reset state after silence
        // This helps with recognition consistency for isolated phrases
        config.enable_endpoint = 1;
        config.rule1_min_trailing_silence = 2.4f;
        config.rule2_min_trailing_silence = 1.2f;
        config.rule3_min_utterance_length = 0.0f;
        
        recognizer = SherpaOnnxCreateOnlineRecognizer(&config);
        if (!recognizer) {
            error_msg = "引擎创建失败 (内部错误)";
        } else {
            BLOG(LOG_INFO, "ASR Model Loaded: %s", model_path.c_str());
        }
    }
    
    ~ASRModel() {
        if (recognizer) {
            SherpaOnnxDestroyOnlineRecognizer(recognizer);
            BLOG(LOG_INFO, "ASR Model Unloaded: %s", model_path.c_str());
        }
    }
};

class ModelManager {
public:
    static shared_ptr<ASRModel> Get(const string& path, string& error_out) {
        lock_guard<mutex> lock(mutex_);
        
        // Check if already loaded
        auto it = models_.find(path);
        if (it != models_.end()) {
            auto ptr = it->second.lock();
            if (ptr) {
                return ptr;
            }
            models_.erase(it);
        }
        
        // Load new
        BLOG(LOG_INFO, "🆕 [ModelManager] Loading NEW model for: %s", path.c_str());
        auto ptr = make_shared<ASRModel>(path, error_out);
        if (!ptr->recognizer) {
            return nullptr; // Failed
        }
        
        models_[path] = ptr;
        return ptr;
    }
    
private:
    static std::map<string, weak_ptr<ASRModel>> models_;
    static mutex mutex_;
};

std::map<string, weak_ptr<ASRModel>> ModelManager::models_;
mutex ModelManager::mutex_;

// --- Helper Functions ---

static string NormalizePinyin(const string& p) {
    string s = p;
    // Map zh->z, ch->c, sh->s
    if (s.size() >= 2) {
        if (s.substr(0, 2) == "zh") s = "z" + s.substr(2);
        else if (s.substr(0, 2) == "ch") s = "c" + s.substr(2);
        else if (s.substr(0, 2) == "sh") s = "s" + s.substr(2);
    }
    // Map ng->n (ang->an, eng->en, ing->in, ong->on)
    if (s.size() >= 2 && s.substr(s.size()-2) == "ng") {
        s = s.substr(0, s.size()-1);
    }
    return s;
}

// --- Plugin Instance ---

struct ProfanityFilter {
    obs_source_t *context;
    
    // Local Properties
    bool enabled = true; // Only local property
    
    // Global Cache (synced in audio thread or ASR thread)
    string target_model_path;
    string loaded_model_path;
    double cached_delay = 1.5;
    
    // History
    deque<string> log_history;
    string current_partial_text; 
    mutex history_mutex;

    // State
    shared_ptr<ASRModel> asr_model; 
    const SherpaOnnxOnlineStream *stream = nullptr;
    
    // Audio Buffer
    struct ChannelBuffer {
        vector<float> buffer;
        size_t head = 0; 
        uint64_t total_written = 0;
    };
    
    vector<ChannelBuffer> channels;
    atomic<uint32_t> sample_rate{48000};
    atomic<double> sample_rate_ratio{3.0}; // sample_rate / 16000.0
    atomic<uint64_t> total_samples_written{0}; // Safe access for ASRLoop
    size_t channels_count = 0;
    
    // Resampler state
    float resample_acc = 0.0f;
    
    // ASR Thread
    thread asr_thread;
    atomic<bool> running{false};
    mutex queue_mutex;
    deque<float> asr_queue; 
    
    // Beep Map
    struct BeepRange {
        uint64_t start_sample; 
        uint64_t end_sample;
    };
    mutex beep_mutex;
    vector<BeepRange> pending_beeps;
    
    string initialization_error = "";
    float current_rms = 0.0f;
    
    obs_data_t *settings = nullptr;
    uint64_t last_reset_sample_16k = 0;
    set<size_t> processed_matches; 
    atomic<size_t> dropped_beeps_count{0};
    
    // Pinyin Support
    shared_ptr<Pinyin::Pinyin> pinyin_converter;
    vector<vector<string>> cached_pinyin_patterns;
    string cached_dirty_words_str_for_pinyin;
    // Cache for single hanzi pinyin to avoid re-conversion
    map<string, vector<string>> pinyin_cache;
    
    ProfanityFilter(obs_source_t *ctx) : context(ctx) {
        // Initial sync with global config
        GlobalConfig *cfg = GetGlobalConfig();
        lock_guard<mutex> lock(cfg->mutex);
        target_model_path = cfg->model_path;
        cached_delay = cfg->delay_seconds;
    }
    
    ~ProfanityFilter() {
        Stop();
        if (settings) obs_data_release(settings);
        if (stream) {
            SherpaOnnxDestroyOnlineStream(stream);
            stream = nullptr;
        }
        asr_model.reset();
    }

    void LogToFile(const string& message) {
        // Truncate overly long messages to protect memory
        string safe_message = message;
        if (safe_message.length() > 1000) {
            safe_message = safe_message.substr(0, 1000) + "...(truncated)";
        }

        GlobalConfig *cfg = GetGlobalConfig();
        string debug_log_path;
        {
            lock_guard<mutex> lock(cfg->mutex);
            debug_log_path = cfg->debug_log_path;
        }
        
        {
            lock_guard<mutex> lock(history_mutex);
            auto t = time(nullptr);
            auto tm = *localtime(&t);
            stringstream ss;
            ss << put_time(&tm, "[%H:%M:%S] ") << safe_message;
            
            log_history.push_front(ss.str());
            if (log_history.size() > 50) log_history.pop_back();
        }

        if (debug_log_path.empty()) return;
        try {
            ofstream outfile;
            outfile.open(debug_log_path, ios_base::app);
            if (outfile.is_open()) {
                auto t = time(nullptr);
                auto tm = *localtime(&t);
                outfile << put_time(&tm, "[%Y-%m-%d %H:%M:%S] ") << safe_message << endl;
            }
        } catch (...) {}
    }
    
    string GetHistoryString() {
        lock_guard<mutex> lock(history_mutex);
        stringstream ss;
        auto t = time(nullptr);
        auto tm = *localtime(&t);
        
        ss << "=== 实时状态 (更新时间: " << put_time(&tm, "%H:%M:%S") << ") ===" << endl;
        if (asr_model && asr_model->recognizer) {
             ss << "引擎状态: 🟢 运行中 (" << loaded_model_path << ")" << endl;
             ss << "当前音量: " << fixed << setprecision(4) << current_rms << endl;
        } else {
             ss << "引擎状态: 🔴 未就绪" << endl;
             if (!initialization_error.empty()) {
                 ss << "错误信息: " << initialization_error << endl;
             } else {
                 ss << "提示信息: 请在工具菜单中配置模型" << endl;
             }
        }
        ss << "待处理队列: " << asr_queue.size() << " 样本" << endl;
        
        size_t beeps_count = 0;
        {
            lock_guard<mutex> beep_lock(beep_mutex);
            beeps_count = pending_beeps.size();
        }
        ss << "待播放Beep: " << beeps_count << endl;
        
        size_t dropped = dropped_beeps_count.load();
        if (dropped > 0) {
            ss << "⚠️ 已丢弃Beep (延迟过高): " << dropped << " 次 (建议增加延迟时间)" << endl;
        }

        ss << "实时识别: " << (current_partial_text.empty() ? "(..." : current_partial_text) << endl;
        
        int count = 0;
        for (const auto& line : log_history) {
            ss << line << "\n";
            if (++count >= 20) break;
        }
        return ss.str();
    }

    void LoadModel(const string& path) {
        if (stream) {
            SherpaOnnxDestroyOnlineStream(stream);
            stream = nullptr;
        }
        asr_model.reset();
        initialization_error = "";
        
        if (path.empty()) {
             initialization_error = "未选择模型路径";
             return;
        }
        
        string err;
        asr_model = ModelManager::Get(path, err);
        
        if (asr_model && asr_model->recognizer) {
            stream = SherpaOnnxCreateOnlineStream(asr_model->recognizer);
            loaded_model_path = path;
            LogToFile("引擎初始化成功");
        } else {
            initialization_error = err.empty() ? "引擎初始化失败" : err;
            LogToFile("错误: " + initialization_error);
        }
    }
    
    void Start() {
        if (running) return;
        running = true;
        asr_thread = thread(&ProfanityFilter::ASRLoop, this);
    }
    
    void Stop() {
        running = false;
        if (asr_thread.joinable()) asr_thread.join();
    }
    
    void ASRLoop() {
        uint64_t total_samples_popped_16k = 0;
        
        // Time sync
        uint64_t start_offset_input = 0;
        uint64_t last_feed_offset = 0;
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
            
            if (asr_model && asr_model->recognizer && stream) {
                SherpaOnnxOnlineStreamAcceptWaveform(stream, 16000, chunk.data(), (int32_t)chunk.size());
                while (SherpaOnnxIsOnlineStreamReady(asr_model->recognizer, stream)) {
                    SherpaOnnxDecodeOnlineStream(asr_model->recognizer, stream);
                }
                
                const SherpaOnnxOnlineRecognizerResult *result = SherpaOnnxGetOnlineStreamResult(asr_model->recognizer, stream);
                if (result) {
                    // Get Patterns and Config from Global
                    GlobalConfig *cfg = GetGlobalConfig();
                    vector<regex> patterns;
                    bool use_pinyin;
                    string current_dirty_words;
                    {
                        lock_guard<mutex> lock(cfg->mutex);
                        patterns = cfg->dirty_patterns; // Copy
                        use_pinyin = cfg->use_pinyin;
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

                        // 1. Regex Matching
                        for (const auto& pattern : patterns) {
                            sregex_iterator begin(full_text.begin(), full_text.end(), pattern);
                            sregex_iterator end;
                            
                            for (auto i = begin; i != end; ++i) {
                                smatch match = *i;
                                size_t m_start_char = match.position();
                                if (processed_matches.count(m_start_char)) continue;

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
                                    
                                    // Safe margin: 50ms = 0.05 * sr
                                    uint32_t margin = (uint32_t)(0.05 * current_sr);
                                    start_abs = (start_abs > margin) ? start_abs - margin : 0; 
                                    end_abs += margin; 
                                    
                                    lock_guard<mutex> b_lock(beep_mutex);
                                    pending_beeps.push_back({start_abs, end_abs});
                                    
                                    stringstream ss;
                                    ss << "已屏蔽: " << match.str();
                                    LogToFile(ss.str());
                                    
                                    processed_matches.insert(m_start_char);
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
                                     if (VirtualQuery((LPCVOID)&obs_module_pointer, &mbi, sizeof(mbi))) {
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
                                                 // DLL: root/bin/64bit/plugin.dll
                                                 // Dict: root/data/dict
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
                                    LogToFile("Pinyin Engine Initialized from: " + dict_path);
                                } else {
                                    static bool logged_error = false;
                                    if (!logged_error) {
                                        LogToFile("Error: Could not find 'dict' directory for Pinyin engine.");
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
                                    LogToFile(ss.str());
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
                                            
                                            // Avoid double blocking (if regex already caught it)
                                            if (processed_matches.count(char_pos)) continue;
                                            
                                            float start_time = result->timestamps[start_token];
                                            float end_time = (end_token < result->count - 1) ? result->timestamps[end_token+1] : (result->timestamps[end_token] + 0.2f);
                                            
                                            uint64_t start_16k = last_reset_sample_16k + (uint64_t)(start_time * 16000.0f);
                                            uint64_t end_16k = last_reset_sample_16k + (uint64_t)(end_time * 16000.0f);
                                            
                                            uint64_t start_abs = (uint64_t)(start_16k * current_ratio) + start_offset_input;
                                            uint64_t end_abs = (uint64_t)(end_16k * current_ratio) + start_offset_input;
                                            
                                            // Safe margin: 50ms = 0.05 * sr
                                            uint32_t margin = (uint32_t)(0.05 * current_sr);
                                            start_abs = (start_abs > margin) ? start_abs - margin : 0; 
                                            end_abs += margin; 
                                            
                                            lock_guard<mutex> b_lock(beep_mutex);
                                            pending_beeps.push_back({start_abs, end_abs});
                                            
                                            stringstream ss;
                                            ss << "已屏蔽(拼音): ";
                                            for(const auto& p : pat) ss << p << " ";
                                            ss << "[匹配源: ";
                                            for(int k=0; k<pat.size(); k++) ss << text_pinyins[i+k] << " ";
                                            ss << "]";
                                            LogToFile(ss.str());
                                            
                                            processed_matches.insert(char_pos);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    SherpaOnnxDestroyOnlineRecognizerResult(result);
                }
                
                // Check endpoint or force reset if segment is too long (> 600s = 10min)
                bool force_reset = (total_samples_popped_16k - last_reset_sample_16k) > (16000 * 600);

                if (force_reset || SherpaOnnxOnlineStreamIsEndpoint(asr_model->recognizer, stream)) {
                    if (force_reset) {
                        LogToFile("Info: Periodic reset of ASR stream (segment > 10min)");
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
};

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
    uint32_t frames = audio->frames;
    if (!audio->data[0]) return audio;

    // Sync with Global Config
    GlobalConfig *cfg = GetGlobalConfig();
    double global_delay;
    string global_model;
    bool global_mute;
    int global_freq;
    int global_mix;
    
    {
        lock_guard<mutex> lock(cfg->mutex);
        global_delay = cfg->delay_seconds;
        global_model = cfg->model_path;
        global_mute = cfg->mute_mode;
        global_freq = cfg->beep_frequency;
        global_mix = cfg->beep_mix_percent;
    }
    
    // Update Filter State
    {
        lock_guard<mutex> lock(filter->queue_mutex);
        filter->target_model_path = global_model;
    }
    filter->cached_delay = global_delay;
    
    // 1. Push to ASR (Only if enabled and model loaded)
    // But we always calculate RMS for status
    const float *input = (const float *)audio->data[0];
    double sum_sq = 0.0;
    for (size_t i = 0; i < frames; i++) sum_sq += input[i] * input[i];
    filter->current_rms = (float)sqrt(sum_sq / frames);
    
    // Update Sample Rate (Dynamic)
    uint32_t current_sr = 0;
    struct obs_audio_info aoi;
    if (obs_get_audio_info(&aoi)) {
        current_sr = aoi.samples_per_sec;
    }
    if (current_sr == 0) current_sr = 48000; // Fallback
    
    // Update state if changed
    if (filter->sample_rate != current_sr) {
        filter->sample_rate = current_sr;
        filter->sample_rate_ratio = (double)current_sr / 16000.0;
        // Reset resampler
        filter->resample_acc = 0.0f;
    }
    
    double current_ratio = filter->sample_rate_ratio.load();

    if (filter->enabled && !filter->target_model_path.empty()) {
        lock_guard<mutex> lock(filter->queue_mutex);

        // Safety: Limit queue size to prevent memory leak if ASR is too slow
        // Limit to ~60 seconds of audio (16000 * 60 = 960,000 samples)
        if (filter->asr_queue.size() > 960000) {
            filter->asr_queue.clear();
            // Note: We cannot safely log from audio thread usually, but this is a critical fallback
        }

        // Dynamic Downsampling (Nearest Neighbor / Accumulator)
        // We want to output 16000Hz samples.
        // Step size = input_rate / 16000.
        // e.g. 48k -> step 3.0; 44.1k -> step 2.75625
        
        for (size_t i = 0; i < frames; i++) {
             filter->resample_acc += 1.0f;
             if (filter->resample_acc >= current_ratio) {
                 filter->resample_acc -= (float)current_ratio;
                 // Take sample (Nearest) or Average?
                 // For simplicity and speed, just take current sample.
                 // Better quality: average previous samples.
                 // Given the previous implementation was averaging 3 samples for 48k, 
                 // let's stick to simple picking or basic averaging if possible.
                 // To keep it extremely simple and robust for 44.1k: simple decimation is "okay" for ASR.
                 // Ideally we should use a low-pass filter but that's heavy.
                 
                 filter->asr_queue.push_back(input[i]);
             }
        }
    }
    
    // 2. Buffer Logic
    if (filter->channels.empty()) {
        for (size_t c = 0; c < 8; c++) {
            if (audio->data[c]) filter->channels_count++; else break;
        }
        if (filter->channels_count == 0) filter->channels_count = 1;
        filter->channels.resize(filter->channels_count);
        
        size_t buf_size = current_sr * 12; // Max 12s
        for (auto& ch : filter->channels) {
            ch.buffer.resize(buf_size, 0.0f);
        }
    }
    
    // Ensure buffer size covers delay
    size_t delay_samples = (size_t)(filter->cached_delay * current_sr);
    size_t current_buf_size = filter->channels[0].buffer.size();
    
    // Resize check (handle sample rate change or large delay)
    if (delay_samples * 2 > current_buf_size || current_buf_size != current_sr * 12) {
         size_t new_size = max((size_t)(current_sr * 12), delay_samples * 2);
         if (new_size != current_buf_size) {
             // Resize buffers (clears content, causing a glitch, but SR change is rare)
             current_buf_size = new_size;
             for (auto& ch : filter->channels) {
                 vector<float> new_buf(new_size, 0.0f);
                 // Copy old data? Complex due to circular buffer. 
                 // Just reset for safety on SR change.
                 ch.buffer = new_buf;
                 ch.head = 0;
                 // Keep total_written to maintain continuity if possible, 
                 // but indices are invalid. Resetting is safer.
                 // However, we can't reset total_written easily without affecting ASR sync.
                 // So we just accept a gap in audio buffer.
             }
         }
    }
    
    if (delay_samples >= current_buf_size) delay_samples = current_buf_size - current_sr;
    
    // Write to buffer
    for (size_t c = 0; c < filter->channels_count; c++) {
        if (!audio->data[c]) continue;
        float *data_in = (float *)audio->data[c];
        auto& ch = filter->channels[c];
        for (size_t i = 0; i < frames; i++) {
            ch.buffer[ch.head] = data_in[i];
            ch.head = (ch.head + 1) % current_buf_size;
            ch.total_written++;
        }
    }
    filter->total_samples_written.fetch_add(frames);
    
    // Apply Beeps (Only if enabled)
    if (filter->enabled) {
        lock_guard<mutex> lock(filter->beep_mutex);
        uint64_t current_write_pos = filter->channels[0].total_written;
        uint64_t play_head_pos = 0;
        if (current_write_pos > delay_samples) {
            play_head_pos = current_write_pos - delay_samples;
        }
        
        for (auto it = filter->pending_beeps.begin(); it != filter->pending_beeps.end(); ) {
             // 1. Handling Late Beeps (Latency > Delay)
             // If the beep start is already played (behind play_head_pos), move it to now.
             if (it->start_sample < play_head_pos) {
                 it->start_sample = play_head_pos;
             }

             uint64_t start = it->start_sample;
             uint64_t end = it->end_sample;
             
             if (start >= end) {
                 // Beep duration became 0 or negative (completely missed and passed)
                 // Log warning for the first few times or if periodic
                 size_t dropped = ++filter->dropped_beeps_count;
                 if (dropped <= 5 || dropped % 10 == 0) {
                     blog(LOG_WARNING, "[Profanity Filter] Beep dropped! Latency > Delay. Increase delay setting. (Start: %llu, End: %llu, Head: %llu)", 
                         (unsigned long long)start, (unsigned long long)end, (unsigned long long)play_head_pos);
                 }

                 it = filter->pending_beeps.erase(it);
                 continue;
             }
             
             if (end < current_write_pos - current_buf_size) {
                 it = filter->pending_beeps.erase(it);
                 continue;
             }
             
             for (size_t c = 0; c < filter->channels_count; c++) {
                 auto& ch = filter->channels[c];
                 for (uint64_t s = start; s < end; s++) {
                     if (s >= current_write_pos) break; 
                     if (s < current_write_pos - current_buf_size) continue;
                     
                     size_t diff = (size_t)(current_write_pos - s);
                     size_t idx = (ch.head + current_buf_size - (diff % current_buf_size)) % current_buf_size;
                     
                     float val = 0.0f;
                     if (!global_mute) {
                        // Use double for phase calculation to avoid precision loss after long runtime
                        double cycles = (double)s * (double)global_freq / (double)current_sr;
                        double phase = cycles - floor(cycles);
                        val = 0.1f * (float)sin(2.0 * 3.14159265358979323846 * phase);
                     }
                     
                     float mix = (float)global_mix / 100.0f;
                     float original = ch.buffer[idx];
                     ch.buffer[idx] = (val * mix) + (original * (1.0f - mix));
                 }
             }

             if (end > current_write_pos) {
                 if (it->start_sample < current_write_pos) {
                     it->start_sample = current_write_pos;
                 }
                 ++it;
             } else {
                 it = filter->pending_beeps.erase(it);
             }
        }
    }
    
    // Output Delayed
    uint64_t current_written = filter->channels[0].total_written;
    for (size_t c = 0; c < filter->channels_count; c++) {
        if (!audio->data[c]) continue;
        float *data_out = (float *)audio->data[c];
        auto& ch = filter->channels[c];
        
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
