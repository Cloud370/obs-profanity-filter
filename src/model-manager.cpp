#include "model-manager.hpp"
#include "logging-macros.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QDirIterator>
#include <obs-module.h>
#include <obs.h>

PluginModelManager::PluginModelManager(QObject *parent) : FileDownloader(parent) {
}

PluginModelManager::~PluginModelManager() {
    // 基类析构函数会处理取消下载和线程清理
}

void PluginModelManager::LoadModels(const QString &jsonPath) {
    models.clear();

    BLOG(LOG_INFO, "Loading models from: %s", jsonPath.toStdString().c_str());

    if (!jsonPath.isEmpty()) {
        QFile file(jsonPath);
        if (file.open(QIODevice::ReadOnly)) {
            QByteArray data = file.readAll();
            QJsonDocument doc = QJsonDocument::fromJson(data);

            if (doc.isObject()) {
                QJsonArray arr = doc.object()["models"].toArray();
                BLOG(LOG_INFO, "Found %lld models in JSON", arr.count());
                for (const auto &val : arr) {
                    QJsonObject obj = val.toObject();
                    ModelInfo info;
                    info.name = obj["name"].toString();
                    info.url = obj["url"].toString();
                    info.id = obj["id"].toString();
                    if (obj.contains("offset")) {
                        info.offset = obj["offset"].toInt();
                    }
                    if (obj.contains("delay")) {
                        info.delay = obj["delay"].toInt();
                    }
                    models.push_back(info);
                }
            } else {
                BLOG(LOG_WARNING, "JSON document is not an object");
            }
        } else {
            BLOG(LOG_WARNING, "Failed to open model file: %s", jsonPath.toStdString().c_str());
        }
    } else {
        BLOG(LOG_INFO, "No model file path provided");
    }

    // Fallback: If no models loaded (file missing or empty), load defaults
    if (models.empty()) {
        BLOG(LOG_INFO, "No models loaded, using default fallback models");
        models = {
            {
                "[54M]轻量",
                "https://modelscope.cn/models/cloud370/obs-profanity-filter/resolve/master/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23.zip",
                "sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23",
                0,
                1000
            },
            {
                "[357MB]标准",
                "https://modelscope.cn/models/cloud370/obs-profanity-filter/resolve/master/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20.zip",
                "sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20",
                0,
                500
            },
            {
                "[597MB]最强",
                "https://modelscope.cn/models/cloud370/obs-profanity-filter/resolve/master/sherpa-onnx-streaming-zipformer-zh-2025-06-30.zip",
                "sherpa-onnx-streaming-zipformer-zh-2025-06-30",
                0,
                1000
            }
        };
    }

    BLOG(LOG_INFO, "Total models loaded: %zu", models.size());
}

const std::vector<ModelInfo>& PluginModelManager::GetModels() const {
    return models;
}

QString PluginModelManager::GetModelPath(const QString &modelId) const {
    // We store models in the plugin config directory under "models" subdirectory
    char *path = obs_module_get_config_path(obs_current_module(), "models");
    if (!path) return QString();

    QString qpath = QString::fromUtf8(path);
    bfree(path);

    return QDir(qpath).filePath(modelId);
}

bool PluginModelManager::IsModelInstalled(const QString &modelId) const {
    QString path = GetModelPath(modelId);
    if (path.isEmpty()) return false;

    // Check if directory exists and is not empty
    QDir dir(path);
    return dir.exists() && !dir.isEmpty();
}

bool PluginModelManager::DeleteModel(const QString &modelId) {
    QString path = GetModelPath(modelId);
    if (path.isEmpty()) return false;

    QDir dir(path);
    if (dir.exists()) {
        BLOG(LOG_INFO, "Deleting model: %s", path.toStdString().c_str());
        return dir.removeRecursively();
    }
    return false;
}

void PluginModelManager::DownloadModel(const QString &modelId) {
    if (IsDownloading()) {
        emit downloadError("另一个下载正在进行中");
        return;
    }

    currentDownloadId = modelId;

    // Find url
    QString url;
    for (const auto &m : models) {
        if (m.id == modelId) {
            url = m.url;
            break;
        }
    }

    if (url.isEmpty()) {
        emit downloadError("在配置中找不到此模型");
        return;
    }

    // Prepare destination
    char *base_path_c = obs_module_get_config_path(obs_current_module(), "models");
    QString basePath = QString::fromUtf8(base_path_c);
    bfree(base_path_c);

    QDir dir(basePath);
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    // Temp file
    QString fileName = url.split('/').last();
    downloadDestPath = dir.filePath(fileName);

    // 使用基类的下载功能
    StartDownload(url, downloadDestPath, modelId);
}

bool PluginModelManager::OnDownloadComplete(const QString &downloadId, const QString &destPath) {
    // Extract to a temporary directory to handle various zip structures
    char *base_path_c = obs_module_get_config_path(obs_current_module(), "models");
    QString basePath = QString::fromUtf8(base_path_c);
    bfree(base_path_c);

    QString tempExtractPath = QDir(basePath).filePath(downloadId + "_temp");
    QDir tempDir(tempExtractPath);
    if (tempDir.exists()) {
        tempDir.removeRecursively();
    }
    tempDir.mkpath(".");

    BLOG(LOG_INFO, "Extracting to temporary directory: %s", tempExtractPath.toStdString().c_str());

    bool success = ExtractArchive(destPath, tempExtractPath, &GetCancelFlag());
    QFile::remove(destPath); // Remove zip file

    if (!success) {
        tempDir.removeRecursively();
        if (GetCancelFlag().load()) {
            BLOG(LOG_INFO, "Download cancelled during extraction.");
        } else {
            emit downloadError("解压文件失败");
        }
        return false;
    }

    if (GetCancelFlag().load()) {
        BLOG(LOG_INFO, "Download cancelled during extraction.");
        tempDir.removeRecursively();
        return false;
    }

    // Finalize installation
    if (!FinalizeModelInstallation(downloadId, tempExtractPath)) {
        tempDir.removeRecursively();
        return false;
    }

    return true;
}

bool PluginModelManager::FinalizeModelInstallation(const QString &modelId, const QString &tempExtractPath) {
    // Logic to find the actual model root (where tokens.txt is)
    QString modelRootPath;
    bool found = false;

    // Search recursively for tokens.txt to find the model root directory
    QDirIterator it(tempExtractPath, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QString filePath = it.next();
        if (QFileInfo(filePath).fileName() == "tokens.txt") {
            modelRootPath = QFileInfo(filePath).absolutePath();
            found = true;
            break;
        }
    }

    if (!found) {
        BLOG(LOG_ERROR, "tokens.txt not found in extracted files.");
        // List files for debugging
        QDirIterator debugIt(tempExtractPath, QDirIterator::Subdirectories);
        while (debugIt.hasNext()) {
            BLOG(LOG_INFO, "Found file: %s", debugIt.next().toStdString().c_str());
        }
        emit downloadError("解压完成但 tokens.txt 缺失 (无效的模型结构)");
        return false;
    }

    QString finalModelPath = GetModelPath(modelId);
    QDir finalDir(finalModelPath);
    if (finalDir.exists()) {
        finalDir.removeRecursively();
    }

    // Rename/Move the found root to the final destination
    if (QDir().rename(modelRootPath, finalModelPath)) {
        BLOG(LOG_INFO, "Model installed to: %s", finalModelPath.toStdString().c_str());

        // Clean up temp dir if we moved a subdirectory out of it
        QDir tempDir(tempExtractPath);
        if (modelRootPath != tempExtractPath) {
            tempDir.removeRecursively();
        }
        return true;
    } else {
        BLOG(LOG_WARNING, "Rename failed, model installation incomplete.");
        emit downloadError("无法将模型移动到最终目录");
        return false;
    }
}
