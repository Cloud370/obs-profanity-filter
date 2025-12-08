#include "plugin-config.hpp"
#include "video-delay.hpp"
#include "profanity-filter.hpp"
#include "logging-macros.hpp"
#include <obs-module.h>
#include <obs.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <QPointer>

using namespace std;

// Unified default dirty words list definition removed.
// The system dirty words list is now loaded from data/builtin_dirty_words.txt at runtime.


static GlobalConfig* g_config = nullptr;
static obs_module_t* g_module = nullptr;
static QPointer<ConfigDialog> g_dialog;

GlobalConfig* GetGlobalConfig() {
    if (!g_config) {
        g_config = new GlobalConfig();
    }
    return g_config;
}

void SetGlobalConfigModule(obs_module_t *module) {
    g_module = module;
}

void GlobalConfig::ParsePatterns() {
    dirty_patterns.clear();
    
    // Combine system and user dirty words
    std::string combined = system_dirty_words_str;
    if (!combined.empty() && !user_dirty_words_str.empty()) {
        combined += ", ";
    }
    combined += user_dirty_words_str;
    
    // Update the legacy string just in case
    dirty_words_str = combined;

    stringstream ss(combined);
    string item;
    while (getline(ss, item, ',')) {
        // Trim
        item.erase(0, item.find_first_not_of(" \t\n\r"));
        item.erase(item.find_last_not_of(" \t\n\r") + 1);
        if (!item.empty()) {
            try {
                dirty_patterns.emplace_back(item, regex::icase);
            } catch(...) {}
        }
    }
}

void GlobalConfig::Save() {
    obs_data_t *data = obs_data_create();
    string path_to_save;
    string custom_words_path;

    {
        lock_guard<std::mutex> lock(this->mutex);

        obs_data_set_bool(data, "global_enable", global_enable);
        obs_data_set_string(data, "model_path", model_path.c_str());
        obs_data_set_int(data, "model_offset_ms", model_offset_ms);
        obs_data_set_double(data, "delay_seconds", delay_seconds);
        // dirty_words stored in external files now
        obs_data_set_bool(data, "use_pinyin", use_pinyin);
        obs_data_set_bool(data, "comedy_mode", comedy_mode);
        obs_data_set_int(data, "audio_effect", audio_effect);
        obs_data_set_int(data, "beep_freq", beep_frequency);
        obs_data_set_int(data, "beep_mix", beep_mix_percent);
        obs_data_set_bool(data, "enable_agc", enable_agc);
        obs_data_set_bool(data, "video_delay_enabled", video_delay_enabled);

        // GPU Settings
        obs_data_set_bool(data, "enable_gpu", enable_gpu);
        obs_data_set_string(data, "onnx_provider", onnx_provider.c_str());
        obs_data_set_int(data, "cuda_device_id", cuda_device_id);

        ParsePatterns();
    }
    
    // Save Custom Dirty Words to custom_dirty_words.txt
    if (g_module) {
        char *p = obs_module_get_config_path(g_module, "custom_dirty_words.txt");
        if (p) {
            filesystem::path txtPath(p);
            bfree(p);
            
            if (txtPath.has_parent_path()) {
                try {
                    filesystem::create_directories(txtPath.parent_path());
                } catch(...) {}
            }
            
            try {
                ofstream f(txtPath, ios::binary);
                if (f.is_open()) {
                    unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
                    f.write((char*)bom, 3);
                    f << user_dirty_words_str;
                }
            } catch(...) {}
        }
    }
    
    if (g_module) {
        char *config_path = obs_module_get_config_path(g_module, "global_config.json");
        if (config_path) {
            path_to_save = config_path;
            bfree(config_path);
        }
    }
    
    if (!path_to_save.empty()) {
        filesystem::path p(path_to_save);
        if (p.has_parent_path()) {
            try {
                filesystem::create_directories(p.parent_path());
            } catch(...) {}
        }
        obs_data_save_json(data, path_to_save.c_str());
    }
    
    obs_data_release(data);
}

void GlobalConfig::Load() {
    lock_guard<std::mutex> lock(this->mutex);
    
    // Helper to get config path
    auto get_config_path = [](const char* filename) -> string {
        string res;
        if (g_module) {
            char *p = obs_module_get_config_path(g_module, filename);
            if (p) {
                res = p;
                bfree(p);
            }
        }
        return res;
    };
    
    // Helper to read file
    auto read_file = [](const string& path) -> string {
        if (path.empty() || !filesystem::exists(path)) return "";
        try {
            ifstream f(path);
            if (f.is_open()) {
                stringstream buffer;
                buffer << f.rdbuf();
                return buffer.str();
            }
        } catch(...) {}
        return "";
    };

    // Helper to write file
    auto write_file = [](const string& path, const string& content) {
        if (path.empty()) return;
        filesystem::path p(path);
        if (p.has_parent_path()) {
            try { filesystem::create_directories(p.parent_path()); } catch(...) {}
        }
        try {
            ofstream f(path, ios::binary);
            if (f.is_open()) {
                unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
                f.write((char*)bom, 3);
                f << content;
                f.close();
            }
        } catch(...) {}
    };

    // 1. Handle System Dirty Words (builtin_dirty_words.txt)
    // Load from plugin installation directory (read-only system list)
    char *builtin_path = obs_find_module_file(g_module, "data/builtin_dirty_words.txt");
    if (!builtin_path) {
         builtin_path = obs_find_module_file(g_module, "builtin_dirty_words.txt");
    }
    
    if (builtin_path) {
        system_dirty_words_str = read_file(builtin_path);
        bfree(builtin_path);
    } else {
        system_dirty_words_str = ""; 
    }
    
    // 2. Handle User/Custom Dirty Words
    string custom_path = get_config_path("custom_dirty_words.txt");
    
    if (filesystem::exists(custom_path)) {
        user_dirty_words_str = read_file(custom_path);
    } else {
        // First run, empty custom list
        user_dirty_words_str = "";
        write_file(custom_path, ""); 
    }

    // 3. Load JSON config
    string json_path = get_config_path("global_config.json");
    
    if (json_path.empty() || !filesystem::exists(json_path)) {
        video_delay_enabled = true;
        is_first_run = true;
        ParsePatterns();
        return;
    }
    
    obs_data_t *data = obs_data_create_from_json_file(json_path.c_str());
    
    if (data) {
        if (obs_data_has_user_value(data, "global_enable")) {
            global_enable = obs_data_get_bool(data, "global_enable");
        } else {
            global_enable = true;
        }

        const char* s = obs_data_get_string(data, "model_path");
        model_path = s ? s : "";
        
        if (obs_data_has_user_value(data, "model_offset_ms")) {
            model_offset_ms = obs_data_get_int(data, "model_offset_ms");
        }

        delay_seconds = obs_data_get_double(data, "delay_seconds");
        if (delay_seconds < 0.01) delay_seconds = 0.5;
        
        use_pinyin = obs_data_get_bool(data, "use_pinyin");
        
        if (obs_data_has_user_value(data, "comedy_mode")) {
            comedy_mode = obs_data_get_bool(data, "comedy_mode");
        }

        if (obs_data_has_user_value(data, "audio_effect")) {
            audio_effect = obs_data_get_int(data, "audio_effect");
        }
        
        if (obs_data_has_user_value(data, "beep_freq")) {
            beep_frequency = obs_data_get_int(data, "beep_freq");
        }
        
        if (obs_data_has_user_value(data, "beep_mix")) {
            beep_mix_percent = obs_data_get_int(data, "beep_mix");
        }
        
        if (obs_data_has_user_value(data, "enable_agc")) {
            enable_agc = obs_data_get_bool(data, "enable_agc");
        }

        if (obs_data_has_user_value(data, "video_delay_enabled")) {
            video_delay_enabled = obs_data_get_bool(data, "video_delay_enabled");
        }

        // GPU Settings
        if (obs_data_has_user_value(data, "enable_gpu")) {
            enable_gpu = obs_data_get_bool(data, "enable_gpu");
        }
        if (obs_data_has_user_value(data, "onnx_provider")) {
            const char* provider = obs_data_get_string(data, "onnx_provider");
            onnx_provider = provider ? provider : "cpu";
        }
        if (obs_data_has_user_value(data, "cuda_device_id")) {
            cuda_device_id = (int)obs_data_get_int(data, "cuda_device_id");
        }

        obs_data_release(data);
    }
    
    ParsePatterns();
    loaded = true;
}

// --- UI Implementation ---

ConfigDialog::ConfigDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle("语音脏话屏蔽 - 全局配置");
    resize(600, 550);
    
    // Init Model Manager
    modelManager = new PluginModelManager(this);
    QString jsonPathStr;
    if (g_module) {
        char *jsonPath = obs_find_module_file(g_module, "models.json");
        if (jsonPath) {
            jsonPathStr = QString::fromUtf8(jsonPath);
            bfree(jsonPath);
        }
    }
    modelManager->LoadModels(jsonPathStr); // Always load, even if path is empty (triggers fallback)
    
    connect(modelManager, &PluginModelManager::downloadProgress, this, &ConfigDialog::onDownloadProgress);
    connect(modelManager, &PluginModelManager::downloadFinished, this, &ConfigDialog::onDownloadFinished);
    connect(modelManager, &PluginModelManager::downloadError, this, &ConfigDialog::onDownloadError);
    
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    
    // Global Enable
    chkGlobalEnable = new QCheckBox("启用全局脏话过滤功能 (Master Switch)");
    chkGlobalEnable->setToolTip("关闭此选项将完全禁用插件功能：\n1. 卸载语音识别模型 (释放内存)\n2. 停止所有音频检测\n3. 禁用视频延迟同步\n(相当于插件完全未运行)");
    QFont font = chkGlobalEnable->font();
    font.setBold(true);
    chkGlobalEnable->setFont(font);
    mainLayout->addWidget(chkGlobalEnable);
    
    // --- Container for switchable content ---
    settingsContainer = new QWidget();
    QVBoxLayout *containerLayout = new QVBoxLayout(settingsContainer);
    containerLayout->setContentsMargins(0, 0, 0, 0);
    
    // Model Group
    QGroupBox *grpModel = new QGroupBox("模型设置 (Model)");
    QFormLayout *layoutModel = new QFormLayout(grpModel);
    
    lblModelStatus = new QLabel("状态: 检查中...");
    QFont statusFont = lblModelStatus->font();
    statusFont.setBold(true);
    lblModelStatus->setFont(statusFont);
    layoutModel->addRow("当前状态:", lblModelStatus);
    
    comboModel = new QComboBox();
    const auto &loadedModels = modelManager->GetModels();
    BLOG(LOG_INFO, "Populating combo box with %zu models", loadedModels.size());

    for (const auto &m : loadedModels) {
        comboModel->addItem(m.name, m.id);
        BLOG(LOG_INFO, "Added model to combo: %s (%s)", m.name.toStdString().c_str(), m.id.toStdString().c_str());
    }
    comboModel->addItem("使用自定义路径 (Custom Path)...", "custom");
    connect(comboModel, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ConfigDialog::onModelComboChanged);
    
    QHBoxLayout *boxPath = new QHBoxLayout();
    editModelPath = new QLineEdit();
    editModelPath->setPlaceholderText("选择或输入模型文件夹路径...");
    QPushButton *btnBrowse = new QPushButton("浏览...");
    connect(btnBrowse, &QPushButton::clicked, this, &ConfigDialog::onBrowseModel);
    boxPath->addWidget(editModelPath);
    boxPath->addWidget(btnBrowse);
    
    QHBoxLayout *boxDownload = new QHBoxLayout();
    btnDownloadModel = new QPushButton("⬇️ 一键下载此模型");
    connect(btnDownloadModel, &QPushButton::clicked, this, &ConfigDialog::onModelAction);
    progressDownload = new QProgressBar();
    progressDownload->setRange(0, 100);
    progressDownload->setValue(0);
    progressDownload->setVisible(false);
    lblDownloadStatus = new QLabel("");
    
    boxDownload->addWidget(btnDownloadModel);
    boxDownload->addWidget(progressDownload);
    boxDownload->addWidget(lblDownloadStatus);
    boxDownload->addStretch();
    
    layoutModel->addRow("选择模型:", comboModel);
    lblPathTitle = new QLabel("模型路径:");
    layoutModel->addRow(lblPathTitle, boxPath);

    spinModelOffset = new QSpinBox();
    spinModelOffset->setRange(-2000, 2000);
    spinModelOffset->setSingleStep(50);
    spinModelOffset->setSuffix(" ms");
    spinModelOffset->setToolTip("模型延迟补偿 (Offset)\n不同模型可能有不同的处理延迟，导致哔声位置偏移。\n调整此值可校准哔声位置。\n正值: 哔声延后\n负值: 哔声提前");
    layoutModel->addRow("延迟补偿:", spinModelOffset);
    
    layoutModel->addRow("", boxDownload);
    
    containerLayout->addWidget(grpModel);
    
    // Audio Group
    QGroupBox *grpAudio = new QGroupBox("音频处理");
    QFormLayout *layoutAudio = new QFormLayout(grpAudio);
    
    spinDelay = new QSpinBox();
    spinDelay->setRange(0, 10000);
    spinDelay->setSingleStep(50);
    spinDelay->setSuffix(" ms");
    
    chkEnableAGC = new QCheckBox("启用自动增益 (Auto Gain Control)");
    chkEnableAGC->setToolTip("开启后，将自动调整音量以保持稳定的识别效果。\n(推荐开启，可解决声音过小导致识别不到的问题)");

    // Audio Effect Selection
    comboEffect = new QComboBox();
    comboEffect->addItem("标准哔声 (Beep)", 0);
    comboEffect->addItem("静音 (Silence)", 1);
    comboEffect->addItem("小黄人音效 (Minion)", 2);
    comboEffect->addItem("电报音效 (Telegraph)", 3);
    
    layoutAudio->addRow("全局延迟时间:", spinDelay);
    layoutAudio->addRow("", chkEnableAGC);
    layoutAudio->addRow("屏蔽音效:", comboEffect);
    
    chkEnableVideoDelay = new QCheckBox("启用音画同步缓冲 (自动应用到所有场景)");
    chkEnableVideoDelay->setToolTip("开启后，将自动向所有场景添加音画同步滤镜。\n关闭后，将从所有场景移除该滤镜。");
    layoutAudio->addRow("", chkEnableVideoDelay);
    
    lblVideoMemory = new QLabel("当前音画同步显存占用: 0.0 MB");
    lblVideoMemory->setStyleSheet("color: #888; font-style: italic;");
    layoutAudio->addRow("", lblVideoMemory);
    
    containerLayout->addWidget(grpAudio);
    
    // Words Group
    QGroupBox *grpWords = new QGroupBox("屏蔽词设置");
    QVBoxLayout *layoutWords = new QVBoxLayout(grpWords);
    
    // Header for Custom Words
    QHBoxLayout *headerLayout = new QHBoxLayout();
    headerLayout->addWidget(new QLabel("自定义屏蔽词 (逗号分隔):"));
    chkHideDirtyWords = new QCheckBox("隐藏内容 (密码模式)");
    chkHideDirtyWords->setToolTip("勾选后将隐藏下方自定义屏蔽词内容，防止直播时意外泄露。");
    headerLayout->addWidget(chkHideDirtyWords);
    headerLayout->addStretch();
    layoutWords->addLayout(headerLayout);

    editDirtyWords = new QTextEdit();
    layoutWords->addWidget(editDirtyWords);
    
    connect(chkHideDirtyWords, &QCheckBox::toggled, this, [this](bool checked){
        if (checked) {
            // Hide User Words
            m_cachedUserWords = editDirtyWords->toPlainText();
            editDirtyWords->setText("****************** (Content Hidden / 内容已隐藏) ******************");
            editDirtyWords->setReadOnly(true);
            editDirtyWords->setStyleSheet("color: #888; font-style: italic; background-color: #f0f0f0;");
            
            // Hide System Words
            m_cachedSystemWords = editSystemDirtyWords->toPlainText();
            editSystemDirtyWords->setText("****************** (Content Hidden / 内容已隐藏) ******************");
        } else {
            // Show User Words
            editDirtyWords->setText(m_cachedUserWords);
            editDirtyWords->setReadOnly(false);
            editDirtyWords->setStyleSheet("");
            
            // Show System Words
            editSystemDirtyWords->setText(m_cachedSystemWords);
        }
    });
    
    // System Words (Read-only)
    layoutWords->addWidget(new QLabel("系统内置屏蔽词 (只读):"));
    editSystemDirtyWords = new QTextEdit();
    editSystemDirtyWords->setReadOnly(true);
    editSystemDirtyWords->setStyleSheet("color: #666; background-color: #f0f0f0;");
    editSystemDirtyWords->setMaximumHeight(80); // Smaller height for system words
    layoutWords->addWidget(editSystemDirtyWords);
    
    chkUsePinyin = new QCheckBox("启用拼音增强识别 (模糊匹配)");
    chkUsePinyin->setToolTip("开启后将使用拼音进行匹配，忽略声调和平卷舌差异，提高识别率。");
    layoutWords->addWidget(chkUsePinyin);

    chkComedyMode = new QCheckBox("精准变声模式 (优先匹配短词)");
    chkComedyMode->setToolTip("开启后，当匹配到多个词时（如'我爱你'和'爱你'），\n优先只屏蔽较短的词（'爱你'），从而保留'我'的原声。\n配合变音特效可实现更生动的喜剧效果。");
    layoutWords->addWidget(chkComedyMode);

    containerLayout->addWidget(grpWords);

    // GPU Acceleration Group
    QGroupBox *grpGpu = new QGroupBox("GPU 加速设置 (实验性)");
    QFormLayout *layoutGpu = new QFormLayout(grpGpu);

    // Init Runtime Manager
    runtimeManager = RuntimeManager::Get();
    if (!runtimeManager) {
        RuntimeManager::Initialize();
        runtimeManager = RuntimeManager::Get();
    }
    QString runtimeJsonPath;
    if (g_module) {
        char *runtimePath = obs_find_module_file(g_module, "runtime.json");
        if (runtimePath) {
            runtimeJsonPath = QString::fromUtf8(runtimePath);
            bfree(runtimePath);
        }
    }
    if (runtimeManager) {
        runtimeManager->LoadConfig(runtimeJsonPath);
        connect(runtimeManager, &RuntimeManager::downloadProgress, this, &ConfigDialog::onRuntimeDownloadProgress);
        connect(runtimeManager, &RuntimeManager::downloadFinished, this, &ConfigDialog::onRuntimeDownloadFinished);
        connect(runtimeManager, &RuntimeManager::downloadError, this, &ConfigDialog::onRuntimeDownloadError);
    }

    chkEnableGpu = new QCheckBox("启用 GPU 加速");
    chkEnableGpu->setToolTip("启用后将使用 GPU 进行语音识别推理，可显著提升性能。\n需要下载对应的 Runtime 文件。");
    layoutGpu->addRow(chkEnableGpu);

    // GPU Settings Container (显示/隐藏根据 chkEnableGpu)
    gpuSettingsContainer = new QWidget();
    QFormLayout *layoutGpuSettings = new QFormLayout(gpuSettingsContainer);
    layoutGpuSettings->setContentsMargins(0, 0, 0, 0);

    comboProvider = new QComboBox();
    comboProvider->addItem("CPU (默认)", "cpu");
    comboProvider->addItem("CUDA (NVIDIA GPU)", "cuda");
    connect(comboProvider, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        updateGpuStatus();
    });
    layoutGpuSettings->addRow("执行提供者:", comboProvider);

    comboGpuDevice = new QComboBox();
    // 检测 GPU
    QStringList gpuNames = RuntimeManager::GetGpuNames();
    if (gpuNames.isEmpty()) {
        comboGpuDevice->addItem("未检测到 NVIDIA GPU", 0);
        comboGpuDevice->setEnabled(false);
    } else {
        for (int i = 0; i < gpuNames.size(); i++) {
            comboGpuDevice->addItem(QString("GPU %1: %2").arg(i).arg(gpuNames[i]), i);
        }
    }
    layoutGpuSettings->addRow("CUDA 设备:", comboGpuDevice);

    lblGpuStatus = new QLabel("状态: 检查中...");
    QFont gpuStatusFont = lblGpuStatus->font();
    gpuStatusFont.setBold(true);
    lblGpuStatus->setFont(gpuStatusFont);
    layoutGpuSettings->addRow("Runtime 状态:", lblGpuStatus);

    QHBoxLayout *boxRuntime = new QHBoxLayout();
    btnRuntimeAction = new QPushButton("⬇️ 下载 CUDA Runtime");
    connect(btnRuntimeAction, &QPushButton::clicked, this, &ConfigDialog::onRuntimeAction);
    progressRuntime = new QProgressBar();
    progressRuntime->setRange(0, 100);
    progressRuntime->setValue(0);
    progressRuntime->setVisible(false);
    lblRuntimeStatus = new QLabel("");

    boxRuntime->addWidget(btnRuntimeAction);
    boxRuntime->addWidget(progressRuntime);
    boxRuntime->addWidget(lblRuntimeStatus);
    boxRuntime->addStretch();

    layoutGpuSettings->addRow("", boxRuntime);

    layoutGpu->addRow(gpuSettingsContainer);

    // 连接 GPU 开关
    connect(chkEnableGpu, &QCheckBox::toggled, this, &ConfigDialog::onGpuEnableChanged);
    gpuSettingsContainer->setVisible(false); // 默认隐藏

    containerLayout->addWidget(grpGpu);
    
    // Add container to main layout
    mainLayout->addWidget(settingsContainer);
    
    // Connect Visibility Toggle
    connect(chkGlobalEnable, &QCheckBox::toggled, settingsContainer, &QWidget::setVisible);
    connect(chkGlobalEnable, &QCheckBox::toggled, this, [this](bool checked){
         this->resize(this->width(), this->minimumSizeHint().height());
    });
    
    mainLayout->addStretch();
    
    // Buttons
    QHBoxLayout *btnLayout = new QHBoxLayout();
    QPushButton *btnSave = new QPushButton("确定");
    QPushButton *btnApply = new QPushButton("应用");
    QPushButton *btnCancel = new QPushButton("取消");
    
    connect(btnSave, &QPushButton::clicked, this, &ConfigDialog::onSave);
    connect(btnApply, &QPushButton::clicked, this, &ConfigDialog::onApply);
    connect(btnCancel, &QPushButton::clicked, this, &ConfigDialog::reject);
    
    btnLayout->addStretch();
    btnLayout->addWidget(btnSave);
    btnLayout->addWidget(btnApply);
    btnLayout->addWidget(btnCancel);
    mainLayout->addLayout(btnLayout);
    
    LoadToUI();
    
    // Status Timer
    statusTimer = new QTimer(this);
    connect(statusTimer, &QTimer::timeout, this, &ConfigDialog::updateStatus);
    statusTimer->start(1000); // Update every 1 second
    updateStatus();
}

ConfigDialog::~ConfigDialog() {
    if (statusTimer) statusTimer->stop();
}

void ConfigDialog::LoadToUI() {
    GlobalConfig *cfg = GetGlobalConfig();
    lock_guard<std::mutex> lock(cfg->mutex);
    
    chkGlobalEnable->setChecked(cfg->global_enable);
    settingsContainer->setVisible(cfg->global_enable);
    
    // Set model path
    QString currentPath = QString::fromStdString(cfg->model_path);
    editModelPath->setText(currentPath);
    
    // Try to match current path with combo box
    int foundIndex = -1;
    for (int i = 0; i < comboModel->count(); i++) {
        QString id = comboModel->itemData(i).toString();
        if (id == "custom") continue;
        
        QString path = modelManager->GetModelPath(id);
        // Normalize separators for comparison
        QString p1 = QDir::cleanPath(path);
        QString p2 = QDir::cleanPath(currentPath);
        if (p1 == p2) {
            foundIndex = i;
            break;
        }
    }
    
    if (foundIndex != -1) {
        comboModel->setCurrentIndex(foundIndex);
    } else if (currentPath.isEmpty() && comboModel->count() > 1) {
        // If path is empty and we have presets, select the first preset
        comboModel->setCurrentIndex(0);
    } else {
        comboModel->setCurrentIndex(comboModel->count() - 1); // Custom
    }
    
    spinModelOffset->setValue(cfg->model_offset_ms);
    spinDelay->setValue((int)(cfg->delay_seconds * 1000));
    chkEnableAGC->setChecked(cfg->enable_agc);
    
    // Ensure we are in visible mode before setting text to avoid overwriting "Hidden" text
    chkHideDirtyWords->setChecked(false); 
    editDirtyWords->setText(QString::fromStdString(cfg->user_dirty_words_str));
    editSystemDirtyWords->setText(QString::fromStdString(cfg->system_dirty_words_str));
    
    chkUsePinyin->setChecked(cfg->use_pinyin);
    chkComedyMode->setChecked(cfg->comedy_mode);
    
    // Map audio_effect to combo
    int effect_idx = comboEffect->findData(cfg->audio_effect);
    if (effect_idx != -1) comboEffect->setCurrentIndex(effect_idx);
    else comboEffect->setCurrentIndex(0); // Default to Beep

    chkEnableVideoDelay->setChecked(cfg->video_delay_enabled);

    // GPU Settings
    chkEnableGpu->setChecked(cfg->enable_gpu);
    gpuSettingsContainer->setVisible(cfg->enable_gpu);

    int providerIdx = comboProvider->findData(QString::fromStdString(cfg->onnx_provider));
    if (providerIdx != -1) comboProvider->setCurrentIndex(providerIdx);
    else comboProvider->setCurrentIndex(0); // Default to CPU

    if (cfg->cuda_device_id < comboGpuDevice->count()) {
        comboGpuDevice->setCurrentIndex(cfg->cuda_device_id);
    }

    // Update GPU status
    updateGpuStatus();

    // Trigger update of download button state
    onModelComboChanged(comboModel->currentIndex());
}

void ConfigDialog::updateStatus() {
    // Model Status Update
    auto status = ProfanityFilter::GetGlobalModelStatus();
    lblModelStatus->setText(QString::fromStdString(status.second));
    
    if (status.first) {
        // Loading
        lblModelStatus->setStyleSheet("color: #E6A23C; font-weight: bold;"); // Warning Orange
    } else if (status.second.find("运行中") != std::string::npos) {
        lblModelStatus->setStyleSheet("color: #67C23A; font-weight: bold;"); // Success Green
    } else if (status.second.find("错误") != std::string::npos) {
        lblModelStatus->setStyleSheet("color: #F56C6C; font-weight: bold;"); // Danger Red
    } else {
        lblModelStatus->setStyleSheet("color: #909399; font-style: italic;"); // Info Gray
    }

    double mb = VideoDelayFilter::total_memory_mb.load();
    QString text = QString("当前音画同步显存占用: %1 MB").arg(mb, 0, 'f', 1);
    
    if (!chkGlobalEnable->isChecked()) {
        text += " (全局已禁用)";
    } else if (mb < 0.1 && chkEnableVideoDelay->isChecked()) {
        text += " (待机中)";
    }
    
    if (mb > 1000.0) {
        text += " (⚠️ 高占用)";
        lblVideoMemory->setStyleSheet("color: red; font-weight: bold;");
    } else {
        lblVideoMemory->setStyleSheet("color: #888; font-style: italic;");
    }
    lblVideoMemory->setText(text);
}

void ConfigDialog::onModelComboChanged(int index) {
    QString id = comboModel->itemData(index).toString();
    
    // Auto-set offset and delay from model default if available
    if (id != "custom") {
        const auto &models = modelManager->GetModels();
        for (const auto &m : models) {
            if (m.id == id) {
                // Only update if we are not in the middle of LoadToUI (checked via visible?)
                // Actually, LoadToUI sets the saved value AFTER this, so it's safe to always set here.
                spinModelOffset->setValue(m.offset);
                
                // Also suggest/set recommended delay if current delay is less than recommended
                int recommended = m.delay;
                if (spinDelay->value() < recommended) {
                    spinDelay->setValue(recommended);
                    // Optional: Maybe show a tooltip or flash?
                    // For now just auto-updating is safest for user experience.
                }
                break;
            }
        }
    }
    
    if (id == "custom") {
        lblPathTitle->setText("自定义路径:");
        editModelPath->setEnabled(true);
        editModelPath->setPlaceholderText("请选择包含 tokens.txt 的模型文件夹...");
        btnDownloadModel->setVisible(false);
        progressDownload->setVisible(false);
        lblDownloadStatus->setVisible(false);
    } else {
        lblPathTitle->setText("安装位置:");
        editModelPath->setEnabled(false); // Make it read-only for presets
        
        QString path = modelManager->GetModelPath(id);
        editModelPath->setText(path);
        
        bool installed = modelManager->IsModelInstalled(id);
        btnDownloadModel->setVisible(true);
        btnDownloadModel->setEnabled(true);
        progressDownload->setVisible(false);
        
        if (installed) {
            btnDownloadModel->setText("🗑️ 删除模型");
            lblDownloadStatus->setText("✅ 已安装 (Ready)");
            lblDownloadStatus->setVisible(true);
        } else {
            btnDownloadModel->setText("⬇️ 一键下载此模型");
             lblDownloadStatus->setText("⚠️ 未安装 (需要下载)");
             lblDownloadStatus->setVisible(true);
        }
    }
}

void ConfigDialog::onModelAction() {
    QString id = comboModel->currentData().toString();
    if (id == "custom") return;
    
    if (modelManager->IsModelInstalled(id)) {
        // Delete Action
        QMessageBox::StandardButton reply;
        reply = QMessageBox::question(this, "确认删除", 
            "确定要删除此模型吗?\n此操作将移除模型文件以释放空间。",
            QMessageBox::Yes | QMessageBox::No);
            
        if (reply == QMessageBox::Yes) {
            if (modelManager->DeleteModel(id)) {
                // Update UI immediately
                onModelComboChanged(comboModel->currentIndex());
            } else {
                QMessageBox::critical(this, "删除失败", "无法删除模型文件夹，可能文件正在被使用。");
            }
        }
    } else {
        // Download Action
        btnDownloadModel->setEnabled(false);
        progressDownload->setValue(0);
        progressDownload->setVisible(true);
        lblDownloadStatus->setText("准备下载...");
        lblDownloadStatus->setVisible(true);
        
        modelManager->DownloadModel(id);
    }
}

void ConfigDialog::onDownloadProgress(qint64 received, qint64 total) {
    if (total > 0) {
        int percent = (int)((received * 100) / total);
        progressDownload->setValue(percent);
        double mbReceived = received / (1024.0 * 1024.0);
        double mbTotal = total / (1024.0 * 1024.0);
        lblDownloadStatus->setText(QString("正在下载: %1 MB / %2 MB").arg(mbReceived, 0, 'f', 1).arg(mbTotal, 0, 'f', 1));
    } else {
        progressDownload->setValue(0);
        lblDownloadStatus->setText("正在下载...");
    }
}

void ConfigDialog::onDownloadFinished(const QString &modelId) {
    progressDownload->setVisible(false);
    lblDownloadStatus->setText("✅ 下载并解压完成!");
    
    // Update UI state
    onModelComboChanged(comboModel->currentIndex());
    
    QMessageBox::information(this, "下载完成", "模型已成功下载并安装。");
}

void ConfigDialog::onDownloadError(const QString &msg) {
    progressDownload->setVisible(false);
    lblDownloadStatus->setText("❌ 下载失败");
    btnDownloadModel->setEnabled(true);
    
    QMessageBox::critical(this, "下载失败", "错误信息: " + msg);
}

void ConfigDialog::onBrowseModel() {
    QString dir = QFileDialog::getExistingDirectory(this, "选择模型文件夹", editModelPath->text());
    if (!dir.isEmpty()) {
        editModelPath->setText(dir);
        // Switch to custom if not matching any preset
        comboModel->setCurrentIndex(comboModel->count() - 1);
    }
}

void ConfigDialog::onApply() {
    // Validation
    if (chkGlobalEnable->isChecked()) {
        QString path = editModelPath->text();
        if (path.isEmpty()) {
             QMessageBox::warning(this, "未配置模型", "启用插件需要选择一个模型路径。");
             // We allow saving empty path (it will error in log but not crash), 
             // but warning is good. User can ignore.
        } else {
             QDir dir(path);
             if (!dir.exists("tokens.txt")) {
                 QMessageBox::StandardButton reply;
                 reply = QMessageBox::warning(this, "模型路径无效", 
                     "选定的模型路径似乎无效 (未找到 tokens.txt)。\n"
                     "这会导致插件无法工作。\n\n"
                     "请确保选择了包含 tokens.txt 的文件夹。\n"
                     "如果是自动下载的模型，可能解压失败或路径层级不正确。\n\n"
                     "是否仍然保存设置?",
                     QMessageBox::Yes | QMessageBox::No);
                 if (reply == QMessageBox::No) return;
             }
        }
    }

    GlobalConfig *cfg = GetGlobalConfig();
    
    bool old_enabled_state;
    {
        lock_guard<std::mutex> lock(cfg->mutex);
        old_enabled_state = cfg->video_delay_enabled;
        
        cfg->global_enable = chkGlobalEnable->isChecked();
        cfg->model_path = editModelPath->text().toStdString();
        cfg->model_offset_ms = spinModelOffset->value();
        cfg->delay_seconds = (double)spinDelay->value() / 1000.0;
        cfg->enable_agc = chkEnableAGC->isChecked();
        
        if (chkHideDirtyWords->isChecked()) {
            cfg->user_dirty_words_str = m_cachedUserWords.toStdString();
        } else {
            cfg->user_dirty_words_str = editDirtyWords->toPlainText().toStdString();
        }
        
        cfg->use_pinyin = chkUsePinyin->isChecked();
        cfg->comedy_mode = chkComedyMode->isChecked();
        
        cfg->audio_effect = comboEffect->currentData().toInt();

        cfg->video_delay_enabled = chkEnableVideoDelay->isChecked();

        // GPU Settings
        cfg->enable_gpu = chkEnableGpu->isChecked();
        cfg->onnx_provider = comboProvider->currentData().toString().toStdString();
        cfg->cuda_device_id = comboGpuDevice->currentData().toInt();
    }

    cfg->Save();

    // Apply state change ONLY if switch toggled
    if (old_enabled_state != cfg->video_delay_enabled) {
        UpdateVideoDelayFiltersState();
    }
}

void ConfigDialog::onSave() {
    onApply();
    accept();
}

// --- GPU Runtime Slots ---

void ConfigDialog::onGpuEnableChanged(bool enabled) {
    gpuSettingsContainer->setVisible(enabled);
    updateGpuStatus();
}

void ConfigDialog::updateGpuStatus() {
    if (!runtimeManager) {
        lblGpuStatus->setText("⚠️ Runtime Manager 未初始化");
        lblGpuStatus->setStyleSheet("color: #E6A23C;");
        btnRuntimeAction->setEnabled(false);
        return;
    }

    QString provider = comboProvider->currentData().toString();

    if (provider == "cpu") {
        lblGpuStatus->setText("✅ CPU 模式无需额外组件");
        lblGpuStatus->setStyleSheet("color: #67C23A;");
        btnRuntimeAction->setVisible(false);
        progressRuntime->setVisible(false);
        lblRuntimeStatus->setVisible(false);
        return;
    }

    // CUDA mode
    btnRuntimeAction->setVisible(true);
    lblRuntimeStatus->setVisible(true);

    if (runtimeManager->IsRuntimeInstalled("cuda")) {
        lblGpuStatus->setText("✅ CUDA Runtime 已就绪");
        lblGpuStatus->setStyleSheet("color: #67C23A;");
        btnRuntimeAction->setText("🗑️ 删除 CUDA Runtime");
        btnRuntimeAction->setEnabled(true);
        lblRuntimeStatus->setText("已安装");
    } else {
        // 检查是否配置了下载 URL
        const auto &runtimes = runtimeManager->GetRuntimes();
        bool hasUrl = false;
        for (const auto &r : runtimes) {
            if (r.id == "cuda" && !r.url.isEmpty()) {
                hasUrl = true;
                break;
            }
        }

        if (hasUrl) {
            lblGpuStatus->setText("⚠️ 需要下载 CUDA Runtime");
            lblGpuStatus->setStyleSheet("color: #E6A23C;");
            btnRuntimeAction->setText("⬇️ 下载 CUDA Runtime");
            btnRuntimeAction->setEnabled(true);
            lblRuntimeStatus->setText("未安装");
        } else {
            lblGpuStatus->setText("❌ 未配置 CUDA Runtime 下载地址");
            lblGpuStatus->setStyleSheet("color: #F56C6C;");
            btnRuntimeAction->setText("⬇️ 下载 CUDA Runtime");
            btnRuntimeAction->setEnabled(false);
            lblRuntimeStatus->setText("请在 runtime.json 中配置下载地址");
        }
    }
}

void ConfigDialog::onRuntimeAction() {
    if (!runtimeManager) return;

    QString provider = comboProvider->currentData().toString();
    if (provider != "cuda") return;

    if (runtimeManager->IsRuntimeInstalled("cuda")) {
        // Delete
        QMessageBox::StandardButton reply = QMessageBox::question(this, "确认删除",
            "确定要删除 CUDA Runtime 吗?\n删除后需要重新下载才能使用 GPU 加速。",
            QMessageBox::Yes | QMessageBox::No);

        if (reply == QMessageBox::Yes) {
            if (runtimeManager->DeleteRuntime("cuda")) {
                updateGpuStatus();
            } else {
                QMessageBox::critical(this, "删除失败", "无法删除 CUDA Runtime，可能文件正在被使用。");
            }
        }
    } else {
        // Download
        btnRuntimeAction->setEnabled(false);
        progressRuntime->setValue(0);
        progressRuntime->setVisible(true);
        lblRuntimeStatus->setText("准备下载...");

        runtimeManager->DownloadRuntime("cuda");
    }
}

void ConfigDialog::onRuntimeDownloadProgress(qint64 received, qint64 total) {
    if (total > 0) {
        int percent = (int)((received * 100) / total);
        progressRuntime->setValue(percent);
        double mbReceived = received / (1024.0 * 1024.0);
        double mbTotal = total / (1024.0 * 1024.0);
        lblRuntimeStatus->setText(QString("正在下载: %1 MB / %2 MB").arg(mbReceived, 0, 'f', 1).arg(mbTotal, 0, 'f', 1));
    } else {
        progressRuntime->setValue(0);
        lblRuntimeStatus->setText("正在下载...");
    }
}

void ConfigDialog::onRuntimeDownloadFinished(const QString &runtimeId) {
    (void)runtimeId;
    progressRuntime->setVisible(false);
    lblRuntimeStatus->setText("✅ 下载并解压完成!");
    updateGpuStatus();
    QMessageBox::information(this, "下载完成", "CUDA Runtime 已成功下载并安装。");
}

void ConfigDialog::onRuntimeDownloadError(const QString &msg) {
    progressRuntime->setVisible(false);
    lblRuntimeStatus->setText("❌ 下载失败");
    btnRuntimeAction->setEnabled(true);
    QMessageBox::critical(this, "下载失败", "错误信息: " + msg);
}

void InitGlobalConfig() {
    GetGlobalConfig()->Load();
}

void FreeGlobalConfig() {
    if (g_config) {
        delete g_config;
        g_config = nullptr;
    }
}

void OpenGlobalConfigDialog() {
    if (!g_dialog) {
        g_dialog = new ConfigDialog(nullptr); // Parent is null for top-level
        g_dialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    g_dialog->show();
    g_dialog->raise();
    g_dialog->activateWindow();
}

void FreeConfigDialog() {
    if (g_dialog) {
        g_dialog->close();
    }
}
