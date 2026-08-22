# AGENTS.md 维护指南

> 本文档面向后续 Agent 和维护者，记录项目关键约定、构建规则、架构决策和已知问题。  
> 内容依据本仓库内的实际文档和源码提炼；所有路径、工具链版本和环境信息均描述**本地验证机器**，换机时须重新确认。  
> 文档索引：[docs/README.md](docs/README.md)。构建和维护参考：[docs/README.md](docs/README.md)、[docs/CODEX_HANDOFF.md](docs/CODEX_HANDOFF.md)、[docs/TESTING.md](docs/TESTING.md)。GitHub 推送内容边界见 [docs/PUSH_SCOPE.md](docs/PUSH_SCOPE.md)。产品行为以 [docs/IMPLEMENTATION_SPEC.md](docs/IMPLEMENTATION_SPEC.md) 为准。  
> UI 布局参考：[docs/UI_LAYOUT_BACKUP.md](docs/UI_LAYOUT_BACKUP.md)
> 面向用户的回复必须使用中文。

---

## 规范优先级

> **[docs/IMPLEMENTATION_SPEC.md](docs/IMPLEMENTATION_SPEC.md) 是产品行为和实现的权威目标契约。**
>
> 凡本文档（AGENTS.md）与 `docs/IMPLEMENTATION_SPEC.md` 存在冲突时，以 `docs/IMPLEMENTATION_SPEC.md` 为准。
>
> 本文档（AGENTS.md）聚焦于构建命令、工具链配置、环境路径和维护约定，不重复定义产品行为。

---

## 1. 项目拓扑

- **语言**：C++17，Windows-only，可执行目标 `ai`，入口 `mybot.cpp`（位于项目根 `<repo-root>`）。
- **后端**：当前 `CMakeLists.txt`（lines 24–26）在 `AIMBOT_USE_CUDA=OFF` 时直接以 `FATAL_ERROR` 中止，lines 28–31 明确注释"CUDA/TensorRT is the only supported backend"，并无条件定义宏 `USE_CUDA`（line 422）。当前产品仅支持 CUDA/TensorRT；DML 不支持，不得配置、构建或打包 DML 路径。
- **配置权威**：`config/config.h`（字段声明和常量）和 `config/config.cpp`（`Config::Config()` 构造函数中的默认值）是 schema 和默认值的唯一来源；`config.ini` 是运行时生成状态，不是默认值定义。
- **串口库**：`serial` 以 `serial_embedded` 静态库形式嵌入；编辑 `mybot.cpp` 时保留附近的中文日志字符串。
- **编译标志**：MSVC `/utf-8 /W3 /sdl /permissive- /EHsc`，`ai` 和 `serial_embedded` 均使用静态 MSVC 运行时。

### 目录结构要点

| 目录/文件 | 用途 |
|---|---|
| `scr/`、`detector/`、`mouse/` 等 | 全部应用源码（项目根 `<repo-root>` 即源码根） |
| `CMakeLists.txt` | 后端选择和源文件清单 |
| `build_current.ps1` | 当前唯一构建入口：注入 VS/CUDA/Ninja 环境并 `--fresh` 重配后编译 |
| `CUDA.TensorRT/` | CUDA/TensorRT SDK 和运行时资源（本地） |
| `packages/` | 未使用的依赖目录；CMake 不使用，不得作为产品构建依赖 |
| `build_cuda/` | 当前有效的 CUDA 构建树 |

**发行路径约定**：不使用 `build\dml`、`build\cuda` 或 `release\` 作为交付路径，除非经过脚本/CMake 对齐后明确重新约定。不对这些目录的当前是否存在作任何断言。

---

## 2. 三热键架构

### 核心约定

- `MAX_MOUSE_HOTKEYS = 3`，热键4、热键5已删除。
- 激活模式固定为**按住生效**：按下时当前热键生效，松开即失效，Toggle 枚举和逻辑已删除。
- 热键优先级固定为 Slot 1 > Slot 2 > Slot 3；不得依赖可配置 `priority`、创建顺序或按下时间。
- 选择函数在三个有效槽位中按固定顺序选择第一个已按住的热键。

### 热键局部配置

每个热键页面承载当前热键的**局部配置**，包括：检测参数、瞄准偏移、移动修正、预测、压枪、动态范围、类别子集和扳机区域/时序。类别子集必须显式配置；缺失类别条目默认关闭，不得回退为全局启用。全局配置只保留程序级内容（模型、全局类别启用/优先级、全局 FOV、输入设备等）。

### 类别两层过滤

全局类别启用是程序硬上限；热键类别子集只能是全局启用集合的子集。全局类别变化时，三个热键的类别子集重置并复制新的全局集合，每个热键至少保留一个类别。运行时判断必须同时满足全局启用和当前热键子集启用，未启用类别不参与目标选择、锁定、鼠标移动和自动扳机。

---

## 3. 统一归一化瞄准偏移

- **已删除**：`disable_headshot`（禁用锁头）。所有头部特殊分支、头部合并逻辑均已移除。
- `aim_offset_x` / `aim_offset_y` 使用目标框归一化坐标，`(0,0)` = 左上角，`(1,1)` = 右下角，`(0.5, 0.5)` = 中心。
- 计算：`targetX = box.x + box.width * pivotX`，`targetY = box.y + box.height * pivotY`。
- 该目标点必须在所有模块中复用：目标选择、目标锁定、卡尔曼预测测量、UI 绘制、自动扳机区域判断。

---

## 4. 构建命令与活动目录

> ⚠️ **当前 CMake 仅支持 CUDA/TensorRT 后端。** DML 构建命令不受支持；不要尝试执行 DML 配置或构建命令。

### 当前有效 Release 输出目录

| 后端 | 构建目录 | 最终程序 |
|---|---|---|
| CUDA/TensorRT（当前唯一支持） | `build_cuda` | `build_cuda\Release\ai.exe` |

不使用 `build_dml_alt`、`build\dml`、`build\cuda` 或 `release\` 作为交付路径，除非经脚本/CMake 对齐后重新约定。

### 唯一构建入口（PowerShell）

在仓库根目录 `<repo-root>` 直接运行：

```powershell
& "<repo-root>\build_current.ps1"
```

脚本必须在本机 PowerShell 直接执行，不能用 Bash 嵌套调用。脚本会注入非标准安装的 VS2022、CUDA 13.2 和 Ninja 环境，使用 `--fresh` 重配 `build_cuda` 后构建，产物固定为 `<repo-root>\build_cuda\Release\ai.exe`。

**不要绕过该脚本手动调用 cmake**；本机 `vswhere` 枚举不到 VS，直接调用会因缺少 Windows SDK 头文件（如 `winsock2.h`）而失败。

---

## 5. CMake 生成器锁定

CMake 将生成器写入构建目录的 `CMakeCache.txt`，同一目录不能在不清理缓存的情况下切换生成器。当前 `build_cuda` 缓存使用 `Ninja Multi-Config`；若其他构建目录使用 Visual Studio 生成器，不得在原目录强行切换：

```
Error: generator : Ninja Multi-Config
Does not match the generator used previously: Visual Studio 18 2026
```

**处理规则**：

1. 已成功配置的目录，使用原生成器的 `cmake --build` 增量编译。
2. 不要在同一目录中强行切换 Visual Studio 和 Ninja。
3. 需要切换生成器时，使用新目录（如 `build_cuda_<generator>`），不要破坏现有有效目录。
4. 查看当前目录生成器：`Select-String -LiteralPath 'build_cuda\CMakeCache.txt' -Pattern '^CMAKE_GENERATOR:'`
5. 日常维护直接运行 `build_current.ps1`，避免手工清理或切换生成器。

---

## 6. 工具链与 CUDA 环境（本地机器说明）

以下信息描述**本地验证机器**，不代表通用环境：

- 非标准安装的 Visual Studio（本机路径以 `<VS-install-root>` 代替；实际值见本机 `build_current.ps1`），`vswhere` 无法枚举，必须由 `build_current.ps1` 注入 VsDevCmd + PATH
- CUDA 版本：13.2；TensorRT 位于项目本地 `CUDA.TensorRT/`
- ONNX Runtime 1.28.0 位于 `../onnxruntime-win-x64-1.28.0`，由 CMake 引用，不需要手工改动
- 换机或 VS 安装位置变化后，使用以下命令找到本机的 `VsDevCmd.bat` 和 `cmake.exe`：

```powershell
Get-ChildItem -Path (Get-PSDrive -PSProvider FileSystem).Root -Recurse -Filter "VsDevCmd.bat" -ErrorAction SilentlyContinue | Select-Object FullName
```

---

## 7. 依赖与 OpenCV 路径

- **CUDA（当前唯一支持的后端）**：CUDA Toolkit + TensorRT 头/库；C++/WinRT 头来自 Windows SDK。OpenCV 默认路径为 `modules/opencv/build/cuda/install`（由 `AIMBOT_OPENCV_CUDA_ROOT` 控制）；TensorRT 搜索顺序为项目本地 `CUDA.TensorRT/`、`modules/TensorRT-*`、`%CUDA_PATH%` 父目录和 `C:\Program Files\NVIDIA GPU Computing Toolkit`（见 CMakeLists.txt lines 39–72）。CUDA OpenCV 首次准备可能较慢，包装脚本会从源码构建。
- **DML 依赖**（不支持）：DML 的 OpenCV、NuGet 包（`packages/`）和 ONNX Runtime DLL 不作为产品依赖；当前 CMakeLists.txt 在构建后阶段明确移除 `onnxruntime.dll` 和 `onnxruntime_providers_*.dll`（lines 487–489）。
- 修改 CMake 生成器检查或后端/依赖布局时，必须同步更新对应的包装脚本。

---

## 8. 配置默认值与 INI 兼容性

- 所有默认值定义在 `config.cpp` 的 `Config::Config()` 构造函数中；`config.ini` 是运行时状态，不是默认值权威。
- 修改默认值后，需点击"恢复出厂设置参数"或删除 `config.ini` 才能使新默认值生效。
- 关键默认值（已验证）：

| 参数 | 默认值 | 定义域 |
|---|---|---|
| `confidence_threshold` | 0.45 | 0.1–0.9 |
| `nms_threshold` | 0.50 | 0.1–0.9 |
| `max_detections` | 8 | 1–20 |
| `aim_offset_x/y` | 0.5 / 0.5 | 0.0–1.0 |
| `fovX / fovY` | 85 / 55 | — |
| `MAX_CLASSES` | 80 | 内部存储容量，不是允许模型类别数 |
| `DEFAULT_MODEL_CLASS_COUNT` | 2 | 兼容常量；运行时不得使用，模型类别数必须从输出张量推断且为 1..19 |

- 配置项 `virtual_camera_heigth`（拼写含错字 `heigth`，兼容拼写）：修改或重命名前需确认 INI 读写两端同步，避免无声地丢失配置。
- 发行包打包脚本统一写入 `aim_offset = 0.5,0.5` 和扳机内部矩形默认值 `0.1,0.1,0.8,0.8`，并移除热键4/5配置行。`USE_CUDA` 宏由当前 CMake 无条件定义（line 422）。

---

## 9. 类别数量推断

程序从模型输出张量形状自动推断类别数，禁止依赖文件名猜测或写死静态值。

**核心规则**：

- YOLOv8/v11/v26 输出：`C = 4 + NC`，`NC = C - 4`
- YOLOv5/v7 输出（含 objectness）：`C = 5 + NC`，`NC = C - 5`
- 端到端 NMS 格式（`[N, 6]`）：从 ONNX metadata 读取 `names` 字典长度，TensorRT engine 的 metadata 在序列化后丢失，需编译前保存 NC。
- SunPoint 多输出格式（含 `heat`/`box`/`offset` 节点）：不按固定 2 类处理；当前产品必须通过模型输出推断并验证 NC 为 1..19。

C++ 推断路径：当前 CUDA 后端走 `trt_detector.cpp` 中的输出维度读取；通用兼容函数 `TryResolveClassLayout()` 位于 `postProcess.cpp`。DML 路径不受支持，CMake 不包含该后端。

---

## 10. 验证与基准测试

- 无完整单元测试体系。修改配置或检测代码后，用对应包装脚本构建，再以模型运行 `ai.exe` 验证运行时行为。
- 运行时：从输出目录执行 `ai.exe`，程序自动切换到其所在目录，创建/使用 `models/` 和 `config.ini`。
- CUDA 首次运行会将 ONNX 模型编译为相邻的 `.engine` 文件，属正常现象。
- 非 WIN32 输入方式不会自动回退到 WIN32。

### 基准测试命令

```powershell
.\ai.exe --benchmark-providers
```

结果输出到 `benchmark_results/`。

### kmboxNet / 输入设备专项验证

- 构建后运行 `build_cuda\Release\kmboxNet_protocol_test.exe`，验证 kmboxNet 按下/抬起/移动/释放协议包内容。
- 运行 `build_cuda\Release\trigger_system_smoke_test.exe`，验证扳机状态机的基础状态流转。
- 以上测试不依赖真实鼠标硬件，但仍需真实盒子回环验证才能覆盖收发超时、断连和物理按键交互。

### 源码模拟测试方案

完整场景列表见 [docs/SOURCE_LOGIC_SIMULATION_TEST_PLAN.md](docs/SOURCE_LOGIC_SIMULATION_TEST_PLAN.md)（S01–S42）。方案在不运行程序、不控制真实鼠标的情况下通过 `FakeMouseInput`、`FakeDetectionFrame` 和 `FakeClock` 模拟用户操作。

优先执行顺序：S01/S04/S06/S08（基础状态和热键）→ S09/S10/S12/S14/S15（类别和检测过滤）→ S18/S19/S21/S22/S23（瞄准和移动边界）→ S24/S25/S26/S27/S28/S30（扳机和点击释放）→ S34/S35/S36/S37（暂停/重载）→ S38–S42（异常场景）。

**当前验证边界**：已完成源码静态检查和 CUDA Release 构建检查；DML 构建不做验证（当前 CMake 不支持 DML）。尚未连接真实游戏和鼠标硬件，三热键物理按键抢占、动态范围收缩时序、内部扳机边界和实际开火/释放仍需现场运行测试。

---

## 11. UI 页面顺序与双层类别/扳机合约

### 顶层 Tab 顺序（`kOverlayTabs[]`）

1. 画面捕获
2. 热键 1
3. 热键 2
4. 热键 3
5. 全局配置
6. 鼠标输入
7. 调试工具

### 热键页面分区顺序（每个热键页相同布局）

热键启用状态 → 鼠标热键绑定（优先级/创建顺序）→ 检测参数 → 瞄准与目标控制 → 瞄准偏移 → 移动修正 → 预测与辅助 → 简易压枪 → 动态范围 → 轨迹预测与辅助 → 目标类别排序与启用 → 自动扳机

性能统计已从独立一级菜单移入**画面捕获**页底部，原捕获设置顺序保持不变；折线图和进度条已移除，改为纯文字/数字显示。

### 类别两层合约

| 层 | 入口 | 作用 |
|---|---|---|
| 全局类别启用（全局配置页） | `class_enabled[i]` | 程序硬上限；未启用类别完全屏蔽 |
| 热键类别子集（热键页） | `hotkey.localClassEnabled[i]` | 只能是全局启用集的子集 |

修改全局类别时，三个热键子集自动重置；每个热键至少保留一个类别。

### 扳机两层合约

最终触发条件（全部为真时才开火）：

```
globalTriggerEnabled
&& hotkey.triggerEnabled
&& activeTargetExists
&& crosshairInsideTriggerRegion
```

持续扳机唯一归属：同一时间只允许一个热键开启持续扳机，配置写入时自动关闭其他热键的持续扳机。

---

## 12. 已确认的 P0–P2 问题（含源码位置）

以下条目记录已确认问题及其修复状态。产品语义以 [IMPLEMENTATION_SPEC.md](docs/IMPLEMENTATION_SPEC.md) 为准。

### 已修复 P0：自动扳机区域判断符合权威规范

**位置**：`runtime/trigger_system.cpp:99–105`、`runtime/trigger_system.cpp:58–70`

扳机判断必须遵循 [IMPLEMENTATION_SPEC.md §5.1](docs/IMPLEMENTATION_SPEC.md#51-几何定义)：检测帧中心（准心）位于目标框内部触发区才可触发。不得以 UI 文案、目标中心或屏幕上的其他区域替代该契约。

**状态**：已修复。当前实现计算目标框内部触发区，并判断检测帧中心是否位于该区域；后续改动必须保持该契约。

### 已修复 P1：`key_delay_ms` 从准心进入区域开始计时

**位置**：`runtime/trigger_system.cpp:128–153`

**状态**：已修复。状态机仅在准心首次进入有效触发区时进入 `KeyDelay`；延时未到期前离开区域会立即重置至 `Idle`。

### 已修复 P1：`leftUp()` 失败时仍重置 `mouse_pressed`

**位置**：`mouse/mouse.cpp` 的 `releaseMouse()` 及其调用范围退出释放路径

**状态**：已修复。`releaseMouse()` 和范围退出释放路径在请求 `leftUp()` 后无条件清除软件按下状态，避免设备 I/O 失败导致重连后的幽灵按下。

### P2：非 WIN32 后端的 `move()` / 按键函数无条件返回成功

**位置**：`mouse/MouseInput.cpp:88–109`、`138–160`、`467–504`、`518–554`

Arduino、KmboxA、Makcu 等后端仅检查 `isOpen()`，底层发送失败后仍返回 `true`。自动扳机会误认为 `leftDown()` 已成功，不会重试或重新同步状态。需设备实测确认影响程度。

### P2：目标丢失时扳机状态机同步依赖

**位置**：`runtime/trigger_system.cpp:107–123`、`182–205`

`Firing` 阶段只有持续调用 `TriggerSystem::update()` 时停火计时才继续；`autoShoot` 中途变为 `false` 时外层调用 `releaseMouse()`，但 `triggerSystem.resetAll()` 只在 `config.auto_shoot` 状态变化时执行，存在状态同步依赖。需补充测试：目标丢失 + autoShoot 关闭、目标丢失 + 热键释放、目标丢失 + 输入设备断开。

---

## 13. 发布验证

此项目副本未包含便携版打包脚本。发布流程应以已验证的
`build_cuda\Release\` 内容为输入，并由独立维护的打包流程处理。
发布名称使用“咔蚯”；版本号与 Git tag 一致，例如 `v20260822`；Release title 必须与 tag 完全一致，资产文件名使用纯英文。

## 14. 源码修改约定

- 修改 config 或检测代码时，检查 CUDA 后端路径；`USE_CUDA` 宏由 CMake 无条件定义，无需条件编译分支切换。
- UI 显示、配置保存、运行时计算、调试绘制必须使用同一套语义——不能只隐藏控件而不接入最终鼠标移动路径，也不能只改配置名称而不改变实际行为。
- 视觉调试开关（绘制预测位置、轨迹曲线、触发矩形绘制、目标框显示）只影响 UI 显示，不得改变瞄准、锁定或扳机行为。
- 速度计算参数关系约束：`近距半径 <= 吸附半径`、`最小速度倍率 <= 最大速度倍率`、`速度曲线指数 > 0`、`吸附加速倍数 >= 0`。
- 不要在已有构建目录中切换 CMake 生成器；不要用 `Remove-Item -Recurse` 删除构建目录，除非已确认需要完全重新配置。

## 15. 输入设备与 kmboxNet 约定

- kmboxNet 侧键掩码为 `0x08`（side1）和 `0x10`（side2）；`releaseAllButtons()` 必须同时清空这两个侧键位，避免多键混合后残留。
- 连续 3 次 ACK 失败才算断连；单次或偶发 ACK 失败不得立即复位连接状态，避免网络抖动导致指令重发或鼠标失控。
- kmboxNet 发出的信号与自动扳机信号共享同一物理鼠标：用户手动按下/抬起会和程序按下/抬起交错；释放路径必须幂等，并确保 `releaseAllButtons()` 在停止、切换热键、目标丢失和设备断开时都会执行。
- mock/协议级测试覆盖：左键、右键、side1、side2 混合按下，移动，释放，`releaseAll()` 清空侧键。真实盒子收发超时、断连恢复仍需实机回环验证。
