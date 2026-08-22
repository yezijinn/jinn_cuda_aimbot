<div align="center">

# 咔蚯 (C++)

[![C++](https://img.shields.io/badge/C%2B%2B-17-blue)](https://github.com/SunOner/mybot)
[![GitHub stars](https://img.shields.io/github/stars/SunOner/mybot?color=ffb500)](https://github.com/SunOner/mybot)
[![CUDA 13.2](https://img.shields.io/badge/CUDA-13.2-76B900?logo=nvidia&logoColor=white)](https://developer.nvidia.com/cuda-downloads)
[![Discord server](https://badgen.net/discord/online-members/37WVp6sNEh)](https://discord.gg/37WVp6sNEh)

  <p>
    <a href="https://github.com/SunOner/mybot/releases" target="_blank">
      <img width="75%" src="https://github.com/SunOner/sunone_aimbot/blob/main/media/one.gif">
    </a>
  </p>
</div>

---

## CUDA + TensorRT Build (NVIDIA Only)

This project runs on **NVIDIA GPUs with CUDA/TensorRT**. The current codebase has one supported backend; DML is not available in this build.

**GPU requirements:** GTX 1660, RTX 2000/3000/4000/5000 series
**Not supported:** Pascal (GTX 10xx) and older (TensorRT limitation)
**Software:** Latest NVIDIA driver + [CUDA 13.2](https://developer.nvidia.com/cuda-13-2-0-download-archive)

Before running, update your NVIDIA graphics driver to the latest version and install CUDA 13.2.

Pre-compiled builds are available on the [Discord server](https://discord.gg/37WVp6sNEh) in the **pre-releases** channel. Just download, unpack, and run `ai.exe`.

---

## How to Run

1. Update your NVIDIA graphics driver to the latest version.
2. Install [CUDA 13.2](https://developer.nvidia.com/cuda-13-2-0-download-archive) if not already present.
3. Run `ai.exe` from the output directory. On first launch the model exports to a `.engine` file, which takes a few minutes.
4. Place your `.onnx` model in the `models` folder and select it in the overlay.
5. All settings are in the overlay panel (open with the **Home** key).

### Build From Source on Windows

Use the project's PowerShell build entry point from the repository root. The current script is machine-specific and must be adapted if the local VS/CUDA/ONNX Runtime paths change.

```powershell
& ".\build_current.ps1"
```

The verified output is `build_cuda\Release\ai.exe`.

### Controls

| Key | Action |
|-----|--------|
| Right Mouse Button | Aim at detected target |
| F2 | Exit |
| F3 | Pause aiming |
| F4 | Reload config |
| Home | Open/close overlay |

---

## Documentation

Full documentation index: **[docs/README.md](docs/README.md)**

| Document | Purpose |
|----------|---------|
| [docs/README.md](docs/README.md) | Documentation index |
| [docs/CODEX_HANDOFF.md](docs/CODEX_HANDOFF.md) | New maintainer / agent handoff |
| [docs/IMPLEMENTATION_SPEC.md](docs/IMPLEMENTATION_SPEC.md) | Product behavior contract |
| [docs/TESTING.md](docs/TESTING.md) | Validation and simulation tests |
| [docs/PUSH_SCOPE.md](docs/PUSH_SCOPE.md) | GitHub push scope and exclusion rules |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Contribution guidelines |

Maintainer and agent reference: [AGENTS.md](AGENTS.md)

---

## References

* [TensorRT Documentation](https://docs.nvidia.com/deeplearning/tensorrt/)
* [OpenCV Documentation](https://docs.opencv.org/4.x/d1/dfb/intro.html)
* [ImGui](https://github.com/ocornut/imgui)
* [CppWinRT](https://github.com/microsoft/cppwinrt)
* [GLFW](https://www.glfw.org/)
* [WindMouse](https://ben.land/post/2021/04/25/windmouse-human-mouse-movement/)
* [KMBOX](https://www.kmbox.top/)
* [MAKCU](https://makcu.com)
* [depth-anything-tensorrt](https://github.com/spacewalk01/depth-anything-tensorrt)

---

## Licenses

**OpenCV:** [Apache License 2.0](https://opencv.org/license.html)
**ImGui:** [MIT License](https://github.com/ocornut/imgui/blob/master/LICENSE)

---

## Support the Project

This project is developed with support from [Boosty](https://boosty.to/sunone) and [Patreon](https://www.patreon.com/c/sunone) backers, who also get access to improved AI models.

**Need help?** Join the [Discord server](https://discord.gg/37WVp6sNEh) or open a GitHub issue.

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=SunOner/mybot&type=date&legend=top-left)](https://www.star-history.com/#SunOner/mybot&type=date&legend=top-left)
