# Architecture — 咔蚯

**Status (current):** 2026-07-28  
**Authority:** [IMPLEMENTATION_SPEC.md](IMPLEMENTATION_SPEC.md) overrides this document on all product-behavior questions.

---

## Project Topology

| Item | Detail |
|------|--------|
| Language | C++17, Windows-only |
| Build target | Executable `ai` |
| Entry point | `mybot.cpp` (project root) |
| Compiler | MSVC, flags: `/utf-8 /W3 /sdl /permissive- /EHsc` |
| Runtime | Static MSVC runtime for both `ai` and `serial_embedded` |
| Build system | CMake, generator: Ninja Multi-Config |
| Build tree | `build_cuda` |

### Directory Map

| Path | Purpose |
|------|---------|
| `scr/`, `detector/`, `mouse/`, etc. | Application source (project root `<repo-root>` is the source root) |
| `CMakeLists.txt` | Backend selection, source list, post-build DLL copy/removal |
| `CUDA.TensorRT/` | Local CUDA/TensorRT SDK and runtime DLLs |
| `modules/` | OpenCV and third-party modules |
| `build_cuda/` | Active configured CUDA build tree |
| `build_cuda/Release/ai.exe` | Current release executable |
| `tools/` | Build, dependency, and packaging scripts |
| `packages/` | Unused NuGet dependency directory (not used by current CMake) |

---

## Backend Enforcement (Current: CUDA/TensorRT Only)

`CMakeLists.txt` lines 24-26 abort with `FATAL_ERROR` when `AIMBOT_USE_CUDA=OFF`. Line 422 unconditionally defines `USE_CUDA`. There is no DML code path in the current build.

DML scripts, build trees (`build_dml_alt/`), and DML packaging scripts are not supported by the current source. See [PROJECT_STATUS.md](PROJECT_STATUS.md) for DML status.

---

## Runtime Architecture

The application captures the screen, runs YOLO detection via TensorRT, applies category and confidence filters, and drives mouse movement via a configurable input backend.

### Core processing pipeline

```
Screen capture -> CUDA frame buffer
-> TensorRT inference (ai_model, e.g. Jinn.engine)
-> Post-processing (class filter, confidence threshold, NMS)
-> Target selection (category two-layer filter)
-> Aiming (Kalman prediction, aim_offset, movement speed)
-> Mouse output (WIN32, KmboxNet, KmboxA, Makcu)
```

### Thread structure

The application uses separate threads for capture, detection, aiming, and the overlay UI. Thread-safe immutable configuration snapshots are a planned improvement, not a verified current implementation guarantee; consult `PROJECT_STATUS.md` before relying on that property.

---

## Configuration Authority

| Source | Role |
|--------|------|
| `config/config.h` | Field declarations and constants (schema authority) |
| `config/config.cpp` `Config::Config()` | All default values |
| `config.ini` (runtime, beside ai.exe) | User-saved overrides; runtime state only, not a defaults source |

`config.ini` is generated at runtime. Deleting it causes the program to write a fresh file from the compiled defaults.

### Key defaults (from config.cpp, verified 2026-07-28)

| Parameter | Default | Range |
|-----------|---------|-------|
| `backend` | `TRT` | current only |
| `ai_model` | `Jinn.engine` | filename |
| `confidence_threshold` | 0.45 | 0.1-0.9 |
| `nms_threshold` | 0.50 | 0.1-0.9 |
| `max_detections` | 8 | 1-20 |
| `aim_offset_x` / `aim_offset_y` | 0.5 / 0.5 | 0.0-1.0 |
| `fovX` / `fovY` | 85 / 55 | |
| `capture_use_cuda` | false | |

The field `virtual_camera_heigth` is an intentionally retained spelling. It must be read and written with the same spelling. Renaming it requires updating both the INI read and write paths.

---

## Three-Hotkey System

The program has exactly 3 hotkey slots (`MAX_MOUSE_HOTKEYS = 3`). Activation is hold-to-activate only; no toggle mode.

**Priority rule (deterministic, no exceptions):** Slot 1 > Slot 2 > Slot 3. When multiple hotkeys are held simultaneously, the lowest-numbered active slot wins.

Each hotkey slot carries its own local configuration: confidence threshold, NMS threshold, aim offset, movement speed, prediction, recoil, dynamic range, category subset, and trigger parameters.

When `auto_aim` is enabled, physical hotkey state is ignored. Slot 1 is the baseline active profile.

---

## Category Two-Layer Filter

| Layer | Config field | Effect |
|-------|-------------|--------|
| Global enable | `class_enabled[i]` | Hard program-level block; disabled categories participate in nothing |
| Per-hotkey subset | `hotkey.localClassEnabled[i]` | Must be a subset of the global enable set |

When global category settings change, all three hotkey subsets reset to match the new global set. Each hotkey retains at least one enabled category.

---

## Trigger Geometry and State Machine

**Geometry:** The detection frame center (crosshair position) is the origin. The trigger zone is the rectangle defined by `trigger_rect` inside the target bounding box, relative to `aim_offset_x/y`. Firing triggers when the crosshair falls inside that inner rectangle.

**State machine:**

```
Idle -> KeyDelay -> PreFire -> Firing -> Cooldown -> Idle
```

- `Idle -> KeyDelay`: crosshair first enters trigger zone
- `key_delay_ms` resets to Idle if crosshair leaves zone before timeout
- `KeyDelay -> Firing`: delay expires, crosshair still inside
- `Firing -> Cooldown`: duration expires or crosshair leaves zone; leftUp() called
- Reset to Idle on: hotkey change, auto_aim change, globalTriggerEnabled off, config reload, device disconnect

See [IMPLEMENTATION_SPEC.md](IMPLEMENTATION_SPEC.md) §5 for the normative contract.

---

## Aim Offset Normalization

`aim_offset_x` / `aim_offset_y` use normalized bounding-box coordinates: `(0,0)` = top-left, `(1,1)` = bottom-right, `(0.5, 0.5)` = center. The target point is:

```
targetX = box.x + box.width * aim_offset_x
targetY = box.y + box.height * aim_offset_y
```

This target point is used for target selection, locking, Kalman prediction measurement, UI drawing, and trigger zone anchor.

---

## Model Lifecycle

**Class count:** Inferred automatically from the TensorRT output tensor shape. NC must be 1-19. If inference produces NC outside that range, the program must show a user-readable error and block startup.

**Engine compatibility failure:** When a `.engine` file fails to load, it is renamed with `_old_<YYYYMMDD_HHMMSS>` before any rebuild attempt. A `.onnx` file with the same base name is used to rebuild the engine. Failing that, startup is blocked with a clear error.

---

## GPU Capture Path (Currently Disabled)

The D3D11/CUDA zero-copy capture path is currently disabled due to multi-adapter mismatch risk. Re-enabling it requires runtime verification that the capture adapter and CUDA device are the same physical GPU. See [IMPLEMENTATION_SPEC.md](IMPLEMENTATION_SPEC.md) §8 for the full acceptance criteria.

---

## Known P2 Risks

See [PROJECT_STATUS.md](PROJECT_STATUS.md) for the full issue list. Summary:

1. Non-WIN32 input backends (`move()`, `leftDown()`, `leftUp()`) check `isOpen()` only; send failures still return true. The trigger system may misinterpret a failed `leftDown()` as successful.
2. Trigger state machine sync on target loss: if `autoShoot` changes to false mid-cycle, `resetAll()` depends on a specific code path. Needs supplemental testing with target loss + hotkey release + device disconnect scenarios.

These require on-device testing with real hardware to characterize. No hardware validation has been performed in the current session.

---

## Serial Library

The `serial` library is embedded as a static library `serial_embedded`. Chinese log strings near the serial include in `mybot.cpp` must be preserved when editing that file.

---

## Verification Limitations

- No full unit test framework.
- No CI pipeline.
- Hardware/GUI/game validation not performed.

See [TESTING.md](TESTING.md) for the full verification boundary.

---

*Architecture document | Status: current | 2026-07-28*
