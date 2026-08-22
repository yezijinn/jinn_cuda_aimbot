#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <thread>
#include <mutex>
#include <unordered_map>

#include "config.h"
#include "keyboard_listener.h"
#include "mouse.h"
#include "mouse/AimbotTarget.h"
#include "keycodes.h"
#include "mybot.h"
#include "capture.h"
#include "runtime/thread_loops.h"

extern std::atomic<bool> shouldExit;
extern std::atomic<bool> aiming;
extern std::atomic<bool> shooting;
extern std::atomic<bool> zooming;
extern std::atomic<bool> detectionPaused;
extern std::atomic<bool> detector_model_changed;

extern MouseThread* globalMouseThread;

const float OFFSET_STEP = 0.01f;
const float NORECOIL_STEP = 5.0f;

// Arrow key vectors
const std::vector<std::string> upArrowKeys = { "UpArrow" };
const std::vector<std::string> downArrowKeys = { "DownArrow" };
const std::vector<std::string> leftArrowKeys = { "LeftArrow" };
const std::vector<std::string> rightArrowKeys = { "RightArrow" };
const std::vector<std::string> shiftKeys = { "LeftShift", "RightShift" };

// Previous key states
thread_local bool prevUpArrow = false;
thread_local bool prevDownArrow = false;
thread_local bool prevLeftArrow = false;
thread_local bool prevRightArrow = false;

namespace
{
struct KeyboardConfigSnapshot
{
    bool autoAim = false;
    bool enableArrowsSettings = false;
    Config::MouseHotkeyContainer mouseHotkeys{};
    std::vector<std::string> buttonTargeting;
    std::vector<std::string> buttonShoot;
    std::vector<std::string> buttonZoom;
    std::vector<std::string> buttonExit;
    std::vector<std::string> buttonPause;
    std::vector<std::string> buttonReloadConfig;
    std::vector<std::string> buttonOpenOverlay;
};

KeyboardConfigSnapshot SnapshotKeyboardConfig()
{
    std::lock_guard<std::mutex> lock(configMutex);
    KeyboardConfigSnapshot snapshot;
    snapshot.autoAim = config.auto_aim;
    snapshot.enableArrowsSettings = config.enable_arrows_settings;
    snapshot.mouseHotkeys = config.mouse_hotkeys;
    snapshot.buttonTargeting = config.button_targeting;
    snapshot.buttonShoot = config.button_shoot;
    snapshot.buttonZoom = config.button_zoom;
    snapshot.buttonExit = config.button_exit;
    snapshot.buttonPause = config.button_pause;
    snapshot.buttonReloadConfig = config.button_reload_config;
    snapshot.buttonOpenOverlay = config.button_open_overlay;
    return snapshot;
}

bool isAimingActiveFromDevices()
{
    std::lock_guard<std::mutex> lock(inputDevicesMutex);
    return activeMouseInputOwner && activeMouseInputOwner->isOpen() && activeMouseInputOwner->aimingActive();
}

bool isShootingActiveFromDevices()
{
    std::lock_guard<std::mutex> lock(inputDevicesMutex);
    return activeMouseInputOwner && activeMouseInputOwner->isOpen() && activeMouseInputOwner->shootingActive();
}

bool isZoomingActiveFromDevices()
{
    std::lock_guard<std::mutex> lock(inputDevicesMutex);
    return activeMouseInputOwner && activeMouseInputOwner->isOpen() && activeMouseInputOwner->zoomingActive();
}

bool isAnyKeyPressedInternal(const std::vector<std::string>& keys)
{
    bool usePhysicalDevice = false;

    std::lock_guard<std::mutex> lock(inputDevicesMutex);

    IMouseInput* input = activeMouseInputOwner.get();
    if (input && input->isOpen() && input->hasPhysicalButtonState())
        usePhysicalDevice = true;

    for (const auto& key_name : keys)
    {
        int key_code = KeyCodes::getKeyCode(key_name);
        bool pressed = false;

        if (input && input->isOpen())
            pressed = input->keyPressed(key_name);

        // Win32 API
        if (!pressed && key_code != -1)
        {
            bool isMouse = (key_name == "LeftMouseButton" ||
                key_name == "RightMouseButton" ||
                key_name == "MiddleMouseButton" ||
                key_name == "X1MouseButton" ||
                key_name == "X2MouseButton");

            if (!isMouse || !usePhysicalDevice)
            {
                pressed = (GetAsyncKeyState(key_code) & 0x8000) != 0;
            }
        }

        if (pressed) return true;
    }
    return false;
}
} // namespace

bool isAnyKeyPressed(const std::vector<std::string>& keys)
{
    return isAnyKeyPressedInternal(keys);
}

bool isAnyKeyPressedWin32Only(const std::vector<std::string>& keys)
{
    for (const auto& key_name : keys)
    {
        int key_code = KeyCodes::getKeyCode(key_name);
        if (key_code != -1 && (GetAsyncKeyState(key_code) & 0x8000))
            return true;
    }
    return false;
}

void keyboardListener()
{
    while (!shouldExit)
    {
        KeyboardConfigSnapshot cfg = SnapshotKeyboardConfig();

        // Mouse hotkeys use one physical-button snapshot and listener-thread-local toggle state.
        std::unordered_map<std::string, bool> pressedButtons;
        for (const auto& hotkey : cfg.mouseHotkeys)
        {
            if (hotkey.id.empty() || !hotkey.enabled)
                continue;

            bool pressed = false;
            for (const auto& button : hotkey.buttons)
            {
                const bool buttonPressed = isAnyKeyPressedInternal({button});
                pressedButtons[button] = buttonPressed;
                pressed = pressed || buttonPressed;
            }
        }

        const Config::MouseHotkey* activeMouseHotkey =
            Config::selectActiveMouseHotkey(cfg.mouseHotkeys, pressedButtons);
        int activeSlot = -1;
        if (activeMouseHotkey != nullptr)
        {
            for (std::size_t i = 0; i < cfg.mouseHotkeys.size(); ++i)
            {
                if (&cfg.mouseHotkeys[i] == activeMouseHotkey)
                {
                    activeSlot = static_cast<int>(i);
                    break;
                }
            }
        }

        int continuousTriggerSlot = -1;
        for (std::size_t i = 0; i < cfg.mouseHotkeys.size(); ++i)
        {
            const auto& hotkey = cfg.mouseHotkeys[i];
            if (hotkey.id.empty() || !hotkey.enabled)
                continue;
            if (hotkey.localBool("trigger_enabled", false) &&
                hotkey.localBool("trigger_enabled_for_hotkey", true) &&
                hotkey.localBool("trigger_continuous", false))
            {
                continuousTriggerSlot = static_cast<int>(i);
                break;
            }
        }

        const bool selectedProfileAutoAim = activeMouseHotkey != nullptr &&
            activeMouseHotkey->localBool("auto_aim", false);
        // 仅当槽 0（默认回退）本身处于启用状态时才采纳其 auto_aim；否则一个被禁用的
        // profile 仍会经后续 fallback 分支令 continuousAim 为真，复活已禁用的瞄准配置。
        const bool slot1AutoAim = cfg.mouseHotkeys[0].enabled &&
            !cfg.mouseHotkeys[0].id.empty() &&
            cfg.mouseHotkeys[0].localBool("auto_aim", false);
        const bool continuousAim = cfg.autoAim || selectedProfileAutoAim ||
            (activeMouseHotkey == nullptr && slot1AutoAim);

        if (activeSlot < 0 && continuousTriggerSlot >= 0)
            activeSlot = continuousTriggerSlot;
        // 先把最终槽位算完，再单次发布。
        // 原实现是"先 store(activeSlot)，若需回退再 store(0)"的两阶段写法，中间会短暂
        // 出现 -1 这个错误值。而 active_mouse_hotkey_slot 的读取方是 1000Hz 级的鼠标线程
        // （mouse_thread_loop.cpp）和检测线程，它们完全可能采样到这个瞬时的 -1，
        // 从而在该帧走"无激活热键"分支 —— 类别使能集合与瞄准偏移随之跳变，
        // 表现为连续瞄准（continuousAim）开启时准星出现随机抖动 / 短暂丢目标。
        // 合并为单次 store 后，外部观测到的永远是最终值，不存在错误中间态。
        if (continuousAim && activeSlot < 0)
        {
            activeSlot = 0;
        }
        active_mouse_hotkey_slot.store(activeSlot, std::memory_order_relaxed);

        // Aiming is driven by the selected profile; legacy targeting remains the fallback.
        if (!continuousAim)
        {
            aiming = activeMouseHotkey != nullptr ||
                (activeMouseHotkey == nullptr && cfg.mouseHotkeys[0].id.empty() &&
                    (isAnyKeyPressedInternal(cfg.buttonTargeting) || isAimingActiveFromDevices()));
        }
        else
        {
            aiming = true;
        }

        // Shooting
        shooting = isAnyKeyPressedInternal(cfg.buttonShoot) ||
            isShootingActiveFromDevices();

        // Zooming
        zooming = isAnyKeyPressedInternal(cfg.buttonZoom) ||
            isZoomingActiveFromDevices();

        // Exit - Win32
        if (isAnyKeyPressedWin32Only(cfg.buttonExit))
        {
            shouldExit = true;
            detectionBuffer.cv.notify_all();
            frameCV.notify_all();
        }

        // Pause detection - Win32
        thread_local bool pausePressed = false;
        if (isAnyKeyPressedWin32Only(cfg.buttonPause))
        {
            if (!pausePressed)
            {
                detectionPaused = !detectionPaused;
                pausePressed = true;
            }
        }
        else
        {
            pausePressed = false;
        }

        // Reload config - Win32
        thread_local bool reloadPressed = false;
        if (isAnyKeyPressedWin32Only(cfg.buttonReloadConfig))
        {
            if (!reloadPressed)
            {
                // 先持锁快照旧值（廉价，无 IO）。
                int oldDetectionResolution = 0;
                int oldCaptureFps = 0;
                std::string oldCaptureMethod;
                std::string oldCaptureTarget;
                std::string oldCaptureWindowTitle;
                int oldMonitorIdx = 0;
                bool oldCaptureBorders = false;
                bool oldCaptureCursor = false;
                std::string oldVirtualCameraName;
                int oldVirtualCameraWidth = 0;
                int oldVirtualCameraHeight = 0;
                std::string oldUdpIp;
                int oldUdpPort = 0;
                std::string oldBackend;
                std::string oldAiModel;
                std::string oldInputMethod;
                {
                    std::lock_guard<std::mutex> lock(configMutex);
                    oldDetectionResolution = config.detection_resolution;
                    oldCaptureFps = config.capture_fps;
                    oldCaptureMethod = config.capture_method;
                    oldCaptureTarget = config.capture_target;
                    oldCaptureWindowTitle = config.capture_window_title;
                    oldMonitorIdx = config.monitor_idx;
                    oldCaptureBorders = config.capture_borders;
                    oldCaptureCursor = config.capture_cursor;
                    oldVirtualCameraName = config.virtual_camera_name;
                    oldVirtualCameraWidth = config.virtual_camera_width;
                    oldVirtualCameraHeight = config.virtual_camera_heigth;
                    oldUdpIp = config.udp_ip;
                    oldUdpPort = config.udp_port;
                    oldBackend = config.backend;
                    oldAiModel = config.ai_model;
                    oldInputMethod = config.input_method;
                }
                // 磁盘 IO（读取 ini + 控制台输出）移到锁外：避免阻塞鼠标线程（≫1000Hz 忙循环）
                // 数十毫秒导致瞄准停摆。loadConfig 期间并发读者可能看到一帧的部分更新，
                // 属可接受的瞬时不一致（原实现持锁则会直接饿死实时线程）。
                config.loadConfig();
                {
                    std::lock_guard<std::mutex> lock(configMutex);
                    if (config.detection_resolution != oldDetectionResolution)
                    {
                        detection_resolution_changed.store(true);
                        detector_model_changed.store(true);
                    }

                    if (config.capture_fps != oldCaptureFps)
                        capture_fps_changed.store(true);

                    if (config.capture_method != oldCaptureMethod ||
                        config.capture_target != oldCaptureTarget ||
                        config.capture_window_title != oldCaptureWindowTitle ||
                        config.monitor_idx != oldMonitorIdx ||
                        config.virtual_camera_name != oldVirtualCameraName ||
                        config.virtual_camera_width != oldVirtualCameraWidth ||
                        config.virtual_camera_heigth != oldVirtualCameraHeight ||
                        config.udp_ip != oldUdpIp ||
                        config.udp_port != oldUdpPort)
                    {
                        capture_method_changed.store(true);
                        capture_window_changed.store(true);
                    }

                    if (config.capture_borders != oldCaptureBorders)
                        capture_borders_changed.store(true);

                    if (config.capture_cursor != oldCaptureCursor)
                        capture_cursor_changed.store(true);

                    if (config.backend != oldBackend || config.ai_model != oldAiModel)
                        detector_model_changed.store(true);

                    if (config.input_method != oldInputMethod)
                        input_method_changed.store(true);

                    if (globalMouseThread)
                    {
                        globalMouseThread->updateConfig(
                            config.detection_resolution,
                            config.fovX,
                            config.fovY,
                            config.minSpeedMultiplier,
                            config.maxSpeedMultiplier,
                            config.predictionInterval,
                            config.auto_shoot,
                            config.bScope_multiplier
                        );
                    }
                }
                reloadPressed = true;
            }
        }
        else
        {
            reloadPressed = false;
        }

        // Open overlay - Win32
        thread_local bool overlayPressed = false;
        if (isAnyKeyPressedWin32Only(cfg.buttonOpenOverlay))
        {
            if (!overlayPressed)
            {
                overlayPressed = true;
            }
        }
        else
        {
            overlayPressed = false;
        }

        // Arrow key detection - Win32
        bool upArrow = isAnyKeyPressedWin32Only(upArrowKeys);
        bool downArrow = isAnyKeyPressedWin32Only(downArrowKeys);
        bool leftArrow = isAnyKeyPressedWin32Only(leftArrowKeys);
        bool rightArrow = isAnyKeyPressedWin32Only(rightArrowKeys);
        bool shiftKey = isAnyKeyPressedWin32Only(shiftKeys);

        // Adjust the active hotkey's enabled classes so tuning matches runtime targeting.
        if (cfg.enableArrowsSettings)
        {
            const TargetClassConfigKeys& classKeys = targetClassConfigKeys();
            // 复用本轮图 209 已计算并发布的 activeSlot，不在锁外从原子量重新加载。
            // 原写法「锁外 load(activeSlot) → 锁内用 config.mouse_hotkeys[activeSlot]」存在
            // 时间窗歧义（load 与加锁之间配置可能被本线程 reload 路径刷新），且让同一
            // 槽位判定出现两个取值来源。改用局部变量保证与循环其余部分同源、单来源判定。
            const bool hasActiveHotkey = activeSlot >= 0 &&
                activeSlot < static_cast<int>(Config::MAX_MOUSE_HOTKEYS);
            if (upArrow && !prevUpArrow)
            {
                std::lock_guard<std::mutex> lock(configMutex);
                if (hasActiveHotkey)
                {
                    auto& profile = config.mouse_hotkeys[static_cast<std::size_t>(activeSlot)];
                    for (int cls = 0; cls < Config::FIXED_TARGET_CLASS_COUNT; ++cls)
                    {
                        if (config.isClassEnabled(cls) && profile.localBool(classKeys.enabled[cls], false))
                        {
                            profile.setLocalFloat(classKeys.aimOffsetY[cls], std::max(0.0f, profile.localFloat(classKeys.aimOffsetY[cls], 0.5f) - OFFSET_STEP));
                        }
                    }
                }
            }
            if (downArrow && !prevDownArrow)
            {
                std::lock_guard<std::mutex> lock(configMutex);
                if (hasActiveHotkey)
                {
                    auto& profile = config.mouse_hotkeys[static_cast<std::size_t>(activeSlot)];
                    for (int cls = 0; cls < Config::FIXED_TARGET_CLASS_COUNT; ++cls)
                    {
                        if (config.isClassEnabled(cls) && profile.localBool(classKeys.enabled[cls], false))
                        {
                            profile.setLocalFloat(classKeys.aimOffsetY[cls], std::min(1.0f, profile.localFloat(classKeys.aimOffsetY[cls], 0.5f) + OFFSET_STEP));
                        }
                    }
                }
            }
            if (leftArrow && !prevLeftArrow)
            {
                std::lock_guard<std::mutex> lock(configMutex);
                if (hasActiveHotkey)
                {
                    auto& profile = config.mouse_hotkeys[static_cast<std::size_t>(activeSlot)];
                    for (int cls = 0; cls < Config::FIXED_TARGET_CLASS_COUNT; ++cls)
                    {
                        if (config.isClassEnabled(cls) && profile.localBool(classKeys.enabled[cls], false))
                        {
                            profile.setLocalFloat(classKeys.aimOffsetX[cls], std::max(0.0f, profile.localFloat(classKeys.aimOffsetX[cls], 0.5f) - OFFSET_STEP));
                        }
                    }
                }
            }
            if (rightArrow && !prevRightArrow)
            {
                std::lock_guard<std::mutex> lock(configMutex);
                if (hasActiveHotkey)
                {
                    auto& profile = config.mouse_hotkeys[static_cast<std::size_t>(activeSlot)];
                    for (int cls = 0; cls < Config::FIXED_TARGET_CLASS_COUNT; ++cls)
                    {
                        if (config.isClassEnabled(cls) && profile.localBool(classKeys.enabled[cls], false))
                        {
                            profile.setLocalFloat(classKeys.aimOffsetX[cls], std::min(1.0f, profile.localFloat(classKeys.aimOffsetX[cls], 0.5f) + OFFSET_STEP));
                        }
                    }
                }
            }
        }
        
        // Update previous key states
        prevUpArrow = upArrow;
        prevDownArrow = downArrow;
        prevLeftArrow = leftArrow;
        prevRightArrow = rightArrow;

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
