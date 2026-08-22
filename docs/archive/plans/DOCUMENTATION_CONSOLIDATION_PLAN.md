# 咔蚯 文档整合计划

> **【历史规划文档，已执行完成】**  
> 本文档为 2026-07-26 生成的文档整合执行计划，已于 2026-07-26 完成执行，归档至 `docs/archive/plans/`。  
> 当前文档结构以 [docs/README.md](../../README.md) 为准。

> 生成时间：2026-07-26  
> 项目根目录：`<historical-local-root>\mybot-main`
> 状态：**已执行完成**

---

## 一、文档分类汇总表

### 1.1 当前权威文档（保持不变）

| 文件路径 | 用途 | 关联关系 | 操作 |
|---------|------|---------|------|
| `AGENTS.md` | 维护者指南，构建命令、工具链、已知问题 | 引用 IMPLEMENTATION_SPEC.md、BUILD_RELEASE_GUIDE.md、SOURCE_LOGIC_SIMULATION_TEST_PLAN.md、UI_LAYOUT_BACKUP.md | **保持** |
| `README.md` | 项目入口，面向最终用户 | 引用 docs/build.md、docs/config.md、docs/guides.md | **保持** |
| `docs/IMPLEMENTATION_SPEC.md` | 产品行为权威契约 | 被 AGENTS.md 引用为优先级最高规范 | **保持** |
| `docs/build.md` | 从源码构建指南（英文） | 被 README.md 引用 | **保持** |
| `docs/config.md` | 配置文件参考（英文） | 被 README.md、guides.md 引用 | **保持** |
| `docs/guides.md` | 使用指南索引（英文） | 引用 guides/ 子目录全部指南 | **保持** |
| `docs/guides/*.md` | 实用诊断和使用指南（9 个文件） | 相互引用，构成完整诊断网络 | **保持** |

**子目录指南文件清单（9个）**：
- `first-launch.md`、`backends.md`、`recipes.md`、`troubleshooting.md`
- `udp-capture.md`、`capture-diagnostics.md`、`circle-fov.md`、`data-collection.md`
- `input-methods.md`、`overlay.md`、`build-workflow.md`

---

### 1.2 需要更新的文档（与当前状态不符）

| 文件路径 | 问题 | 与权威文档冲突点 | 推荐操作 |
|---------|------|-----------------|---------|
| `docs/BUILD_RELEASE_GUIDE.md` | 记录 DML + CUDA 双后端构建流程 | AGENTS.md §4、IMPLEMENTATION_SPEC.md §1.1 明确当前仅支持 CUDA/TensorRT；DML 构建命令已失效 | **更新**：添加顶部警告标记 DML 部分为历史参考，强调当前唯一有效路径为 CUDA/TensorRT（`build_cuda_current`） |
| `编译指南.md`（根目录） | 中文编译指南，提及 DML 和 CUDA 双后端 | 与 BUILD_RELEASE_GUIDE.md 冲突相同 | **更新**：添加顶部警告，说明当前 CMakeLists.txt 仅支持 CUDA，DML 路径不可执行 |
| `docs/SOURCE_LOGIC_SIMULATION_TEST_PLAN.md` | 测试场景中关于 key_delay 和扳机区域的假设 | IMPLEMENTATION_SPEC.md §5 和 AGENTS.md §12 已明确修正：key_delay 从准心进入区域计时，扳机判断为准心落入目标内部区域 | **更新**：在文档顶部添加"已修复问题"章节，说明 S24-S29 扳机场景和 key_delay 场景的前提已按 IMPLEMENTATION_SPEC.md 修正 |
| `docs/UI_LAYOUT_BACKUP.md` | 当前 UI 布局备份，记录三热键页面 | 内容正确，但 AGENTS.md §11 提到"热键4/5已删除"，需确认文档是否已同步 | **检查并更新**：确认文档已反映 `MAX_MOUSE_HOTKEYS = 3` 的事实，若有热键4/5残留描述需删除 |

---

### 1.3 历史参考文档（需归档或标注）

| 文件路径 | 内容 | 为何归档 | 推荐操作 |
|---------|------|---------|---------|
| `.claude/artifacts/plans/multi-hotkey-local-config.md` | 多热键局部配置实现计划 | 历史设计文档，提及"最多5个热键"，但当前已改为3个 | **移动至** `docs/archive/design/` 并添加"已过时"标记 |
| `.claude/artifacts/designs/multi-hotkey-local-config.md` | 多热键设计规范 | 历史设计文档，功能已实现并调整 | **移动至** `docs/archive/design/` 并添加"已过时"标记 |
| `docs/源码模拟和问题定位.txt` | 源码模拟检查结果，记录已确认的 P0-P2 问题 | 内容被 AGENTS.md §12 引用，但 AGENTS.md 已标注"已修复"，此文件记录历史状态 | **重命名为** `docs/archive/源码模拟和问题定位_2026-07.txt`，标注为历史诊断记录 |
| `穿越火线调参指南.md`（根目录） | 特定游戏调参指南 | 用户参考资料，非开发文档 | **移动至** `docs/game-guides/crossfire-tuning.md` |
| `别人的成品项目界面UI/Sunset_AI调参指南.md` | 外部项目参考 | 外部参考资料，非本项目文档 | **移动至** `docs/references/sunset-ai-tuning.md` |
| `别人的成品项目界面UI/Sunset_AI_UI参考分析.md` | 外部 UI 参考分析 | 外部参考资料 | **移动至** `docs/references/sunset-ai-ui-analysis.md` |
| `UI参考文档_中英文对照.md`（根目录） | 外部项目 UI 对照表 | 外部参考资料 | **移动至** `docs/references/ui-translation-reference.md` |
| `<external-ui-reference>/UI参考文档_中英文对照.md` | 另一外部项目 UI 参考 | 外部参考资料 | **移动至** `docs/references/genshin-ui-reference.md` |

---

### 1.4 临时或过时文件（建议删除或归档）

| 文件路径 | 内容 | 问题 | 推荐操作 |
|---------|------|------|---------|
| `一键编译的命令是.txt` | 单行命令：`cmake --build build --config Release --target ai` | 不完整（缺少 VsDevCmd 环境初始化），容易误导；AGENTS.md §4 已给出正确命令 | **删除**（已被 AGENTS.md 完整覆盖） |
| `纯默认配置.txt` | 旧版默认参数清单 | 部分参数名称和默认值已过时（如"头部Y偏移"已废弃）；AGENTS.md §8 和 config.md 为权威来源 | **删除或归档至** `docs/archive/legacy-defaults.txt` |
| `Google Style Guides.txt` | 两行 URL 链接 | 无实质内容，可整合到贡献指南 | **删除**（URL 可移入 CONTRIBUTING.md 或 AGENTS.md） |

---

### 1.5 第三方/生成文档（排除，不纳入整合范围）

已排除以下目录（供记录）：

- `mybot/modules/_downloads/opencv-4.13.0-extract/`：OpenCV 源码文档
- `mybot/modules/opencv/build/`：OpenCV 构建生成的许可证和 README
- `packages/Microsoft.ML.OnnxRuntime.DirectML.*/`：NuGet 包文档
- `packages/Microsoft.AI.DirectML.*/`：DirectML SDK 文档
- `build*/`：CMake 生成的构建树文件
- `mybot/modules/serial/`：嵌入式 serial 库文档（保持原位，不移动）

---

## 二、文档内链更新需求

### 2.1 需要更新的内部链接

| 源文件 | 目标文件（旧路径） | 目标文件（新路径） | 操作 |
|--------|------------------|------------------|------|
| AGENTS.md | `docs/源码模拟和问题定位.txt` | `docs/archive/源码模拟和问题定位_2026-07.txt` | 更新链接 |
| （待创建）CONTRIBUTING.md | —— | 引用 Google Style Guides 链接 | 新建贡献指南，整合 `Google Style Guides.txt` 内容 |

### 2.2 外部参考资料引用检查

以下外部 URL 在项目文档中被引用（已验证，无需修改）：

- README.md：GitHub、CUDA 下载、文档站点
- docs/build.md：无外部链接
- docs/config.md：无外部链接
- docs/guides/：各指南之间相互引用正确

---

## 三、推荐目录结构（整合后）

```
mybot-main/
├── AGENTS.md                      # 维护者指南（保持）
├── README.md                      # 项目入口（保持）
├── CONTRIBUTING.md                # 贡献指南（新建，整合 Google Style Guides.txt）
├── 编译指南.md                    # 中文编译指南（更新：添加 DML 废弃警告）
├── docs/
│   ├── IMPLEMENTATION_SPEC.md     # 权威产品规范（保持）
│   ├── BUILD_RELEASE_GUIDE.md     # 构建发布指南（更新：添加 DML 历史标记）
│   ├── SOURCE_LOGIC_SIMULATION_TEST_PLAN.md  # 测试计划（更新：添加已修复问题说明）
│   ├── UI_LAYOUT_BACKUP.md        # UI 布局备份（检查更新）
│   ├── build.md                   # 英文构建指南（保持）
│   ├── config.md                  # 配置参考（保持）
│   ├── guides.md                  # 指南索引（保持）
│   ├── guides/                    # 实用指南（11个文件，保持）
│   ├── game-guides/               # 游戏专用调参指南（新建目录）
│   │   └── crossfire-tuning.md    # 穿越火线调参（移入）
│   ├── references/                # 外部参考资料（新建目录）
│   │   ├── sunset-ai-tuning.md    # Sunset AI 调参参考（移入）
│   │   ├── sunset-ai-ui-analysis.md  # Sunset AI UI 分析（移入）
│   │   ├── ui-translation-reference.md  # UI 翻译对照表（移入）
│   │   └── genshin-ui-reference.md  # 原神 UI 参考（移入）
│   └── archive/                   # 历史文档归档（新建目录）
│       ├── design/                # 历史设计文档
│       │   ├── multi-hotkey-local-config-plan.md  # 多热键计划（移入）
│       │   └── multi-hotkey-local-config-design.md  # 多热键设计（移入）
│       ├── 源码模拟和问题定位_2026-07.txt  # 历史诊断记录（移入并重命名）
│       └── legacy-defaults.txt    # 旧版默认配置（移入或删除）
├── mybot/
│   └── modules/
│       └── serial/
│           ├── README.md          # serial 库文档（保持原位）
│           └── changes.txt        # serial 库变更日志（保持原位）
└── （已排除）
    ├── 一键编译的命令是.txt        # 删除
    ├── Google Style Guides.txt     # 删除（整合至 CONTRIBUTING.md）
    ├── 别人的成品项目界面UI/       # 原目录可删除（内容已移入 docs/references/）
    └── <external-ui-reference>/  # 原目录可删除（内容已移入 docs/references/）
```

---

## 四、执行步骤（严格按序）

### 阶段 1：创建新目录结构（不移动文件）

```powershell
New-Item -ItemType Directory -Path "docs\game-guides" -Force
New-Item -ItemType Directory -Path "docs\references" -Force
New-Item -ItemType Directory -Path "docs\archive\design" -Force
```

### 阶段 2：更新现有文档（添加警告/说明）

#### 2.1 更新 `docs/BUILD_RELEASE_GUIDE.md`

在文件顶部第 1 行后插入：

```markdown
> ⚠️ **历史版本偏移警告**：本文档记录了历史上 DML 和 CUDA 双后端的构建流程。**当前 CMakeLists.txt 仅支持 CUDA/TensorRT 后端**（见 [AGENTS.md](../AGENTS.md) §4 和 [IMPLEMENTATION_SPEC.md](IMPLEMENTATION_SPEC.md) §1.1）。DML 相关构建命令、DML 发行包生成流程和 `build_dml_alt` 目录描述保留在此作为历史参考，**不代表当前可复现的构建路径**。当前唯一有效的构建目录为 `build_cuda_current`。
```

#### 2.2 更新 `编译指南.md`

在文件顶部第 1 行后插入：

```markdown
> ⚠️ **重要更新**：本指南编写时项目支持 CUDA 和 DML 双后端。**当前源码仅支持 CUDA/TensorRT 后端**，DML 构建路径已在 CMakeLists.txt 中移除（见 [AGENTS.md](AGENTS.md) §4）。下文中提及的"自动检测 CUDA 后切换到 CUDA/TensorRT"行为不再适用，`build_dml.bat` 在当前源码下无法生成有效的 DML 构建。请参考 [docs/build.md](docs/build.md) 或 [AGENTS.md](AGENTS.md) §4 了解当前有效的 CUDA 构建命令。
```

#### 2.3 更新 `docs/SOURCE_LOGIC_SIMULATION_TEST_PLAN.md`

在第 1 节"文档目的"后插入新章节：

```markdown
## 1.5 已修复问题说明

本测试方案编写时记录了部分待确认行为。以下问题已按 [IMPLEMENTATION_SPEC.md](IMPLEMENTATION_SPEC.md) 明确并修复（见 [AGENTS.md](../AGENTS.md) §12）：

- **自动扳机区域判断**（S24-S29 相关场景）：已修正为"检测帧中心（准心）落入目标框内部触发区"，不再使用目标中心或屏幕固定区域判断。
- **key_delay_ms 计时起点**：已修正为从准心首次进入目标内部区域时开始计时，而非从热键按下时计时。
- **leftUp() 失败时的状态同步**：已修正为无条件清除软件按下状态，避免 I/O 失败导致的幽灵按下。

下文场景描述保持原始设计意图，实际验证时应以修复后行为为准。
```

#### 2.4 检查并更新 `docs/UI_LAYOUT_BACKUP.md`

检查文档是否有"热键4"或"热键5"描述，若有则删除并确认仅包含"热键1、热键2、热键3"。

### 阶段 3：移动文件到新位置

```powershell
# 移动游戏调参指南
Move-Item -LiteralPath "穿越火线调参指南.md" -Destination "docs\game-guides\crossfire-tuning.md"

# 移动外部参考资料
Move-Item -LiteralPath "别人的成品项目界面UI\Sunset_AI调参指南.md" -Destination "docs\references\sunset-ai-tuning.md"
Move-Item -LiteralPath "别人的成品项目界面UI\Sunset_AI_UI参考分析.md" -Destination "docs\references\sunset-ai-ui-analysis.md"
Move-Item -LiteralPath "UI参考文档_中英文对照.md" -Destination "docs\references\ui-translation-reference.md"
Move-Item -LiteralPath "<external-ui-reference>\UI参考文档_中英文对照.md" -Destination "docs\references\genshin-ui-reference.md"

# 移动历史设计文档
Move-Item -LiteralPath ".claude\artifacts\plans\multi-hotkey-local-config.md" -Destination "docs\archive\design\multi-hotkey-local-config-plan.md"
Move-Item -LiteralPath ".claude\artifacts\designs\multi-hotkey-local-config.md" -Destination "docs\archive\design\multi-hotkey-local-config-design.md"

# 移动并重命名历史诊断记录
Move-Item -LiteralPath "docs\源码模拟和问题定位.txt" -Destination "docs\archive\源码模拟和问题定位_2026-07.txt"

# 可选：归档旧默认配置
Move-Item -LiteralPath "纯默认配置.txt" -Destination "docs\archive\legacy-defaults.txt"
```

### 阶段 4：更新内部链接

#### 4.1 更新 AGENTS.md 中的链接

将 `[docs/源码模拟和问题定位.txt](docs/源码模拟和问题定位.txt)` 改为：  
`[docs/archive/源码模拟和问题定位_2026-07.txt](docs/archive/源码模拟和问题定位_2026-07.txt)`

### 阶段 5：创建新文档（可选但推荐）

#### 5.1 创建 `CONTRIBUTING.md`

```markdown
# 贡献指南

感谢你对 咔蚯 项目的关注！

## 代码风格

本项目遵循 [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)。

完整指南参见：https://github.com/google/styleguide

## 提交流程

1. Fork 本仓库
2. 创建功能分支
3. 遵循代码风格编写代码
4. 提交 Pull Request

## 维护者参考

维护者和 Agent 请参阅 [AGENTS.md](AGENTS.md) 了解构建、工具链和已知问题。
```

### 阶段 6：删除冗余文件

```powershell
# 删除单行命令文件（已被 AGENTS.md 覆盖）
Remove-Item -LiteralPath "一键编译的命令是.txt"

# 删除简单 URL 文件（已整合至 CONTRIBUTING.md）
Remove-Item -LiteralPath "Google Style Guides.txt"

# 可选：删除空目录（确认内容已移出后）
Remove-Item -LiteralPath "别人的成品项目界面UI" -Recurse -Force
Remove-Item -LiteralPath "<external-ui-reference>" -Recurse -Force
Remove-Item -LiteralPath ".claude\artifacts" -Recurse -Force
```

---

## 五、验证检查清单

执行完成后，逐项检查：

- [ ] `AGENTS.md` 中的所有链接指向正确路径
- [ ] `README.md` 中的链接未受影响
- [ ] `docs/guides.md` 及其子目录的相互引用仍然有效
- [ ] `docs/BUILD_RELEASE_GUIDE.md` 顶部包含 DML 废弃警告
- [ ] `编译指南.md` 顶部包含 DML 废弃警告
- [ ] `docs/SOURCE_LOGIC_SIMULATION_TEST_PLAN.md` 包含"已修复问题说明"章节
- [ ] `docs/UI_LAYOUT_BACKUP.md` 仅描述三个热键，无热键4/5残留
- [ ] 新目录 `docs/game-guides/`、`docs/references/`、`docs/archive/design/` 已创建
- [ ] 所有移动的文件已到达目标位置
- [ ] 冗余文件已删除
- [ ] `CONTRIBUTING.md` 已创建

---

## 六、风险与注意事项

### 6.1 关键风险

1. **DML 构建路径的用户依赖**：若有用户仍依赖旧版 DML 构建文档，移动后需确保他们能找到归档位置。建议在 README.md 中添加"历史版本说明"链接。

2. **内部链接失效**：移动文件后，任何未在本计划中列出的内部引用都可能失效。执行前建议全局搜索被移动文件的原路径。

### 6.2 回滚方案

若执行后发现严重问题，使用 Git 回滚：

```powershell
git checkout HEAD -- .
```

或手动撤销文件移动（反向执行阶段 3 的 Move-Item 命令）。

---

## 七、文档冲突优先级明确

根据 AGENTS.md §规范优先级 和 IMPLEMENTATION_SPEC.md §权威性声明：

**优先级顺序（高到低）**：

1. **IMPLEMENTATION_SPEC.md**（产品行为和实现的目标契约）
2. **AGENTS.md**（构建、工具链和维护约定）
3. **docs/BUILD_RELEASE_GUIDE.md**、**编译指南.md**（构建流程，历史部分已过时）
4. **docs/SOURCE_LOGIC_SIMULATION_TEST_PLAN.md**（测试计划，部分前提已修正）
5. 其他文档（参考资料、历史归档）

任何文档内容与上述优先级高的文档冲突时，以高优先级文档为准。

---

## 八、后续维护建议

1. **定期检查**：每次重大架构变更后，重新审查文档一致性。
2. **版本标记**：历史文档在移入 `docs/archive/` 后，在文件名或文件顶部标注日期。
3. **文档审查流程**：Pull Request 中涉及功能变更时，强制要求同步更新相关文档。
4. **链接验证工具**：考虑集成 Markdown 链接检查工具（如 `markdown-link-check`）到 CI 流程。

---

**状态：计划已完成，等待执行确认。**

**执行前必读**：本计划仅为分析和建议，实际执行前请务必备份项目或提交当前 Git 状态。
