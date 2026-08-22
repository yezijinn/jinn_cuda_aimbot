# Project File Disposition Inventory

> **现行状态注记（2026-08-21 清理后）**：本仓库已重命名为 `mybot`，现行编译入口为根目录 `build_current.ps1`，产物锁定 `build_cuda/Release/ai.exe`（旧路径 `D:\aim\main`、`build_cuda_current` 及 `BUILDER.ps1`/`build.md` 等文档均已废弃清理）。下表行内路径前缀（`D:\aim\main`、`mybot-main`）为历史审计记录，保留原样仅供参考，不代表当前实际路径。

Status: Updated 2026-07-29 to reflect project flattening to `D:\aim\main`. Original inventory recorded 2026-07-28 when source was under `mybot-main`; path prefixes in the table rows below reflect the pre-flatten inventory and are preserved as historical record. The active source root is now `D:\aim\main`.

## Authority

`D:\aim\main` is the source and build root. Product behavior is authoritative in `docs/IMPLEMENTATION_SPEC.md`; maintenance, build, and packaging status is described by `AGENTS.md`, `CMakeLists.txt`, the configuration sources, and the current `build_cuda` cache. The current backend is CUDA/TensorRT only. DML content is historical and must not be executed or treated as a current build path.

## Decision Vocabulary

- `retain`: keep in its current path; it is source, authority, dependency, active output, backup, or otherwise operationally significant.
- `archive`: historical material that may be moved only in a later approved task, with links and provenance preserved.
- `delete`: disposable and reproducible; deletion is not performed by Todo 1.
- `defer`: evidence is incomplete, the item is user-owned/uncertain, or a later task must first quarantine/update references.

## Authority and Product Source

| Path | Current status | Reference evidence | Reproducibility / risk | Action | Rollback |
|---|---|---|---|---|---|
| `mybot-main/` | Source and build root; 9,741 files observed | `AGENTS.md` §§1,4; `docs/README.md`; CMake source root | Unique product source; deleting or moving breaks the project | retain | No rollback operation planned; preserve path and contents |
| `mybot-main/.git/` | Not present at inspected path | Path existence check returned missing; workspace root is not a Git repository | Provenance is unavailable at this workspace level; absence is not permission to reconstruct/delete | defer | Preserve all current files; obtain repository metadata before any history operation |
| `mybot-main/AGENTS.md` | Maintenance/build authority | Explicitly identifies source root, CUDA-only backend, current output, and historical DML | Unique authority; changes could mislead future workers | retain | Restore from recorded pre-change hash/backup if later edited |
| `mybot-main/docs/IMPLEMENTATION_SPEC.md` | Product behavior authority | File header and §§1, 13 identify authoritative contract and DML prohibition | Unique contract; not reproducible from generated output | retain | Restore from backup or a separately recorded hash before edits |
| `mybot-main/docs/README.md` | Canonical documentation index | Lines 1-19 define hierarchy and current/historical links | Current navigation; links require later documentation task | retain | Restore prior file from backup/hash before edits |
| `mybot-main/CMakeLists.txt` | Active build definition | Lines 24-26 reject CUDA OFF; 39-72 locate TensorRT; 422 defines `USE_CUDA`; 468-489 copy/remove runtime DLLs | Rebuild-critical and unique | retain | Restore from source backup/hash before edits |
| `mybot-main/mybot/config/config.h` | Configuration schema declaration | Fields and `loadConfig/saveConfig` declarations; `USE_CUDA` fields | Unique schema authority; not safe to delete or infer from INI | retain | Restore from source backup/hash |
| `mybot-main/mybot/config/config.cpp` | Runtime defaults and INI implementation | `Config::loadConfig` initializes defaults; `config.ini` is runtime state | Unique defaults and behavior; deleting changes product behavior | retain | Restore from source backup/hash |
| `mybot-main/mybot/` | Application source tree | CMake source list and AGENTS topology | Unique product implementation | retain | Preserve source tree; use source backup only for rollback |

## Build, Dependency, and Generated Paths

| Path | Current status | Reference evidence | Reproducibility / risk | Action | Rollback |
|---|---|---|---|---|---|
| `build_cuda/` (formerly `mybot-main/build_cuda_current/`) | Current configured CUDA build tree | AGENTS §4; cache records `AIMBOT_USE_CUDA=ON`, `CMAKE_GENERATOR=Ninja Multi-Config`, local TensorRT/OpenCV roots | Rebuildable in principle but expensive and machine-dependent; current verified output depends on it | retain | Preserve tree; rebuild with the recorded cache/generator and dependency paths |
| `build_cuda/Release/ai.exe` | Existing Release executable, 1,852,416 bytes, nonzero | AGENTS §4 and §13; path existence and size check passed | Current verification artifact; rebuild requires CUDA/TensorRT/OpenCV/VS environment | retain | Rebuild from `CMakeLists.txt` using the recorded cache/generator |
| `build_cuda/Release/DirectML.dll` | Generated/copy artifact, 18,527,776 bytes | CMake broad DLL loop at lines 473-483; CUDA packager excludes DML-only names | Exact duplicate of `CUDA.TensorRT/DirectML.dll`; CMake can copy it again, but generated build outputs are referenced state | delete | Before deletion record hash and Release manifest; rollback by rerunning the existing CUDA build/post-build copy |
| `build_cuda/Release/*` | Current runtime DLLs, libraries, CMake output, and model/config state | CMake runtime-copy loop; package script reads this directory; build cache and generated scripts reference it | Mixed active/generated output; deleting directory risks losing verified executable and runtime state | retain | Rebuild only after preserving cache and dependency resolution; do not recursive-delete as a unit |
| `mybot-main/CUDA.TensorRT/` | Local CUDA/TensorRT SDK/runtime bundle | CMake lines 39-72 and 473-484; cache selects this TensorRT root | Rebuild-critical dependency and vendor/runtime material | retain | Restore from original dependency archive or backup; preserve notices/licenses |
| `mybot-main/CUDA.TensorRT/DirectML.dll` | Historical/extra runtime file in SDK bundle; 18,527,776 bytes | Current CMake glob sees all DLLs; DML is prohibited by implementation spec but this path is a dependency bundle | Exact duplicate of generated Release copy, but provenance/SDK bundle ownership is uncertain | defer | Preserve original hash; do not remove until package/SDK ownership and future CMake behavior are reviewed |
| `mybot-main/CUDA.TensorRT/onnxruntime.dll` | Historical DML runtime DLL in dependency bundle | DML package/build scripts and CMake removal list; current CMake does not link it | Not used by current CUDA target, but vendor/runtime ownership is uncertain | defer | Preserve hash and restore from the SDK/dependency source if later quarantined |
| `mybot-main/CUDA.TensorRT/onnxruntime_providers_shared.dll` | Historical DML runtime DLL in dependency bundle | Same as above; CMake explicitly removes matching runtime files beside `ai.exe` | Not current CUDA input; uncertain bundle provenance | defer | Preserve hash; restore from original dependency bundle |
| `mybot-main/mybot/modules/_downloads/` | Downloaded OpenCV source/cache, including long third-party paths | `tools/build_cuda.ps1` resolves/builds OpenCV from this dependency area | Rebuild inputs and third-party source; deletion can force downloads and may lose local reproducibility | retain | Re-run dependency setup/build or restore downloaded archive/source |
| `mybot-main/packages/` | Present package/dependency directory with historical DML use | `tools/build_dml.ps1` and `build_common.ps1` inspect DirectML NuGet packages | Historical but potentially required for forensic reproducibility; not proven empty/duplicate | defer | Preserve package manifest and original files |
| `mybot-main/tools/.bin/ninja.exe` | Local build tool, 569,856 bytes | `tools/build_cuda.ps1` calls `Ensure-Ninja`; hash recorded | Rebuild helper; source archive exists but binary/tool provenance matters | retain | Restore from `ninja-win.zip` or approved tool source |
| `mybot-main/tools/.bin/ninja-win.zip` | Local tool archive, 275,425 bytes | `Ensure-Ninja`/tool cache path; hash recorded | Rebuild fallback; not proven duplicate of executable | retain | Extract from this archive |
| `mybot-main/tools/.bin/7zr.exe` | Local archive helper | Build/common tooling area | Tool input; no duplicate/rebuild proof | retain | Restore from approved 7-Zip distribution |
| `mybot-main/tools/.bin/nuget.exe` | Local package helper | DML/common tooling references | Historical but usable package helper; provenance uncertain | defer | Restore from approved NuGet distribution |
| `mybot-main/compile_commands.json` and CMake generated files | Generated build metadata | Present under `build_cuda_current`; CMake references source/build paths | Regenerable but useful for audit and current build provenance | defer | Regenerate with the same CMake generator/cache |

## Scripts and Entry Points

| Path | Current status | Reference evidence | Reproducibility / risk | Action | Rollback |
|---|---|---|---|---|---|
| `mybot-main/BUILDER.ps1` | Current CUDA wrapper | `ValidateSet("CUDA", "")`; forwards to `tools/build_cuda.ps1` | Active entry point; deleting breaks documented build command | retain | Restore from source backup/hash |
| `mybot-main/tools/build_cuda.ps1` | Current CUDA build implementation | Default `BuildDir=build_cuda_current`; configures and builds CMake | Active build path; retain | retain | Restore from source backup/hash |
| `mybot-main/tools/build_common.ps1` | Shared build/dependency helper | Used by CUDA and DML builders; resolves dependency roots | Active shared helper and historical dependency authority | retain | Restore from source backup/hash |
| `mybot-main/build_cuda.bat` | CUDA wrapper | Existing root entry point; no DML references found in scan | Active/possibly compatibility wrapper; retain pending Todo 2 audit | retain | Restore from source backup/hash |
| `mybot-main/build_release.bat` | Legacy-looking release wrapper | Copies `config.ini`; references build/release output behavior | Operational reference not reconciled yet | defer | Preserve; later quarantine/update only after reference audit |
| `mybot-main/build_no-options.ps1` and `.bat` | Wrapper with legacy DML path hit | PowerShell scan found `build\\dml` | Referenced/active-looking stale entry point; cannot delete before quarantine | defer | Preserve; later guard or update, then retain rollback copy |
| `mybot-main/build_dml.bat` | DML entry point | Calls `tools/build_dml.ps1` and defaults to `build\\dml` | Explicit historical path; still executable-looking, so deletion requires later quarantine and reference update | defer | Preserve script and record original hash before any later change |
| `mybot-main/tools/build_dml.ps1` | DML builder | Uses `AIMBOT_USE_CUDA=OFF`, DML OpenCV/NuGet/DirectML inputs | Current CMake rejects CUDA OFF; historical but still operational-looking | archive | Later move only with links and a first-line historical warning; rollback by restoring original path/hash |
| `mybot-main/tools/setup_opencv_dml.ps1` | DML dependency setup helper | Found by path scan and DML build references | Historical dependency setup; not current CUDA path | archive | Restore original path/hash if needed for historical reproduction |
| `D:\TRAE_Project\Jinn_aimbot\package_cuda_portable.ps1` | Current CUDA packager | Source/build defaults point to `build_cuda_current`; excludes DML-only runtime names | Active packaging entry point; retain | Restore from root hash recorded in evidence |
| `D:\TRAE_Project\Jinn_aimbot\package_dml_portable.ps1` | DML packager | Defaults to missing `build_dml_alt`; requires DML DLLs | Historical and currently non-reproducible; keep until Todo 2/4 labels and references are complete | archive | Restore original root path/hash |
| `D:\TRAE_Project\Jinn_aimbot\_ocr_script.py`, `final_translate_v2.py`, `pt_to_onnx_check_shape_metadata.py` | Root utility scripts | `pt_to_onnx_check_shape_metadata.py` has a generated `.pyc`; project docs reference class-count utility | Relevant tools; utility ownership and consumers not fully audited | retain | Restore from root hashes recorded in evidence |
| `D:\TRAE_Project\Jinn_aimbot\__pycache__\pt_to_onnx_check_shape_metadata.cpython-311.pyc` | Generated Python bytecode; source exists | Source `pt_to_onnx_check_shape_metadata.py` exists (7,691 bytes); no source reference to `.pyc` | Reproducible by importing/running source; no product/build input found | delete | Recreate with `python -m py_compile pt_to_onnx_check_shape_metadata.py`; pre-delete hash is recorded in evidence |

## Root Docs, Archives, Backups, and State

| Path | Current status | Reference evidence | Reproducibility / risk | Action | Rollback |
|---|---|---|---|---|---|
| `D:\TRAE_Project\Jinn_aimbot\docs\archive\` (build-records, conversations, design, source-notes, translation subdirs) | **ARCHIVED** — historical docs moved to purpose-labeled subdirs with provenance warnings | SHA-256 manifest: `.omo/evidence/codex-handoff-cleanup/task-4-codex-handoff-cleanup.txt` | Historical provenance preserved; first-line warnings prepended to all moved files | archived | Restore from pre-move hashes in evidence file |
| `D:\TRAE_Project\Jinn_aimbot\docs\archive\translation\translation_table_complete.md` (moved from repo root) | **ARCHIVED** — historical translation reference; no current build authority | SHA-256: F509FF0E992EC00C4875173E5808CEBF5E5F69E3EA89470FBE1A838CD2ACFB98 | Historical reference only; provenance warning prepended | archived | Restore to original root path using recorded hash |
| `D:\TRAE_Project\Jinn_aimbot\docs\Kmbox_Net_开发调用文档.md` | Device/reference documentation | Root docs inventory; device-specific content | Unique hardware reference; uncertain current use | defer | Preserve original file and hash |
| `D:\TRAE_Project\Jinn_aimbot\backups\` | User-owned backup area; 9,246 files observed | Backup manifest and SHA256SUMS; targeted snapshot explicitly records purpose and source root | User-owned rollback material; never delete based on age/size | retain | Backup manifests and recorded SHA-256 files provide restoration |
| `backups\ui_refactor_prechange_targeted_20260727_133144\` | Targeted pre-refactor snapshot | `BACKUP_MANIFEST.txt` and `SHA256SUMS.txt` list 17 files and purpose | Explicit rollback snapshot; unique even when source duplicates exist | retain | Restore each listed path using the manifest hashes |
| `D:\TRAE_Project\Jinn_aimbot\.codegraph\` | Codegraph database, lock, SHM/WAL; DB 633,507,840 bytes | Directory inventory found `codegraph.db`, `-shm`, `-wal`, lock, and `.gitignore` | Active generated/index state; SQLite WAL/SHM cannot be judged independently or safely deleted | defer | Preserve DB plus WAL/SHM/lock together; regenerate only with the owning codegraph tool |
| `D:\TRAE_Project\Jinn_aimbot\.omo\` | Agent plans, ledgers, continuation state, and evidence workspace | Exact plan is under `.omo/plans`; continuation files and ledger exist | Active handoff/audit state; deleting loses traceability | retain | Preserve session/ledger files; restore from workspace backup if altered |
| `D:\TRAE_Project\Jinn_aimbot\.uploads\` | Empty upload directory | Directory inventory reports zero files | Empty but ownership/purpose is not proven disposable | defer | No-op; preserve directory until owner confirms |
| `D:\TRAE_Project\Jinn_aimbot\CUDA_Portable_Release*` | No matching directory or ZIP found | Path existence checks returned missing | Absence is not proof that future package output may be deleted | defer | None required for missing paths; verify before any future cleanup |
| `D:\TRAE_Project\Jinn_aimbot\DML_Portable_Release*` | No matching directory or ZIP found | Path existence checks returned missing | Historical package path remains referenced by script/docs; do not infer cleanup | defer | None required for missing paths; preserve scripts until reconciled |

## Delete Candidate Gate

Only two items currently meet the evidence threshold for a proposed `delete` action, and neither is deleted by Todo 1:

1. `mybot-main/build_cuda_current/Release/DirectML.dll`: exact SHA-256 duplicate of `CUDA.TensorRT/DirectML.dll`; current CUDA package explicitly excludes it; CMake post-build behavior can reproduce the generated copy. The later cleanup task must recheck the hash and generated manifest immediately before removal.
2. `__pycache__/pt_to_onnx_check_shape_metadata.cpython-311.pyc`: generated bytecode with the source present; Python can regenerate it. The later cleanup task must recheck source/hash and use a non-destructive compile/rebuild check.

All other candidate paths are `retain`, `archive`, or `defer`. In particular, DML scripts/assets, backups, `.codegraph`, the current build tree, SDK/runtime files, and missing package paths are not delete-approved.

## Todo 1 Boundary

- No product source, CMake file, configuration file, build output, package, backup, archive, or generated state was moved or deleted.
- The evidence receipt is `.omo/evidence/codex-handoff-cleanup/task-1-codex-handoff-cleanup.txt`.
- Todo 2 must reconcile active-looking wrappers and DML labels before any archive/delete operation.
- Todo 4 must establish archive locations and historical warnings before moving historical documents.
- Todo 5 may act only on rows marked `delete`, with fresh hashes, manifests, and rollback receipts.

## Todo 4 Boundary

- All `archive`-classified rows have been moved to purpose-labeled subdirectories under `docs/archive/` with historical/non-authoritative provenance warnings prepended.
- Deferred item `docs/Kmbox_Net_开发调用文档.md` remains in place (classified `defer`; unique hardware reference).
- No product source, CMake file, configuration file, SDK, backup, or uncertain DML asset was touched. The sole approved generated cleanup target, `build_cuda_current/Release/DirectML.dll`, was removed in Todo 5 after duplicate-hash verification.
- Changes to `mybot-main/docs` restricted to link/status label updates in this file only.
- Evidence receipt with post-move SHA-256 manifest: `.omo/evidence/codex-handoff-cleanup/task-4-codex-handoff-cleanup.txt`.
