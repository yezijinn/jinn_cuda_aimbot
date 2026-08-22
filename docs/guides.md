# Usage and Troubleshooting Guides

This file is the central index for practical setup and diagnosis. Keep detailed instructions in separate files under `docs/guides/` so this index stays short.

## Start Here

| Guide | Use when |
|---|---|
| [First launch checklist](guides/first-launch.md) | You are starting a fresh build or fresh unpacked release. |
| [Backend selection and checks](guides/backends.md) | You need to confirm CUDA/TensorRT is active or diagnose basic backend behavior. |
| [Common recipes](guides/recipes.md) | You want ready-to-copy config snippets and benchmark commands. |
| [Troubleshooting order](guides/troubleshooting.md) | You need a short checklist for unknown problems. |

## Capture and Performance

| Guide | Use when |
|---|---|
| [UDP capture over LAN](guides/udp-capture.md) | You want to receive MJPEG frames over UDP from another PC or process. |
| [Capture diagnostics](guides/capture-diagnostics.md) | You are reading `[CaptureDiag]` output or checking CPU/GPU capture paths. |
| [Circle FOV](guides/circle-fov.md) | You are configuring the circular FOV limiter and overlay preview. |
| [Data collection](guides/data-collection.md) | You are collecting training data or checking its performance impact. |

## Controls and UI

| Guide | Use when |
|---|---|
| [Input methods](guides/input-methods.md) | You are setting up Win32, KMBOX, MAKCU, or other control paths. |
| [ImGui 全宽复选框与等宽列布局（中文）](guides/imgui-checkbox-layout-zh.md) | 需要在自定义全宽复选框中实现等宽多列布局。 |
| [Overlay and GUI behavior](guides/overlay.md) | You are tuning preview, game overlay, or latency compensation. |

## Development

> 编译方法：本工程唯一编译入口为根目录 `build_current.ps1`（PowerShell 直接运行），产物锁定 `build_cuda/Release/ai.exe`。详细环境注入步骤参见仓库 `AGENTS.md` §4。

Core references:

- [Configuration guide](config.md)
