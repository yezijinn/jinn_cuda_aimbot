# Contributing to 咔蚯

---

## C++ Code Style

This project follows the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html).

Full guide: <https://google.github.io/styleguide/cppguide.html>  
Style guide repository: <https://github.com/google/styleguide>

Key rules that apply to this codebase:

- **Naming**: `snake_case` for variables and functions, `PascalCase` for classes and structs, `kCamelCase` for constants.
- **Headers**: Use `#pragma once`. Keep includes minimal and ordered: own header, C system, C++ standard, third-party, project.
- **Ownership**: Prefer `unique_ptr` / `shared_ptr` over raw owning pointers. Never use `new`/`delete` directly in application code.
- **Error handling**: No exceptions in performance-critical paths. Use return codes or `std::optional`.
- **Comments**: Explain *why*, not *what*. Preserve existing Chinese-language log strings in `mybot.cpp`.
- **Compiler flags**: This project uses MSVC `/utf-8 /W3 /sdl /permissive- /EHsc`. Do not introduce code that breaks under these flags.

---

## Architecture Rules

Before making changes, read [AGENTS.md](AGENTS.md) in full. Key constraints:

- **Backend**: Current CMake supports CUDA/TensorRT only. Do not attempt to restore DML paths.
- **Config authority**: All default values live in `config.cpp` `Config::Config()`. Do not hardcode defaults elsewhere.
- **Hotkeys**: `MAX_MOUSE_HOTKEYS = 3`. Do not add slots 4 or 5.
- **Aiming offset**: `aim_offset_x/y` is normalized `[0,1]` over the bounding box. The same pivot point must be used in all modules — selection, lock, Kalman measurement, UI, trigger.
- **Product behavior contract**: [docs/IMPLEMENTATION_SPEC.md](docs/IMPLEMENTATION_SPEC.md) is the highest-authority document. Any change that would violate it requires updating the spec first and getting explicit approval.

---

## Documentation Updates

When a change affects product behavior, build commands, config fields, or UI layout, update the relevant document **in the same commit**:

| Changed area | Document to update |
|---|---|
| Product behavior / trigger / hotkey logic | [docs/IMPLEMENTATION_SPEC.md](docs/IMPLEMENTATION_SPEC.md) |
| Build commands, toolchain, environment | [AGENTS.md](AGENTS.md) §4–§7 and [docs/BUILD_RELEASE_GUIDE.md](docs/BUILD_RELEASE_GUIDE.md) |
| Config fields or defaults | [docs/config.md](docs/config.md) and [AGENTS.md](AGENTS.md) §8 |
| UI layout | [docs/UI_LAYOUT_BACKUP.md](docs/UI_LAYOUT_BACKUP.md) |
| Test scenarios | [docs/SOURCE_LOGIC_SIMULATION_TEST_PLAN.md](docs/SOURCE_LOGIC_SIMULATION_TEST_PLAN.md) |

Do not move or rename files under `modules/`, `packages/`, `build*/`, or `CUDA.TensorRT/` — these are third-party or generated trees.

---

## Submission

1. Fork the repository.
2. Create a feature branch from `main`.
3. Follow the code style and architecture rules above.
4. Update relevant documentation in the same branch.
5. Open a Pull Request with a concise title (≤70 chars) and a description covering: what changed, why, and what was tested.

For questions, open a GitHub issue.

Maintainer and agent reference: [AGENTS.md](AGENTS.md)
