# OBS 语音脏话屏蔽与延迟插件 - 开发文档

本文档详细说明了 **obs-profanity-filter** 插件的设计原理、编译步骤以及核心实现细节，旨在帮助开发者理解代码并进行二次开发。

## 1. 核心原理

本插件的核心目标是：**在直播流中实现“预读”音频，利用 ASR（语音识别）检测脏话，并在脏话播放给观众之前将其屏蔽（静音或哔声）。**

为了实现这一目标，插件采用了 **“延迟+回溯”** 的架构：

### 1.1 音频流处理架构
1.  **输入拦截**：插件作为 OBS 的“音频滤镜”挂载在麦克风源上，拦截 48kHz 的原始 PCM 音频数据。
2.  **双路分发**：
    *   **路径 A（识别路）**：音频被降采样至 16kHz（Sherpa-ONNX 要求），推入一个线程安全的队列 (`asr_queue`)。
    *   **路径 B（输出路 - 环形缓冲区）**：原始 48kHz 音频被写入一个巨大的 **环形缓冲区 (Circular Buffer)**。
3.  **延迟输出**：`filter_audio` 回调函数不会立即返回刚刚写入的数据，而是返回 **延迟时间（如 1.5秒）之前** 的数据。
    *   这给了 ASR 引擎 1.5 秒的时间差去处理最新的音频。

### 1.2 脏话屏蔽逻辑 (Time Travel)
1.  **后台识别**：独立的 `ASRLoop` 线程不断从 `asr_queue` 取出音频进行识别。
2.  **模式匹配**：识别出的文本通过正则表达式 (`std::regex`) 匹配脏话列表。
3.  **回溯修改**：
    *   一旦检测到脏话，插件计算该脏话在音频流中的 **绝对时间戳**。
    *   由于输出是延迟的，这个时间戳对应的数据 **仍然位于环形缓冲区中，尚未被读取播放**。
    *   插件锁定缓冲区，直接修改对应位置的采样点数据（替换为静音 0.0 或正弦波哔声）。
4.  **最终播放**：当 OBS 最终从缓冲区读取该段数据时，已经是被“净化”过的音频。

## 2. 编译指南

本项目基于 standard OBS Plugin Template 和 CMake 构建系统。

### 2.1 环境要求
*   **操作系统**: Windows 10/11 (x64)
*   **编译器**: Visual Studio 2022 (MSVC) 或支持 C++20 的编译器
*   **构建工具**: CMake 3.28+
*   **依赖库**:
    *   `libobs` (由 CMake 自动查找或指定)
    *   `Sherpa-ONNX` (CMake 会自动从 HuggingFace 下载预编译库)

### 2.2 编译步骤

1.  **克隆仓库**:
    ```powershell
    git clone <repo_url>
    cd obs-profanity-filter
    ```

2.  **配置项目 (CMake)**:
    ```powershell
    # 在项目根目录下执行
    cmake -B build -G "Visual Studio 17 2022" -A x64
    ```
    *注意：CMake 脚本会自动下载 Sherpa-ONNX 的库文件，这可能需要网络连接。*

3.  **执行构建**:
    ```powershell
    cmake --build build --config Release
    ```

4.  **安装/部署**:
    编译完成后，插件 DLL 和必要的依赖 DLL (`onnxruntime.dll`, `sherpa-onnx-c-api.dll`) 会生成在 `build/Release/` 目录下。
    *   将生成的文件复制到 OBS Studio 的插件目录（通常是 `C:\Program Files\obs-studio\obs-plugins\64bit\`）。

## 3. 核心实现细节与注意事项

### 3.1 环形缓冲区 (Circular Buffer)
*   **实现**: 使用 `std::vector<float>` 作为底层存储，维护 `head` 指针。
*   **多通道支持**: 代码动态检测输入音频的通道数，并为每个通道独立分配缓冲区。
*   **难点**: 必须正确处理读写指针的“回绕 (Wrap-around)”。
    *   写入位置：`head`
    *   读取位置：`(head - delay_samples + buffer_size) % buffer_size`

### 3.2 线程安全
*   **互斥锁**:
    *   `queue_mutex`: 保护 ASR 输入队列，防止 OBS 音频线程与 ASR 线程冲突。
    *   `history_mutex`: 保护日志历史和实时部分文本 (`current_partial_text`)，供 UI 刷新使用。
    *   `beep_mutex`: 保护 `pending_beeps` 列表，防止在修改缓冲区时发生竞争。
*   **性能**: 锁的粒度要尽可能小，绝对不能阻塞 `filter_audio` 回调太久，否则会导致直播爆音。

### 3.3 ASR 引擎集成 (Sherpa-ONNX)
*   **C API**: 项目使用 Sherpa-ONNX 的 C API 而不是 C++ API，以减少 ABI 兼容性问题。
*   **降采样**: OBS 默认 48kHz，ASR 模型通常需要 16kHz。
    *   **算法**: 代码中使用简单的“三采样平均法” (`(s1+s2+s3)/3`) 进行降采样。虽然不如 FIR 滤波器完美，但性能极高且满足语音识别需求。
*   **模型文件**: 必须确保 `tokens.txt`, `encoder.onnx`, `decoder.onnx`, `joiner.onnx` 存在，否则引擎初始化会失败。代码中加入了详细的文件存在性检查。

### 3.4 独立控制台窗口 (Console Hack)
*   **功能**: 为了方便调试，插件支持弹出一个独立的 cmd 窗口显示实时识别结果。
*   **风险**: Windows 控制台窗口的“关闭 (X)”按钮默认会发送 `CTRL_CLOSE_EVENT`，这不仅会关闭控制台，**还会强制终止父进程 (OBS)**。
*   **解决方案**:
    ```cpp
    // 获取系统菜单并禁用关闭按钮
    HWND hwnd = GetConsoleWindow();
    HMENU hMenu = GetSystemMenu(hwnd, FALSE);
    EnableMenuItem(hMenu, SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
    ```
    这强制用户必须通过插件设置页面来关闭控制台，从而避免意外杀掉 OBS。

### 3.5 UI 与交互
*   **日志显示**: 使用 `OBS_TEXT_MULTILINE` 实现多行日志框。
*   **实时刷新**: OBS 的属性面板通常是静态的。为了实现实时动态显示（如音量跳动、文字上屏），我们利用了 `refresh_history` 回调，并配合 `obs_property_set_long_description` 或直接修改 `settings` 来强制 UI 更新。

### 3.6 关键代码路径
*   `src/plugin-main.cpp`: 包含所有核心逻辑。
    *   `filter_audio()`: 音频入口，负责写入缓冲、降采样、应用屏蔽、读取延迟数据。
    *   `ASRLoop()`: 后台线程，负责运行识别引擎。
    *   `InitializeASR()`: 加载模型。
    *   `ToggleConsole()`: 管理调试窗口。

## 4. 常见问题排查

*   **无识别结果**:
    1.  检查状态面板中的“音量”是否有跳动。若为 0，说明滤镜未正确挂载或静音。
    2.  检查模型路径下文件是否齐全。
*   **OBS 闪退**:
    *   通常是因为 DLL 依赖缺失。请确保 `onnxruntime.dll` 和 `sherpa-onnx-c-api.dll` 与插件 DLL 在同一目录。
*   **音画不同步**:
    *   本插件只延迟音频。**必须**手动在视频源上添加“视频延迟 (异步)”滤镜，并设置为相同的时间（默认 1500ms）。

---
*Last Updated: 2025-12-03*
