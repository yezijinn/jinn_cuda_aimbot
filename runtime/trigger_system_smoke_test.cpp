#include "runtime/trigger_system.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace
{
[[noreturn]] void fail(const char* message)
{
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void expect(bool condition, const char* message)
{
    if (!condition)
        fail(message);
}

TriggerConfig activeConfig()
{
    TriggerConfig config;
    config.enabled = true;
    config.continuous = true;
    config.zone_offset_x = 0.0f;
    config.zone_offset_y = 0.0f;
    config.zone_size_x = 1.0f;
    config.zone_size_y = 1.0f;
    config.key_delay_ms = 20;
    config.pre_fire_delay_ms = 20;
    config.fire_duration_ms = 30;
    config.fire_duration_random_ms = 0;
    config.cooldown_ms = 20;
    config.cooldown_random_ms = 0;
    return config;
}

bool update(TriggerSystem& system, const float* box)
{
    return system.update(box, 0, 100, 100);
}

bool waitForFiring(TriggerSystem& system, const float* box, int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (update(system, box))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
}

int main()
{
    const float inZoneBox[] = { 0.0f, 0.0f, 100.0f, 100.0f };
    TriggerSystem system;
    TriggerConfig config = activeConfig();
    system.updateConfig(config, TriggerConfig{}, TriggerConfig{});

    expect(!update(system, inZoneBox), "key delay begins without firing");
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    expect(!update(system, inZoneBox), "key delay completes without firing");
    expect(!update(system, inZoneBox), "pre-fire delay begins without firing");
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    expect(update(system, inZoneBox), "pre-fire delay completes and fires");
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    expect(!update(system, inZoneBox), "fire duration ends firing");
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    expect(!update(system, inZoneBox), "cooldown completes without immediately firing");

    config.key_delay_ms = 0;
    config.pre_fire_delay_ms = 0;
    config.fire_duration_ms = 10;
    config.fire_duration_random_ms = 30;
    config.cooldown_ms = 10;
    config.cooldown_random_ms = 30;
    system.resetAll();
    system.updateConfig(config, TriggerConfig{}, TriggerConfig{});
    expect(!update(system, inZoneBox), "armed before randomized timing test");
    expect(waitForFiring(system, inZoneBox, 20), "starts randomized firing");
    std::this_thread::sleep_for(std::chrono::milliseconds(45));
    expect(!update(system, inZoneBox), "randomized fire duration is bounded by configured maximum");
    std::this_thread::sleep_for(std::chrono::milliseconds(45));
    expect(!update(system, inZoneBox), "randomized cooldown is bounded by configured maximum");

    config.key_delay_ms = 0;
    config.pre_fire_delay_ms = 0;
    config.fire_duration_ms = 1000;
    config.cooldown_ms = 0;
    config.stop_fire_on_loss = true;
    config.stop_fire_delay_ms = 30;
    system.resetAll();
    system.updateConfig(config, TriggerConfig{}, TriggerConfig{});
    expect(!update(system, inZoneBox), "armed before immediate fire");
    expect(update(system, inZoneBox), "fires before target loss");
    expect(update(system, nullptr), "holds fire during target-loss delay");
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    expect(!update(system, nullptr), "stops firing after target-loss delay");

    config.stop_fire_on_loss = false;
    system.resetAll();
    system.updateConfig(config, TriggerConfig{}, TriggerConfig{});
    expect(!update(system, inZoneBox), "armed before disabled-loss test");
    expect(update(system, inZoneBox), "fires before disabled-loss test");
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    expect(update(system, nullptr), "continues firing when target-loss stop is disabled");

    std::cout << "PASS: trigger timing and target-loss behavior\n";
}
