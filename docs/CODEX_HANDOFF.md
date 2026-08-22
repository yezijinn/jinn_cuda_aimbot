# Codex Handoff — 咔蚯

> **当前状态**：编译入口为根目录 `build_current.ps1`，产物 `build_cuda/Release/ai.exe`。

**Status:** Current as of 2026-07-29  
**Source root:** `<repo-root>`（GitHub 仓库根目录）

This document is the single entry point for understanding, building, and maintaining this project.

---

## What You Need to Know First

**Product behavior authority:** [IMPLEMENTATION_SPEC.md](IMPLEMENTATION_SPEC.md)  
**Maintenance and build:** [AGENTS.md](../AGENTS.md)  
**Documentation index:** [README.md](README.md)

This is a Windows-only C++17 CUDA/TensorRT aim-assist application. It captures the screen, runs YOLO object detection on a GPU, and moves the mouse to assist aiming. The current backend is CUDA/TensorRT only; DirectML (DML) is not supported by the current source.

---

## Architecture and Design

See [ARCHITECTURE.md](ARCHITECTURE.md) for:

- Project topology and directory structure
- CUDA/TensorRT backend enforcement
- Configuration authority (config.h/config.cpp vs config.ini)
- Three-hotkey system and priority rules
- Category filtering (global enable + per-hotkey subset)
- Trigger geometry and state machine
- Known P2 risks (non-WIN32 I/O failure handling, trigger state sync)

---

## Build and Release

> 编译入口为根目录 `build_current.ps1`（PowerShell 直接运行），产物 `build_cuda/Release/ai.exe`。详见 [README.md](README.md)。

- 前提：Visual Studio（MSVC）、CUDA 13.2、TensorRT、Ninja
- 当前构建目录：`build_cuda`
- CMake 生成器锁定：Ninja Multi-Config
- 发布可执行文件位置与验证：`build_cuda/Release/ai.exe`

---

## Testing and Verification

See [TESTING.md](TESTING.md) for:

- Current verification boundary (build, startup, no full hardware/CI)
- Source logic simulation plan (42 scenarios, S01-S42)
- Known limitations (no Git root, no GUI/game/device validation)
- Manual QA checklist (build cache, executable size, 10-second startup)

---

## Project Status and Known Issues

See [PROJECT_STATUS.md](PROJECT_STATUS.md) for:

- Backend status: CUDA/TensorRT only, DML unsupported
- P0-P2 issue tracker with source locations and fix status
- Issue-fix tracking and verification boundary

---

## Quick Start for a New Maintainer

1. Read [IMPLEMENTATION_SPEC.md](IMPLEMENTATION_SPEC.md) to understand product behavior contracts.
2. Read [AGENTS.md](../AGENTS.md) sections 1-6 for build commands and toolchain.
3. Verify the current build:
   ```powershell
   Get-Item build_cuda\Release\ai.exe
   Select-String -LiteralPath build_cuda\CMakeCache.txt -Pattern "CMAKE_GENERATOR:|AIMBOT_USE_CUDA:"
   ```
4. For incremental changes, use the fast rebuild command via `build_current.ps1`.
5. 打包：按现行分发流程处理 `build_cuda/Release/` 已验证内容。

---

## Where Things Live

| What | Where |
|------|-------|
| Product behavior contract | [IMPLEMENTATION_SPEC.md](IMPLEMENTATION_SPEC.md) |
| Maintenance guide | [../AGENTS.md](../AGENTS.md) |
| Source code | `../scr/`, `../detector/`, `../mouse/`, etc. (project root is the source root) |
| Build configuration | `../CMakeLists.txt` |
| Config schema and defaults | `../config/config.h` and `config.cpp` |
| Current build tree | `../build_cuda/` |
| Release executable | `../build_cuda/Release/ai.exe` |
| CUDA/TensorRT SDK | `../CUDA.TensorRT/` |
| Build scripts | `../tools/` |
| Documentation index | [README.md](README.md) |

---

## Current Limitations

- No full unit test framework or CI.
- Hardware validation (GPU capture, mouse I/O, real game) requires manual on-device testing.
- DML backend is not supported; current CMakeLists.txt rejects CUDA OFF with FATAL_ERROR.

---

**Last verified:** 2026-07-28  
**Executable hash (SHA-256):** `8E2E23875C4858F206A0E0591C05CB19390210EFCF76170058F2B14AB863CCDC`  
**Build directory:** `build_cuda`  
**CMake generator:** Ninja Multi-Config  
**Backend:** CUDA/TensorRT (AIMBOT_USE_CUDA=ON)
