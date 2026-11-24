#pragma once

#include "clipboard.h"

#include <array>
#include <filesystem>

namespace mcs {

struct Shortcut {
    UINT modifiers = 0;
    UINT key = 0;
};

inline constexpr std::size_t kFileShortcutIndex = 4;

struct Settings {
    int previewLimit = 24;
    bool bottomRight = false;
    bool paused = false;
    std::array<Shortcut, kFileShortcutIndex + 1> shortcuts{{
        {MOD_CONTROL | MOD_SHIFT, L'V'},
        {MOD_CONTROL, L'1'},
        {MOD_CONTROL, L'2'},
        {MOD_CONTROL, L'3'},
        {MOD_CONTROL | MOD_SHIFT, L'F'},
    }};
};

Settings LoadSettings();
bool SaveSettings(const Settings& settings);
std::shared_ptr<const ClipData> LoadSlot(int index);
bool SaveSlot(int index, const ClipData* clip);
bool ClearStoredData();
bool IsStartOnSignInEnabled();
bool SetStartOnSignIn(bool enabled, const std::filesystem::path& residentExe);

} // namespace mcs
