#pragma once

#include "storage.h"

#include <array>
#include <atomic>
#include <future>
#include <thread>

namespace mcs {

inline constexpr UINT kCopyKey = WM_APP + 20;
inline constexpr UINT kShowPicker = WM_APP + 21;
inline constexpr UINT kMovePicker = WM_APP + 22;
inline constexpr UINT kFinishPicker = WM_APP + 23;
inline constexpr UINT kPasteSlot = WM_APP + 24;
inline constexpr ULONG_PTR kPasteExtraInfo = 0x4D435350;

class InputMonitor {
public:
    ~InputMonitor();
    bool Start(HWND receiver);
    void Stop();
    void Configure(const Settings& settings, unsigned savedSlots);

private:
    void Run(std::promise<bool> ready);
    bool HandleKey(const KBDLLHOOKSTRUCT& key, bool isDown);
    void FinishPicker(bool paste);
    void UpdateMouseHook();
    UINT Modifiers() const;
    static LRESULT CALLBACK KeyboardProc(int code, WPARAM message, LPARAM data);
    static LRESULT CALLBACK MouseProc(int code, WPARAM message, LPARAM data);

    static thread_local InputMonitor* current_;
    HWND receiver_ = nullptr;
    std::thread thread_;
    DWORD threadId_ = 0;
    HHOOK mouseHook_ = nullptr;
    std::array<std::atomic<DWORD>, kFileShortcutIndex + 1> shortcuts_{};
    std::atomic<unsigned> savedSlots_{0};
    std::atomic<bool> paused_{false};
    std::array<bool, 256> down_{};
    std::array<bool, 256> swallowed_{};
    bool picking_ = false;
    UINT pickerKey_ = 0;
    UINT pickerModifiers_ = 0;
    int wheelRemainder_ = 0;
    int pendingSlot_ = -1;
    HWND slotTarget_ = nullptr;
};

}
