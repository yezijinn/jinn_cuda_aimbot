# 源码内部用户场景模拟测试方案

> **产品行为权威**：[docs/IMPLEMENTATION_SPEC.md](IMPLEMENTATION_SPEC.md)  
> 本文中所有场景的预期结果，凡与 IMPLEMENTATION_SPEC.md 冲突，以 IMPLEMENTATION_SPEC.md 为准。  
> 已修复问题说明见下方 §1.5；维护者参考 [AGENTS.md](../AGENTS.md) §12。

## 1. 文档目的

本文用于在**不直接运行程序、不控制真实鼠标、不连接真实硬件、不进入游戏**的情况下，依据当前源码模拟用户操作和状态变化，检查热键、检测、瞄准、鼠标移动、自动扳机和配置切换逻辑是否符合预期。

本文不是单元测试代码，也不修改任何功能代码。执行者可以使用调试器、断点、临时内存观察、离线伪造输入状态或人工逐步推演完成验证。

## 1.5 已修复问题说明

本测试方案编写时记录了部分待确认行为。以下问题已按 [IMPLEMENTATION_SPEC.md](IMPLEMENTATION_SPEC.md) 明确并修复（见 [AGENTS.md](../AGENTS.md) §12）：

- **自动扳机区域判断**（S24–S29 相关场景）：已修正为"检测帧中心（准心）落入目标框内部触发区"。判断基准是目标框内缩后的矩形区域，不使用目标中心、屏幕固定区域或 UI 文案描述的区域。见 `runtime/trigger_system.cpp:99–105`、`58–70`。
- **`key_delay_ms` 计时起点**：已修正为从准心**首次进入**目标内部触发区时开始计时。延时期间准心离开区域则立即重置至 `Idle`，不保留已计时状态。见 `runtime/trigger_system.cpp:128–153`。
- **`leftUp()` 失败时的状态同步**：已修正为无条件清除软件按下状态，避免设备 I/O 失败导致重连后的幽灵按下。见 `mouse/mouse.cpp:792–805`。

下文场景描述保持原始设计意图。实际验证时，以上三点修复后的行为为准，与场景原描述冲突时以修复后行为为准。

## 2. 固定测试前提

所有场景默认满足：

- 程序已经运行。
- 模型已经加载成功。
- 模型类别数量为 5：`class 0` 到 `class 4`。
- 检测分辨率为 `320`，画面中心为 `(160, 160)`。
- 仅模拟内存状态和函数输入，不调用真实 `SendInput`、串口、HID、GHUB、RAZER、Kmbox 或 Makcu。
- 鼠标输出使用 `FakeMouseInput` 或等价的记录器：只记录 `move(dx, dy)`、`leftDown()`、`leftUp()`、`rightDown()`、`rightUp()`，不向操作系统发送事件。
- 每个场景从干净的配置快照开始，场景之间不得复用上一场景的按键状态、目标状态或扳机状态。

## 3. 观测对象

每个场景至少记录以下状态：

| 状态 | 说明 |
| --- | --- |
| `active_mouse_hotkey_slot` | 当前生效的鼠标热键槽位，未激活为 `-1` |
| `aiming` | 当前是否允许瞄准逻辑运行 |
| `shooting` | 当前是否处于射击状态 |
| `zooming` | 当前是否处于缩放/瞄准镜状态 |
| `detectionPaused` | 检测是否暂停 |
| `detectionBuffer.boxes` | 当前检测框数量和位置 |
| `detectionBuffer.classes` | 每个检测框的类别 ID |
| `detectionBuffer.confidences` | 每个检测框的置信度 |
| `FakeMouseInput` | 移动和按键调用顺序及参数 |
| `TriggerConfig` | 扳机启用、停火和时序参数 |
| `MouseThread` | FOV、速度、预测和 Kalman 状态 |

## 4. 推荐的模拟对象

### 4.1 FakeMouseInput

```text
isOpen()       -> 可配置 true/false
hasPhysicalButtonState() -> 可配置 true/false
keyPressed(key) -> 由场景表返回 true/false
move(dx, dy)   -> 只写入 moveEvents，不发送真实输入
leftDown()     -> 只写入 buttonEvents
leftUp()       -> 只写入 buttonEvents
rightDown()    -> 只写入 buttonEvents
rightUp()      -> 只写入 buttonEvents
releaseAllButtons() -> 记录 releaseAll 事件
```

### 4.2 FakeDetectionFrame

每帧输入包含：

```text
boxes        = [Rect(...)]
classes      = [0..4]
confidences  = [0.0..1.0]
frameTime    = 单调递增时间
```

非法类别、负尺寸框、空目标列表、重复目标和低置信度目标都要作为独立场景测试。

### 4.3 FakeClock

扳机测试必须使用可控时间，不使用真实 `sleep` 推测结果：

```text
t0 = 0 ms
t1 = 10 ms
t2 = 50 ms
t3 = 100 ms
t4 = 500 ms
t5 = 1000 ms
```

这样可以稳定验证按键延迟、前摇延迟、持续时间和冷却时间。

## 5. 基础状态测试

### S01：无任何输入

**配置**

```text
auto_aim = false
mouse_hotkeys[0..2].enabled = false
button_targeting = []
button_shoot = []
button_zoom = []
```

**模拟**

```text
所有 keyPressed = false
所有设备状态 = false
检测帧可以为空
```

**预期**

```text
active_mouse_hotkey_slot = -1
aiming = false
shooting = false
zooming = false
FakeMouseInput 没有 move/down/up 事件
```

**检查**：空闲循环不得产生鼠标移动、点击或扳机事件。

### S02：全局 `auto_aim = true`

**配置**

```text
auto_aim = true
所有鼠标热键均未按下
```

**预期**

```text
aiming = true
active_mouse_hotkey_slot = -1
```

**检查**：`auto_aim` 只应让瞄准状态开启，不应凭空产生射击或鼠标按键事件。

### S03：全局目标、射击、缩放热键

分别模拟以下输入：

| 输入 | 预期状态 |
| --- | --- |
| `button_targeting` 中任意键按下 | `aiming = true` |
| `button_shoot` 中任意键按下 | `shooting = true` |
| `button_zoom` 中任意键按下 | `zooming = true` |
| 所有键释放 | 对应状态恢复 `false` |

检查按键释放后状态不会永久保持为 `true`。

## 6. 鼠标热键和优先级

### S04：单个热键按下

```text
hotkey[0].id = "hotkey_1"
hotkey[0].enabled = true
hotkey[0].buttons = ["LeftMouseButton"]
hotkey[0].priority = 10
LeftMouseButton = true
```

**预期**

```text
active_mouse_hotkey_slot = 0
```

如果当前 profile 启用了扳机并且检测目标满足条件，才允许后续扳机流程产生事件；热键本身不能直接等同于射击。

### S05：热键禁用

与 S04 相同，但设置：

```text
hotkey[0].enabled = false
```

**预期**：该槽位不参与选择，`active_mouse_hotkey_slot = -1`。

### S06：三个热键同时按下

```text
hotkey[0].priority = 30
hotkey[1].priority = 10
hotkey[2].priority = 20
hotkey[0..2].enabled = true
hotkey[0..2].buttons = 不同物理按键
三个按键都为 true
```

**预期**：选择优先级最高的热键，即 priority 数值最小的槽位 `1`。

### S07：两个热键使用相同物理按键

```text
hotkey[0].buttons = ["LeftMouseButton"]
hotkey[1].buttons = ["LeftMouseButton"]
```

分别测试不同 priority 和相同 priority：

- 不同 priority：必须稳定选择优先级更高者。
- 相同 priority：记录 `Config::selectActiveMouseHotkey()` 的实际 tie-break 行为，不能凭假设判定。
- UI 配置层面应检查是否阻止重复绑定。

### S08：热键按下后释放

使用 S04 的输入，按以下帧序列执行：

```text
frame 1: LeftMouseButton = false
frame 2: LeftMouseButton = true
frame 3: LeftMouseButton = true
frame 4: LeftMouseButton = false
```

**预期**：槽位按 `-1 -> 0 -> 0 -> -1` 变化，不应残留激活状态。

## 7. 全局类别和五类别模型

### S09：五个类别全部开启

```text
model_num_classes = 5
class_enabled = [true, true, true, true, true]
```

依次输入类别 `0..4` 的检测框，检查每个类别可以进入目标筛选流程。

### S10：关闭类别 2

```text
class_enabled = [true, true, false, true, true]
```

检测帧包含类别 `[0, 1, 2, 3, 4]`。

**预期**：类别 2 不参与锁定、瞄准和扳机，其余四类仍可参与。

### S11：尝试关闭最后一个类别

依次关闭类别，直到只剩一个 `true`，然后尝试关闭最后一个。

**预期**：最后一个类别仍保持开启，符合 UI 中“至少保留一个类别”的约束。

### S12：热键类别子集

```text
global class_enabled = [true, true, false, true, true]
```

**预期**：热键最多只能使用全局启用类别的子集，类别 2 即使热键局部值为 `true` 也不能绕过全局关闭。

### S13：类别排序

```text
class_order = [4, 0, 3, 1, 2]
```

输入多个同等条件目标，检查目标选择顺序是否遵守当前热键类别排序；拖动排序只应改变顺序，不应改变 `class_enabled_*` 值。

## 8. 置信度、检测框和目标选择

### S14：置信度边界

以 `confidence_threshold = 0.45` 为例：

```text
confidence = 0.44 -> 应被过滤
confidence = 0.45 -> 按当前比较运算符记录实际边界行为
confidence = 0.46 -> 应保留
```

边界值必须以源码中的 `>=` 或 `>` 为准，不得只按 UI 文案猜测。

### S15：空检测结果

帧序列：

```text
frame 1: 一个有效目标
frame 2: boxes/classes/confidences 全为空
frame 3: 仍为空
```

**预期**：不产生新的瞄准移动；如果启用了“目标丢失时停火”，应按停火延迟释放按键。

### S16：五类别混合目标

```text
boxes = 5 个框
classes = [0, 1, 2, 3, 4]
confidences = [0.9, 0.8, 0.7, 0.6, 0.5]
```

检查：

- 类别映射不偏移。
- 类别 4 不被误认为类别 0。
- 只关闭某一类时，其他类仍可用。
- 目标排序和置信度过滤顺序与源码一致。

### S17：非法类别 ID

输入：

```text
classes = [-1, 5, 999]
```

这是边界测试，不预设正确业务结果。要求记录程序是否：

- 安全跳过非法目标。
- 发生越界访问。
- 产生错误日志后继续运行。
- 崩溃或污染其他目标状态。

## 9. 瞄准和鼠标移动

### S18：目标在屏幕中心

```text
resolution = 320
target box center = (160, 160)
aim_offset = (0.5, 0.5)
```

**预期**：目标误差接近 `(0, 0)`，不应产生明显移动事件；若源码对零移动有显式忽略，必须保持无 `move(0,0)`。

### S19：目标在中心右侧

```text
target center = (220, 160)
aim_offset = (0.5, 0.5)
```

**预期**：产生向右修正的移动命令，符号方向必须与 `MouseThread` 坐标约定一致；不能只检查绝对值。

### S20：目标在中心上方

```text
target center = (160, 80)
```

**预期**：产生向上修正；同时测试上下方向没有反转。

### S21：FOV 边界

分别测试：

```text
fovX = 1, fovY = 1
fovX = 360, fovY = 360
fovX = 121, fovY = 90
```

检查：

- 不出现除零、NaN 或无穷大。
- 移动量仍是有限数值。
- FOV 修改后 `MouseThread::updateConfig()` 更新中心、FOV 和最大距离。

### S22：速度倍率边界

```text
minSpeedMultiplier = 0.001
maxSpeedMultiplier = 10.0
```

分别令 min 大于 max、min 等于 max、两个值都为最小值，检查 UI 验证和移动计算是否避免反向区间或异常速度。

### S23：预测和 Kalman 开关

同一目标轨迹下分别测试：

```text
predictionInterval = 0
predictionInterval = 0.1
kalman_enabled = false
kalman_enabled = true
```

**预期**：关闭时不使用对应预测/滤波路径；开启后状态清理和参数更新不会产生 NaN、跳变或线程崩溃。

## 10. 自动扳机时序

使用可控时钟和一个位于扳机区域内的类别 0 目标。

### S24：扳机关闭

```text
trigger_enabled = false
```

即使目标在区域内、热键激活、`shooting` 为 false，也不应调用 `leftDown()`。

### S25：扳机正常时序

示例配置：

```text
trigger_enabled = true
trigger_enabled_for_hotkey = true
stop_fire_on_loss = true
key_delay_ms = 20
pre_fire_delay_ms = 30
fire_duration_ms = 80
cooldown_ms = 100
```

时间序列：

```text

必须检查 `leftDown`/`leftUp` 成对出现，不能重复按下而没有释放。

### S26：目标在前摇阶段消失

```text

**预期**：不应产生 `leftDown()`。如果已经进入按下状态，则依据停火逻辑等待并释放。

### S27：目标在持续射击阶段消失

```text

启用 `stop_fire_on_loss`，检查在 `stop_fire_delay_ms` 后产生唯一一次 `leftUp()`。

### S28：热键局部扳机开关关闭

```text
global trigger_enabled = true
hotkey local trigger_enabled_for_hotkey = false
```

**预期**：该热键不扳机；不能因为全局开关开启而绕过热键局部开关。

### S29：持续扳机无需热键

```text
trigger_continuous = true
hotkey 未激活
目标在区域内
```

检查实际源码定义的“无需热键”语义：应明确是否允许自动扳机；如果行为与 UI 文案不一致，记录为产品逻辑问题，不在测试中擅自修复。

## 11. 点击和按键释放

### S30：左键按下/释放配对

一个完整扳机周期必须满足：

```text
leftDown count = leftUp count
任意时刻 downCount - upCount <= 1
```

在目标丢失、热键释放、暂停、配置重载、输入设备断开和程序退出路径分别检查。

### S31：输入设备断开

将 `FakeMouseInput::isOpen()` 从 `true` 切换为 `false`：

- `move()` 不应继续发送。
- 新的 `leftDown()`/`rightDown()` 应失败或被阻止。
- 已按下的按键必须有释放/清理路径。
- 程序不应崩溃。

### S32：无物理按键状态设备

```text
hasPhysicalButtonState = false
```

检查鼠标热键是否按源码预期使用 Win32 查询；不要把“设备没有物理状态”误认为按键持续按下。

### S33：有物理按键状态设备

```text
hasPhysicalButtonState = true
keyPressed("LeftMouseButton") = true
Win32 同名键状态 = false
```

**预期**：使用设备状态识别热键；不应因为 Win32 状态为 false 而误判为未按下。

## 12. 暂停、重载和配置修改

### S34：暂停检测热键

输入序列：

```text
pause = false
按下 pause -> pause = true
保持按住多个 listener loop -> 仍为 true，不重复翻转
释放 -> 保持 true
再次按下 -> pause = false
```

这是边沿触发测试，重点是 `pausePressed` 防抖状态。

### S35：重载配置

修改配置文件中的：

```text
detection_resolution
capture_fps
input_method
ai_model
fovX/fovY
```

模拟 reload 热键，检查对应 changed flag 是否被设置，并检查 `globalMouseThread->updateConfig()` 是否收到新值。

### S36：重载配置时保持热键状态

在热键激活期间执行 reload：

- 不应出现空指针。
- 不应重复创建鼠标线程。
- 不应丢失 `active_mouse_hotkey_slot` 的合法性。
- 目标和扳机状态应按新配置重新计算，而不是沿用失效旧参数。

### S37：方向键修改瞄准偏移

设置：

```text
aim_offset_x = 0.0
aim_offset_y = 0.0
```

模拟单次边沿：

| 按键 | 预期 |
| --- | --- |
| Up | Y 减少，但不低于 0 |
| Down | Y 增加，但不超过 1 |
| Left | X 减少，但不低于 0 |
| Right | X 增加，但不超过 1 |
| 持续按住 | 每个按下边沿只改变一次 |

## 13. 安全边界和异常场景

### S38：空热键 ID

```text
hotkey.id = ""
hotkey.buttons = ["LeftMouseButton"]
```

**预期**：该热键被跳过，不参与 active profile 选择。

### S39：未知按键名称

```text
buttons = ["UnknownKey"]
```

检查 `KeyCodes::getKeyCode()` 返回 `-1` 后，不出现越界、异常点击或高 CPU 忙循环。

### S40：空配置数组

将 `button_targeting`、`button_shoot`、`button_zoom` 和截图按键配置设为空，检查：

- 不崩溃。
- 状态回到 false。
- UI 是否显示合理的空状态。

### S41：重复快速按键

按下/释放 pause、reload、overlay 热键，时间间隔设置为 `1ms`、`10ms`、`100ms`，检查边沿状态不会重复执行或丢失永久状态。

### S42：五类别全部目标同时出现

输入五个类别、相同置信度、相同距离的目标，记录选择结果。此场景必须根据源码的类别排序、目标排序和 tie-break 规则判定，不能只要求“任意一个都可以”。

## 14. 每个场景的判定模板

```text
场景编号：Sxx
配置输入：
检测帧：
按键帧序列：
设备状态：
FakeClock：

观察到的状态：
- active_mouse_hotkey_slot =
- aiming =
- shooting =
- zooming =
- detectionPaused =
- move events =
- button events =

源码依据：文件和函数名、行号
预期结果：
实际结果：
结论：PASS / FAIL / BLOCKED
失败原因：
```

## 15. 优先执行顺序

资源有限时，按以下顺序执行：

1. S01、S04、S06、S08：基础状态和热键选择。
2. S09、S10、S12、S14、S15：五类别和检测过滤。
3. S18、S19、S21、S22、S23：瞄准和移动边界。
4. S24、S25、S26、S27、S28、S30：扳机和点击释放。
5. S34、S35、S36、S37：暂停、重载和用户调参。
6. S38、S39、S40、S41、S42：异常和压力场景。

## 16. 当前验证边界

本方案是源码内部模拟方案，不能替代以下真实测试：

- 真实 Windows `GetAsyncKeyState` 时序。
- 真实 `SendInput` 的系统响应。
- 串口、HID、GHUB、RAZER、Kmbox、Makcu 设备反馈。
- 游戏窗口捕获、真实模型帧率和真实目标抖动。
- 多线程调度在高负载下的竞态。

模拟测试发现问题后，应使用最小输入序列复现，再决定是否增加自动化测试或修复代码。没有明确复现输入、观察状态和源码路径时，不应把潜在风险直接判定为已确认 Bug。
