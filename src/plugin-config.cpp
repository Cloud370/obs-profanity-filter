#include "plugin-config.hpp"
#include <obs-module.h>
#include <obs.h>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace std;

static GlobalConfig* g_config = nullptr;
static obs_module_t* g_module = nullptr;

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
    lock_guard<std::mutex> lock(this->mutex);
    
    obs_data_t *data = obs_data_create();
    obs_data_set_string(data, "model_path", model_path.c_str());
    obs_data_set_double(data, "delay_seconds", delay_seconds);
    obs_data_set_string(data, "dirty_words", dirty_words_str.c_str());
    obs_data_set_bool(data, "mute_mode", mute_mode);
    obs_data_set_int(data, "beep_freq", beep_frequency);
    obs_data_set_bool(data, "show_console", show_console);
    obs_data_set_string(data, "debug_log_path", debug_log_path.c_str());
    
    if (g_module) {
        char *config_path = obs_module_get_config_path(g_module, "global_config.json");
        if (config_path) {
            filesystem::path p(config_path);
            if (p.has_parent_path()) {
                filesystem::create_directories(p.parent_path());
            }
            obs_data_save_json(data, config_path);
            bfree(config_path);
        }
    }
    
    obs_data_release(data);
    
    ParsePatterns();
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
        dirty_words_str = "fuck, shit, bitch, 卧槽, 他妈, 傻逼, 操, 逼的, 你妈, 死全家";
        ParsePatterns();
        return;
    }
    
    obs_data_t *data = obs_data_create_from_json_file(path.c_str());
    
    if (data) {
        const char* s = obs_data_get_string(data, "model_path");
        model_path = s ? s : "";
        
        delay_seconds = obs_data_get_double(data, "delay_seconds");
        if (delay_seconds < 0.1) delay_seconds = 1.5;
        
        s = obs_data_get_string(data, "dirty_words");
        dirty_words_str = s ? s : "fuck, shit, bitch, 卧槽, 他妈, 傻逼, 操, 逼的, 你妈, 死全家";
        
        mute_mode = obs_data_get_bool(data, "mute_mode");
        beep_frequency = obs_data_get_int(data, "beep_freq");
        if (beep_frequency < 200) beep_frequency = 1000;
        
        show_console = obs_data_get_bool(data, "show_console");
        
        s = obs_data_get_string(data, "debug_log_path");
        debug_log_path = s ? s : "";
        
        obs_data_release(data);
    } else {
        // Defaults
        dirty_words_str = "fuck, shit, bitch, 卧槽, 他妈, 傻逼, 操, 逼的, 你妈, 死全家";
    }
    
    ParsePatterns();
    loaded = true;
}

// --- UI Implementation ---

ConfigDialog::ConfigDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle("语音脏话屏蔽 - 全局配置");
    resize(600, 500);
    
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    
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
    mainLayout->addWidget(grpModel);
    
    // Audio Group
    QGroupBox *grpAudio = new QGroupBox("音频处理");
    QFormLayout *layoutAudio = new QFormLayout(grpAudio);
    
    spinDelay = new QDoubleSpinBox();
    spinDelay->setRange(0.1, 10.0);
    spinDelay->setSingleStep(0.1);
    spinDelay->setSuffix(" 秒");
    
    chkMuteMode = new QCheckBox("静音模式 (不播放哔声)");
    
    spinBeepFreq = new QSpinBox();
    spinBeepFreq->setRange(200, 5000);
    spinBeepFreq->setSingleStep(100);
    spinBeepFreq->setSuffix(" Hz");
    
    layoutAudio->addRow("全局延迟时间:", spinDelay);
    layoutAudio->addRow("", chkMuteMode);
    layoutAudio->addRow("哔声频率:", spinBeepFreq);
    mainLayout->addWidget(grpAudio);
    
    // Words Group
    QGroupBox *grpWords = new QGroupBox("屏蔽词设置");
    QVBoxLayout *layoutWords = new QVBoxLayout(grpWords);
    
    layoutWords->addWidget(new QLabel("屏蔽词列表 (逗号分隔):"));
    editDirtyWords = new QTextEdit();
    layoutWords->addWidget(editDirtyWords);
    mainLayout->addWidget(grpWords);
    
    // Debug Group
    QGroupBox *grpDebug = new QGroupBox("调试选项");
    QFormLayout *layoutDebug = new QFormLayout(grpDebug);
    
    chkShowConsole = new QCheckBox("显示调试控制台 (独立窗口)");
    
    QHBoxLayout *boxLog = new QHBoxLayout();
    editLogPath = new QLineEdit();
    QPushButton *btnBrowseLog = new QPushButton("浏览...");
    connect(btnBrowseLog, &QPushButton::clicked, this, &ConfigDialog::onBrowseLog);
    boxLog->addWidget(editLogPath);
    boxLog->addWidget(btnBrowseLog);
    
    layoutDebug->addRow("", chkShowConsole);
    layoutDebug->addRow("日志文件路径:", boxLog);
    mainLayout->addWidget(grpDebug);
    
    // Buttons
    QHBoxLayout *btnLayout = new QHBoxLayout();
    QPushButton *btnSave = new QPushButton("保存并应用");
    QPushButton *btnCancel = new QPushButton("取消");
    
    connect(btnSave, &QPushButton::clicked, this, &ConfigDialog::onSave);
    connect(btnCancel, &QPushButton::clicked, this, &ConfigDialog::reject);
    
    btnLayout->addStretch();
    btnLayout->addWidget(btnSave);
    btnLayout->addWidget(btnCancel);
    mainLayout->addLayout(btnLayout);
    
    LoadToUI();
}

ConfigDialog::~ConfigDialog() {}

void ConfigDialog::LoadToUI() {
    GlobalConfig *cfg = GetGlobalConfig();
    lock_guard<std::mutex> lock(cfg->mutex);
    
    editModelPath->setText(QString::fromStdString(cfg->model_path));
    spinDelay->setValue(cfg->delay_seconds);
    editDirtyWords->setText(QString::fromStdString(cfg->dirty_words_str));
    chkMuteMode->setChecked(cfg->mute_mode);
    spinBeepFreq->setValue(cfg->beep_frequency);
    chkShowConsole->setChecked(cfg->show_console);
    editLogPath->setText(QString::fromStdString(cfg->debug_log_path));
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

void ConfigDialog::onSave() {
    GlobalConfig *cfg = GetGlobalConfig();
    
    {
        lock_guard<std::mutex> lock(cfg->mutex);
        cfg->model_path = editModelPath->text().toStdString();
        cfg->delay_seconds = spinDelay->value();
        cfg->dirty_words_str = editDirtyWords->toPlainText().toStdString();
        cfg->mute_mode = chkMuteMode->isChecked();
        cfg->beep_frequency = spinBeepFreq->value();
        cfg->show_console = chkShowConsole->isChecked();
        cfg->debug_log_path = editLogPath->text().toStdString();
    }
    
    cfg->Save();
    accept();
}

void InitGlobalConfig() {
    GetGlobalConfig()->Load();
}

void OpenGlobalConfigDialog() {
    static ConfigDialog *dialog = nullptr;
    // Note: We must be careful about thread safety with Qt, but the callback from Menu is usually on UI thread.
    if (!dialog) {
        QWidget *mainWindow = (QWidget*)obs_frontend_get_main_window();
        dialog = new ConfigDialog(mainWindow);
    }
    dialog->LoadToUI();
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}
