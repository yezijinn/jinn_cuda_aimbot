#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "imgui/imgui.h"
#include "mybot.h"
#include "overlay.h"
#include "overlay/config_dirty.h"
#include "overlay/ui_sections.h"

namespace
{
    std::filesystem::path ConfigDirectory()
    {
        wchar_t exePath[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (length > 0 && length < MAX_PATH)
            return std::filesystem::path(exePath).parent_path();

        std::error_code ec;
        const auto dir = std::filesystem::current_path(ec);
        return ec ? std::filesystem::path(".") : dir;
    }

    std::string ToUtf8(const std::filesystem::path& path)
    {
#ifdef _WIN32
        const std::wstring wide = path.wstring();
        if (wide.empty())
            return {};

        const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                               nullptr, 0, nullptr, nullptr);
        if (length <= 0)
            return {};

        std::string out(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                            out.data(), length, nullptr, nullptr);
        return out;
#else
        return path.u8string();
#endif
    }

    struct ConfigFileEntry
    {
        std::string display;
        std::filesystem::path path;
    };

    double& ConfigFileCacheTimestamp()
    {
        static double lastRefreshAt = -1.0;
        return lastRefreshAt;
    }

    void InvalidateConfigFileCache()
    {
        ConfigFileCacheTimestamp() = -1.0;
    }

    const std::vector<ConfigFileEntry>& RefreshConfigFileList()
    {
        static std::vector<ConfigFileEntry> files;
        const double now = ImGui::GetTime();
        if (ConfigFileCacheTimestamp() >= 0.0 && (now - ConfigFileCacheTimestamp()) < 2.0)
            return files;
        ConfigFileCacheTimestamp() = now;
        files.clear();

        std::error_code ec;
        const auto dir = ConfigDirectory();
        std::filesystem::directory_iterator it(dir, ec), end;
        for (; it != end; it.increment(ec))
        {
            if (ec)
                break;

            const auto& entry = it->path();
            const std::string filename = entry.filename().string();
            if (filename == "config.ini")
            {
                files.push_back({ToUtf8(entry.filename()), entry});
                continue;
            }

            if (filename.rfind("config_", 0) != 0 || filename.size() <= 7)
                continue;

            const std::size_t iniPos = filename.rfind(".ini");
            if (iniPos == std::string::npos || iniPos + 4 != filename.size())
                continue;

            files.push_back({ToUtf8(entry.filename()), entry});
        }

        std::sort(files.begin(), files.end(), [](const ConfigFileEntry& lhs, const ConfigFileEntry& rhs)
        {
            return lhs.display < rhs.display;
        });
        return files;
    }

    int CurrentConfigIndex(const std::vector<ConfigFileEntry>& files)
    {
        const std::filesystem::path current = config.configPath().lexically_normal();
        for (std::size_t i = 0; i < files.size(); ++i)
        {
            if (files[i].path.lexically_normal() == current)
                return static_cast<int>(i);
        }
        return -1;
    }

    std::string SanitizeProfileName(const char* command)
    {
        if (!command)
            return {};

        std::string out;
        out.reserve(std::strlen(command));
        for (const unsigned char c : std::string(command))
        {
            if (c == ' ' || c == '\t')
            {
                if (!out.empty())
                    out.push_back('_');
                continue;
            }

            if (c < 0x20 || c == 0x7f)
                continue;

            const char cc = static_cast<char>(c);
            if (std::strchr("<>:\"/\\|?*", cc) != nullptr)
                continue;

            out.push_back(cc);
        }

        while (!out.empty() && (out.back() == '.' || out.back() == ' ' || out.back() == '_'))
            out.pop_back();

        if (out == "." || out == "..")
            return {};
        return out;
    }

    void SaveProfile(const char* command)
    {
        OverlayConfig_MarkDirty();
        const std::string profile = SanitizeProfileName(command);
        const std::string target = profile.empty()
            ? "config.ini"
            : "config_" + profile + ".ini";
        const auto file = ConfigDirectory() / target;
        OverlayConfig_SaveNow(file.string().c_str());
        InvalidateConfigFileCache();
    }
}

void draw_overlay()
{
    ImGui::PushID("overlay_section_config");
    ImGui::SeparatorText("保存配置");

    const auto& files = RefreshConfigFileList();
    int current = CurrentConfigIndex(files);
    std::vector<const char*> items;
    items.reserve(files.size());
    for (const auto& file : files)
        items.push_back(file.display.c_str());

    ImGui::SetNextItemWidth(UiLayout::kActionButtonWidth);
    if (ImGui::Combo("加载配置文件", &current, items.data(), static_cast<int>(items.size()), -1))
    {
        if (current >= 0 && current < static_cast<int>(files.size()))
        {
            if (config.loadConfig(files[current].path.string()))
                Overlay_ApplyCaptureExclusion();
            OverlayConfig_ClearDirty();
        }
    }
    ShowSettingTooltip("加载配置文件");

    static char commandProfile[128] = {};
    ImGui::SetNextItemWidth(UiLayout::kActionButtonWidth);
    ImGui::InputText("命令配置文件", commandProfile, sizeof(commandProfile));
    const std::string sanitizedProfile = SanitizeProfileName(commandProfile);
    const std::size_t sanitizedSize = std::min(sanitizedProfile.size(), sizeof(commandProfile) - 1);
    if (sanitizedProfile != commandProfile)
    {
        std::memcpy(commandProfile, sanitizedProfile.data(), sanitizedSize);
        commandProfile[sanitizedSize] = '\0';
    }
    ShowSettingTooltip("命令配置文件");
    const auto target = sanitizedProfile.empty() ? (ConfigDirectory() / "config.ini") :
                                                   (ConfigDirectory() / ("config_" + sanitizedProfile + ".ini"));
    const std::string preview = "将保存为 " + ToUtf8(target.filename());
    ImGui::TextUnformatted(preview.c_str());

    if (ImGui::Button("保存配置文件", ImVec2(UiLayout::kActionButtonWidth, 0.0f)))
        SaveProfile(commandProfile);
    ShowSettingTooltip("保存配置文件");

    ImGui::Spacing();
    ImGui::SeparatorText("界面设置");
    if (ImGui::Checkbox("从捕获中隐藏面板", &config.overlay_exclude_from_capture))
    {
        Overlay_ApplyCaptureExclusion();
        OverlayConfig_MarkDirty();
    }
    ShowSettingTooltip("从捕获中隐藏面板");
    ImGui::Spacing();
    ImGui::PopID();
}
