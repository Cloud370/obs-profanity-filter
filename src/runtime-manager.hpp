#pragma once

#include "file-downloader.hpp"
#include <vector>
#include <string>

#ifdef _WIN32
#include <Windows.h>
#endif

struct RuntimeInfo {
    QString id;           // "cuda", "directml", etc.
    QString name;         // 显示名称
    QString url;          // 下载地址
    QString version;      // Runtime 版本
    QString cuda_version; // CUDA 版本 (仅 CUDA)
    QStringList files;    // 包含的 DLL 文件列表
    QString sha256;       // 校验值 (可选)
};

/**
 * RuntimeManager - GPU Runtime DLL 管理器
 *
 * 负责：
 * - 检测系统 CUDA 环境
 * - 下载 CUDA 版本的 ONNX Runtime DLL
 * - 管理 DLL 路径和加载
 */
class RuntimeManager : public FileDownloader {
    Q_OBJECT

public:
    explicit RuntimeManager(QObject *parent = nullptr);
    ~RuntimeManager() override;

    // 单例访问
    static RuntimeManager* Get();
    static void Initialize();
    static void Shutdown();

    // 从 runtime.json 加载配置
    void LoadConfig(const QString &jsonPath);

    // 获取已配置的 Runtime 列表
    const std::vector<RuntimeInfo>& GetRuntimes() const;

    // 检查指定 Runtime 是否已安装
    bool IsRuntimeInstalled(const QString &runtimeId) const;

    // 获取 Runtime 安装路径
    QString GetRuntimePath(const QString &runtimeId) const;

    // 下载指定 Runtime
    void DownloadRuntime(const QString &runtimeId);

    // 删除指定 Runtime
    bool DeleteRuntime(const QString &runtimeId);

    // 加载 Runtime DLL (设置 DLL 搜索路径)
    bool LoadRuntimeDLLs(const QString &runtimeId);

    // 卸载 Runtime DLL (恢复 DLL 搜索路径)
    void UnloadRuntimeDLLs();

    // 获取当前加载的 Runtime ID
    QString GetLoadedRuntimeId() const { return loadedRuntimeId; }

    // --- CUDA 环境检测 ---

    // 检测系统是否有 NVIDIA GPU
    static bool IsNvidiaGpuPresent();

    // 检测系统 CUDA 版本 (从 nvidia-smi 或注册表)
    static QString GetSystemCudaVersion();

    // 获取 GPU 名称列表
    static QStringList GetGpuNames();

signals:
    // Runtime 准备就绪
    void runtimeReady(const QString &runtimeId);

    // Runtime 加载错误
    void runtimeLoadError(const QString &message);

protected:
    // 重写基类的下载完成处理
    bool OnDownloadComplete(const QString &downloadId, const QString &destPath) override;

private:
    std::vector<RuntimeInfo> runtimes;
    QString currentDownloadId;
    QString loadedRuntimeId;

#ifdef _WIN32
    // 保存原始 DLL 目录
    wchar_t originalDllDirectory[MAX_PATH];
    bool dllDirectoryChanged = false;
#endif

    // 单例实例
    static RuntimeManager* s_instance;

    // 验证 DLL 完整性
    bool ValidateRuntimeFiles(const QString &runtimeId);

    // 将解压后的嵌套目录结构展平 (把 lib/*.dll 移动到根目录)
    bool FlattenExtractedFiles(const QString &extractPath);
};
