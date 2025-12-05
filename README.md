# OBS 语音脏话屏蔽与延迟插件 (OBS Profanity Filter)

**obs-profanity-filter** 是一个为 OBS Studio 开发的高级音频插件，旨在直播过程中实时检测并屏蔽脏话。

## 核心功能

- **预读屏蔽 (Time Travel)**: 插件通过强制延迟音频输出，利用这段时间差进行语音识别。当检测到脏话时，插件会在音频播放给观众**之前**将其替换为静音或“哔”声，实现真正的“直播消音”。
- **本地离线识别**: 集成 **Sherpa-ONNX** 语音识别引擎，无需联网，保护隐私，且占用资源极低。
- **全局统一配置**: 通过 OBS 的“工具”菜单统一管理模型路径、屏蔽词库和延迟参数。
- **拼音模糊匹配**: 新增拼音增强识别功能（由 [cpp-pinyin](https://github.com/wolfgitpr/cpp-pinyin) 驱动）。
  - 忽略声调（如“bǐ”匹配“bi”）。
  - 模糊音处理（自动平翘舌 z-zh, c-ch, s-sh 转换，前后鼻音 ng-n 归一化）。
  - 即使语音识别结果字形错误（如“比”），只要拼音接近，也能精准屏蔽。
- **高度可定制**:
  - 自定义屏蔽词列表（支持正则表达式）。
  - 可选“静音”模式或“哔声”模式（可调频率）。
  - 支持独立的调试控制台窗口，实时查看识别结果。

## 更新日志

### v0.0.3 (2025-12-06)
- **架构重构**:
  - 将单体插件代码重构为模块化架构，提升代码结构清晰度与可维护性。
  - 统一构建配置，规范化构建目录。
- **核心修复**:
  - **非48kHz适配**: 修复在非 48kHz 采样率下的音画同步问题，支持动态采样率。
  - **音频流稳定性**: 修复延迟累积、索引冲突及多线程样本计数同步问题，提升长时间运行的稳定性。


### v0.0.2 (2025-12-04)
- **优化识别率**: 针对短语（一两个字）识别率低的问题进行了核心优化。
  - 将解码算法从 `greedy_search` 升级为 `modified_beam_search`，大幅提升短语和生僻词的捕捉能力。
  - 启用了端点检测 (Endpoint Detection)，解决说话停顿后可能导致的“吞字”现象。
- **体验改进**:
  - 默认开启拼音模糊匹配模式，新手上手更友好。
  - 默认延迟时间从 1.0秒 调整为 500毫秒，平衡了延迟与识别稳定性。
  - 配置界面单位优化：延迟设置现在以 **毫秒 (ms)** 为单位，调节更直观。

---

## 安装指南

### 1. 下载插件

请前往 [Releases](https://github.com/Cloud370/obs-profanity-filter/releases) 页面下载最新版本的安装程序（推荐）或插件压缩包。

### 2. 安装

- **安装程序 (推荐)**: 直接运行下载的 `.exe` 安装包，按照提示完成安装。
- **手动安装**: 将压缩包内的文件解压到您的 OBS Studio 安装目录下的 `obs-plugins/64bit/` 文件夹中。
  - 通常路径为: `C:\Program Files\obs-studio\obs-plugins\64bit\`
  - 确保 `obs-profanity-filter.dll` 以及依赖文件 (`onnxruntime.dll`, `sherpa-onnx-c-api.dll` 等) 都在该目录下。

### 3. 准备模型

本插件需要 Sherpa-ONNX 格式的语音识别模型。

1.  下载预训练模型（推荐使用 `sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20`）：  
    [ModelScope 下载地址](https://modelscope.cn/models/cloud370/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20/file/view/master/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20.zip)
2.  解压模型文件到一个固定目录（例如 `D:\OBS-Models\`）。
3.  确保目录内包含 `tokens.txt`, `encoder.onnx`, `decoder.onnx`, `joiner.onnx` 等核心文件。

---

## 使用教程

### 第一步：全局配置

1.  启动 OBS Studio。
2.  点击顶部菜单栏的 **“工具 (Tools)”** -> **“语音脏话屏蔽配置 (Profanity Filter Settings)”**。
3.  在弹出的设置窗口中：
    - **模型路径**: 选择您刚才解压的模型文件夹路径。
    - **全局延迟时间**: 设置音频延迟时间（建议 500 毫秒，**最低不能低于 300 毫秒**，否则根据原理 Beep 声无法跟上语速）。
    - **屏蔽词列表**: 输入需要屏蔽的词汇（英文逗号分隔）。
    - **音频处理**: 选择是静音还是播放哔声。
4.  点击“保存并应用”。

### 第二步：添加滤镜

1.  在 OBS 主界面，找到您的麦克风/音频输入源。
2.  右键点击该源 -> **“滤镜 (Filters)”**。
3.  点击左下角的 **“+”** 号，选择 **“语音脏话屏蔽 (Profanity Filter)”**。
4.  添加后，您应该能看到滤镜的状态面板显示“引擎状态: 运行中”。

5.  **提示**: 如果您想确认效果，可以右键点击混音器 -> **“高级音频设置 (Advanced Audio Properties)”**，将该音源的监听设置为 **“仅监听 (Monitor Only)”** 或 **“监听并输出 (Monitor and Output)”**。这样您就能在耳机中听到延迟后的处理结果。

### 第三步：同步视频（重要！） 🎬

由于音频被延迟了（例如 500 毫秒），您必须手动延迟视频画面以保持音画同步。

1.  在“场景”列表中，右键点击您的直播场景 -> **“滤镜 (Filters)”**。
2.  添加 **“渲染延迟 (Render Delay)”** 滤镜。
3.  **注意**：该滤镜单次最大延迟为 500 毫秒。如果您需要 1 秒（1000 毫秒）的延迟，请重复添加 2 个该滤镜，并将每个都设置为 500 毫秒。

---

## 编译指南 (Build from Source)

如果您是开发者并希望自行修改代码，请遵循以下步骤。

### 环境要求

- **操作系统**: Windows 10/11 (x64)
- **编译器**: Visual Studio 2022 (MSVC) - 需支持 C++20
- **构建工具**: CMake 3.28 或更高版本
- **依赖库**:
  - `Qt 6` (用于配置界面)
  - `libobs` (OBS 核心库)
  - `obs-frontend-api` (OBS 前端接口)

### 构建步骤

1.  **克隆仓库**:

    ```powershell
    # 务必使用 --recursive 参数以拉取 cpp-pinyin 子模块
    git clone --recursive https://github.com/Cloud370/obs-profanity-filter.git
    cd obs-profanity-filter

    # 如果已经克隆但没有子模块:
    # git submodule update --init --recursive
    ```

2.  **配置项目**:

    ```powershell
    # 确保 Qt6 在您的环境变量中，或者通过 CMAKE_PREFIX_PATH 指定
    cmake -B build -G "Visual Studio 17 2022" -A x64
    ```

    _CMake 会自动从 HuggingFace 下载 Sherpa-ONNX 依赖库。_

3.  **编译**:

    ```powershell
    cmake --build build --config Release
    ```

4.  **安装**:
    编译完成后，生成的 DLL 文件位于 `build/Release/`。将它们复制到 OBS 插件目录进行测试。

5.  **制作安装包 (可选)**:
    如果您想生成 Windows 安装程序 (`.exe`)，请运行项目根目录下的 PowerShell 脚本：
    ```powershell
    .\Make-Installer.ps1
    ```
    该脚本会自动检测环境、编译 Release 版本并生成 InnoSetup 安装包。
    _需预先安装 [Inno Setup 6](https://jrsoftware.org/isdl.php)_

---

## 技术原理

本插件采用了 **"延迟+回溯" (Delay & Backtrack)** 的架构设计：

1.  **环形缓冲区 (Circular Buffer)**: 插件维护一个巨大的音频缓冲区，所有输入的麦克风声音都会先进入这个缓冲区。
2.  **异步识别 (Async ASR)**: 独立的后台线程从缓冲区获取最新的音频数据，送入 Sherpa-ONNX 引擎进行识别。
3.  **延迟回放**: OBS 从插件获取的音频数据并非“当前”的声音，而是 **1 秒之前** 的声音。
4.  **时间戳映射**: 当 ASR 识别出脏话时，插件会计算该脏话在原始音频流中的精确时间戳。由于输出有 1 秒 的延迟，这段脏话的音频数据此刻 **仍然在缓冲区中等待播放**。
5.  **即时修改**: 插件直接定位到缓冲区中的对应位置，将脏话片段的数据抹除（置零或替换为正弦波），从而在声音播出前完成“净化”。

---

## 常见问题

**Q: 为什么添加滤镜后没有声音？**
A: 检查“全局配置”中的延迟设置是否过大，或者模型是否加载失败。请打开“显示调试控制台”查看是否有错误日志。

**Q: 为什么脏话没有被屏蔽？**
A:

1. 确认模型路径正确且模型文件齐全。
2. 确认屏蔽词是否在列表中。
3. 语速过快或背景噪音可能影响识别率。
4. 延迟时间设置太短，导致识别结果出来时，声音已经播放出去了（**最低需设置为 300 毫秒**，建议 500 毫秒以上）。

**Q: 音画不同步怎么办？**
A: 这是预期行为。请务必按照“使用教程-第三步”对视频源添加相同的延迟滤镜。

---

## 鸣谢 (Acknowledgments)

- [Sherpa-ONNX](https://github.com/k2-fsa/sherpa-onnx): 强大的开源语音识别框架。
- [cpp-pinyin](https://github.com/wolfgitpr/cpp-pinyin): 高效的中文拼音转换库。
