# First Launch Checklist

Use this checklist for a fresh unpacked build or a fresh local build.

The current build is CUDA/TensorRT only.

1. Put the detector model in the `models` folder beside `ai.exe`:
   - Use a TensorRT `.engine` model, or an `.onnx` if you do not have a pre-built engine.
   - When an `.onnx` is selected and no matching `.engine` exists, the backend builds the engine on first load.
2. Start the app once so `config.ini` is generated.
3. Open the GUI or overlay and save settings from there when possible.
4. Confirm the selected `input_method` matches the device/control path you actually want.

Related docs:

- [Backend selection](backends.md)
- [Configuration guide](../config.md)
- [Build（现行）](../README.md#现行编译规范2026-08-21-更新)：根目录 `build_current.ps1`，产物 `build_cuda/Release/ai.exe`
