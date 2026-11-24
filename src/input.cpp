#include "input.h"
#include "ipc.h"

#include <algorithm>
#include <utility>

namespace mcs {
namespace {

constexpr UINT kUpdateMouseHook = WM_APP + 40;

UINT NormalizeKey(const KBDLLHOOKSTRUCT& key) {
    switch (key.vkCode) {
    case VK_CONTROL:
        return (key.flags & LLKHF_EXTENDED) ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU:
        return (key.flags & LLKHF_EXTENDED) ? VK_RMENU : VK_LMENU;
    case VK_SHIFT:
        return key.scanCode == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    default:
        return key.vkCode;
    }
}

bool IsSettingsWindow(HWND foreground) {
    wchar_t className[64]{};
    const HWND owner = GetAncestor(foreground, GA_ROOTOWNER);
    return GetClassNameW(owner, className, static_cast<int>(std::size(className))) &&
           std::wstring_view(className) == kSettingsWindowClass;
}

}

thread_local InputMonitor* InputMonitor::current_ = nullptr;

InputMonitor::~InputMonitor() {
    Stop();
}

bool InputMonitor::Start(HWND receiver) {
    receiver_ = receiver;
    std::promise<bool> ready;
    auto started = ready.get_future();
    thread_ = std::thread(&InputMonitor::Run, this, std::move(ready));
    if (!started.get()) {
        thread_.join();
        return false;
    }
    return true;
}

void InputMonitor::Stop() {
    if (thread_.joinable()) {
        PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
        thread_.join();
    }
}

void InputMonitor::Configure(const Settings& settings, unsigned savedSlots) {
    for (size_t index = 0; index < shortcuts_.size(); ++index) {
        const auto [modifiers, key] = settings.shortcuts[index];
        shortcuts_[index].store(MAKELONG(key, modifiers), std::memory_order_relaxed);
    }
    paused_.store(settings.paused, std::memory_order_relaxed);
    savedSlots_.store(savedSlots, std::memory_order_relaxed);
}

void InputMonitor::Run(std::promise<bool> ready) {
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    threadId_ = GetCurrentThreadId();
    current_ = this;
    for (const UINT key : {VK_LCONTROL, VK_RCONTROL, VK_LSHIFT, VK_RSHIFT,
                           VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN}) {
        down_[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
    }

    const HHOOK keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, GetModuleHandleW(nullptr), 0);
    ready.set_value(keyboard != nullptr);
    if (keyboard) {
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (message.message == kUpdateMouseHook) {
                UpdateMouseHook();
            }
        }
        if (mouseHook_) {
            UnhookWindowsHookEx(mouseHook_);
        }
        UnhookWindowsHookEx(keyboard);
    }
    current_ = nullptr;
}

UINT InputMonitor::Modifiers() const {
    UINT modifiers = 0;
    if (down_[VK_LCONTROL] || down_[VK_RCONTROL]) modifiers |= MOD_CONTROL;
    if (down_[VK_LSHIFT] || down_[VK_RSHIFT]) modifiers |= MOD_SHIFT;
    if (down_[VK_LMENU] || down_[VK_RMENU]) modifiers |= MOD_ALT;
    if (down_[VK_LWIN] || down_[VK_RWIN]) modifiers |= MOD_WIN;
    return modifiers;
}

bool InputMonitor::HandleKey(const KBDLLHOOKSTRUCT& key, bool isDown) {
    const UINT vk = NormalizeKey(key);
    if (vk >= down_.size() || ((key.flags & LLKHF_INJECTED) && key.dwExtraInfo == kPasteExtraInfo)) {
        return false;
    }

    const bool repeat = down_[vk];
    down_[vk] = isDown;
    const UINT modifiers = Modifiers();
    if (!isDown) {
        const bool swallowed = std::exchange(swallowed_[vk], false);
        if (modifiers == 0 && std::none_of(swallowed_.begin(), swallowed_.end(), [](bool state) { return state; })) {
            if (picking_) {
                FinishPicker(true);
            }
            if (pendingSlot_ >= 0) {
                PostMessageW(receiver_, kPasteSlot, static_cast<WPARAM>(pendingSlot_), reinterpret_cast<LPARAM>(slotTarget_));
                pendingSlot_ = -1;
            }
        }
        return swallowed;
    }

    if (picking_) {
        int direction = 0;
        if (vk == VK_UP || vk == VK_LEFT) direction = -1;
        if (vk == VK_DOWN || vk == VK_RIGHT || vk == pickerKey_) direction = 1;
        if (vk == VK_ESCAPE || direction != 0) {
            swallowed_[vk] = true;
            if (vk == VK_ESCAPE) {
                FinishPicker(false);
            } else if (vk != pickerKey_ || (!repeat && modifiers == pickerModifiers_)) {
                PostMessageW(receiver_, kMovePicker, static_cast<WPARAM>(direction), 0);
            }
            return true;
        }
    }
    if (repeat) {
        return swallowed_[vk];
    }

    if (modifiers == MOD_CONTROL && (vk == 'C' || vk == 'X') && !paused_.load(std::memory_order_relaxed)) {
        PostMessageW(receiver_, kCopyKey, GetClipboardSequenceNumber(), reinterpret_cast<LPARAM>(GetForegroundWindow()));
    }

    if (picking_ || modifiers == 0) {
        return false;
    }
    const DWORD chord = MAKELONG(vk, modifiers);
    for (size_t index = 0; index < shortcuts_.size(); ++index) {
        if (shortcuts_[index].load(std::memory_order_relaxed) != chord) {
            continue;
        }
        const HWND target = GetForegroundWindow();
        if (!target || IsSettingsWindow(target)) {
            return false;
        }
        if (index == 0 || index == kFileShortcutIndex) {
            picking_ = true;
            pickerKey_ = vk;
            pickerModifiers_ = modifiers;
            wheelRemainder_ = 0;
            pendingSlot_ = -1;
            PostMessageW(receiver_, kShowPicker, index == kFileShortcutIndex, reinterpret_cast<LPARAM>(target));
            PostThreadMessageW(threadId_, kUpdateMouseHook, 0, 0);
        } else {
            if ((savedSlots_.load(std::memory_order_relaxed) & (1u << (index - 1))) == 0) {
                return false;
            }
            pendingSlot_ = static_cast<int>(index - 1);
            slotTarget_ = target;
        }
        swallowed_[vk] = true;
        return true;
    }
    return false;
}

void InputMonitor::FinishPicker(bool paste) {
    picking_ = false;
    PostMessageW(receiver_, kFinishPicker, paste, 0);
    PostThreadMessageW(threadId_, kUpdateMouseHook, 0, 0);
}

void InputMonitor::UpdateMouseHook() {
    if (picking_ && !mouseHook_) {
        mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    } else if (!picking_ && mouseHook_) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }
}

LRESULT CALLBACK InputMonitor::KeyboardProc(int code, WPARAM message, LPARAM data) {
    if (code == HC_ACTION && current_) {
        const auto& key = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
        const bool isDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        if (current_->HandleKey(key, isDown)) {
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}

LRESULT CALLBACK InputMonitor::MouseProc(int code, WPARAM message, LPARAM data) {
    if (code == HC_ACTION && current_ && current_->picking_ && message == WM_MOUSEWHEEL) {
        const auto& mouse = *reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
        current_->wheelRemainder_ += static_cast<short>(HIWORD(mouse.mouseData));
        const int steps = current_->wheelRemainder_ / WHEEL_DELTA;
        current_->wheelRemainder_ %= WHEEL_DELTA;
        if (steps != 0) {
            PostMessageW(current_->receiver_, kMovePicker, static_cast<WPARAM>(-steps), 0);
        }
        return 1;
    }
    return CallNextHookEx(nullptr, code, message, data);
}

}
