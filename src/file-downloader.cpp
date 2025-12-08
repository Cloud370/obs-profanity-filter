#include "file-downloader.hpp"
#include "logging-macros.hpp"
#include <QDir>
#include <QFileInfo>
#include <obs-module.h>
#include "unzip.h"
#ifdef _WIN32
#include "iowin32.h"
#endif

FileDownloader::FileDownloader(QObject *parent) : QObject(parent) {
    // CURL 初始化由插件主模块管理，这里不重复调用 curl_global_init
}

FileDownloader::~FileDownloader() {
    CancelDownload();
    if (downloadThread.joinable()) {
        downloadThread.join();
    }
}

void FileDownloader::CancelDownload() {
    if (isDownloading.load()) {
        cancelRequested = true;
    }
}

void FileDownloader::StartDownload(const QString &url, const QString &destPath, const QString &downloadId) {
    if (isDownloading.load()) {
        emit downloadError("另一个下载正在进行中 / Another download is in progress");
        return;
    }

    // 确保目标目录存在
    QFileInfo fileInfo(destPath);
    QDir dir = fileInfo.absoluteDir();
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    // 重置状态
    cancelRequested = false;
    isDownloading = true;

    // 等待之前的线程结束
    if (downloadThread.joinable()) {
        downloadThread.join();
    }

    // 启动下载线程
    downloadThread = std::thread(&FileDownloader::DownloadWorker, this, url, destPath, downloadId);
}

size_t FileDownloader::WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    QFile *file = static_cast<QFile*>(userp);
    if (!file || !file->isOpen()) return 0;

    qint64 bytesWritten = file->write(static_cast<const char*>(contents), size * nmemb);
    return static_cast<size_t>(bytesWritten);
}

int FileDownloader::ProgressCallback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    (void)ultotal;
    (void)ulnow;

    FileDownloader *downloader = static_cast<FileDownloader*>(clientp);

    if (downloader->cancelRequested.load()) {
        return 1; // 中止下载
    }

    if (dltotal > 0) {
        emit downloader->downloadProgress(static_cast<qint64>(dlnow), static_cast<qint64>(dltotal));
    }

    return 0;
}

void FileDownloader::DownloadWorker(QString url, QString destPath, QString downloadId) {
    CURL *curl = nullptr;
    CURLcode res;
    QFile file(destPath);

    if (!file.open(QIODevice::WriteOnly)) {
        emit downloadError(QString("无法打开文件进行写入: %1").arg(destPath));
        isDownloading = false;
        return;
    }

    curl = curl_easy_init();
    if (!curl) {
        emit downloadError("CURL 初始化失败");
        file.close();
        isDownloading = false;
        return;
    }

    // 配置 CURL
    curl_easy_setopt(curl, CURLOPT_URL, url.toStdString().c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    // SSL 选项 (Windows Schannel)
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);

    // 跟随重定向
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    // HTTP 错误时失败
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

    // User Agent
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "OBS-Profanity-Filter-Plugin/1.0");

    // 执行下载
    res = curl_easy_perform(curl);

    curl_easy_cleanup(curl);
    file.close();

    if (res == CURLE_OK) {
        // 调用子类的后处理
        if (OnDownloadComplete(downloadId, destPath)) {
            emit downloadFinished(downloadId);
        }
        // 如果 OnDownloadComplete 返回 false, 它应该已经发出了错误信号
    } else {
        if (cancelRequested.load()) {
            BLOG(LOG_INFO, "下载已被用户取消");
        } else {
            QString errorMsg = QString("下载失败: %1").arg(curl_easy_strerror(res));
            emit downloadError(errorMsg);
        }
        QFile::remove(destPath);
    }

    isDownloading = false;
}

bool FileDownloader::OnDownloadComplete(const QString &downloadId, const QString &destPath) {
    // 默认实现：什么都不做，直接成功
    (void)downloadId;
    (void)destPath;
    return true;
}

bool FileDownloader::ExtractArchive(const QString &archivePath, const QString &destDir, std::atomic<bool> *cancelFlag) {
    unzFile zipfile = nullptr;

#ifdef _WIN32
    zlib_filefunc64_def ffunc;
    fill_win32_filefunc64W(&ffunc);
    zipfile = unzOpen2_64(archivePath.toStdWString().c_str(), &ffunc);
#else
    zipfile = unzOpen(archivePath.toStdString().c_str());
#endif

    if (!zipfile) {
        BLOG(LOG_ERROR, "无法打开 ZIP 文件: %s", archivePath.toStdString().c_str());
        return false;
    }

    unz_global_info global_info;
    if (unzGetGlobalInfo(zipfile, &global_info) != UNZ_OK) {
        BLOG(LOG_ERROR, "无法读取 ZIP 全局信息");
        unzClose(zipfile);
        return false;
    }

    char read_buffer[8192];

    for (uLong i = 0; i < global_info.number_entry; ++i) {
        if (cancelFlag && cancelFlag->load()) {
            unzClose(zipfile);
            return false;
        }

        unz_file_info file_info;
        char filename[1024];
        if (unzGetCurrentFileInfo(zipfile, &file_info, filename, sizeof(filename), NULL, 0, NULL, 0) != UNZ_OK) {
            BLOG(LOG_ERROR, "无法读取 ZIP 文件信息");
            unzClose(zipfile);
            return false;
        }

        QString currentFileName;
        // 检查 UTF-8 标志 (bit 11)
        if (file_info.flag & (1 << 11)) {
            currentFileName = QString::fromUtf8(filename);
        } else {
            currentFileName = QString::fromLocal8Bit(filename);
        }

        QString fullPath = QDir(destDir).filePath(currentFileName);

        // 检查是否为目录
        bool isDir = currentFileName.endsWith('/') || currentFileName.endsWith('\\');

        if (isDir) {
            QDir dir(fullPath);
            if (!dir.exists()) {
                dir.mkpath(".");
            }
        } else {
            // 文件
            if (unzOpenCurrentFile(zipfile) != UNZ_OK) {
                BLOG(LOG_ERROR, "无法打开 ZIP 内文件: %s", filename);
                unzClose(zipfile);
                return false;
            }

            // 确保父目录存在
            QFileInfo fileInfo(fullPath);
            QDir dir = fileInfo.dir();
            if (!dir.exists()) {
                dir.mkpath(".");
            }

            QFile outFile(fullPath);
            if (!outFile.open(QIODevice::WriteOnly)) {
                BLOG(LOG_ERROR, "无法创建目标文件: %s", fullPath.toStdString().c_str());
                unzCloseCurrentFile(zipfile);
                unzClose(zipfile);
                return false;
            }

            int error = UNZ_OK;
            do {
                error = unzReadCurrentFile(zipfile, read_buffer, sizeof(read_buffer));
                if (error < 0) {
                    BLOG(LOG_ERROR, "读取 ZIP 内容时出错");
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
                BLOG(LOG_ERROR, "无法读取下一个 ZIP 文件条目");
                unzClose(zipfile);
                return false;
            }
        }
    }

    unzClose(zipfile);
    return true;
}
