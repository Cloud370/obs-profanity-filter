#pragma once

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QDialog>
#include <QWidget>
#include <QCheckBox>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QComboBox>
#include <QTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QLabel>
#include <QGroupBox>
#include <QFormLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QTimer>
#include <QProgressBar>

#include "model-manager.hpp"

#include <string>
#include <vector>
#include <mutex>
#include <regex>

// Global Configuration Structure
struct GlobalConfig {
    bool loaded = false;
    bool is_first_run = false;
    
    // Settings
    bool global_enable = true;
    std::string model_path;
    double delay_seconds = 0.5;
    std::string dirty_words_str; // Combined (for internal use)
    std::string system_dirty_words_str; // Read-only built-in
    std::string user_dirty_words_str;   // User custom
    int audio_effect = 0; // 0=Beep, 1=Silence, 2=Squeaky, 3=Robot
    int beep_frequency = 1000;
    int beep_mix_percent = 100;
    bool use_pinyin = true;
    bool comedy_mode = false;
    bool video_delay_enabled = true;
    
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
    void onSave();
    void onApply();
    void updateStatus();
    void onModelComboChanged(int index);
    void onModelAction();
    void onDownloadProgress(qint64 received, qint64 total);
    void onDownloadFinished(const QString &modelId);
    void onDownloadError(const QString &msg);
    
private:
    QCheckBox *chkGlobalEnable;
    QComboBox *comboModel; // Replaces editModelPath for main selection
    QLineEdit *editModelPath; // Hidden or advanced
    QPushButton *btnDownloadModel;
    QProgressBar *progressDownload;
    QLabel *lblDownloadStatus;
    QLabel *lblModelStatus;
    
    PluginModelManager *modelManager;
    
    QSpinBox *spinDelay;
    QTextEdit *editDirtyWords; // User Custom Words
    QTextEdit *editSystemDirtyWords; // System Built-in Words (Read-only)
    QCheckBox *chkHideDirtyWords;
    QString m_cachedUserWords;
    QString m_cachedSystemWords;
    
    QCheckBox *chkMuteMode; // Deprecated UI, replaced by comboEffect
    QComboBox *comboEffect;
    QCheckBox *chkUsePinyin;
    QCheckBox *chkComedyMode;
    QCheckBox *chkEnableVideoDelay;
    QLabel *lblVideoMemory;
    QLabel *lblPathTitle; // Added for dynamic label update
    QTimer *statusTimer;
    
    // Container for all settings below global switch
    QWidget *settingsContainer;
};

void InitGlobalConfig();
void FreeGlobalConfig();
void SetGlobalConfigModule(obs_module_t *module);
void OpenGlobalConfigDialog();
void FreeConfigDialog();

// Function implemented in plugin-main.cpp
void UpdateVideoDelayFiltersState();
