#include "asr-model.hpp"
#include <obs.h>
#include <cstring>
#include <cstdio>
#include "logging-macros.hpp"

ASRModel::ASRModel(const std::string& path, const std::string& prov, std::string& error_msg)
    : model_path(path), provider(prov) {
    SherpaOnnxOnlineRecognizerConfig config;
    memset(&config, 0, sizeof(config));

    config.feat_config.sample_rate = 16000;
    config.feat_config.feature_dim = 80;

    std::string tokens = model_path + "/tokens.txt";
    std::string encoder = model_path + "/encoder-epoch-99-avg-1.onnx";

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

    std::string decoder = model_path + "/decoder-epoch-99-avg-1.onnx";
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

    std::string joiner = model_path + "/joiner-epoch-99-avg-1.onnx";
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

    // 使用传入的 provider 参数
    config.model_config.provider = provider.c_str();

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
        BLOG(LOG_INFO, "ASR Model Loaded: %s (provider: %s)", model_path.c_str(), provider.c_str());
    }
}

ASRModel::~ASRModel() {
    if (recognizer) {
        SherpaOnnxDestroyOnlineRecognizer(recognizer);
        BLOG(LOG_INFO, "ASR Model Unloaded: %s", model_path.c_str());
    }
}

std::map<ModelCacheKey, std::weak_ptr<ASRModel>> ModelManager::models_;
std::mutex ModelManager::mutex_;

std::shared_ptr<ASRModel> ModelManager::Get(const std::string& path, const std::string& provider, std::string& error_out) {
    std::lock_guard<std::mutex> lock(mutex_);

    ModelCacheKey key{path, provider};

    // Check if already loaded
    auto it = models_.find(key);
    if (it != models_.end()) {
        auto ptr = it->second.lock();
        if (ptr) {
            return ptr;
        }
        models_.erase(it);
    }

    // Load new
    BLOG(LOG_INFO, "🆕 [ModelManager] Loading NEW model for: %s (provider: %s)", path.c_str(), provider.c_str());
    auto ptr = std::make_shared<ASRModel>(path, provider, error_out);
    if (!ptr->recognizer) {
        return nullptr; // Failed
    }

    models_[key] = ptr;
    return ptr;
}
