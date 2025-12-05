#pragma once

#include <string>
#include <map>
#include <memory>
#include <mutex>
#include "sherpa-onnx/c-api/c-api.h"

struct ASRModel {
    const SherpaOnnxOnlineRecognizer *recognizer = nullptr;
    std::string model_path;
    
    ASRModel(const std::string& path, std::string& error_msg);
    ~ASRModel();
};

class ModelManager {
public:
    static std::shared_ptr<ASRModel> Get(const std::string& path, std::string& error_out);
    
private:
    static std::map<std::string, std::weak_ptr<ASRModel>> models_;
    static std::mutex mutex_;
};
