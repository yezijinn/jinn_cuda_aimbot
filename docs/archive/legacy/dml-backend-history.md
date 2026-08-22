> ⚠️ **历史存档，非权威内容。**
> 本文件描述已废弃的 DirectML（DML）后端历史背景，仅供参考。
> 当前产品仅支持 CUDA/TensorRT 后端，本文件中的所有命令、路径和配置均**不可复现**，不得参照执行。
> 权威构建文档：[docs/BUILD_RELEASE_GUIDE.md](../../BUILD_RELEASE_GUIDE.md)、[docs/guides/build-zh.md](../../guides/build-zh.md)。

---

# DML 后端历史记录（已失效）

DML（DirectML）后端曾作为 Sunone Aimbot 2 的早期 Windows GPU 推理路径，基于 ONNX Runtime + DirectML，
支持非 NVIDIA GPU，以 `.onnx` 模型格式运行。

主要历史构建工件：

- 构建目录：`build_dml_alt`（已无法重新配置为 DML 后端）
- 历史发行包：`DML_Portable_Release.zip`
- 历史打包脚本：`package_dml_portable.ps1`
- 历史依赖：`packages/Microsoft.ML.OnnxRuntime.DirectML.*`、`packages/Microsoft.AI.DirectML.*`、DML OpenCV

DML 后端于当前版本从源码中完整移除。`CMakeLists.txt` 在 `AIMBOT_USE_CUDA=OFF` 时以 `FATAL_ERROR` 中止，
`USE_CUDA` 宏无条件定义。历史 `build_dml_alt/` 目录无法再重新配置为 DML 后端。
