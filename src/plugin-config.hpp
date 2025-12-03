#pragma once

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QDialog>
#include <QWidget>
#include <QCheckBox>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QLabel>
#include <QGroupBox>
#include <QFormLayout>
#include <QFileDialog>
#include <QMessageBox>

#include <string>
#include <vector>
#include <mutex>
#include <regex>

// Global Configuration Structure
struct GlobalConfig {
    bool loaded = false;
    
    // Settings
    std::string model_path;
    double delay_seconds = 1.0;
    std::string dirty_words_str; // Comma separated
    bool mute_mode = false;
    int beep_frequency = 1000;
    int beep_mix_percent = 100;
    bool show_console = false;
    std::string debug_log_path;
    bool use_pinyin = false;
    
    // Parsed State
    std::vector<std::regex> dirty_patterns;
    
    mutable std::mutex mutex;

    void Save();
    void Load();
    void ParsePatterns();
};

// Singleton Access
GlobalConfig* GetGlobalConfig();

// Config Dialog
class ConfigDialog : public QDialog {
    Q_OBJECT
public:
    ConfigDialog(QWidget *parent = nullptr);
    ~ConfigDialog();
    void LoadToUI();
    
private slots:
    void onBrowseModel();
    void onBrowseLog();
    void onSave();
    
private:
    QLineEdit *editModelPath;
    QDoubleSpinBox *spinDelay;
    QTextEdit *editDirtyWords; // Use TextEdit for multiline
    QCheckBox *chkMuteMode;
    QCheckBox *chkUsePinyin;
    QSpinBox *spinBeepFreq;
    QSpinBox *spinBeepMix;
    QCheckBox *chkShowConsole;
    QLineEdit *editLogPath;
};

void InitGlobalConfig();
void FreeGlobalConfig();
void SetGlobalConfigModule(obs_module_t *module);
void OpenGlobalConfigDialog();
void FreeConfigDialog();
