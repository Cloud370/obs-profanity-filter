#pragma once

#include <QObject>
#include <QFile>
#include <QString>
#include <thread>
#include <atomic>
#include <functional>
#include <curl/curl.h>

/**
 * FileDownloader - 通用文件下载器基类
 *
 * 提供 HTTP 下载、ZIP 解压、进度回调等通用功能。
 * 可被 PluginModelManager、RuntimeManager 等子类继承复用。
 */
class FileDownloader : public QObject {
    Q_OBJECT

public:
    explicit FileDownloader(QObject *parent = nullptr);
    virtual ~FileDownloader();

    // 下载状态
    bool IsDownloading() const { return isDownloading.load(); }

    // 取消当前下载
    void CancelDownload();

signals:
    // 下载进度 (已下载字节, 总字节)
    void downloadProgress(qint64 bytesReceived, qint64 bytesTotal);

    // 下载完成 (下载ID由子类定义)
    void downloadFinished(const QString &id);

    // 下载错误
    void downloadError(const QString &message);

protected:
    // 启动下载任务 (供子类调用)
    // @param url 下载地址
    // @param destPath 本地保存路径
    // @param downloadId 下载标识符 (用于完成信号)
    void StartDownload(const QString &url, const QString &destPath, const QString &downloadId);

    // 解压 ZIP 文件到目标目录
    // @param archivePath ZIP 文件路径
    // @param destDir 解压目标目录
    // @param cancelFlag 取消标志 (可选)
    // @return 是否成功
    bool ExtractArchive(const QString &archivePath, const QString &destDir, std::atomic<bool> *cancelFlag = nullptr);

    // 下载完成后的处理 (供子类重写)
    // @param downloadId 下载标识符
    // @param destPath 下载文件路径
    // @return 如果返回 false, 将触发 downloadError 信号
    virtual bool OnDownloadComplete(const QString &downloadId, const QString &destPath);

    // 获取取消标志引用 (供子类检查)
    std::atomic<bool>& GetCancelFlag() { return cancelRequested; }

private:
    // 下载工作线程
    void DownloadWorker(QString url, QString destPath, QString downloadId);

    // CURL 回调
    static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp);
    static int ProgressCallback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);

    // 线程管理
    std::thread downloadThread;
    std::atomic<bool> isDownloading{false};
    std::atomic<bool> cancelRequested{false};
};
