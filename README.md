# ScreenCast

> 局域网屏幕共享（投屏）系统 —— 教师端采集本机屏幕，**40 个客户端同屏观看**，高清原画、低延时。

**ScreenCast** 是一套面向**课堂教学 / 会议投屏**场景的 Windows 局域网屏幕共享软件，
使用 **C++17** 编写，采集走 **DXGI Desktop Duplication**、编码解码走 **FFmpeg**、
网络走 **UDP（含 FEC 前向纠错）**、渲染走 **D3D11**，界面基于 **Qt 6**。

教师端把屏幕画面实时推给一组客户端，客户端边收边解码直接上屏，
**端到端延迟实测 16–63 ms**，组播模式下**一份带宽即可服务全部客户端**。

---

## 核心特性

| 能力 | 说明 |
| --- | --- |
| **硬件级屏幕捕获** | DXGI Desktop Duplication，自带**脏区矩形**，Win8+ 官方推荐方案 |
| **差分投屏** | 静态画面只传变化区域，带宽大幅下降（核心优化） |
| **组播分发** | 一路流量服务 40 个客户端，交换机只需开 IGMP Snooping |
| **FEC 前向纠错** | 丢包自愈，实测 **1% 丢包下 80% 受损帧被救回** |
| **自适应码率（v3.6）** | 客户端上报接收质量，教师端动态调整码率 |
| **自动重连（v3.6）** | 客户端断流后自动恢复 |
| **关键帧三机制协同** | 定时关键帧 + 丢包触发重建 + 客户端主动请求 |
| **编码器驱动降级** | NVENC 不可用时自动回退 libx264 |
| **低延时架构** | 无 B 帧、零延迟编码、`LOW_DELAY` 解码、网络线程直接解码入队 |
| **中文界面** | Qt6 Widgets + 全宽字符 API，控制台 UTF-8 |

---

## 系统架构

```
┌─────────────────────────── 教师端 (teacher) ───────────────────────────┐
│                                                                        │
│  DXGI Desktop Duplication                                              │
│         │  脏区矩形 (DirtyRects)                                        │
│         ▼                                                              │
│   差分处理  ──►  FFmpeg 编码 (NVENC / libx264)  ──►  视频包             │
│                                                       │                │
│                                          FEC 编码 ──► 分包 (20B 头)     │
│                                                       │                │
│                                                       ▼                │
│                                              UDP 5004 (组播 / 单播)     │
└───────────────────────────────────────────────────────┬────────────────┘
                                                        │
                                    UDP 5005 控制流 ▲   │ 视频流
                                   (KEYFRAME/HELLO/ │   │
                                    质量上报)        │   ▼
┌─────────────────────────── 客户端 (client) ─────────┴──────────────────┐
│                                                                        │
│   UDP 收包  ──►  分片重组 (边界全量校验)  ──►  FEC 纠错恢复               │
│                        │                                               │
│                        ▼                                               │
│              FFmpeg 解码 (LOW_DELAY, 单线程)                            │
│                        │                                               │
│                        ▼                                               │
│              D3D11 渲染  ──►  窗口上屏                                  │
│                                                                        │
└────────────────────────────────────────────────────────────────────────┘
```

**技术选型对比（节选自 `docs/需求与技术方案.md`）**

| 环节 | 方案 | 结论 |
| --- | --- | --- |
| 屏幕捕获 | GDI `BitBlt` | ❌ 无脏区信息、CPU 拷贝开销大、被遮挡窗口内容错误 |
| 屏幕捕获 | **DXGI Desktop Duplication** | ✅ 硬件加速 + 脏区矩形，**选用** |
| 编码 | FFmpeg | ✅ 硬件优先（NVENC），软件兜底（libx264） |
| 网络 | **UDP** | ✅ 低延时优先，配合 FEC 抗丢包 |
| 渲染 | D3D11 | ✅ 硬解直出，零拷贝上屏 |

---

## 性能目标与实测

| 维度 | 目标 | 实测 |
| --- | --- | --- |
| 并发客户端 | 40（组播） | ✅ 组播压测通过（`stress_test`） |
| 分辨率 | 1080p | 源 2560×1600 实测 |
| 帧率 | 30–60 FPS | 受屏幕内容与编码器影响 |
| 端到端延迟 | < 100 ms | ✅ **16–63 ms** |
| 丢包容忍 | FEC + 关键帧重建 | ✅ 1% 丢包下救回 80% 受损帧 |

---

## 网络与端口约定

| 用途 | 端口 | 方向 |
| --- | --- | --- |
| 视频流（UDP） | **5004** | 教师端 → 组播组 / 单播 IP |
| 控制流（UDP） | **5005** | 客户端 → 教师端（KEYFRAME 请求 8B / HELLO 心跳 5B / 质量上报 ≤11B） |

---

## 目录结构

```
screen_cast_cpp/
├── CMakeLists.txt                # 构建定义（C++17，MSVC /utf-8）
├── cmake/FindFFmpeg.cmake        # FFmpeg 查找模块
├── version.h                     # 版本号单一来源（build.bat / pack.ps1 自动递增）
├── capture/                      # 屏幕捕获（DXGI Desktop Duplication）
│   └── dxgi_capture.{h,cpp}
├── encode/                       # 视频编码（FFmpeg，NVENC → libx264 降级）
│   └── ffmpeg_encoder.{h,cpp}
├── decode/                       # 视频解码（FFmpeg，低延时）
│   └── ffmpeg_decoder.{h,cpp}
├── render/                       # D3D11 渲染
│   ├── d3d11_renderer.{h,cpp}
│   └── vs_bytecode.inc · ps_bytecode.inc
├── packet/                       # 协议层
│   ├── video_packet.h            # 20 字节包头（类 RTP）
│   └── fragment_reassembler.h    # 分片重组（含边界校验）
├── net/                          # 网络层
│   ├── udp_socket.{h,cpp}        # UDP 收发 / 组播加入
│   └── client_receiver.{h,cpp}   # 客户端收流
├── core/                         # 教师端流水线
│   └── streamer.{h,cpp}
├── ui/                           # Qt6 界面
│   ├── teacher_window.*          # 教师端主窗口
│   ├── client_window.*           # 学生端主窗口
│   ├── video_widget.* · preview_widget.*
│   ├── connect_dialog.*          # 连接对话框
│   ├── flow_layout.h · qss_dark.h   # 布局与深色主题
│   ├── preview.*
│   └── legacy/                   # 旧 Win32 界面实现（已归档，不参与编译）
│       └── teacher_gui_win32.cpp · client_gui_win32.cpp
├── msi/                          # 打包资源（图标 / 资源脚本 / WiX 源）
│   ├── teacher.ico · student.ico
│   ├── teacher_gui.rc · client_gui.rc · installer.rc
│   ├── Product.wxs · zh-CN.wxl
│   └── make_icons.py
├── docs/
│   └── 需求与技术方案.md           # 完整需求分析、协议设计、实测数据
├── scripts/version_bump.ps1      # 版本号递增
├── ScreenCast.wxs                # WiX 安装包定义
├── build.bat                     # 一键构建 + 打包（版本递增 → CMake → 编译 → windeployqt → MSI/zip）
├── pack.bat · pack.ps1           # 打包 MSI + 解压版 zip
├── build-debug.bat
└── 入口文件
    ├── teacher_main.cpp · teacher_gui.cpp · teacher_cli.cpp
    ├── client_main.cpp · client_gui.cpp · client_console.cpp
    ├── installer.cpp · setup_pack.cpp
    ├── diag_dirty.cpp · diag_decode.cpp · diag_fec.cpp    # 诊断工具
    └── cursor_probe.cpp · test_btn_draw.cpp
```

---

## 快速开始

### 1. 环境要求

| 依赖 | 版本 | 说明 |
| --- | --- | --- |
| Windows | 10 / 11 x64 | 需 MSVC 工具链 |
| Visual Studio | 2022 | 含「使用 C++ 的桌面开发」工作负载 |
| CMake | ≥ 3.20 | |
| **Qt 6** | 6.8.3 `msvc2022_64` | 仅 GUI 目标需要；默认路径 `D:\Qt\6.8.3\msvc2022_64` |
| FFmpeg | 见下文 | 构建 + 运行都需要 |

### 2. 准备依赖（`third_party/` 未随仓库提供）

由于体积原因，`third_party/` **未提交到仓库**（FFmpeg 约 193 MB，WiX 约 117 MB），
克隆后需自行准备：

```
third_party/
├── ffmpeg/            # 【必需】
│   ├── include/       #   头文件
│   ├── lib/           #   导入库（avcodec / avutil / avformat / swscale / swresample）
│   └── bin/           #   运行时 DLL（会被自动拷贝到 exe 同级目录）
└── wix/               # 【可选】仅打包 MSI 时需要（candle.exe / light.exe）
```

**FFmpeg 版本要求**（本项目使用的构建）：

| 组件 | ABI 主版本 |
| --- | --- |
| libavcodec | **63** |
| libavformat | **63** |
| libavutil | **61** |
| libswscale | **10** |
| libswresample | **7** |

> 本项目开发时使用的是 `N-125978-g95c43d7df7-20260806`（2026-08-06 的 master 共享库构建）。
> 请下载 **win64 shared (GPL/LGPL)** 版本，保持上述 ABI 主版本一致，
> 解压后把 `include/`、`lib/`、`bin/` 放到 `third_party/ffmpeg/` 下即可。

### 3. 构建

**方式 A —— 一键构建（推荐，含版本递增与打包）**

```bat
build.bat
```

流程：版本递增 → CMake 配置 → 编译 Release → `windeployqt` 部署 Qt 运行时 → 打包 MSI + zip。
跳过版本递增：`build.bat --nobump`

**方式 B —— 手动 CMake**

```bash
mkdir build && cd build

cmake .. -G "Visual Studio 17 2022" -A x64 \
  -DQT6_ROOT_DIR="D:/Qt/6.8.3/msvc2022_64" \
  -DFFMPEG_ROOT="D:/screen_cast_cpp/third_party/ffmpeg"

cmake --build . --config Release
```

产物位于 `build/Release/`。

### 4. 运行

```bat
:: 单播 1:1
teacher_gui.exe 192.168.1.100              :: 教师端（本机 IP）
client_gui.exe  192.168.1.100              :: 客户端（教师端 IP）

:: 组播（交换机需开启 IGMP Snooping）
teacher_gui.exe                            :: 教师端勾选「组播模式」
client_gui.exe  192.168.1.100 239.255.1.1  :: 客户端指定组播组

:: 命令行版本
teacher_cli.exe  239.255.1.1 2000 d f      :: 组播，2000ms 关键帧间隔，差分开
client_console.exe 192.168.1.100
```

### 5. 可执行文件清单（9 个）

| 类别 | 可执行文件 |
| --- | --- |
| GUI | `teacher_gui` · `client_gui` |
| 命令行 | `teacher_cli` · `client_console` |
| 旧版 | `teacher` · `client` · `client_main` |
| 诊断 | `diag_dirty`（脏区）· `diag_decode`（解码）· `diag_fec`（纠错） |

---

## 文档

- **`docs/需求与技术方案.md`** —— 完整的需求分析、技术选型对比（GDI/DXGI/D3D11）、
  模块划分与线程模型、分包协议与 FEC 设计、自适应码率协议、实测数据、开发路线回顾。

---

## 说明

- 本项目为**局域网内网投屏**设计，不涉及公网穿透，也不上传任何画面到第三方服务器。
- 组播模式要求交换机**支持并开启 IGMP Snooping**，否则可能退化为广播泛洪。
- 使用组播时请确保网络环境可控；在多网段 / VPN 环境中建议改用单播模式。
- 请仅在**你拥有合法授权的设备与网络**上使用本软件。
