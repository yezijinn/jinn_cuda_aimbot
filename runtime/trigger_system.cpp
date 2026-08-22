#include "trigger_system.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr int kKeysTargetingBit = 1 << 0;

std::mt19937& triggerRng()
{
    thread_local std::mt19937 rng(std::random_device{}());
    return rng;
}
constexpr int kKeysShootBit     = 1 << 1;
constexpr int kKeysZoomBit      = 1 << 2;

int slotBit(TriggerSlotId slot) {
    switch (slot) {
        case TriggerSlotId::Targeting: return kKeysTargetingBit;
        case TriggerSlotId::Shoot:     return kKeysShootBit;
        case TriggerSlotId::Zoom:      return kKeysZoomBit;
        default:                       return 0;
    }
}
} // namespace

bool calculateTriggerZone(const float* box,
                          int detectionWidth,
                          int detectionHeight,
                          float offsetX,
                          float offsetY,
                          float sizeX,
                          float sizeY,
                          TriggerZone& outZone)
{
    if (!box || detectionWidth <= 0 || detectionHeight <= 0 ||
        !std::isfinite(box[0]) || !std::isfinite(box[1]) ||
        !std::isfinite(box[2]) || !std::isfinite(box[3]) ||
        !std::isfinite(offsetX) || !std::isfinite(offsetY) ||
        !std::isfinite(sizeX) || !std::isfinite(sizeY) ||
        box[2] <= 0.0f || box[3] <= 0.0f || sizeX <= 0.0f || sizeY <= 0.0f)
        return false;

    const float clampedOffsetX = std::clamp(offsetX, 0.0f, 1.0f);
    const float clampedOffsetY = std::clamp(offsetY, 0.0f, 1.0f);
    const float clampedSizeX = std::clamp(sizeX, 0.01f, 1.0f);
    const float clampedSizeY = std::clamp(sizeY, 0.01f, 1.0f);
    const float left = box[0] + box[2] * clampedOffsetX;
    const float top = box[1] + box[3] * clampedOffsetY;

    outZone.left = std::clamp(left, box[0], box[0] + box[2]);
    outZone.top = std::clamp(top, box[1], box[1] + box[3]);
    outZone.right = std::clamp(outZone.left + box[2] * clampedSizeX, outZone.left, box[0] + box[2]);
    outZone.bottom = std::clamp(outZone.top + box[3] * clampedSizeY, outZone.top, box[1] + box[3]);
    return std::isfinite(outZone.left) && std::isfinite(outZone.top) &&
           std::isfinite(outZone.right) && std::isfinite(outZone.bottom);
}

bool isCrosshairInsideTriggerZone(const TriggerZone& zone,
                                  int detectionWidth,
                                  int detectionHeight)
{
    if (detectionWidth <= 0 || detectionHeight <= 0 ||
        !std::isfinite(zone.left) || !std::isfinite(zone.top) ||
        !std::isfinite(zone.right) || !std::isfinite(zone.bottom) ||
        zone.left > zone.right || zone.top > zone.bottom)
        return false;
    const float cx = detectionWidth * 0.5f;
    const float cy = detectionHeight * 0.5f;
    return cx >= zone.left && cx <= zone.right &&
           cy >= zone.top && cy <= zone.bottom;
}

bool TriggerSystem::isTargetInTriggerZone(const float* box, int detectionResolution,
                                          float offsetX, float offsetY,
                                          float sizeX, float sizeY) {
    TriggerZone zone;
    return calculateTriggerZone(box, detectionResolution, detectionResolution,
                                offsetX, offsetY, sizeX, sizeY, zone) &&
           isCrosshairInsideTriggerZone(zone, detectionResolution, detectionResolution);
}

void TriggerSystem::runSlotStateMachine(TriggerSlotId slot,
                                        const float* activeTargetBox,
                                        bool slotActive, int detectionWidth,
                                        int detectionHeight) {
    const int idx = static_cast<int>(slot);
    const TriggerConfig& cfg = configs_[idx];
    TriggerSlotState& st = states_[idx];
    const auto now = std::chrono::steady_clock::now();

    if (!slotActive) {
        // Slot deactivated → return to idle
        if (st.phase != TriggerPhase::Idle) {
            st.reset();
        }
        return;
    }

    // Check if the crosshair is inside this slot's zone in detection coordinates.
    TriggerZone zone;
    const bool inZone = calculateTriggerZone(
        activeTargetBox, detectionWidth, detectionHeight,
        cfg.zone_offset_x, cfg.zone_offset_y,
        cfg.zone_size_x, cfg.zone_size_y, zone) &&
        isCrosshairInsideTriggerZone(zone, detectionWidth, detectionHeight);

    // Target loss detection
    if (cfg.stop_fire_on_loss) {
        if (!inZone && !st.targetLost) {
            st.targetLost = true;
            st.targetLostTime = now;
        } else if (inZone) {
            st.targetLost = false;
        }
    }

    bool stopFireDueToLoss = false;
    if (cfg.stop_fire_on_loss && st.targetLost) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - st.targetLostTime).count();
        if (elapsed >= cfg.stop_fire_delay_ms) {
            stopFireDueToLoss = true;
        }
    }

    st.targetInZone = inZone;

    switch (st.phase) {
    case TriggerPhase::Idle:
        if (!inZone) {
            break;
        }
        if (cfg.key_delay_ms > 0) {
            st.phase = TriggerPhase::KeyDelay;
            st.phaseStart = now;
        } else {
            st.phase = TriggerPhase::Armed;
            st.phaseStart = now;
        }
        break;

    case TriggerPhase::KeyDelay: {
        if (!inZone) {
            // The delay is valid only while the crosshair remains in the zone.
            st.reset();
            break;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - st.phaseStart).count();
        if (elapsed >= cfg.key_delay_ms) {
            st.phase = TriggerPhase::Armed;
            st.phaseStart = now;
        }
        break;
    }

    case TriggerPhase::Armed:
        if (inZone) {
            if (cfg.pre_fire_delay_ms > 0) {
                st.phase = TriggerPhase::PreFireDelay;
                st.phaseStart = now;
            } else {
            // Start firing immediately
            // fire_duration_random_ms 理论上应由上层钳为非负（mouse_thread_loop 已 clamp），
            // 但 TriggerSystem 自身 API 无防御：若直接 updateConfig 传入负值，
            // uniform_int_distribution(0, 负) 是未定义行为。此处以 max(0,..) 兜底，
            // 与零 random 行为一致（random=0 时 durDist 恒返回 0），不影响合法输入。
            std::uniform_int_distribution<int> durDist(0, std::max(0, cfg.fire_duration_random_ms));
            st.activeDurationMs = cfg.fire_duration_ms + durDist(triggerRng());
                st.phase = TriggerPhase::Firing;
                st.phaseStart = now;
            }
        }
        break;

    case TriggerPhase::PreFireDelay: {
        if (!inZone) {
            // Target left zone before pre-fire completed
            st.phase = TriggerPhase::Armed;
            st.phaseStart = now;
            break;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - st.phaseStart).count();
        if (elapsed >= cfg.pre_fire_delay_ms) {
            std::uniform_int_distribution<int> durDist(0, std::max(0, cfg.fire_duration_random_ms));
            st.activeDurationMs = cfg.fire_duration_ms + durDist(triggerRng());
            st.phase = TriggerPhase::Firing;
            st.phaseStart = now;
        }
        break;
    }

    case TriggerPhase::Firing: {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - st.phaseStart).count();

        // Stop if target lost for too long
        if (stopFireDueToLoss) {
            st.targetLost = false;
            st.phase = TriggerPhase::Armed;
            st.phaseStart = now;
            break;
        }

        // Stop if duration exceeded
        if (elapsed >= st.activeDurationMs) {
            std::uniform_int_distribution<int> cdDist(0, std::max(0, cfg.cooldown_random_ms));
            st.activeCooldownMs = cfg.cooldown_ms + cdDist(triggerRng());
            if (st.activeCooldownMs > 0) {
                st.phase = TriggerPhase::Cooldown;
                st.phaseStart = now;
            } else {
                st.phase = TriggerPhase::Armed;
                st.phaseStart = now;
            }
        }
        break;
    }

    case TriggerPhase::Cooldown: {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - st.phaseStart).count();
        if (elapsed >= st.activeCooldownMs) {
            st.phase = TriggerPhase::Armed;
            st.phaseStart = now;
        }
        break;
    }
    }
}

bool TriggerSystem::update(const float* activeTargetBox,
                           int keysHeld, int detectionWidth,
                           int detectionHeight) {
    // ── Continuous trigger management ──
    // Detect key press edges
    int keysPressed = keysHeld & ~prevKeysHeld_;
    int keysReleased = prevKeysHeld_ & ~keysHeld;
    prevKeysHeld_ = keysHeld;

    // Find which slot has continuous mode enabled and is configured
    int desiredContinuous = -1;
    for (int i = 0; i < static_cast<int>(TriggerSlotId::Count); ++i) {
        if (configs_[i].enabled && configs_[i].continuous) {
            desiredContinuous = i;
            break; // first one wins
        }
    }

    // Update continuous slot assignment
    if (desiredContinuous != continuousSlot_) {
        // Reset old continuous slot
        if (continuousSlot_ >= 0) {
            states_[continuousSlot_].reset();
        }
        continuousSlot_ = desiredContinuous;
    }

    // If a non-continuous key is pressed, temporarily disable continuous
    if (continuousSlot_ >= 0) {
        int contBit = 0;
        switch (static_cast<TriggerSlotId>(continuousSlot_)) {
            case TriggerSlotId::Targeting: contBit = kKeysTargetingBit; break;
            case TriggerSlotId::Shoot:     contBit = kKeysShootBit;     break;
            case TriggerSlotId::Zoom:      contBit = kKeysZoomBit;      break;
            default: break;
        }
        // Check if any OTHER key was pressed
        int otherKeysPressed = keysPressed & ~contBit;
        if (otherKeysPressed) {
            continuousTemporarilyDisabled_ = true;
        }
        // Restore continuous when all other keys are released
        int otherKeysHeld = keysHeld & ~contBit;
        if (continuousTemporarilyDisabled_ && otherKeysHeld == 0) {
            continuousTemporarilyDisabled_ = false;
        }
    } else {
        continuousTemporarilyDisabled_ = false;
    }

    // ── Run state machine for each slot ──
    bool anySlotFiring = false;

    for (int i = 0; i < static_cast<int>(TriggerSlotId::Count); ++i) {
        TriggerSlotId slot = static_cast<TriggerSlotId>(i);
        const TriggerConfig& cfg = configs_[i];

        if (!cfg.enabled) {
            if (states_[i].phase != TriggerPhase::Idle) {
                states_[i].reset();
            }
            continue;
        }

        int bit = slotBit(slot);
        bool keyHeld = (keysHeld & bit) != 0;

        // Continuous mode: slot is active when continuous and not temporarily disabled
        bool continuousActive = (continuousSlot_ == i) && !continuousTemporarilyDisabled_;
        bool slotActive = keyHeld || continuousActive;

        runSlotStateMachine(slot, activeTargetBox, slotActive, detectionWidth, detectionHeight);

        if (states_[i].phase == TriggerPhase::Firing) {
            anySlotFiring = true;
        }
    }

    return anySlotFiring;
}

void TriggerSystem::resetAll() {
    for (int i = 0; i < static_cast<int>(TriggerSlotId::Count); ++i) {
        states_[i].reset();
    }
    continuousSlot_ = -1;
    continuousTemporarilyDisabled_ = false;
    prevKeysHeld_ = 0;
}
