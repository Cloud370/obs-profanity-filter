#pragma once

#include "file-downloader.hpp"
#include <vector>
#include <string>

struct ModelInfo {
    QString name;
    QString url;
    QString id; // Folder name
    int offset = 0; // Default offset in ms
    int delay = 500; // Recommended delay in ms
};

class PluginModelManager : public FileDownloader {
    Q_OBJECT

public:
    explicit PluginModelManager(QObject *parent = nullptr);
    ~PluginModelManager() override;

    // Load available models from models.json
    void LoadModels(const QString &jsonPath);

    // Get list of loaded models
    const std::vector<ModelInfo>& GetModels() const;

    // Check if a model is already downloaded/installed
    bool IsModelInstalled(const QString &modelId) const;

    // Get the full path to the installed model directory
    QString GetModelPath(const QString &modelId) const;

    // Start downloading a model
    void DownloadModel(const QString &modelId);

    // Delete an installed model
    bool DeleteModel(const QString &modelId);

protected:
    // 重写基类的下载完成处理
    bool OnDownloadComplete(const QString &downloadId, const QString &destPath) override;

private:
    std::vector<ModelInfo> models;
    QString currentDownloadId;
    QString downloadDestPath;

    // 模型专用：查找并移动模型根目录
    bool FinalizeModelInstallation(const QString &modelId, const QString &tempExtractPath);
};
