#pragma once

#include <windows.h>

namespace mcs {

inline constexpr wchar_t kResidentWindowClass[] = L"MultiCopySystem.Resident";
inline constexpr wchar_t kSettingsWindowClass[] = L"MultiCopySystem.Settings";
inline constexpr wchar_t kResidentExeName[] = L"MultiCopySystem.exe";
inline constexpr wchar_t kSettingsExeName[] = L"MultiCopySettings.exe";

inline constexpr UINT kRequestHistory = WM_APP + 1;
inline constexpr UINT kDeleteHistoryItem = WM_APP + 2;
inline constexpr UINT kClearHistory = WM_APP + 3;
inline constexpr UINT kReloadSettings = WM_APP + 4;
inline constexpr UINT kResetHistory = WM_APP + 5;
inline constexpr UINT kHistoryChanged = WM_APP + 6;
inline constexpr ULONG_PTR kHistoryPayload = 0x4D435348;

struct HistoryRow {
    UINT id;
    UINT kind;
    wchar_t label[257];
};

struct HistorySnapshot {
    UINT count;
    HistoryRow rows[20];
};

}
