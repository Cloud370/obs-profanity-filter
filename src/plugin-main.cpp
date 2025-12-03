#include <obs-module.h>
#include <obs.h>
#include <string>
#include <vector>
#include <mutex>
#include <deque>
#include <thread>
#include <atomic>
#include <regex>
#include <iostream>
#include <cmath>
#include <algorithm>

#include "sherpa-onnx/c-api/c-api.h"

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
        
        config.decoding_method = "greedy_search";
        config.max_active_paths = 4;
        
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
                BLOG(LOG_INFO, "♻️ [ModelManager] Reuse existing model for: %s (Ref Count: %ld)", path.c_str(), ptr.use_count());
                return ptr;
            }
            // Expired, remove
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

// --- Plugin Instance ---

struct ProfanityFilter {
    obs_source_t *context;
    
    // Properties
    bool enabled = true;
    string model_dir;
    float delay_seconds = 1.5f;
    vector<regex> dirty_patterns;
    string debug_log_path;
    string last_log_text = "";
    bool show_console = false;
    
    // Advanced Properties
    bool mute_mode = false; // true = silence, false = beep
    int beep_frequency = 1000;
    
    // History
    deque<string> log_history;
    string current_partial_text; // Real-time partial result
    mutex history_mutex;

    // State
    shared_ptr<ASRModel> asr_model; // Shared model instance
    const SherpaOnnxOnlineStream *stream = nullptr;
    
    // Audio Buffer (Circular Buffer per channel)
    struct ChannelBuffer {
        vector<float> buffer;
        size_t head = 0; 
        uint64_t total_written = 0;
    };
    
    vector<ChannelBuffer> channels;
    uint32_t sample_rate = 48000;
    size_t channels_count = 0;
    
    // ASR Thread
    thread asr_thread;
    atomic<bool> running{false};
    mutex queue_mutex;
    vector<float> asr_queue; // Input audio for ASR (mono, 16k)
    
    // Beep Map
    struct BeepRange {
        uint64_t start_sample; // Absolute sample index at OBS sample rate
        uint64_t end_sample;
    };
    mutex beep_mutex;
    vector<BeepRange> pending_beeps;
    
    string initialization_error = "";
    float current_rms = 0.0f;
    
    // OBS Settings Reference
    obs_data_t *settings = nullptr;

    // ASR Tracking
    uint64_t asr_segment_start_sample = 0; // Absolute sample index (OBS rate) where current ASR segment started
    set<size_t> processed_matches; // Offsets in current segment text that have been processed
    
    ProfanityFilter(obs_source_t *ctx) : context(ctx) {
        // Default dirty patterns (Chinese & English)
        vector<string> defaults = {"fuck", "shit", "bitch", "卧槽", "他妈", "傻逼", "操", "逼的", "你妈", "死全家"};
        UpdatePatterns(defaults);
    }
    
    ~ProfanityFilter() {
        Stop();
        if (settings) obs_data_release(settings);
        if (show_console) ToggleConsole(false);
        
        // Order matters: Stream depends on Recognizer (inside asr_model).
        // We must destroy stream before releasing asr_model.
        if (stream) {
            SherpaOnnxDestroyOnlineStream(stream);
            stream = nullptr;
        }
        asr_model.reset(); // Release shared model reference
    }

    void ToggleConsole(bool show) {
        if (show) {
            if (GetConsoleWindow()) return; // Already allocated
            
            AllocConsole();
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
            // Set console code page to UTF-8 for Chinese support
            SetConsoleOutputCP(65001);
            
            // Disable the "Close" (X) button to prevent users from accidentally killing OBS
            HWND hwnd = GetConsoleWindow();
            if (hwnd != NULL) {
                HMENU hMenu = GetSystemMenu(hwnd, FALSE);
                if (hMenu != NULL) {
                    EnableMenuItem(hMenu, SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
                }
            }
            
            cout << "=== 语音脏话屏蔽插件 - 调试控制台 ===" << endl;
            cout << "提示: 请勿关闭此窗口，否则会导致 OBS 退出！请在插件设置中取消勾选以关闭。" << endl;
            cout << "------------------------------------------------------------" << endl;
        } else {
            FreeConsole();
        }
        show_console = show;
    }
    
    void UpdatePatterns(const vector<string>& patterns) {
        dirty_patterns.clear();
        for (const auto& p : patterns) {
            try {
                dirty_patterns.emplace_back(p, regex::icase);
            } catch (...) {
                BLOG(LOG_WARNING, "Invalid regex pattern: %s", p.c_str());
            }
        }
    }

    void LogToFile(const string& message) {
        // Add to history
        {
            lock_guard<mutex> lock(history_mutex);
            
            // Format time
            auto t = time(nullptr);
            auto tm = *localtime(&t);
            stringstream ss;
            ss << put_time(&tm, "[%H:%M:%S] ") << message;
            
            log_history.push_front(ss.str());
            if (log_history.size() > 100) {
                log_history.pop_back();
            }
            
            // Console output
            if (show_console) {
                // Clear line first (handle potential partial text)
                cout << "\r                                                                                \r";
                cout << put_time(&tm, "[%H:%M:%S] ") << message << endl;
            }
        }

        if (debug_log_path.empty()) return;
        try {
            ofstream outfile;
            outfile.open(debug_log_path, ios_base::app); // Append mode
            if (outfile.is_open()) {
                auto t = time(nullptr);
                auto tm = *localtime(&t);
                outfile << put_time(&tm, "[%Y-%m-%d %H:%M:%S] ") << message << endl;
                outfile.flush(); // Ensure written immediately
            }
        } catch (...) {
            // Ignore logging errors
        }
    }
    
    string GetHistoryString() {
        lock_guard<mutex> lock(history_mutex);
        stringstream ss;
        
        // Status Info
        auto t = time(nullptr);
        auto tm = *localtime(&t);
        
        ss << "=== 实时状态 (更新时间: " << put_time(&tm, "%H:%M:%S") << ") ===" << endl;
        if (asr_model && asr_model->recognizer) {
             ss << "引擎状态: 🟢 运行中 (" << asr_model->model_path << ")" << endl;
             ss << "当前音量: " << fixed << setprecision(4) << current_rms << endl;
        } else {
             ss << "引擎状态: 🔴 未初始化" << endl;
             if (!initialization_error.empty()) {
                 ss << "错误信息: " << initialization_error << endl;
             } else {
                 ss << "提示信息: 请选择模型路径" << endl;
             }
        }
        ss << "待处理队列: " << asr_queue.size() << " 样本" << endl;
        ss << "实时识别: " << (current_partial_text.empty() ? "(..." : current_partial_text) << endl;
        ss << "----------------------------------------" << endl;
        ss << "=== 最近日志 (显示前 30 条) ===" << endl;
        
        int count = 0;
        for (const auto& line : log_history) {
            ss << line << "\n";
            if (++count >= 30) {
                ss << "... (更多日志请查看调试文件) ...\n";
                break;
            }
        }
        return ss.str();
    }

    void InitializeASR(const string& path) {
        lock_guard<mutex> lock(queue_mutex);
        model_dir = path;
        
        // Cleanup old stream/model
        if (stream) {
            SherpaOnnxDestroyOnlineStream(stream);
            stream = nullptr;
        }
        asr_model.reset();
        
        initialization_error = "";
        
        if (model_dir.empty()) {
             initialization_error = "未选择模型路径";
             return;
        }
        
        // Use ModelManager to get shared instance
        string err;
        asr_model = ModelManager::Get(model_dir, err);
        
        if (asr_model && asr_model->recognizer) {
            stream = SherpaOnnxCreateOnlineStream(asr_model->recognizer);
            LogToFile("引擎初始化成功 (共享实例)");
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
        // Track total samples fed to ASR in this thread
        uint64_t current_segment_offset_16k = 0; 
        // This tracks how many samples we have fed since the last Reset.
        // Actually, Sherpa's timestamps are relative to the last Reset.
        // So we need to know the absolute time of the "Last Reset".
        
        // We need a mapping from "ASR Stream Time" to "Absolute OBS Time".
        // Absolute OBS Time = (Total Samples Fed to Queue * 3) approx.
        // Let's track total samples popped from queue.
        
        uint64_t total_samples_popped_16k = 0;
        uint64_t last_reset_sample_16k = 0;
        
        // Fix: Calculate initial time offset to sync with existing audio stream
        // If we are restarting the filter (e.g. settings change), the audio stream
        // might have been running for a long time. We need to align our 0 time.
        uint64_t start_offset_48k = 0;
        if (!channels.empty()) {
            uint64_t tw = channels[0].total_written;
            size_t q_size;
            {
                lock_guard<mutex> lock(queue_mutex);
                q_size = asr_queue.size();
            }
            // The front of the queue corresponds to (Now - QueueDuration)
            // QueueDuration = q_size * 3 (approx)
            uint64_t backlog_48k = q_size * 3;
            if (tw > backlog_48k) {
                start_offset_48k = tw - backlog_48k;
            }
        }

        while (running) {
            vector<float> chunk;
            {
                lock_guard<mutex> lock(queue_mutex);
                // Process up to 0.2s at a time
                if (!asr_queue.empty()) {
                    size_t n = min((size_t)3200, asr_queue.size()); 
                    chunk.assign(asr_queue.begin(), asr_queue.begin() + n);
                    asr_queue.erase(asr_queue.begin(), asr_queue.begin() + n);
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
                    if (result->count > 0 && result->tokens_arr && result->timestamps) {
                        string full_text = "";
                        struct TokenInfo {
                            string text;
                            float start;
                            float end;
                            size_t start_char;
                            size_t end_char;
                        };
                        vector<TokenInfo> token_map;
                        
                        for(int i=0; i<result->count; i++) {
                            string t = result->tokens_arr[i];
                            float t_start = result->timestamps[i];
                            float t_end = (i < result->count - 1) ? result->timestamps[i+1] : (t_start + 0.2f);
                            
                            size_t start_c = full_text.length();
                            full_text += t;
                            size_t end_c = full_text.length();
                            
                            token_map.push_back({t, t_start, t_end, start_c, end_c});
                        }
                        
                        // Log full text
                        // Optimize: Only log final results to avoid spam
                        if (!full_text.empty()) {
                            last_log_text = full_text;
                            
                            // Update partial text for UI
                            {
                                lock_guard<mutex> lock(history_mutex);
                                current_partial_text = full_text;
                            }

                            // Real-time Console Output
                            if (show_console && !full_text.empty()) {
                                // Truncate text to fit one line to avoid scrolling spam
                                string display_text = full_text;
                                if (display_text.length() > 60) {
                                    display_text = "..." + display_text.substr(display_text.length() - 60);
                                }
                                
                                // Pad with spaces to overwrite previous content
                                cout << "\r实时: " << display_text << "            \r" << flush;
                            }
                        }

                        // Check patterns
                        for (const auto& pattern : dirty_patterns) {
                            sregex_iterator begin(full_text.begin(), full_text.end(), pattern);
                            sregex_iterator end;
                            
                            for (auto i = begin; i != end; ++i) {
                                smatch match = *i;
                                size_t m_start_char = match.position();
                                if (processed_matches.count(m_start_char)) continue;

                                size_t m_end_char = m_start_char + match.length();
                                
                                float m_start_time = -1.0f;
                                float m_end_time = -1.0f;
                                
                                for (const auto& tm : token_map) {
                                    if (tm.end_char > m_start_char && tm.start_char < m_end_char) {
                                        if (m_start_time < 0) m_start_time = tm.start;
                                        m_end_time = tm.end;
                                    }
                                }
                                
                                if (m_start_time >= 0) {
                                    // Timestamp is seconds relative to last reset.
                                    // Absolute 16k sample index = last_reset_sample_16k + (m_start_time * 16000)
                                    
                                    uint64_t start_16k = last_reset_sample_16k + (uint64_t)(m_start_time * 16000.0f);
                                    uint64_t end_16k = last_reset_sample_16k + (uint64_t)(m_end_time * 16000.0f);
                                    
                                    // Convert to 48k (OBS)
                                    // We assume strict 3x ratio for simplicity (16k -> 48k)
                                    // And add the initial offset
                                    uint64_t start_abs = (start_16k * 3) + start_offset_48k;
                                    uint64_t end_abs = (end_16k * 3) + start_offset_48k;
                                    
                                    // Apply padding (at 48k)
                                    start_abs = (start_abs > 2400) ? start_abs - 2400 : 0; 
                                    end_abs += 2400; 
                                    
                                    lock_guard<mutex> b_lock(beep_mutex);
                                    pending_beeps.push_back({start_abs, end_abs});
                                    
                                    stringstream ss;
                                    ss << "已屏蔽: " << match.str() << " (时间: " << m_start_time << "s - " << m_end_time << "s)";
                                    LogToFile(ss.str());
                                    
                                    processed_matches.insert(m_start_char);
                                }
                            }
                        }
                    }
                    SherpaOnnxDestroyOnlineRecognizerResult(result);
                }
                
                if (SherpaOnnxOnlineStreamIsEndpoint(asr_model->recognizer, stream)) {
                    if (!last_log_text.empty()) {
                        LogToFile("识别片段: " + last_log_text);
                    }
                    SherpaOnnxOnlineStreamReset(asr_model->recognizer, stream);
                    // The stream is reset. The NEXT sample fed will be t=0.
                    // So the "Last Reset" sample index becomes the current total samples fed.
                    last_reset_sample_16k = total_samples_popped_16k;
                    last_log_text = ""; // Reset log cache for new segment
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

static const char *get_name(void *unused)
{
    return "语音脏话屏蔽 (延迟处理)";
}

static void *create(obs_data_t *settings, obs_source_t *context)
{
    ProfanityFilter *filter = new ProfanityFilter(context);
    
    filter->enabled = obs_data_get_bool(settings, "enabled");
    const char *model_path = obs_data_get_string(settings, "model_path");
    
    // Keep reference to settings
    filter->settings = settings;
    obs_data_addref(settings);
    
    filter->InitializeASR(model_path ? model_path : "");
    filter->delay_seconds = (float)obs_data_get_double(settings, "delay_ms") / 1000.0f;
    if (filter->delay_seconds < 0.1f) filter->delay_seconds = 0.1f;
    
    filter->Start();
    return filter;
}

static void destroy(void *data)
{
    ProfanityFilter *filter = (ProfanityFilter *)data;
    delete filter;
}

static void update(void *data, obs_data_t *settings)
{
    ProfanityFilter *filter = (ProfanityFilter *)data;
    
    // Update settings reference
    if (filter->settings) obs_data_release(filter->settings);
    filter->settings = settings;
    obs_data_addref(settings);

    filter->enabled = obs_data_get_bool(settings, "enabled");

    const char *path = obs_data_get_string(settings, "model_path");
    string safe_path = path ? path : "";
    if (filter->model_dir != safe_path) {
        // Safely re-initialize
        filter->Stop();
        filter->InitializeASR(safe_path);
        filter->Start();
    }
    
    float new_delay = (float)obs_data_get_double(settings, "delay_ms") / 1000.0f;
    if (new_delay != filter->delay_seconds) {
        // Handle delay change? 
        // It requires resizing buffers. Simple way: just update var, buffer logic adapts.
        filter->delay_seconds = new_delay;
    }
    
    // Update dirty words
    const char *words = obs_data_get_string(settings, "dirty_words");
    if (words) {
        vector<string> pattern_list;
        string s = words;
        stringstream ss(s);
        string item;
        while (getline(ss, item, ',')) {
            // Trim whitespace
            item.erase(0, item.find_first_not_of(" \t\n\r"));
            item.erase(item.find_last_not_of(" \t\n\r") + 1);
            if (!item.empty()) {
                pattern_list.push_back(item);
            }
        }
        if (!pattern_list.empty()) {
            filter->UpdatePatterns(pattern_list);
        }
    }
    
    filter->mute_mode = obs_data_get_bool(settings, "mute_mode");
    filter->beep_frequency = (int)obs_data_get_int(settings, "beep_freq");
    
    bool show_console = obs_data_get_bool(settings, "show_console");
    if (show_console != filter->show_console) {
        filter->ToggleConsole(show_console);
    }

    const char *log_path = obs_data_get_string(settings, "debug_log_path");
    if (log_path) {
        filter->debug_log_path = log_path;
    }
}

static bool refresh_history(obs_properties_t *props, obs_property_t *p, void *data)
{
    ProfanityFilter *filter = (ProfanityFilter *)data;
    if (filter) {
        string hist = filter->GetHistoryString();
        
        // Update the property description for OBS_TEXT_INFO
        obs_property_t *view = obs_properties_get(props, "history_view_info");
        if (view) {
            obs_property_set_long_description(view, hist.c_str());
        }
        
        // Update the value for OBS_TEXT_MULTILINE (if we switch to it)
        if (filter->settings) {
            obs_data_set_string(filter->settings, "history_view", hist.c_str());
        }
        
        return true; 
    }
    return false;
}

static obs_properties_t *get_properties(void *data)
{
    obs_properties_t *props = obs_properties_create();
    
    // --- Group 1: 使用说明 (Usage) ---
    obs_properties_add_group(props, "group_usage", "使用说明 & 重要提示", OBS_GROUP_NORMAL, NULL);
    
    obs_property_t *usage = obs_properties_add_text(props, "usage_info", "", OBS_TEXT_INFO);
    obs_property_set_long_description(usage, 
        "⚠️ 重要提示：\n"
        "本插件仅提供音频延迟和脏话过滤功能。\n"
        "为了保持音画同步，请务必在您的【视频源】上添加【视频延迟 (异步)】滤镜，\n"
        "并将延迟时间设置为与本插件相同的数值 (默认 1500ms)。\n\n"
        "配置步骤：\n"
        "1. 选择 Sherpa-ONNX 模型文件夹。\n"
        "2. 设置延迟时间。\n"
        "3. (可选) 修改屏蔽词列表。\n"
        "4. 为对应的视频源添加视频延迟滤镜。");

    // --- Group 2: 基本设置 (Basic) ---
    obs_properties_add_bool(props, "enabled", "启用过滤");
    obs_properties_add_path(props, "model_path", "模型路径 (Sherpa ONNX)", OBS_PATH_DIRECTORY, NULL, NULL);
    obs_properties_add_float(props, "delay_ms", "延迟时间 (毫秒)", 100.0, 5000.0, 100.0);
    obs_properties_add_text(props, "dirty_words", "屏蔽词列表 (逗号分隔)", OBS_TEXT_MULTILINE);
    
    // --- Group 3: 高级选项 (Advanced) ---
    obs_properties_add_bool(props, "mute_mode", "静音模式 (不播放哔声)");
    obs_properties_add_int(props, "beep_freq", "哔声频率 (Hz)", 200, 5000, 100);
    obs_properties_add_bool(props, "show_console", "显示调试控制台 (独立窗口)");
    obs_properties_add_path(props, "debug_log_path", "调试日志文件 (可选)", OBS_PATH_FILE, NULL, NULL);
    
    // --- Group 4: 识别记录 (History) ---
    obs_properties_add_button(props, "refresh_btn", "🔄 刷新状态与日志", refresh_history);
    
    // Use OBS_TEXT_MULTILINE for a proper text box!
    // Note: OBS_TEXT_MULTILINE binds to 'history_view' in settings.
    // We must ensure 'history_view' is populated in settings.
    obs_property_t *p = obs_properties_add_text(props, "history_view", "日志输出:", OBS_TEXT_MULTILINE);
    
    // Also update the content right now if possible (for initial open)
    if (data) {
        ProfanityFilter *filter = (ProfanityFilter *)data;
        string s = filter->GetHistoryString();
        if (filter->settings) {
             obs_data_set_string(filter->settings, "history_view", s.c_str());
        }
    }

    return props;
}

static void get_defaults(obs_data_t *settings)
{
    obs_data_set_default_bool(settings, "enabled", true);
    obs_data_set_default_double(settings, "delay_ms", 1500.0);
    obs_data_set_default_string(settings, "dirty_words", "fuck, shit, bitch, 卧槽, 他妈, 傻逼, 操, 逼的, 你妈, 死全家");
    obs_data_set_default_bool(settings, "mute_mode", false);
    obs_data_set_default_int(settings, "beep_freq", 1000);
    obs_data_set_default_bool(settings, "show_console", false);
    obs_data_set_default_string(settings, "history_view", "点击上方刷新按钮获取最新日志...");
}

static struct obs_audio_data *filter_audio(void *data, struct obs_audio_data *audio)
{
    ProfanityFilter *filter = (ProfanityFilter *)data;
    uint32_t frames = audio->frames;
    
    if (!audio->data[0]) return audio;

    // 1. Push to ASR (Downmix to Mono) & Calculate RMS
    if (filter->enabled) {
        lock_guard<mutex> lock(filter->queue_mutex);
        const float *input = (const float *)audio->data[0];
        
        // Calculate RMS for monitoring
        double sum_sq = 0.0;
        for (size_t i = 0; i < frames; i++) {
            sum_sq += input[i] * input[i];
        }
        filter->current_rms = (float)sqrt(sum_sq / frames);

        // Resample 48k -> 16k (Average 3 samples)
        for (size_t i = 0; i < frames; i += 3) {
             if (i + 2 < frames) {
                 float val = (input[i] + input[i+1] + input[i+2]) / 3.0f;
                 filter->asr_queue.push_back(val);
             } else if (i < frames) {
                 filter->asr_queue.push_back(input[i]);
             }
        }
    } else {
        filter->current_rms = 0.0f;
    }
    
    // 2. Buffer Logic
    // Ensure channels are initialized
    if (filter->channels.empty()) {
        // Heuristic: Check up to 8 channels
        for (size_t c = 0; c < 8; c++) {
            if (audio->data[c]) filter->channels_count++;
            else break;
        }
        if (filter->channels_count == 0) filter->channels_count = 1; // Fallback
        
        filter->channels.resize(filter->channels_count);
        
        // Pre-allocate buffers for 5 seconds max delay
        size_t buf_size = 48000 * 5;
        for (auto& ch : filter->channels) {
            ch.buffer.resize(buf_size, 0.0f);
            ch.head = 0;
            ch.total_written = 0;
        }
    }
    
    // Check delay in samples
    size_t delay_samples = (size_t)(filter->delay_seconds * 48000); // Assuming 48k
    size_t buf_size = filter->channels[0].buffer.size();
    
    // Write to buffer
    for (size_t c = 0; c < filter->channels_count; c++) {
        if (!audio->data[c]) continue;
        float *data_in = (float *)audio->data[c];
        auto& ch = filter->channels[c];
        
        for (size_t i = 0; i < frames; i++) {
            ch.buffer[ch.head] = data_in[i];
            ch.head = (ch.head + 1) % buf_size;
            ch.total_written++;
        }
    }
    
    // Apply Beeps (Retroactively modify buffer ahead of read pointer)
    // We need to check pending_beeps
    {
        lock_guard<mutex> lock(filter->beep_mutex);
        // Remove old beeps
        // Iterate pending beeps
        for (auto it = filter->pending_beeps.begin(); it != filter->pending_beeps.end(); ) {
             uint64_t start = it->start_sample;
             uint64_t end = it->end_sample;
             
             // Current Read Position (approx) = total_written - delay_samples
             // We can only modify if it hasn't been read yet.
             // Actually, we are about to read.
             // The range to read is [total_written - delay, total_written - delay + frames]
             
             // But we can modify anywhere in the buffer that is valid.
             // Since we just wrote 'frames', we are at 'total_written'.
             // We want to beep range [start, end].
             // Check if this range is still in the future relative to "read pointer".
             
             // For simplicity, we just overwrite the circular buffer content for the specified absolute range.
             // If it's already read, too bad.
             
             bool keep = false;
             // We only need to keep it if we haven't fully overwritten it with beep yet? 
             // Or just do it once?
             // If we do it once, we might miss it if it overlaps the wrap-around?
             // No, random access write to buffer is fine.
             
             // We just process the beep and remove it.
             // BUT: ASR is delayed. The beep might be for a range we just wrote (if ASR is fast) or 1s ago.
             // As long as it is within 'delay_seconds' history, we can beep it.
             
             uint64_t current_write_pos = filter->channels[0].total_written;
             // Oldest valid sample in buffer = current_write_pos - buf_size
             
             if (end < current_write_pos - buf_size) {
                 // Too old, expired
                 it = filter->pending_beeps.erase(it);
                 continue;
             }
             
             // Apply beep
             for (size_t c = 0; c < filter->channels_count; c++) {
                 auto& ch = filter->channels[c];
                 
                 for (uint64_t s = start; s < end; s++) {
                     // Map absolute sample 's' to buffer index
                     // s must be <= current_write_pos (we can't beep future)
                     // s must be > current_write_pos - buf_size
                     
                     if (s >= current_write_pos) break; // Future? shouldn't happen if logic is correct
                     if (s < current_write_pos - buf_size) continue; // Too old
                     
                     // Calculate index
                     // We know: current_write_pos maps to ch.head
                     // diff = current_write_pos - s
                     // idx = (ch.head - diff + buf_size) % buf_size
                     
                     size_t diff = (size_t)(current_write_pos - s);
                     size_t idx = (ch.head + buf_size - (diff % buf_size)) % buf_size;
                     
                     // Generate sine wave beep or silence
                     // t = s / 48000.0
                     float val = 0.0f;
                     if (!filter->mute_mode) {
                        float t = (float)s / 48000.0f;
                        float freq = (float)filter->beep_frequency;
                        val = 0.1f * sinf(2.0f * 3.14159f * freq * t);
                     }
                     
                     ch.buffer[idx] = val;
                 }
             }
             
             // Remove handled beep
             it = filter->pending_beeps.erase(it);
        }
    }
    
    // Read from buffer (Delayed)
    // Output is [total_written - delay_samples, ...]
    // We need to ensure we have enough data (at start, output silence)
    
    uint64_t current_written = filter->channels[0].total_written;
    
    for (size_t c = 0; c < filter->channels_count; c++) {
        if (!audio->data[c]) continue;
        float *data_out = (float *)audio->data[c]; // Overwrite input with delayed output
        auto& ch = filter->channels[c];
        
        for (size_t i = 0; i < frames; i++) {
            // We want sample at absolute index: current_written - frames + i - delay_samples
            // Wait.
            // current_written is the count AFTER writing the current chunk.
            // The samples just written are indices [current_written - frames, current_written - 1].
            // We want to output samples from 'delay_seconds' ago.
            // Target index = (current_written - frames + i) - delay_samples.
            
            int64_t target_abs = (int64_t)(current_written - frames + i) - (int64_t)delay_samples;
            
            if (target_abs < 0) {
                data_out[i] = 0.0f; // Silence at start
            } else {
                // Read from buffer
                // Map target_abs to index
                size_t diff = (size_t)(current_written - target_abs);
                size_t idx = (ch.head + buf_size - (diff % buf_size)) % buf_size;
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
    return true;
}
