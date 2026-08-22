# 咔蚯 (Kaiqiu)

Windows-only、C++17、CUDA/TensorRT 后端的实时 YOLO 推理与控制程序。
Real-time YOLO inference, aiming, and input control for Windows, built with C++17 and CUDA/TensorRT.

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)](https://en.cppreference.com/w/cpp/17)
[![CUDA 13.2](https://img.shields.io/badge/CUDA-13.2-76B900?logo=nvidia&logoColor=white)](https://developer.nvidia.com/cuda-13-2-0-download-archive)
[![TensorRT](https://img.shields.io/badge/TensorRT-10.x-76B900)](https://developer.nvidia.com/tensorrt)
[![Windows](https://img.shields.io/badge/Windows-x64-blue)](https://www.microsoft.com/windows)

---

## 中文说明

### 功能概览

- 屏幕采集 → 图像预处理 → YOLO 推理 → 目标过滤 → 目标预测 → 鼠标控制 → UI 配置
- Windows-only 原生工程，源码根目录为 `main`
- NVIDIA CUDA/TensorRT 推理后端
- ImGui overlay 负责 UI、参数调整、运行状态显示
- 输入设备支持原生鼠标、KMBOX、KMBOX-A、KMBOX-Net、MAKCU 等接入方式
- 鼠标控制算法包含 Kalman、Minimum Jerk、PID、拟人化轨迹与目标发布
- 多线程分工：采集、推理、输入控制、UI 各线程独立

### 构建环境

- Windows 10/11
- NVIDIA GPU：GTX 1660 或 RTX 2000/3000/4000/5000 系列
- 最新 NVIDIA 驱动
- CUDA 13.2
- TensorRT
- ONNX Runtime 1.28.0
- OpenCV CUDA
- Visual Studio 2022 + Ninja + CMake

> 构建脚本会注入本机非标准安装环境，属于工程环境专用模板。其他机器需要按实际路径调整。

### 快速开始

1. 安装 NVIDIA 驱动、CUDA 13.2、VS2022、Ninja。
2. 在 `main` 根目录运行 PowerShell：

```powershell
& ".\build_current.ps1"
```

3. 验证产物：

```text
build_cuda\Release\ai.exe
```

4. 将 `.onnx` 模型放入 `models` 目录。
5. 运行 `ai.exe`，首次加载会生成或加载 `.engine` 文件。
6. 在 ImGui overlay 中选择模型并调整参数。

> GTX 10xx 及更早显卡不在支持目标内，实际可用性取决于 TensorRT 和 CUDA 版本。

### 常用控制

| 按键 | 功能 |
|------|------|
| Right Mouse Button | 对目标执行瞄准控制 |
| F2 | 退出 |
| F3 | 暂停瞄准 |
| F4 | 重载配置 |
| Home | 打开/关闭 overlay |

### 目录结构

| 目录 | 职责 |
|------|------|
| `capture/` | 屏幕采集 |
| `config/` | 配置加载、保存与参数模型 |
| `detector/` | YOLO 推理、后端封装、后处理 |
| `keyboard/` | 键盘监听与键位映射 |
| `mouse/` | 鼠标输入、设备连接、平滑控制算法 |
| `overlay/` | ImGui 绘制、UI 状态与配置展示 |
| `runtime/` | 主循环、线程调度、触发系统 |
| `tensorrt/` | TensorRT 封装与推理监控 |
| `mem/` | CPU/GPU 资源管理 |

### 文档

| 文档 | 用途 |
|------|------|
| [docs/README.md](docs/README.md) | 文档入口 |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 架构与模块关系 |
| [docs/IMPLEMENTATION_SPEC.md](docs/IMPLEMENTATION_SPEC.md) | 行为契约 |
| [docs/TESTING.md](docs/TESTING.md) | 验证与测试说明 |
| [docs/PUSH_SCOPE.md](docs/PUSH_SCOPE.md) | GitHub 推送范围 |
| [AGENTS.md](AGENTS.md) | 维护者和代理约束 |
| [CONTRIBUTING.md](CONTRIBUTING.md) | 贡献指南 |

### 发布约定

- 软件名：咔蚯
- 版本号：与 Git tag 保持一致
- Tag 强制格式：`vYYYYMMDD`，例如 `v20260822`
- GitHub 仓库根目录：`main` 源码目录
- GitHub 不提交：外部研究文档、参考项目、CUDA/TensorRT 依赖树、ONNX Runtime、构建产物、模型和运行日志

---

## English

### Overview

- Screen capture → image preprocessing → YOLO inference → target filtering → target prediction → mouse control → UI configuration
- Windows-only native project, source root: `main`
- NVIDIA CUDA/TensorRT inference backend
- ImGui overlay for UI, parameter adjustment, and runtime status
- Supports native mouse, KMBOX, KMBOX-A, KMBOX-Net, MAKCU input paths
- Mouse control algorithms include Kalman, Minimum Jerk, PID, human-like movement, and target publishing
- Multi-threaded pipeline: capture, inference, input control, and UI run separately

### Build Requirements

- Windows 10/11
- NVIDIA GPU: GTX 1660 or RTX 2000/3000/4000/5000 series
- Latest NVIDIA driver
- CUDA 13.2
- TensorRT
- ONNX Runtime 1.28.0
- OpenCV CUDA
- Visual Studio 2022 + Ninja + CMake

> The current build script injects a non-standard local VS/CUDA/Ninja environment, so keep it as an environment-specific template.

### Quick Start

1. Install the NVIDIA driver, CUDA 13.2, VS2022, and Ninja.
2. Run PowerShell from the `main` root:

```powershell
& ".\build_current.ps1"
```

3. Expected output:

```text
build_cuda\Release\ai.exe
```

4. Put an `.onnx` model into the `models` directory.
5. Run `ai.exe`; first launch exports or loads a `.engine` file.
6. Select the model and adjust settings in the ImGui overlay.

> Pascal and older GPUs, including most GTX 10xx cards, are not supported targets.

### Controls

| Key | Action |
|-----|--------|
| Right Mouse Button | Aim at detected target |
| F2 | Exit |
| F3 | Pause aiming |
| F4 | Reload config |
| Home | Toggle overlay |

### Project Layout

| Directory | Purpose |
|-----------|---------|
| `capture/` | Screen capture |
| `config/` | Config load/save and parameter model |
| `detector/` | YOLO inference, backend wrapper, post-processing |
| `keyboard/` | Keyboard listen and key mapping |
| `mouse/` | Mouse input, device connection, smoothing algorithms |
| `overlay/` | ImGui drawing, UI state, and config display |
| `runtime/` | Main loop, thread scheduling, trigger system |
| `tensorrt/` | TensorRT wrapper and inference monitor |
| `mem/` | CPU/GPU resource management |

### Documentation

| Document | Purpose |
|----------|---------|
| [docs/README.md](docs/README.md) | Documentation index |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Architecture and module relations |
| [docs/IMPLEMENTATION_SPEC.md](docs/IMPLEMENTATION_SPEC.md) | Behavior contract |
| [docs/TESTING.md](docs/TESTING.md) | Testing and validation |
| [docs/PUSH_SCOPE.md](docs/PUSH_SCOPE.md) | GitHub push scope |
| [AGENTS.md](AGENTS.md) | Maintainer and agent constraints |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Contribution guidelines |

### Release Conventions

- Product name: Kaiqiu / 咔蚯
- Version: matches the Git tag
- Tag format: `vYYYYMMDD`, e.g. `v20260822`
- GitHub repository root: `main`
- Git excludes external research docs, reference projects, SDK trees, ONNX Runtime, build artifacts, models, and logs

## References

- [TensorRT Documentation](https://docs.nvidia.com/deeplearning/tensorrt/)
- [OpenCV Documentation](https://docs.opencv.org/4.x/d1/dfb/intro.html)
- [ImGui](https://github.com/ocornut/imgui)
- [CppWinRT](https://github.com/microsoft/cppwinrt)
- [KMBOX](https://www.kmbox.top/)
- [MAKCU](https://makcu.com)
