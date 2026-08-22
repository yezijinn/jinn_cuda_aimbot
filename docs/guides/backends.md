# Backend Selection and Checks

The current build is CUDA/TensorRT only. There is no DML option in the current
source. `AIMBOT_USE_CUDA=OFF` aborts the CMake build with a fatal error.

## Requirements

- Supported NVIDIA GPU. GTX 10xx/Pascal and older are not supported.
- CUDA Toolkit 13.1 or newer.
- TensorRT 10 Windows binary SDK.
- A TensorRT `.engine` model, or an `.onnx` for first-time engine generation.

## Basic Config

```ini
backend = TRT
ai_model = your_model.engine
capture_use_cuda = true
```

If you only have an `.onnx`, set `ai_model` to the `.onnx` path. The backend
builds and saves a matching `.engine` on first load, then updates `config.ini`
to use it.

## CUDA Runs But GPU Usage Spikes

Start by checking which features force CPU-readable frames:

- Debug/preview window: `show_window = true`.
- Data collection.
- Screenshots.
- Any feature that needs pixels on the CPU for display or saving.

For the current CUDA path, the recommended FOV limiter is:

```ini
circle_fov_enabled = true
```

If the spike disappears when the GUI or overlay is open, compare the capture
diagnostics in both states. The GUI/preview can change whether the app requests
CPU copies, which makes the runtime path different from the closed-GUI path.

Related docs:

- [Capture diagnostics](capture-diagnostics.md)
- [Circle FOV](circle-fov.md)
- [Common recipes](recipes.md)
