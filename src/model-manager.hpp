#pragma once

#include <QObject>
#include <QFile>
#include <vector>
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <curl/curl.h>

struct ModelInfo {
    QString name;
    QString url;
    QString id; // Folder name
};

class PluginModelManager : public QObject {
    Q_OBJECT

public:
    explicit PluginModelManager(QObject *parent = nullptr);
    ~PluginModelManager();

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
    
    // Cancel current download
    void CancelDownload();

signals:
    void downloadProgress(qint64 bytesReceived, qint64 bytesTotal);
    void downloadFinished(const QString &modelId);
    void downloadError(const QString &message);

private:
    std::vector<ModelInfo> models;
    QString currentDownloadId;
    QString downloadDestPath;
    
    // Threading for download
    std::thread downloadThread;
    std::atomic<bool> isDownloading{false};
    std::atomic<bool> cancelRequested{false};
    
    // Helper to extract downloaded archive
    // cancelFlag can be null. If provided, extraction checks it to abort early.
    bool ExtractArchive(const QString &archivePath, const QString &destDir, std::atomic<bool> *cancelFlag = nullptr);

    // Worker function for thread
    void DownloadWorker(QString url, QString destPath);

    // Curl callbacks
    static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp);
    static int ProgressCallback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);
};
