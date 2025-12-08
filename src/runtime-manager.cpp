#include "runtime-manager.hpp"
#include "logging-macros.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QProcess>
#include <QSettings>
#include <obs-module.h>
#include <obs.h>

RuntimeManager* RuntimeManager::s_instance = nullptr;

RuntimeManager::RuntimeManager(QObject *parent) : FileDownloader(parent) {
#ifdef _WIN32
    memset(originalDllDirectory, 0, sizeof(originalDllDirectory));
#endif
}

RuntimeManager::~RuntimeManager() {
    UnloadRuntimeDLLs();
}

RuntimeManager* RuntimeManager::Get() {
    return s_instance;
}

void RuntimeManager::Initialize() {
    if (!s_instance) {
        s_instance = new RuntimeManager(nullptr);
    }
}

void RuntimeManager::Shutdown() {
    if (s_instance) {
        delete s_instance;
        s_instance = nullptr;
    }
}

void RuntimeManager::LoadConfig(const QString &jsonPath) {
    runtimes.clear();

    BLOG(LOG_INFO, "Loading runtime config from: %s", jsonPath.toStdString().c_str());

    if (jsonPath.isEmpty()) {
        BLOG(LOG_INFO, "No runtime config path provided, using defaults");
        // 默认配置
        runtimes = {
            {
                "cuda",
                "CUDA 12.x (NVIDIA GPU)",
                "", // URL 需要用户在 runtime.json 中配置
                "1.22.0",
                "12.x",
                {"onnxruntime.dll", "onnxruntime_providers_cuda.dll", "onnxruntime_providers_shared.dll"},
                ""
            }
        };
        return;
    }

    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly)) {
        BLOG(LOG_WARNING, "Failed to open runtime config: %s", jsonPath.toStdString().c_str());
        return;
    }

    QByteArray data = file.readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);

    if (!doc.isObject()) {
        BLOG(LOG_WARNING, "Runtime config is not a valid JSON object");
        return;
    }

    QJsonArray arr = doc.object()["runtimes"].toArray();
    for (const auto &val : arr) {
        QJsonObject obj = val.toObject();
        RuntimeInfo info;
        info.id = obj["id"].toString();
        info.name = obj["name"].toString();
        info.url = obj["url"].toString();
        info.version = obj["version"].toString();
        info.cuda_version = obj["cuda_version"].toString();
        info.sha256 = obj["sha256"].toString();

        QJsonArray filesArr = obj["files"].toArray();
        for (const auto &f : filesArr) {
            info.files.append(f.toString());
        }

        runtimes.push_back(info);
    }

    BLOG(LOG_INFO, "Loaded %zu runtime configurations", runtimes.size());
}

const std::vector<RuntimeInfo>& RuntimeManager::GetRuntimes() const {
    return runtimes;
}

QString RuntimeManager::GetRuntimePath(const QString &runtimeId) const {
    char *path = obs_module_get_config_path(obs_current_module(), "runtime");
    if (!path) return QString();

    QString qpath = QString::fromUtf8(path);
    bfree(path);

    return QDir(qpath).filePath(runtimeId);
}

bool RuntimeManager::IsRuntimeInstalled(const QString &runtimeId) const {
    QString path = GetRuntimePath(runtimeId);
    if (path.isEmpty()) return false;

    QDir dir(path);
    if (!dir.exists()) return false;

    // 检查是否有 onnxruntime.dll
    return QFile::exists(dir.filePath("onnxruntime.dll"));
}

bool RuntimeManager::DeleteRuntime(const QString &runtimeId) {
    // 确保先卸载
    if (loadedRuntimeId == runtimeId) {
        UnloadRuntimeDLLs();
    }

    QString path = GetRuntimePath(runtimeId);
    if (path.isEmpty()) return false;

    QDir dir(path);
    if (dir.exists()) {
        BLOG(LOG_INFO, "Deleting runtime: %s", path.toStdString().c_str());
        return dir.removeRecursively();
    }
    return false;
}

void RuntimeManager::DownloadRuntime(const QString &runtimeId) {
    if (IsDownloading()) {
        emit downloadError("另一个下载正在进行中");
        return;
    }

    currentDownloadId = runtimeId;

    // 查找 URL
    QString url;
    for (const auto &r : runtimes) {
        if (r.id == runtimeId) {
            url = r.url;
            break;
        }
    }

    if (url.isEmpty()) {
        emit downloadError("Runtime 配置中未找到下载地址，请检查 runtime.json");
        return;
    }

    // 准备下载目录
    char *base_path_c = obs_module_get_config_path(obs_current_module(), "runtime");
    QString basePath = QString::fromUtf8(base_path_c);
    bfree(base_path_c);

    QDir dir(basePath);
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    // 临时文件
    QString fileName = url.split('/').last();
    QString destPath = dir.filePath(fileName);

    StartDownload(url, destPath, runtimeId);
}

bool RuntimeManager::OnDownloadComplete(const QString &downloadId, const QString &destPath) {
    QString extractPath = GetRuntimePath(downloadId);
    QDir extractDir(extractPath);

    if (extractDir.exists()) {
        extractDir.removeRecursively();
    }
    extractDir.mkpath(".");

    BLOG(LOG_INFO, "Extracting runtime to: %s", extractPath.toStdString().c_str());

    bool success = ExtractArchive(destPath, extractPath, &GetCancelFlag());
    QFile::remove(destPath);

    if (!success) {
        extractDir.removeRecursively();
        if (GetCancelFlag().load()) {
            BLOG(LOG_INFO, "Runtime download cancelled during extraction.");
        } else {
            emit downloadError("解压 Runtime 文件失败");
        }
        return false;
    }

    // ONNX Runtime ZIP 解压后结构是 onnxruntime-win-x64-gpu-X.X.X/lib/*.dll
    // 需要把 DLL 文件移动到根目录
    if (!FlattenExtractedFiles(extractPath)) {
        BLOG(LOG_WARNING, "Failed to flatten extracted files, trying validation anyway");
    }

    // 验证文件
    if (!ValidateRuntimeFiles(downloadId)) {
        extractDir.removeRecursively();
        emit downloadError("Runtime 文件验证失败，可能下载不完整");
        return false;
    }

    emit runtimeReady(downloadId);
    return true;
}

bool RuntimeManager::FlattenExtractedFiles(const QString &extractPath) {
    QDir extractDir(extractPath);

    // 检查是否已经有 onnxruntime.dll 在根目录
    if (QFile::exists(extractDir.filePath("onnxruntime.dll"))) {
        BLOG(LOG_INFO, "DLL files already in root directory");
        return true;
    }

    // 查找子目录中的 lib 文件夹
    // 结构可能是: extractPath/onnxruntime-win-x64-gpu-1.17.3/lib/
    QStringList subDirs = extractDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QString &subDir : subDirs) {
        QString libPath = extractDir.filePath(subDir + "/lib");
        QDir libDir(libPath);

        if (libDir.exists()) {
            BLOG(LOG_INFO, "Found lib directory at: %s", libPath.toStdString().c_str());

            // 移动所有 DLL 文件到根目录
            QStringList dllFiles = libDir.entryList(QStringList() << "*.dll", QDir::Files);
            bool allMoved = true;

            for (const QString &dll : dllFiles) {
                QString srcPath = libDir.filePath(dll);
                QString dstPath = extractDir.filePath(dll);

                if (QFile::exists(dstPath)) {
                    QFile::remove(dstPath);
                }

                if (QFile::copy(srcPath, dstPath)) {
                    BLOG(LOG_INFO, "Copied: %s -> %s", dll.toStdString().c_str(), dstPath.toStdString().c_str());
                } else {
                    BLOG(LOG_ERROR, "Failed to copy: %s", dll.toStdString().c_str());
                    allMoved = false;
                }
            }

            // 清理子目录
            if (allMoved) {
                QDir(extractDir.filePath(subDir)).removeRecursively();
            }

            return allMoved;
        }
    }

    BLOG(LOG_WARNING, "Could not find lib subdirectory in extracted files");
    return false;
}

bool RuntimeManager::ValidateRuntimeFiles(const QString &runtimeId) {
    QString path = GetRuntimePath(runtimeId);

    // 查找配置中的文件列表
    QStringList requiredFiles;
    for (const auto &r : runtimes) {
        if (r.id == runtimeId) {
            requiredFiles = r.files;
            break;
        }
    }

    if (requiredFiles.isEmpty()) {
        // 默认检查 onnxruntime.dll
        requiredFiles << "onnxruntime.dll";
    }

    QDir dir(path);
    for (const QString &file : requiredFiles) {
        if (!QFile::exists(dir.filePath(file))) {
            BLOG(LOG_ERROR, "Runtime 缺少文件: %s", file.toStdString().c_str());
            return false;
        }
    }

    return true;
}

bool RuntimeManager::LoadRuntimeDLLs(const QString &runtimeId) {
    if (!IsRuntimeInstalled(runtimeId)) {
        emit runtimeLoadError("Runtime 未安装: " + runtimeId);
        return false;
    }

    QString runtimePath = GetRuntimePath(runtimeId);

#ifdef _WIN32
    // 保存原始 DLL 目录
    if (!dllDirectoryChanged) {
        GetDllDirectoryW(MAX_PATH, originalDllDirectory);
    }

    // 设置新的 DLL 搜索路径
    if (SetDllDirectoryW(runtimePath.toStdWString().c_str())) {
        BLOG(LOG_INFO, "DLL 搜索路径已设置为: %s", runtimePath.toStdString().c_str());
        dllDirectoryChanged = true;
        loadedRuntimeId = runtimeId;
        return true;
    } else {
        DWORD error = GetLastError();
        BLOG(LOG_ERROR, "设置 DLL 目录失败，错误码: %lu", error);
        emit runtimeLoadError(QString("设置 DLL 目录失败，错误码: %1").arg(error));
        return false;
    }
#else
    // 非 Windows 平台暂不支持
    BLOG(LOG_WARNING, "GPU Runtime 仅支持 Windows 平台");
    emit runtimeLoadError("GPU Runtime 仅支持 Windows 平台");
    return false;
#endif
}

void RuntimeManager::UnloadRuntimeDLLs() {
#ifdef _WIN32
    if (dllDirectoryChanged) {
        if (originalDllDirectory[0] != L'\0') {
            SetDllDirectoryW(originalDllDirectory);
        } else {
            SetDllDirectoryW(nullptr); // 恢复默认搜索路径
        }
        dllDirectoryChanged = false;
        loadedRuntimeId.clear();
        BLOG(LOG_INFO, "DLL 搜索路径已恢复");
    }
#endif
}

// --- CUDA 环境检测 ---

bool RuntimeManager::IsNvidiaGpuPresent() {
#ifdef _WIN32
    // 方法1: 检查 nvidia-smi 是否存在
    QProcess process;
    process.start("nvidia-smi", QStringList() << "--query-gpu=name" << "--format=csv,noheader");
    process.waitForFinished(3000);

    if (process.exitCode() == 0) {
        QString output = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
        return !output.isEmpty();
    }

    // 方法2: 检查注册表
    QSettings reg("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}",
                  QSettings::NativeFormat);

    QStringList groups = reg.childGroups();
    for (const QString &group : groups) {
        reg.beginGroup(group);
        QString provider = reg.value("ProviderName").toString();
        if (provider.contains("NVIDIA", Qt::CaseInsensitive)) {
            reg.endGroup();
            return true;
        }
        reg.endGroup();
    }
#endif

    return false;
}

QString RuntimeManager::GetSystemCudaVersion() {
#ifdef _WIN32
    QProcess process;
    process.start("nvidia-smi", QStringList());
    process.waitForFinished(3000);

    if (process.exitCode() == 0) {
        QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
        // 解析 CUDA Version: XX.X
        QRegularExpression rx("CUDA Version:\\s*(\\d+\\.\\d+)");
        QRegularExpressionMatch match = rx.match(output);
        if (match.hasMatch()) {
            return match.captured(1);
        }
    }
#endif

    return QString();
}

QStringList RuntimeManager::GetGpuNames() {
    QStringList names;

#ifdef _WIN32
    QProcess process;
    process.start("nvidia-smi", QStringList() << "--query-gpu=name" << "--format=csv,noheader");
    process.waitForFinished(3000);

    if (process.exitCode() == 0) {
        QString output = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
        names = output.split('\n', Qt::SkipEmptyParts);
        for (QString &name : names) {
            name = name.trimmed();
        }
    }
#endif

    return names;
}
