# Testing and Verification — Sunone Aimbot 2

**Status (current):** 2026-07-29

---

## Current Verification Boundary

This project does not have a full unit test suite, CI pipeline, or automated hardware validation. The current verification boundary is:

| Check | Status |
|-------|--------|
| CUDA Release build | Verified: `build_cuda\Release\ai.exe` exists, 1,852,416 bytes |
| CMake backend and generator | Verified: `AIMBOT_USE_CUDA=ON`, `Ninja Multi-Config` |
| DML scripts disabled | Verified: early-exit guards in `build_dml.bat`, `tools/build_dml.ps1`, `tools/setup_opencv_dml.ps1`, `package_dml_portable.ps1` |
| Source static check | Reviewed: authority files, build scripts, config defaults |
| Runtime startup (10 seconds) | Not performed in current session (requires on-device CUDA environment) |
| GUI interaction | Not performed (requires display and running application) |
| Real game / hardware | Not performed (requires target hardware) |
| Full unit tests (S01-S42) | Not performed (test framework not set up) |

---

## Build Verification Checklist

Run these commands from `D:\aim\main` before claiming a build is valid:

```powershell
# 1. Check executable exists and is nonzero
Get-Item -LiteralPath "build_cuda\Release\ai.exe" | Select-Object FullName, Length, LastWriteTime

# 2. Verify backend and generator in CMake cache
Select-String -LiteralPath "build_cuda\CMakeCache.txt" -Pattern "CMAKE_GENERATOR:|AIMBOT_USE_CUDA:"

# 3. Verify runtime DLLs are present
Test-Path "build_cuda\Release\nvinfer_10.dll"
Test-Path "build_cuda\Release\nvonnxparser_10.dll"
Test-Path "build_cuda\Release\opencv_world4130.dll"

# 4. Verify config and models directory
Test-Path "build_cuda\Release\config.ini"
Test-Path "build_cuda\Release\models"
```

Expected results from the last verified build (2026-07-29):
- `ai.exe` size: 1,852,416 bytes
- Generator: `Ninja Multi-Config`
- Backend: `ON`
- All DLLs and directories: present

---

## Source Logic Simulation Plan

The full scenario list is in [SOURCE_LOGIC_SIMULATION_TEST_PLAN.md](SOURCE_LOGIC_SIMULATION_TEST_PLAN.md) (S01-S42).

The simulation approach uses `FakeMouseInput`, `FakeDetectionFrame`, and `FakeClock` to drive logic paths without starting the full application, connecting hardware, or entering a game.

### Priority execution order

1. S01, S04, S06, S08: basic state and hotkey behavior
2. S09, S10, S12, S14, S15: category and detection filtering
3. S18, S19, S21, S22, S23: aiming and movement boundaries
4. S24, S25, S26, S27, S28, S30: trigger and click/release
5. S34, S35, S36, S37: pause and reload
6. S38-S42: exception and error scenarios

These scenarios have not been executed in the current session. They require setting up the fake input harness with a debugger or equivalent instrumentation.

---

## Validation Scenarios (Mandatory, from IMPLEMENTATION_SPEC.md)

These must pass after implementing the corresponding feature:

| Scenario | Behavior | Pass criteria |
|----------|----------|---------------|
| VS-01 | Crosshair outside trigger zone, target exists | Trigger does not fire |
| VS-02 | Crosshair enters trigger zone | `key_delay_ms` starts from this moment |
| VS-03 | `key_delay_ms` active, crosshair leaves zone | Timer resets, returns to Idle |
| VS-04 | `key_delay_ms` expires, crosshair inside | `leftDown()` called |
| VS-05 | Firing, crosshair leaves zone | `leftUp()`, enters Cooldown |
| VS-06 | `auto_aim` active, no hotkey held | Slot 1 logically active, trigger works |
| VS-07 | Slots 1, 2, 3 all held | Only Slot 1 activates |
| VS-08 | Globally disabled category target detected | Not processed by any module |
| VS-09 | `leftUp()` call fails | `mouse_pressed` set to false regardless |
| VS-10 | `.engine` incompatible | Renamed with `_old_<timestamp>`, rebuilt from `.onnx` |
| VS-11 | NC inference produces 0 or >= 20 | User-readable error, startup blocked |
| VS-12 | Capture adapter != CUDA device | Falls back to CPU path, diagnostic output |

---

## Known Limitations

**Source root is `D:\aim\main`.** All build commands and path references use `D:\aim\main` as the project root.

**No full unit test framework.** There is no test runner, no test directory, and no `CMakeLists.txt` test target. The `SOURCE_LOGIC_SIMULATION_TEST_PLAN.md` describes scenarios but requires manual setup of the fake input harness.

**Hardware validation not possible in this session.** GPU capture, mouse I/O, real-game detection, and trigger behavior require on-device testing that was not performed.

**GUI validation not performed.** The overlay UI (ImGui-based) was not loaded or screenshotted. Tab order, layout, and control behavior are documented in [IMPLEMENTATION_SPEC.md](IMPLEMENTATION_SPEC.md) and [UI_LAYOUT_BACKUP.md](UI_LAYOUT_BACKUP.md) but not visually verified.

**DML cannot be validated.** CMakeLists.txt rejects DML. The DML test scenarios in `SOURCE_LOGIC_SIMULATION_TEST_PLAN.md` are historical; do not attempt to execute them.

---

## Benchmark

```powershell
# Run from build_cuda\Release\
.\ai.exe --benchmark-providers
```

Results go to `benchmark_results\`. Requires a real GPU and a model file.

---

## What Constitutes a Successful Build Verification

Minimum evidence before claiming the build is verified:

1. `build_cuda\Release\ai.exe` exists with nonzero size and a build timestamp matching the compile run.
2. `CMakeCache.txt` shows `AIMBOT_USE_CUDA:BOOL=ON` and `CMAKE_GENERATOR:INTERNAL=Ninja Multi-Config`.
3. All expected runtime DLLs and the models directory are present.
4. The executable starts without immediate crash and either loads a model or produces a clear startup error if no model is present (10-second smoke test, not performed in current session).

---

*Testing document | Status: current | Verification boundary as of 2026-07-28*
