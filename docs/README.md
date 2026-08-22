# 咔蚯（原 咔蚯）— 文档索引

本文件是所有项目文档的规范入口。

> **现行编译规范（2026-08-21 更新）**：本仓库已重命名为 `mybot`，唯一编译入口为根目录 `build_current.ps1`（PowerShell 直接运行），产物锁定 `build_cuda/Release/ai.exe`。旧版 `BUILDER.ps1`/`build.md`/`BUILD_AND_RELEASE.md`/`BUILD_RELEASE_GUIDE.md`/`guides/build-*.md` 等文档因引用已废弃脚本与旧路径（`<repo-root>`、`build_cuda_current`），已于本轮清理删除。详细环境注入步骤见 `AGENTS.md` §4 与 `PROJECT_FILE_DISPOSITION.md`。

---

## 文档层级与权威性

| 文档 | 角色 |
|------|------|
| [IMPLEMENTATION_SPEC.md](IMPLEMENTATION_SPEC.md) | **最高权威**：定义全部产品行为（热键、瞄准、扳机、类别）。与其他文档冲突时以此为准。 |
| [../AGENTS.md](../AGENTS.md) | 维护者指南：编译命令、工具链、环境、已知问题。产品语义上服从 IMPLEMENTATION_SPEC。 |
| [CODEX_HANDOFF.md](CODEX_HANDOFF.md) | 新维护者单页入门：架构、编译、测试、状态。 |
| [ARCHITECTURE.md](ARCHITECTURE.md) | 拓扑、后端强制、配置权威、三热键系统、扳机几何、P2 风险。 |
| [TESTING.md](TESTING.md) | 验证边界、模拟计划（S01–S42）、已知限制。 |
| [PUSH_SCOPE.md](PUSH_SCOPE.md) | GitHub 推送范围、排除规则和推送前检查清单。 |
| [PROJECT_STATUS.md](PROJECT_STATUS.md) | 后端状态、P0–P2 问题跟踪、改进计划、清理策略。 |
| [PROJECT_FILE_DISPOSITION.md](PROJECT_FILE_DISPOSITION.md) | 文件留存/归档/删除/暂缓处置清单与证据（历史路径记录）。 |
| [config.md](config.md) | `config.ini` 全字段参考。 |
| [guides.md](guides.md) | 设置、使用、排障指南索引。 |
| [UI_LAYOUT_BACKUP.md](UI_LAYOUT_BACKUP.md) | UI 布局快照（参考备份，非行为权威）。 |
| [SOURCE_LOGIC_SIMULATION_TEST_PLAN.md](SOURCE_LOGIC_SIMULATION_TEST_PLAN.md) | 测试场景 S01–S42，顶部含行为修订注记。 |

---

## 活跃用户与构建文档

- **入门（先看这里）**：[CODEX_HANDOFF.md](CODEX_HANDOFF.md)
- **架构**：[ARCHITECTURE.md](ARCHITECTURE.md)
- **测试与验证**：[TESTING.md](TESTING.md)
- **项目状态与问题**：[PROJECT_STATUS.md](PROJECT_STATUS.md)
- **配置参考**：[config.md](config.md)
- **设置与排障**：[guides.md](guides.md) + [guides/](guides/)
- **编译**：根目录 `build_current.ps1`（见上文注记，勿再用已删旧文档）

---

## 指南目录（guides/）

| 文件 | 主题 |
|------|------|
| [guides/first-launch.md](guides/first-launch.md) | 首次启动清单 |
| [guides/backends.md](guides/backends.md) | 后端选择 |
| [guides/recipes.md](guides/recipes.md) | 常用配置配方 |
| [guides/troubleshooting.md](guides/troubleshooting.md) | 排障 |
| [guides/udp-capture.md](guides/udp-capture.md) | UDP 采集设置 |
| [guides/capture-diagnostics.md](guides/capture-diagnostics.md) | 采集诊断 |
| [guides/circle-fov.md](guides/circle-fov.md) | 圆形 FOV |
| [guides/data-collection.md](guides/data-collection.md) | 数据采集 |
| [guides/input-methods.md](guides/input-methods.md) | 输入方式 |
| [guides/imgui-checkbox-layout-zh.md](guides/imgui-checkbox-layout-zh.md) | ImGui 全宽复选框与等宽列布局（中文） |
| [guides/overlay.md](guides/overlay.md) | Overlay 使用 |

---

## 归档（历史，非权威）

仅保留历史上下文，不描述当前产品。

| 路径 | 内容 |
|------|------|
| [archive/design/multi-hotkey-local-config-plan.md](archive/design/multi-hotkey-local-config-plan.md) | 历史设计：多热键局部配置计划（已过时，当前为 3 热键） |
| [archive/design/multi-hotkey-local-config-design.md](archive/design/multi-hotkey-local-config-design.md) | 历史设计：多热键设计规范（已过时） |
| [archive/diagnostics/source-simulation-findings-2026-07.txt](archive/diagnostics/source-simulation-findings-2026-07.txt) | 历史诊断：源码模拟与问题定位（P0–P2 已修复，见 AGENTS.md §12） |
| [archive/legacy/legacy-defaults.txt](archive/legacy/legacy-defaults.txt) | 历史默认参数（已过时，当前见 AGENTS.md §8 与 config.md） |
| [archive/legacy/dml-backend-history.md](archive/legacy/dml-backend-history.md) | DML 后端历史（已废弃，不可用） |
| [archive/legacy/imgui-tutorial-original.md](archive/legacy/imgui-tutorial-original.md) | ImGui 教程原文（历史） |
| [archive/plans/DOCUMENTATION_CONSOLIDATION_PLAN.md](archive/plans/DOCUMENTATION_CONSOLIDATION_PLAN.md) | 文档整合计划（已执行完成） |
