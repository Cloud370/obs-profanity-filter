#include "model-manager.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QDirIterator>
#include <QStandardPaths>
#include <obs-module.h>
#include <obs.h>
#include "unzip.h"
#ifdef _WIN32
#include "iowin32.h"
#endif
#include <curl/curl.h>

PluginModelManager::PluginModelManager(QObject *parent) : QObject(parent) {
    // Curl global init is usually not needed if we just use easy interface carefully, 
    // but good practice to do it once in main. 
    // However, for a plugin, we might not want to mess with global state too much.
    // curl_global_init(CURL_GLOBAL_ALL); // calling this is not thread safe, so maybe skip or do in plugin load
}

PluginModelManager::~PluginModelManager() {
    CancelDownload();
    if (downloadThread.joinable()) {
        downloadThread.join();
    }
}

void PluginModelManager::LoadModels(const QString &jsonPath) {
    models.clear();
    
    blog(LOG_INFO, "Loading models from: %s", jsonPath.toStdString().c_str());

    if (!jsonPath.isEmpty()) {
        QFile file(jsonPath);
        if (file.open(QIODevice::ReadOnly)) {
            QByteArray data = file.readAll();
            QJsonDocument doc = QJsonDocument::fromJson(data);
            
            if (doc.isObject()) {
                QJsonArray arr = doc.object()["models"].toArray();
                blog(LOG_INFO, "Found %lld models in JSON", arr.count());
                for (const auto &val : arr) {
                    QJsonObject obj = val.toObject();
                    ModelInfo info;
                    info.name = obj["name"].toString();
                    info.url = obj["url"].toString();
                    info.id = obj["id"].toString();
                    models.push_back(info);
                }
            } else {
                blog(LOG_WARNING, "JSON document is not an object");
            }
        } else {
            blog(LOG_WARNING, "Failed to open model file: %s", jsonPath.toStdString().c_str());
        }
    } else {
        blog(LOG_INFO, "No model file path provided");
    }

    // Fallback: If no models loaded (file missing or empty), load defaults
    if (models.empty()) {
        blog(LOG_INFO, "No models loaded, using default fallback models");
        models = {
            {
                "Sherpa-ONNX Streaming Zipformer (Bilingual Zh+En)",
                "https://modelscope.cn/models/cloud370/obs-profanity-filter/resolve/master/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20.zip",
                "sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20"
            }
        };
    }
    
    blog(LOG_INFO, "Total models loaded: %zu", models.size());
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

void PluginModelManager::DownloadModel(const QString &modelId) {
    if (isDownloading) {
        // Already downloading
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
        emit downloadError("Model not found in configuration.");
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
    
    // Reset flags
    cancelRequested = false;
    isDownloading = true;

    // Start thread
    // Join previous if needed (shouldn't happen if isDownloading check works, but safe to check)
    if (downloadThread.joinable()) {
        downloadThread.join();
    }
    
    downloadThread = std::thread(&PluginModelManager::DownloadWorker, this, url, downloadDestPath);
}

void PluginModelManager::CancelDownload() {
    if (isDownloading) {
        cancelRequested = true;
    }
}

size_t PluginModelManager::WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    QFile *file = static_cast<QFile*>(userp);
    if (!file || !file->isOpen()) return 0;
    
    qint64 bytesWritten = file->write(static_cast<const char*>(contents), size * nmemb);
    return (size_t)bytesWritten;
}

int PluginModelManager::ProgressCallback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    PluginModelManager *manager = static_cast<PluginModelManager*>(clientp);
    
    if (manager->cancelRequested) {
        return 1; // Abort
    }
    
    if (dltotal > 0) {
        emit manager->downloadProgress((qint64)dlnow, (qint64)dltotal);
    }
    
    return 0;
}

void PluginModelManager::DownloadWorker(QString url, QString destPath) {
    CURL *curl;
    CURLcode res;
    
    QFile file(destPath);
    if (!file.open(QIODevice::WriteOnly)) {
        emit downloadError("Could not open file for writing: " + destPath);
        isDownloading = false;
        return;
    }

    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.toStdString().c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);
        
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        
        // SSL options
        // On Windows with Schannel, this should use system certs
        // We set NATIVE_CA to be sure
        curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
        
        // Follow redirects
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        
        // Fail on error
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        
        // User Agent
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "OBS-Profanity-Filter-Plugin/1.0");

        res = curl_easy_perform(curl);
        
        // Clean up curl
        curl_easy_cleanup(curl);
        
        // Close file
        file.close();

        if (res == CURLE_OK) {
             // Extract to a temporary directory to handle various zip structures
             char *base_path_c = obs_module_get_config_path(obs_current_module(), "models");
             QString basePath = QString::fromUtf8(base_path_c);
             bfree(base_path_c);
             
             QString tempExtractPath = QDir(basePath).filePath(currentDownloadId + "_temp");
             QDir tempDir(tempExtractPath);
             if (tempDir.exists()) {
                 tempDir.removeRecursively();
             }
             tempDir.mkpath(".");

             blog(LOG_INFO, "Extracting to temporary directory: %s", tempExtractPath.toStdString().c_str());
             
             bool success = ExtractArchive(destPath, tempExtractPath, &cancelRequested);
             QFile::remove(destPath); // Remove zip file
             
             if (success) {
                 if (cancelRequested) {
                     blog(LOG_INFO, "Download cancelled during extraction.");
                     tempDir.removeRecursively();
                     return;
                 }

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

                 if (found) {
                     QString finalModelPath = GetModelPath(currentDownloadId);
                     QDir finalDir(finalModelPath);
                     if (finalDir.exists()) {
                         finalDir.removeRecursively();
                     }

                     // Rename/Move the found root to the final destination
                     // QDir::rename can move directories
                     // Note: Rename fails if destination exists, so we removed it above.
                     // Also rename might fail across partitions, but here we are likely in same config dir.
                     if (QDir().rename(modelRootPath, finalModelPath)) {
                         blog(LOG_INFO, "Model installed to: %s", finalModelPath.toStdString().c_str());
                         emit downloadFinished(currentDownloadId);
                         
                         // Clean up temp dir if we moved a subdirectory out of it
                         // If modelRootPath == tempExtractPath, rename moved the whole dir, so tempExtractPath is already gone/invalid
                         if (modelRootPath != tempExtractPath) {
                             tempDir.removeRecursively();
                         }
                     } else {
                         // Fallback copy if rename fails
                         blog(LOG_WARNING, "Rename failed, trying copy...");
                         // Implementing directory copy is tedious in Qt/C++ without helpers. 
                         // But usually rename works within same drive.
                         // Let's just emit error for now or try to be more robust later if needed.
                         tempDir.removeRecursively(); // Cleanup temp files
                         emit downloadError("Failed to move model to final destination.");
                     }
                 } else {
                     blog(LOG_ERROR, "tokens.txt not found in extracted files.");
                     // List files for debugging
                     QDirIterator it(tempExtractPath, QDirIterator::Subdirectories);
                     while (it.hasNext()) {
                         blog(LOG_INFO, "Found file: %s", it.next().toStdString().c_str());
                     }
                     tempDir.removeRecursively(); // Cleanup temp files
                     emit downloadError("Extraction completed but tokens.txt is missing (invalid model structure).");
                 }
             } else {
                 tempDir.removeRecursively(); // Cleanup temp files
                 if (cancelRequested) {
                     blog(LOG_INFO, "Download cancelled during extraction.");
                 } else {
                     emit downloadError("Failed to extract archive.");
                 }
             }
        } else {
            if (cancelRequested) {
                // User cancelled
                blog(LOG_INFO, "Download cancelled by user.");
            } else {
                QString errorMsg = QString("Download failed: %1").arg(curl_easy_strerror(res));
                emit downloadError(errorMsg);
            }
            QFile::remove(destPath);
        }
    } else {
        emit downloadError("Failed to initialize CURL.");
    }
    
    isDownloading = false;
}

bool PluginModelManager::ExtractArchive(const QString &archivePath, const QString &destDir, std::atomic<bool> *cancelFlag) {
    unzFile zipfile = nullptr;
#ifdef _WIN32
    zlib_filefunc64_def ffunc;
    fill_win32_filefunc64W(&ffunc);
    zipfile = unzOpen2_64(archivePath.toStdWString().c_str(), &ffunc);
#else
    zipfile = unzOpen(archivePath.toStdString().c_str());
#endif

    if (!zipfile) {
        blog(LOG_ERROR, "Cannot open zip file: %s", archivePath.toStdString().c_str());
        return false;
    }

    unz_global_info global_info;
    if (unzGetGlobalInfo(zipfile, &global_info) != UNZ_OK) {
        blog(LOG_ERROR, "Could not read zip global info");
        unzClose(zipfile);
        return false;
    }

    char read_buffer[8192];

    for (uLong i = 0; i < global_info.number_entry; ++i) {
        if (cancelFlag && *cancelFlag) {
            unzClose(zipfile);
            return false;
        }

        unz_file_info file_info;
        char filename[1024];
        if (unzGetCurrentFileInfo(zipfile, &file_info, filename, sizeof(filename), NULL, 0, NULL, 0) != UNZ_OK) {
            blog(LOG_ERROR, "Could not read zip file info");
            unzClose(zipfile);
            return false;
        }

        QString currentFileName;
        // Check if filename is utf8 (flag bit 11)
        if (file_info.flag & (1 << 11)) {
            currentFileName = QString::fromUtf8(filename);
        } else {
            currentFileName = QString::fromLocal8Bit(filename);
        }

        blog(LOG_INFO, "Extracting: %s", currentFileName.toStdString().c_str());

        QString fullPath = QDir(destDir).filePath(currentFileName);
        
        // Check if it is a directory
        bool isDir = false;
        if (currentFileName.endsWith('/') || currentFileName.endsWith('\\')) {
            isDir = true;
        }

        if (isDir) {
            QDir dir(fullPath);
            if (!dir.exists()) {
                dir.mkpath(".");
            }
        } else {
            // It is a file
            if (unzOpenCurrentFile(zipfile) != UNZ_OK) {
                blog(LOG_ERROR, "Could not open file in zip: %s", filename);
                unzClose(zipfile);
                return false;
            }

            // Ensure directory exists
            QFileInfo fileInfo(fullPath);
            QDir dir = fileInfo.dir();
            if (!dir.exists()) {
                dir.mkpath(".");
            }

            QFile outFile(fullPath);
            if (!outFile.open(QIODevice::WriteOnly)) {
                blog(LOG_ERROR, "Could not open destination file: %s", fullPath.toStdString().c_str());
                unzCloseCurrentFile(zipfile);
                unzClose(zipfile);
                return false;
            }

            int error = UNZ_OK;
            do {
                error = unzReadCurrentFile(zipfile, read_buffer, sizeof(read_buffer));
                if (error < 0) {
                    blog(LOG_ERROR, "Error reading zip content");
                    outFile.close();
                    unzCloseCurrentFile(zipfile);
                    unzClose(zipfile);
                    return false;
                }
                if (error > 0) {
                    outFile.write(read_buffer, error);
                }
            } while (error > 0);

            outFile.close();
            unzCloseCurrentFile(zipfile);
        }

        if ((i + 1) < global_info.number_entry) {
            if (unzGoToNextFile(zipfile) != UNZ_OK) {
                blog(LOG_ERROR, "Could not read next file in zip");
                unzClose(zipfile);
                return false;
            }
        }
    }

    unzClose(zipfile);
    return true;
}
