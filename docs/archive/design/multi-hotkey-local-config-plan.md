# 多热键局部配置 Implementation Plan

> **⚠️ 历史文档，非权威参考。**  
> 本文档记录历史设计计划，已归档至 `docs/archive/design/`。  
> 当前产品已实现三热键架构（`MAX_MOUSE_HOTKEYS = 3`），本文提及的"5个热键"等内容已过时。  
> 当前行为规范见 [docs/IMPLEMENTATION_SPEC.md](../../IMPLEMENTATION_SPEC.md)，构建维护见 [AGENTS.md](../../../AGENTS.md)。

> Status: APPROVED (已过时)
> Source: `.claude/artifacts/designs/multi-hotkey-local-config.md`
> Mode: default
> Iterations: 1 / 3
> Author: user
> Last updated: 2026-07-21

## Requirements summary
将当前单一全局瞄准配置扩展为最多 5 个鼠标热键配置。捕获、模型/后端、数据收集、输入设备等保持全局；触发系统、预测、动态范围、类别优先级、目标锁定和鼠标轨迹等作为热键局部快照。热键支持按住和按一下切换，多热键同时激活时按优先级和创建顺序选择唯一生效配置。

## Acceptance criteria
- AC-1：最多创建 5 个独立热键菜单，第 6 个创建请求被拒绝。
- AC-2：绑定选项只有左键、右键、中键、侧键 1、侧键 2，且绑定唯一。
- AC-3：按住模式按物理状态激活，松开停用；切换模式按下沿翻转一次。
- AC-4：同时激活时选择热键优先级最高者，优先级相同选择创建顺序最早者。
- AC-5：局部配置互相隔离，修改一个热键不改变其他热键。
- AC-6：删除热键同步删除局部配置并持久化。
- AC-7：全局菜单范围不被热键化，旧配置可幂等迁移。
- AC-8：构建并加载无热键、单热键和五热键配置不崩溃。

## RALPLAN-DR
### Principles
- 先建立独立数据模型，再接入 UI 和运行时，避免继续扩大 `Config` 的全局字段耦合。
- 运行时只消费不可变 `ActiveHotkeySnapshot`，不让检测线程直接读取 UI 正在修改的配置。
- 保持现有全局捕获、模型、输入设备和数据收集生命周期不变。
- 旧配置迁移必须幂等，非法热键数据不得阻止全局配置加载。
- 每个实现步骤都必须能通过构建或行为验证。

### Decision drivers
- 线程安全和运行时一致性
- 与现有配置文件的兼容性
- 改动范围和回归风险

### Viable options
**Option A：HotkeyProfile + 局部配置快照（选定）**
- 实现思路：新增 `HotkeyProfile`，将现有局部字段复制进局部配置结构；全局 `Config` 保留全局字段，运行时生成 `ActiveHotkeySnapshot`。
- 改动文件：`config/config.h/.cpp`、`keyboard/keyboard_listener.cpp`、`mouse/AimbotTarget.cpp`、`runtime/trigger_system.*`、`overlay/draw_*.cpp`。
- 优点：边界清晰，多个热键天然隔离，运行时可做原子快照。
- 缺点：需要迁移现有调用点，配置序列化字段数量增加。

**Option B：保留 Config 全局字段，增加每个字段的 `[5]` 数组**
- 实现思路：把现有局部标量全部改成数组，用 `activeHotkeyIndex` 选择数组元素。
- 优点：初期改动较少，配置字段可直接扩展。
- 缺点：类型和调用点污染严重，容易漏改；数组边界和运行时切换更容易产生竞态；无法清晰表达热键实体、创建顺序和删除。
- 否决理由：不满足独立实体和快照边界，长期维护风险高。

## Implementation steps
1. 在 `mybot/config/config.h` 增加鼠标热键枚举、激活模式、局部配置结构、`HotkeyProfile` 和 `ActiveHotkeySnapshot`；在 `Config` 增加最多 5 个热键集合、创建序号和校验/迁移接口。保留全局字段，仅把实际局部字段映射到快照结构。
2. 在 `mybot/config/config.cpp` 初始化默认局部模板；实现配置文件读写、数量上限、五种鼠标键白名单、绑定唯一性、优先级范围、创建顺序校验和旧 `button_targeting` 的幂等迁移。删除操作同步移除整个 profile。
3. 在 `mybot/keyboard/keyboard_listener.cpp` 增加五种鼠标键的统一状态读取和按键沿处理；按住模式直接读取物理状态，切换模式只在上升沿翻转；从激活 profile 中按优先级降序、创建顺序升序选择一个 profile，并发布快照。
4. 在 `mybot/runtime/trigger_system.h/.cpp` 改为接收当前热键快照或局部 TriggerConfig；确保冷却、随机延迟和触发状态不在多个 profile 间共享。
5. 在 `mybot/mouse/AimbotTarget.cpp`、`detector/postProcess.cpp`、相关 Kalman/预测和鼠标轨迹调用点，将局部参数读取改为当前快照；全局捕获范围 XY 和全局 AI/输入设备参数继续从 `Config` 读取。
6. 在 `mybot/overlay/draw_ai.cpp` 及其他 UI 绘制文件中保留全局菜单，新增热键菜单栏、添加/删除/选择/优先级/激活模式控件；删除时弹出确认并同步更新当前菜单和运行时快照；隐藏旧的自瞄键、开枪键、瞄准镜独立入口。
7. 在 `mybot/mybot.cpp` 和配置重载路径接入快照刷新；配置重载、删除热键和退出时清除切换模式的瞬时状态，防止幽灵激活。
8. 在 `mybot` 下新增针对配置校验、优先级选择、激活模式和迁移逻辑的轻量测试或可执行验证入口；不引入新的第三方测试框架。

## Workspace setup
- 实施前运行 `git status --short` 和 `git branch --show-current`。
- 当前项目之前已有未提交源码修改和源码备份，实施时不覆盖或清理这些改动。
- 若需要独立分支，应基于当前完整源码备份恢复到单独工作目录后再开发；不在本次计划中自动创建 worktree。

## Risks & mitigations
| Risk | Mitigation |
|---|---|
| 局部字段漏迁移导致部分参数仍是全局 | 先建立字段归属表，逐个搜索旧字段读取点并编译检查；UI 和运行时均只允许快照读取局部字段。 |
| 配置文件格式变化导致旧用户设置丢失 | 保留旧字段读取，迁移只在无新热键列表时执行，并写入迁移标记，重复加载不重复创建。 |
| 按住/切换状态在重载或删除后残留 | profile id 变化、删除、重载和无生效 profile 时清零所有 toggle 状态。 |
| 物理设备与 Win32 鼠标状态不一致 | 统一使用五种内部按钮枚举，输入设备适配层只负责映射。 |
| UI 修改期间运行时读取半更新状态 | 配置锁下复制局部字段，使用不可变快照发布；运行时不保存 UI 指针。 |
| 同时热键选择结果不确定 | 使用 `(priority desc, creationOrder asc)` 的稳定比较器，并加配置级验证。 |
| 过度修改现有全局逻辑 | 先迁移局部读取点，捕获/模型/输入设备生命周期保持原实现。 |

## Verification steps
- V-1：运行项目构建脚本 `tools\build_dml.ps1 -NonInteractive -OpenCvAlreadyBuilt $true`，要求链接成功生成 `build\dml\Release\ai.exe`。
- V-2：使用无热键配置启动并打开 UI，确认没有热键菜单且全局菜单仍可用。
- V-3：依次创建 1、2、3、4、5 个热键，再尝试第 6 个，确认第 6 个被拒绝。
- V-4：为五个热键分别绑定五种鼠标键，重复绑定同一按键应被拒绝。
- V-5：分别测试按住和切换模式，验证按下/松开和单次按下沿行为。
- V-6：同时激活两个或更多热键，确认只生效优先级最高者；优先级相同确认创建顺序最早者生效。
- V-7：修改一个热键的触发、预测、动态范围、类别优先级、速度和轨迹参数，重新打开其他热键确认值不变。
- V-8：删除一个热键并保存、重启，确认其菜单和局部配置不再出现，其他热键绑定不变。
- V-9：用旧版配置加载，确认旧自瞄键只迁移一次且全局捕获/模型/输入设备保持不变。
- V-10：运行 `git diff --check` 或等效空白检查，并检查启动日志无配置解析错误。

## ADR
- **Decision**：采用 `HotkeyProfile + ActiveHotkeySnapshot`，热键实体存储独立局部配置，运行时按激活状态、优先级和创建顺序发布唯一快照。
- **Drivers**：线程安全、配置兼容性、改动可控性。
- **Alternatives considered**：Option A chosen；Option B rejected because array fields would继续扩大全局耦合且难以支持删除、创建顺序和原子快照。
- **Why chosen**：实体模型能直接表达最多 5 个热键、绑定白名单、激活模式、优先级和删除语义；快照让检测和鼠标线程不会读取半成品配置。
- **Consequences**：需要迁移局部字段读取点和配置序列化；UI 需要从单页面变为热键菜单栏；全局 AI/捕获/输入设备代码基本保持不变。
- **Follow-ups**：暂不新增键盘热键、不支持局部参数合并、不保留删除热键的历史配置。

## Architect challenge
### Steelman against favored option
Option A 的最大风险是 `HotkeyProfile` 如果直接复制全部局部字段，会形成一个过大的结构并要求多个旧模块改变接口。若没有明确的字段归属表，开发过程中仍可能把字段错误地从全局 `Config` 读取，造成 UI 显示与运行时行为不一致。

### Tradeoff tensions
- **边界清晰 vs 改动范围**：独立局部结构更安全，但需要触及检测、目标、触发和鼠标调用点；采用快照适配层，先保持全局对象兼容，再逐步替换局部读取。
- **配置兼容性 vs 删除语义**：旧字段必须保留读取，删除后的新 profile 不能残留；通过新字段优先、旧字段仅迁移一次解决。
- **实时响应 vs 快照一致性**：每帧读取全局配置简单但有竞态；在按键状态变化或 UI 修改时发布快照，运行时只读快照。

## Critic verdict
| 维度 | 状态 | 备注 |
|---|---|---|
| Principle consistency | ✓ | 选定方案遵循实体隔离和快照原则。 |
| Alternative exploration | ✓ | 两个方案均可实现，Option B 有明确否决理由。 |
| Risk mitigation clarity | ✓ | 配置迁移、状态残留、输入映射、竞态均有对应措施。 |
| AC testability | ✓ | 每条验收标准可通过 UI 操作、启动日志或构建验证。 |
| Verification concreteness | ✓ | 使用明确构建命令和 10 条行为验证步骤。 |
| File/line coverage | ✓ | 实施步骤均指向具体源码文件和模块。 |
| Scope consistency | ✓ | 未扩展键盘热键、参数合并或历史配置。 |

### Verdict: APPROVED

### Reservations
- `Config` 当前包含大量局部标量字段；如果快照没有覆盖全部调用点，仍可能出现“菜单显示局部、运行时读取全局”的隐蔽回归。实施第 1、5 步必须维护字段归属清单，并在构建前对旧字段读取点进行搜索。
- 当前项目缺少现成单元测试框架；V-3 至 V-9 需要增加最小可执行测试或手工验证记录，否则只能证明编译通过，不能证明热键选择语义。

## Review trail
- Planner draft v1：选择 `HotkeyProfile + ActiveHotkeySnapshot`，并否决按字段数组扩展方案。
- Architect challenge v1：指出局部字段迁移完整性与实时快照边界是主要风险。
- Critic verdict v1：APPROVED，要求维护字段归属清单并补充可执行行为验证。
- Final iterations：1 / 3
