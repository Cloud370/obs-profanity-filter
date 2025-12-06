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
        /*
        beep_frequency = obs_data_get_int(data, "beep_freq");
        if (beep_frequency < 200) beep_frequency = 1000;
        */

        beep_mix_percent = 100;
        /*
        if (obs_data_has_user_value(data, "beep_mix")) {
            beep_mix_percent = obs_data_get_int(data, "beep_mix");
            if (beep_mix_percent < 0 || beep_mix_percent > 100) beep_mix_percent = 100;
        } else {
            beep_mix_percent = 100;
        }
        */
        
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
    resize(600, 500);
    
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
    QGroupBox *grpModel = new QGroupBox("模型设置");
    QFormLayout *layoutModel = new QFormLayout(grpModel);
    
    QHBoxLayout *boxPath = new QHBoxLayout();
    editModelPath = new QLineEdit();
    QPushButton *btnBrowse = new QPushButton("浏览...");
    connect(btnBrowse, &QPushButton::clicked, this, &ConfigDialog::onBrowseModel);
    boxPath->addWidget(editModelPath);
    boxPath->addWidget(btnBrowse);
    
    layoutModel->addRow("Sherpa-ONNX 模型路径:", boxPath);
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
    
    // Deprecated UI elements for beep frequency/mix removed as per user request
    // spinBeepFreq & spinBeepMix are removed
    
    layoutAudio->addRow("全局延迟时间:", spinDelay);
    layoutAudio->addRow("屏蔽音效:", comboEffect);
    // layoutAudio->addRow("哔声频率:", spinBeepFreq);
    // layoutAudio->addRow("哔声/静音混合比例:", spinBeepMix);
    
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
    editModelPath->setText(QString::fromStdString(cfg->model_path));
    spinDelay->setValue((int)(cfg->delay_seconds * 1000));
    editDirtyWords->setText(QString::fromStdString(cfg->dirty_words_str));
    chkUsePinyin->setChecked(cfg->use_pinyin);
    chkComedyMode->setChecked(cfg->comedy_mode);
    
    // Map audio_effect to combo
    int effect_idx = comboEffect->findData(cfg->audio_effect);
    if (effect_idx != -1) comboEffect->setCurrentIndex(effect_idx);
    else comboEffect->setCurrentIndex(0); // Default to Beep

    // spinBeepFreq->setValue(cfg->beep_frequency);
    // spinBeepMix->setValue(cfg->beep_mix_percent);
    editLogPath->setText(QString::fromStdString(cfg->debug_log_path));
    chkEnableVideoDelay->setChecked(cfg->video_delay_enabled);
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

void ConfigDialog::onBrowseModel() {
    QString dir = QFileDialog::getExistingDirectory(this, "选择模型文件夹", editModelPath->text());
    if (!dir.isEmpty()) {
        editModelPath->setText(dir);
    }
}

void ConfigDialog::onBrowseLog() {
    QString file = QFileDialog::getSaveFileName(this, "选择日志文件", editLogPath->text());
    if (!file.isEmpty()) {
        editLogPath->setText(file);
    }
}

void ConfigDialog::onApply() {
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

        // cfg->beep_frequency = spinBeepFreq->value();
        // cfg->beep_mix_percent = spinBeepMix->value();
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

void SetGlobalConfigModule(obs_module_t *module);

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