#ifndef OVERLAY_UI_SECTIONS_H
#define OVERLAY_UI_SECTIONS_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

#include "imgui/imgui.h"

namespace UiLayout
{
inline constexpr float kInputWidthInCharacters = 5.0f;

inline float InputControlWidth() noexcept
{
    return ImGui::GetFontSize() * kInputWidthInCharacters;
}

struct InputControlWidthToken
{
    operator float() const noexcept { return InputControlWidth(); }
};

inline constexpr InputControlWidthToken kNumericWidth{};
inline constexpr InputControlWidthToken kTextShortWidth{};
inline constexpr InputControlWidthToken kTextMediumWidth{};
inline constexpr InputControlWidthToken kComboShortWidth{};
inline constexpr InputControlWidthToken kComboMediumWidth{};
inline constexpr InputControlWidthToken kComboLongWidth{};
inline constexpr float kActionButtonWidth = 160.0f;
}

inline const char* TooltipForSetting(const char* label) noexcept
{
    if (!label) return nullptr;
    if (std::strcmp(label, "置信度阈值") == 0) return "模型认为目标可信的最低分数。提高可减少误检，降低可发现更多目标。";
    if (std::strcmp(label, "NMS 阈值") == 0) return "过滤重叠检测框的阈值。数值越低越容易合并重叠框。";
    if (std::strcmp(label, "移动灵敏度") == 0 || std::strcmp(label, "水平灵敏度") == 0 || std::strcmp(label, "垂直灵敏度") == 0)
        return "匹配游戏鼠标灵敏度和水平、垂直旋转比例。数值越大，同样的鼠标移动产生的视角变化越大。";
    if (std::strcmp(label, "kalman位置过程噪声") == 0 || std::strcmp(label, "kalman速度过程噪声") == 0 ||
        std::strcmp(label, "kalman测量噪声") == 0 || std::strcmp(label, "kalman速度阻尼") == 0 ||
        std::strcmp(label, "kalman最大速度") == 0 || std::strcmp(label, "kalman预热帧数") == 0)
        return "kalman滤波的跟随参数。增大通常会减少抖动但降低响应速度；减小通常更灵敏但更容易受到检测噪声影响。";
    if (std::strcmp(label, "轨迹强度") == 0 || std::strcmp(label, "轨迹微扰强度") == 0 ||
        std::strcmp(label, "轨迹最大速度") == 0 || std::strcmp(label, "轨迹距离") == 0)
        return "调整鼠标移动轨迹的平滑程度和速度。数值越大通常动作更明显或更快，过大可能造成跟随不稳定。";
    if (std::strcmp(label, "动态范围") == 0 || std::strcmp(label, "动态范围缩小范围(px)") == 0)
        return "动态范围用于目标较多时缩小搜索区域。范围数值越大，缩小后的区域越小，可能漏掉边缘目标。";
    if (std::strcmp(label, "自动标注最低置信度") == 0 || std::strcmp(label, "自动标注最大框数") == 0)
        return "自动标注筛选参数。阈值越高结果更严格，最大框数量越大可保留更多目标。";
    if (std::strcmp(label, "模型分辨率") == 0) return "模型输入画面的边长。数值越大细节越多，但推理耗时和显存占用也会增加。";
    if (std::strcmp(label, "采集帧率") == 0) return "限制捕获画面的更新频率。过高可能增加 CPU/GPU 负载，过低会降低目标更新速度。";
    if (std::strcmp(label, "圆形视野") == 0) return "只保留准心附近的画面区域进行处理，可减少边缘目标和无关区域带来的负载。";
    if (std::strcmp(label, "模型") == 0 || std::strcmp(label, "模型路径与后端") == 0) return "选择要使用的模型和推理后端。模型文件必须放在程序的 models 文件夹中。";
    if (std::strcmp(label, "输入方式") == 0) return "选择鼠标指令的输出设备。普通用户建议先使用 WIN32。";
    if (std::strcmp(label, "目标类别") == 0) return "程序级类别总开关。关闭后，该类别不会参与检测后的锁定、瞄准、移动和扳机操作。";
    if (std::strcmp(label, "预测间隔") == 0) return "目标预测使用的时间间隔。数值越大预测更激进，可能提前但也更容易偏移。";
    if (std::strcmp(label, "预测位置数") == 0) return "绘制或计算的未来位置数量。数值越大预测轨迹更长，但计算量也会增加。";
    if (std::strcmp(label, "启用kalman滤波") == 0) return "平滑检测框的位置和速度，减少检测抖动。目标变化很快或检测不稳定时可尝试开启。";
    if (std::strcmp(label, "kalman检测延迟补偿") == 0) return "补偿模型推理和检测传输造成的延迟。开启后才显示下面的额外预测参数。";
    if (std::strcmp(label, "启用轨迹曲线") == 0) return "让鼠标移动轨迹更加平滑。开启后可以调整曲线强度、微扰和速度参数。";
    if (std::strcmp(label, "自动瞄准开关") == 0) return "允许当前热键产生瞄准目标。关闭后该热键仍可检测，但不会驱动瞄准。";
    if (std::strcmp(label, "保持目标锁定") == 0) return "在连续帧之间保持目标锁定，减少目标切换。关闭后主要使用当前帧目标。";
    if (std::strcmp(label, "瞄准模式") == 0) return "距离中心最近适合快速锁定准心附近目标；方框最大优先选择目标框面积更大的目标。";
    if (std::strcmp(label, "置信度阈值") == 0) return "模型认为目标可信的最低分数。提高可减少误检，降低可发现更多目标。";
    if (std::strcmp(label, "NMS 阈值") == 0) return "过滤重叠检测框的阈值。数值越低越容易合并重叠框。";
    if (std::strcmp(label, "敌人检测数") == 0) return "每帧最多保留的检测框数量。数值越大可保留更多目标，但处理量会增加。";
    if (std::strcmp(label, "目标丢失时停火") == 0) return "目标离开检测结果或扳机区域后，是否自动停止持续按键。新用户建议开启。";
    if (std::strcmp(label, "停火延迟(ms)") == 0) return "确认目标丢失后等待多久再松开按键。数值越大越不容易因单帧抖动停火。";
    if (std::strcmp(label, "按键延迟(ms)") == 0) return "热键激活后，进入扳机状态前等待的时间。";
    if (std::strcmp(label, "前摇延迟(ms)") == 0) return "目标进入触发区域后，真正按下射击键前等待的时间。";
    if (std::strcmp(label, "持续时间(ms)") == 0) return "每次自动按键保持按下的基础时长。";
    if (std::strcmp(label, "持续时间随机(ms)") == 0) return "在持续时间上增加随机变化，减少每次按键时长完全固定的情况。";
    if (std::strcmp(label, "冷却时间(ms)") == 0) return "一次按键结束后到下一次允许触发前的基础等待时间。";
    if (std::strcmp(label, "冷却时间随机(ms)") == 0) return "在冷却时间上增加随机变化，减少触发节奏完全固定的情况。";
    if (std::strcmp(label, "启用动态范围") == 0) return "目标数量较多时临时缩小搜索范围，减少目标竞争和鼠标移动距离。";
    if (std::strcmp(label, "绑定鼠标键") == 0) return "按住此鼠标键时启用当前热键配置。每个物理按键只能绑定到一个热键。";
    if (std::strcmp(label, "热键优先级") == 0) return "多个热键同时按下时用于选择配置。数值越小优先级越高。";
    if (std::strcmp(label, "保存配置到文件") == 0) return "立即将当前界面设置写入 config.ini。修改后建议保存一次。";
    if (std::strcmp(label, "FOV范围X") == 0 || std::strcmp(label, "FOV范围Y") == 0)
        return "限制检测和瞄准活动范围。数值越大覆盖区域越广，但处理量和误选目标的可能性也会增加。";
    if (std::strcmp(label, "当前游戏") == 0)
        return "选择当前使用的游戏配置。不同配置可以保存不同灵敏度、视野和瞄准参数。";
    if (std::strcmp(label, "灵敏度") == 0 || std::strcmp(label, "水平灵敏度") == 0 || std::strcmp(label, "垂直灵敏度") == 0)
        return "匹配游戏鼠标灵敏度和水平、垂直旋转比例。数值越大，同样的鼠标移动产生的视角变化越大。";
    if (std::strcmp(label, "视野缩放") == 0)
        return "根据游戏的视野缩放状态修正鼠标移动计算。使用瞄准镜或不同FOV时建议按游戏状态设置。";
    if (std::strcmp(label, "基础视野") == 0)
        return "游戏未缩放时的基础视野角度。数值越大，视角换算越宽。";
    if (std::strcmp(label, "新配置名称") == 0)
        return "输入新游戏配置名称，然后点击右侧按钮创建独立配置。";
    if (std::strcmp(label, "配置") == 0)
        return "删除当前游戏配置。删除前请确认当前配置不是仍在使用的配置。";
    if (std::strcmp(label, "配置管理") == 0)
        return "创建、切换或删除游戏配置。配置修改后请保存到文件。";
    if (std::strcmp(label, "输入设备") == 0 || std::strcmp(label, "输入方式") == 0)
        return "选择输出鼠标移动和按键事件的后端。普通用户建议先使用WIN32标准方式。";
    if (std::strcmp(label, "模型后端") == 0 || std::strcmp(label, "后端") == 0 || std::strcmp(label, "模型路径与后端") == 0)
        return "选择模型推理后端。CUDA适合NVIDIA显卡，DirectML适合支持DirectX的设备；后端必须与模型和运行环境匹配。";
    if (std::strcmp(label, "模型") == 0 || std::strcmp(label, "模型选择") == 0)
        return "选择检测模型。模型越大通常精度越高，但推理速度和显存占用也会增加。";
    if (std::strcmp(label, "窗口") == 0 || std::strcmp(label, "窗口过滤") == 0 || std::strcmp(label, "窗口列表") == 0)
        return "选择要捕获的应用窗口。窗口名称变化时需要重新选择或刷新列表。";
    if (std::strcmp(label, "显示器") == 0 || std::strcmp(label, "显示器列表") == 0)
        return "选择屏幕捕获来源。多显示器环境下请选择游戏所在的显示器。";
    if (std::strcmp(label, "采集卡") == 0)
        return "选择采集卡捕获来源。仅在使用摄像头或视频输入时需要设置。";
    if (std::strcmp(label, "捕获方式") == 0)
        return "选择画面捕获来源。窗口捕获适合指定程序，显示器捕获适合全屏或无法枚举的程序。";
    if (std::strcmp(label, "捕获边框") == 0)
        return "在捕获窗口画面中保留边框。关闭后通常可以减少非游戏区域。";
    if (std::strcmp(label, "捕获光标") == 0)
        return "是否把系统鼠标光标包含在捕获画面中。";
    if (std::strcmp(label, "UDP IP地址") == 0 || std::strcmp(label, "UDP端口") == 0)
        return "UDP画面输入的网络地址和端口。必须与发送端配置一致。";
    if (std::strcmp(label, "UDP启用") == 0 || std::strcmp(label, "启用UDP") == 0)
        return "启用通过UDP接收画面。开启后需要正确填写发送端IP和端口。";
    if (std::strcmp(label, "截图延迟") == 0)
        return "截图前等待的时间。数值越大越容易截到界面更新后的画面，但响应会变慢。";
    if (std::strcmp(label, "记录类别") == 0 || std::strcmp(label, "输出目录") == 0)
        return "数据采集的类别筛选和保存位置。修改后新采集的数据会写入指定目录。";
    if (std::strcmp(label, "详细控制台输出") == 0)
        return "输出更多调试日志。排查问题时开启，正常使用时关闭可减少日志和开销。";
    if (std::strcmp(label, "从捕获中隐藏面板") == 0)
        return "开启后，截图或视频捕获中不会显示设置面板；程序本身仍会显示面板。";
    if (std::strcmp(label, "显示预览窗口 仅测试时开启 正常使用要关闭") == 0)
        return "开启捕获画面预览，便于确认当前捕获源是否正确。预览会增加少量性能开销。";
    if (std::strncmp(label, "类别 ", 7) == 0)
        return "程序级类别开关。关闭后该类别不会进入检测结果、锁定、瞄准或自动扳机。至少保留一个类别开启。";
    if (std::strcmp(label, "恢复全部默认参数") == 0)
        return "将全局配置和热键配置恢复为出厂默认值。执行前会备份当前配置，请谨慎使用。";
    if (std::strcmp(label, "当前配置") == 0)
        return "显示当前游戏配置的名称和关键参数。修改下面的参数后会影响当前配置。";
    if (std::strcmp(label, "动态范围") == 0 || std::strcmp(label, "动态范围缩小范围(px)") == 0)
        return "动态范围用于目标较多时缩小搜索区域。范围数值越大，缩小后的区域越小，可能漏掉边缘目标。";
    if (std::strcmp(label, "轨迹距离") == 0 || std::strcmp(label, "轨迹强度") == 0 ||
        std::strcmp(label, "轨迹微扰强度") == 0 || std::strcmp(label, "轨迹最大速度") == 0)
        return "调整鼠标移动轨迹的平滑程度和速度。数值越大通常动作更明显或更快，过大可能造成跟随不稳定。";
    if (std::strcmp(label, "预测权重") == 0 || std::strcmp(label, "预测间隔") == 0)
        return "目标移动预测参数。数值越大越偏向提前移动，响应更激进但偏移风险也更高。";
    if (std::strcmp(label, "预测未来帧数") == 0 || std::strcmp(label, "预测位置数") == 0)
        return "预测未来位置的数量。数值越大可覆盖更长预测轨迹，但计算量和预测误差也会增加。";
    if (std::strcmp(label, "kalman测量噪声") == 0 || std::strcmp(label, "kalman过程噪声位置") == 0 ||
        std::strcmp(label, "kalman过程噪声速度") == 0 || std::strcmp(label, "kalman速度阻尼") == 0 ||
        std::strcmp(label, "kalman最大速度") == 0 || std::strcmp(label, "kalman重置超时") == 0 ||
        std::strcmp(label, "kalman额外预测毫秒") == 0 || std::strcmp(label, "kalman预热帧数") == 0)
        return "kalman滤波的跟随参数。增大通常会减少抖动但降低响应速度；减小通常更灵敏但更容易受到检测噪声影响。";
    if (std::strcmp(label, "扳机区域宽度") == 0 || std::strcmp(label, "扳机区域高度") == 0 ||
        std::strcmp(label, "扳机区域偏移X") == 0 || std::strcmp(label, "扳机区域偏移Y") == 0)
        return "设置自动扳机的矩形触发区域。宽高越大更容易触发，偏移值用于把区域移动到目标的指定位置。";
    if (std::strcmp(label, "自动扳机") == 0 || std::strcmp(label, "启用扳机") == 0)
        return "开启后，目标进入扳机区域并满足时序条件时自动发送按键。关闭后不会自动触发。";
    if (std::strcmp(label, "目标丢失时停火") == 0)
        return "目标离开检测结果或扳机区域后是否自动松开按键。建议开启，避免目标丢失后持续按键。";
    if (std::strcmp(label, "FOV X") == 0 || std::strcmp(label, "FOV Y") == 0)
        return "限制检测和瞄准活动范围。数值越大覆盖区域越广，但处理量和误选目标的可能性也会增加。";
    if (std::strcmp(label, "Min Speed Multiplier") == 0 || std::strcmp(label, "Max Speed Multiplier") == 0)
        return "控制鼠标移动速度范围。数值越大移动更快，数值越小移动更慢但通常更平滑。";
    if (std::strcmp(label, "Prediction Interval") == 0)
        return "目标预测使用的时间间隔。数值越大预测更激进，可能提前但也更容易偏移；设为零表示关闭预测。";
    if (std::strcmp(label, "Future Positions") == 0 || std::strcmp(label, "Draw Future Positions") == 0)
        return "预测或绘制未来目标位置。数量越多轨迹越长，但计算量和预测误差也会增加。";
    if (std::strcmp(label, "Enable Kalman Filter") == 0)
        return "平滑检测框的位置和速度，减少检测抖动。开启后可以调整kalman滤波参数。";
    if (std::strcmp(label, "Kalman Process Noise Pos") == 0 || std::strcmp(label, "Kalman Process Noise Vel") == 0 ||
        std::strcmp(label, "Kalman Measurement Noise") == 0 || std::strcmp(label, "Kalman Velocity Damping") == 0 ||
        std::strcmp(label, "Kalman Max Velocity") == 0 || std::strcmp(label, "Kalman Warmup Frames") == 0 ||
        std::strcmp(label, "Kalman Compensate Inference Delay") == 0 ||
        std::strcmp(label, "Kalman Additional Predict (ms)") == 0 ||
        std::strcmp(label, "Kalman Reset Timeout (s)") == 0)
        return "kalman滤波参数。增大通常会减少抖动但降低响应速度，减小通常更灵敏但更容易受到检测噪声影响。";
    if (std::strcmp(label, "Snap Radius") == 0 || std::strcmp(label, "Near Radius") == 0 ||
        std::strcmp(label, "Speed Curve Exponent") == 0 || std::strcmp(label, "Snap Boost Factor") == 0)
        return "调整目标修正的范围和速度曲线。数值越大通常修正范围或力度越大，过大可能造成跟随不稳定。";
    if (std::strcmp(label, "Active Game Profile") == 0)
        return "选择当前游戏配置。不同配置可以保存不同灵敏度、视野和瞄准参数。";
    if (std::strcmp(label, "Sensitivity") == 0 || std::strcmp(label, "Yaw") == 0 || std::strcmp(label, "Pitch") == 0)
        return "匹配游戏鼠标灵敏度和水平、垂直旋转比例。数值越大，同样的鼠标移动产生的视角变化越大。";
    if (std::strcmp(label, "FOV Scaled") == 0 || std::strcmp(label, "Base FOV") == 0)
        return "根据游戏视野缩放状态修正鼠标移动计算。使用瞄准镜或不同FOV时按游戏设置调整。";
    if (std::strcmp(label, "New profile name") == 0)
        return "输入新游戏配置名称，然后点击右侧按钮创建独立配置。";
    if (std::strcmp(label, "Profile") == 0)
        return "删除当前游戏配置。删除前请确认当前配置不是仍在使用的配置。";
    if (std::strcmp(label, "Easy No Recoil") == 0)
        return "开启后尝试抵消连续射击造成的后坐力。关闭后不会执行后坐力修正。";
    if (std::strcmp(label, "No Recoil Strength") == 0)
        return "后坐力修正强度。数值越大修正越强，过大可能造成反向移动或异常动作。";
    if (std::strcmp(label, "Auto Shoot") == 0)
        return "开启后允许程序自动发送射击按键。关闭后不会自动射击。";
    if (std::strcmp(label, "bScope Multiplier") == 0)
        return "瞄准镜状态下的速度倍率。数值越大瞄准镜内移动越快。";
    if (std::strcmp(label, "Enable WindMouse") == 0)
        return "使用更平滑、带自然扰动的鼠标移动轨迹。开启后可以调整下面的轨迹参数。";
    if (std::strcmp(label, "Gravity force") == 0 || std::strcmp(label, "Wind fluctuation") == 0 ||
        std::strcmp(label, "Max step (velocity clip)") == 0 ||
        std::strcmp(label, "Distance where behaviour changes") == 0)
        return "WindMouse 轨迹参数。增大通常会提高移动速度或扰动，过大可能造成轨迹不稳定。";
    if (std::strcmp(label, "Wind Mouse") == 0)
        return "将 WindMouse 的轨迹参数恢复为默认值。";
    if (std::strcmp(label, "Mouse Input Method") == 0)
        return "选择输出鼠标移动和按键事件的后端。普通用户建议先使用 WIN32。";
    if (std::strcmp(label, "Makcu Port") == 0)
        return "选择硬件鼠标使用的串口。设备连接或端口变化后需要重新连接。";
    if (std::strcmp(label, "Makcu Baudrate") == 0)
        return "选择串口通信速度。必须与硬件固件使用的波特率一致。";
    if (std::strcmp(label, "IP") == 0 || std::strcmp(label, "Port") == 0 || std::strcmp(label, "UUID") == 0 ||
        std::strcmp(label, "PIDVID") == 0)
        return "填写外部鼠标设备的连接参数。必须与设备实际配置一致。";
    if (std::strcmp(label, "Show Preview Window") == 0)
        return "开启捕获画面预览，便于确认当前捕获源是否正确。预览会增加少量性能开销。";
    if (std::strcmp(label, "Debug scale") == 0)
        return "调整调试画面显示比例。数值越大画面越清晰，但占用的面板空间也越多。";
    if (std::strcmp(label, "Screenshot delay") == 0)
        return "截图前等待的时间。数值越大越容易截到界面更新后的画面，但响应会变慢。";
    if (std::strcmp(label, "Verbose console output") == 0)
        return "输出更多调试日志。排查问题时开启，正常使用时关闭可减少日志。";
    if (std::strcmp(label, "Collect data while playing") == 0 ||
        std::strcmp(label, "Only when aimbot is active") == 0 ||
        std::strcmp(label, "Only when targets exist") == 0)
        return "控制数据采集何时运行。开启限制可以减少无效样本和磁盘占用。";
    if (std::strcmp(label, "Save every N frames") == 0)
        return "每隔多少帧保存一个样本。数值越大采集更稀疏，磁盘占用更低。";
    if (std::strcmp(label, "JPEG quality") == 0)
        return "采集图片质量。数值越高画质和文件体积越大。";
    if (std::strcmp(label, "Output folder") == 0 || std::strcmp(label, "Class filter") == 0)
        return "设置数据保存位置或类别筛选。留空类别筛选表示记录所有类别。";
    if (std::strcmp(label, "Write YOLO txt labels") == 0)
        return "是否同时生成 YOLO 格式的标注文本文件。";
    if (std::strcmp(label, "Min confidence") == 0 || std::strcmp(label, "Max boxes per file") == 0)
        return "自动标注筛选参数。阈值越高结果更严格，最大框数量越大可保留更多目标。";
    return nullptr;
}

inline void ShowSettingTooltip(const char* label) noexcept
{
    const char* text = TooltipForSetting(label);
    if (text && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%s", text);
}

inline void ValidateFloatParam(float* value, float minVal, float maxVal, float defaultVal, const char* unit = nullptr) noexcept
{
    if (*value >= minVal && *value <= maxVal)
        return;

    if (unit)
        ImGui::Text("数值不合理 (%.4f ~ %.4f %s)，已恢复默认值 %.4f", minVal, maxVal, unit, defaultVal);
    else
        ImGui::Text("数值不合理 (%.4f ~ %.4f)，已恢复默认值 %.4f", minVal, maxVal, defaultVal);
    *value = defaultVal;
}

inline void ValidateIntParam(int* value, int minVal, int maxVal, int defaultVal, const char* unit = nullptr) noexcept
{
    if (*value >= minVal && *value <= maxVal)
        return;

    if (unit)
        ImGui::Text("数值不合理 (%d ~ %d %s)，已恢复默认值 %d", minVal, maxVal, unit, defaultVal);
    else
        ImGui::Text("数值不合理 (%d ~ %d)，已恢复默认值 %d", minVal, maxVal, defaultVal);
    *value = defaultVal;
}

#endif // OVERLAY_UI_SECTIONS_H
