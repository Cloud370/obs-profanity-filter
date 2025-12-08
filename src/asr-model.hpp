#pragma once

#include <string>
#include <map>
#include <memory>
#include <mutex>
#include "sherpa-onnx/c-api/c-api.h"

struct ASRModel {
    const SherpaOnnxOnlineRecognizer *recognizer = nullptr;
    std::string model_path;
    std::string provider;

    ASRModel(const std::string& path, const std::string& provider, std::string& error_msg);
    ~ASRModel();
};

// 缓存键：模型路径 + provider
struct ModelCacheKey {
    std::string path;
    std::string provider;

    bool operator<(const ModelCacheKey& other) const {
        if (path != other.path) return path < other.path;
        return provider < other.provider;
    }
};

class ModelManager {
public:
    static std::shared_ptr<ASRModel> Get(const std::string& path, const std::string& provider, std::string& error_out);

private:
    static std::map<ModelCacheKey, std::weak_ptr<ASRModel>> models_;
    static std::mutex mutex_;
};
