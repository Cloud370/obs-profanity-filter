#include "plugin-config.hpp"
#include "video-delay.hpp"
#include <obs-module.h>
#include <obs.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <QPointer>

using namespace std;

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
    stringstream ss(dirty_words_str);
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
    
    {
        lock_guard<std::mutex> lock(this->mutex);
        
        obs_data_set_bool(data, "global_enable", global_enable);
        obs_data_set_string(data, "model_path", model_path.c_str());
        obs_data_set_double(data, "delay_seconds", delay_seconds);
        obs_data_set_string(data, "dirty_words", dirty_words_str.c_str());
        obs_data_set_bool(data, "use_pinyin", use_pinyin);
        obs_data_set_bool(data, "comedy_mode", comedy_mode);
        obs_data_set_bool(data, "mute_mode", mute_mode);
        obs_data_set_int(data, "audio_effect", audio_effect);
        obs_data_set_int(data, "beep_freq", beep_frequency);
        obs_data_set_int(data, "beep_mix", beep_mix_percent);
        obs_data_set_string(data, "debug_log_path", debug_log_path.c_str());
        obs_data_set_bool(data, "video_delay_enabled", video_delay_enabled);
        
        ParsePatterns();
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
    
    string path;
    if (g_module) {
        char *config_path = obs_module_get_config_path(g_module, "global_config.json");
        if (config_path) {
            path = config_path;
            bfree(config_path);
        }
    }
    
    if (path.empty() || !filesystem::exists(path)) {
        // Fallback defaults
        dirty_words_str = "卧槽, 他妈, 傻逼, 操, 逼的, 你妈, 死全家";
        ParsePatterns();
        return;
    }
    
    obs_data_t *data = obs_data_create_from_json_file(path.c_str());
    
    if (data) {
        if (obs_data_has_user_value(data, "global_enable")) {
            global_enable = obs_data_get_bool(data, "global_enable");
        } else {
            global_enable = true;
        }

        const char* s = obs_data_get_string(data, "model_path");
        model_path = s ? s : "";
        
        delay_seconds = obs_data_get_double(data, "delay_seconds");
        if (delay_seconds < 0.01) delay_seconds = 0.5;
        
        s = obs_data_get_string(data, "dirty_words");
        dirty_words_str = s ? s : "卧槽, 他妈, 傻逼, 操, 逼的, 你妈, 死全家";
        
        use_pinyin = obs_data_get_bool(data, "use_pinyin");
        
        if (obs_data_has_user_value(data, "comedy_mode")) {
            comedy_mode = obs_data_get_bool(data, "comedy_mode");
        } else {
            comedy_mode = false;
        }

        mute_mode = obs_data_get_bool(data, "mute_mode");
        if (obs_data_has_user_value(data, "audio_effect")) {
            audio_effect = obs_data_get_int(data, "audio_effect");
        } else {
            // Migration
            audio_effect = mute_mode ? 1 : 0; // 1=Silence, 0=Beep
        }

        // Deprecated: Force defaults to ensure consistency now that UI is removed
        // We ignore saved values for frequency and mix to prevent users from being stuck with bad hidden settings
        beep_frequency = 1000; 
        
        beep_mix_percent = 100;
        
        s = obs_data_get_string(data, "debug_log_path");
        debug_log_path = s ? s : "";
        
        if (obs_data_has_user_value(data, "video_delay_enabled")) {
            video_delay_enabled = obs_data_get_bool(data, "video_delay_enabled");
        } else {
            video_delay_enabled = true;
        }

        obs_data_release(data);
    } else {
        // Defaults
        dirty_words_str = "fuck, shit, bitch, 卧槽, 他妈, 傻逼, 操, 逼的, 你妈, 死全家";
        video_delay_enabled = true;
        is_first_run = true;
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
    
    comboModel = new QComboBox();
    const auto &loadedModels = modelManager->GetModels();
    blog(LOG_INFO, "[ProfanityFilter] Populating combo box with %zu models", loadedModels.size());

    for (const auto &m : loadedModels) {
        comboModel->addItem(m.name, m.id);
        blog(LOG_INFO, "[ProfanityFilter] Added model to combo: %s (%s)", m.name.toStdString().c_str(), m.id.toStdString().c_str());
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
    connect(btnDownloadModel, &QPushButton::clicked, this, &ConfigDialog::onDownloadModel);
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
    layoutModel->addRow("", boxDownload);
    
    containerLayout->addWidget(grpModel);
    
    // Audio Group
    QGroupBox *grpAudio = new QGroupBox("音频处理");
    QFormLayout *layoutAudio = new QFormLayout(grpAudio);
    
    spinDelay = new QSpinBox();
    spinDelay->setRange(0, 10000);
    spinDelay->setSingleStep(50);
    spinDelay->setSuffix(" ms");
    
    // Audio Effect Selection
    comboEffect = new QComboBox();
    comboEffect->addItem("标准哔声 (Beep)", 0);
    comboEffect->addItem("静音 (Silence)", 1);
    comboEffect->addItem("小黄人音效 (Minion)", 2);
    comboEffect->addItem("电报音效 (Telegraph)", 3);
    
    layoutAudio->addRow("全局延迟时间:", spinDelay);
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
    
    layoutWords->addWidget(new QLabel("屏蔽词列表 (逗号分隔):"));
    editDirtyWords = new QTextEdit();
    layoutWords->addWidget(editDirtyWords);
    
    chkUsePinyin = new QCheckBox("启用拼音增强识别 (模糊匹配)");
    chkUsePinyin->setToolTip("开启后将使用拼音进行匹配，忽略声调和平卷舌差异，提高识别率。");
    layoutWords->addWidget(chkUsePinyin);

    chkComedyMode = new QCheckBox("精准变声模式 (优先匹配短词)");
    chkComedyMode->setToolTip("开启后，当匹配到多个词时（如'我爱你'和'爱你'），\n优先只屏蔽较短的词（'爱你'），从而保留'我'的原声。\n配合变音特效可实现更生动的喜剧效果。");
    layoutWords->addWidget(chkComedyMode);

    containerLayout->addWidget(grpWords);
    
    // Debug Group
    QGroupBox *grpDebug = new QGroupBox("调试选项");
    QFormLayout *layoutDebug = new QFormLayout(grpDebug);
    
    QHBoxLayout *boxLog = new QHBoxLayout();
    editLogPath = new QLineEdit();
    QPushButton *btnBrowseLog = new QPushButton("浏览...");
    connect(btnBrowseLog, &QPushButton::clicked, this, &ConfigDialog::onBrowseLog);
    boxLog->addWidget(editLogPath);
    boxLog->addWidget(btnBrowseLog);
    
    layoutDebug->addRow("日志文件路径:", boxLog);
    containerLayout->addWidget(grpDebug);
    
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
    
    spinDelay->setValue((int)(cfg->delay_seconds * 1000));
    editDirtyWords->setText(QString::fromStdString(cfg->dirty_words_str));
    chkUsePinyin->setChecked(cfg->use_pinyin);
    chkComedyMode->setChecked(cfg->comedy_mode);
    
    // Map audio_effect to combo
    int effect_idx = comboEffect->findData(cfg->audio_effect);
    if (effect_idx != -1) comboEffect->setCurrentIndex(effect_idx);
    else comboEffect->setCurrentIndex(0); // Default to Beep

    editLogPath->setText(QString::fromStdString(cfg->debug_log_path));
    chkEnableVideoDelay->setChecked(cfg->video_delay_enabled);
    
    // Trigger update of download button state
    onModelComboChanged(comboModel->currentIndex());
}

void ConfigDialog::updateStatus() {
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
        btnDownloadModel->setVisible(!installed);
        btnDownloadModel->setEnabled(!installed);
        progressDownload->setVisible(false);
        lblDownloadStatus->setVisible(false);
        
        if (installed) {
            lblDownloadStatus->setText("✅ 已安装 (Ready)");
            lblDownloadStatus->setVisible(true);
        } else {
             lblDownloadStatus->setText("⚠️ 未安装 (需要下载)");
             lblDownloadStatus->setVisible(true);
        }
    }
}

void ConfigDialog::onDownloadModel() {
    QString id = comboModel->currentData().toString();
    if (id == "custom") return;
    
    btnDownloadModel->setEnabled(false);
    progressDownload->setValue(0);
    progressDownload->setVisible(true);
    lblDownloadStatus->setText("准备下载...");
    lblDownloadStatus->setVisible(true);
    
    modelManager->DownloadModel(id);
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

void ConfigDialog::onBrowseLog() {
    QString file = QFileDialog::getSaveFileName(this, "选择日志文件", editLogPath->text());
    if (!file.isEmpty()) {
        editLogPath->setText(file);
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
        cfg->delay_seconds = spinDelay->value() / 1000.0;
        cfg->dirty_words_str = editDirtyWords->toPlainText().toStdString();
        cfg->use_pinyin = chkUsePinyin->isChecked();
        cfg->comedy_mode = chkComedyMode->isChecked();
        
        cfg->audio_effect = comboEffect->currentData().toInt();
        cfg->mute_mode = (cfg->audio_effect == 1); // Backward compat

        cfg->debug_log_path = editLogPath->text().toStdString();
        cfg->video_delay_enabled = chkEnableVideoDelay->isChecked();
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
