#include "app_resources.h"
#include "clipboard.h"
#include "history.h"
#include "input.h"
#include "ipc.h"
#include "picker.h"
#include "storage.h"

#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <optional>
#include <utility>

namespace mcs {
namespace {

constexpr UINT kTrayEvent = WM_APP + 30;
constexpr UINT kPickerHover = WM_APP + 31;
constexpr UINT_PTR kClipboardTimer = 1;
constexpr UINT_PTR kPasteTimer = 2;
constexpr UINT kTrayIcon = 1;
constexpr UINT kOpenSettings = 1;
constexpr UINT kTogglePause = 2;
constexpr UINT kQuit = 3;
constexpr UINT kRetryIntervalMs = 15;
constexpr ULONGLONG kCopyWindowMs = 1500;
constexpr ULONGLONG kPasteWindowMs = 250;

bool ClipboardBelongsTo(HWND source) {
    if (!source) return false;
    const HWND owner = GetClipboardOwner();
    if (!owner) return GetForegroundWindow() == source;
    if (owner == source || GetAncestor(owner, GA_ROOTOWNER) == source) return true;

    DWORD ownerProcess = 0;
    DWORD sourceProcess = 0;
    GetWindowThreadProcessId(owner, &ownerProcess);
    const DWORD sourceThread = GetWindowThreadProcessId(source, &sourceProcess);
    if (ownerProcess && ownerProcess == sourceProcess) return true;

    GUITHREADINFO gui{};
    gui.cbSize = sizeof(gui);
    DWORD focusedProcess = 0;
    if (sourceThread && GetGUIThreadInfo(sourceThread, &gui) && gui.hwndFocus) {
        GetWindowThreadProcessId(gui.hwndFocus, &focusedProcess);
    }
    return ownerProcess && ownerProcess == focusedProcess;
}

class Resident {
public:
    bool Initialize(HINSTANCE instance) {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = kResidentWindowClass;
        if (!RegisterClassW(&windowClass)) {
            return false;
        }

        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kResidentWindowClass,
                                  L"Multi-Copy-System", WS_POPUP, 0, 0, 0, 0,
                                  nullptr, nullptr, instance, this);
        if (!window_ || !AddClipboardFormatListener(window_) ||
            !picker_.Initialize(instance, window_, kPickerHover)) {
            return false;
        }
        trayIcon_ = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        ReloadSettings();
        if (!input_.Start(window_)) {
            return false;
        }
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        UpdateTray(NIM_ADD);
        return true;
    }

    ~Resident() {
        if (window_) {
            DestroyWindow(window_);
        }
        if (trayIcon_) {
            DestroyIcon(trayIcon_);
        }
    }

private:
    struct CopyRequest {
        DWORD previousSequence;
        HWND source;
        ULONGLONG deadline;
    };

    struct PasteRequest {
        std::shared_ptr<const ClipData> clip;
        HWND target;
        ULONGLONG deadline;
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            auto* resident = static_cast<Resident*>(creation->lpCreateParams);
            resident->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(resident));
        }
        auto* resident = reinterpret_cast<Resident*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        return resident ? resident->HandleMessage(message, wParam, lParam)
                        : DefWindowProcW(window, message, wParam, lParam);
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        if (taskbarCreated_ && message == taskbarCreated_) {
            UpdateTray(NIM_ADD);
            return 0;
        }
        switch (message) {
        case kCopyKey:
            if (!settings_.paused) {
                copy_ = CopyRequest{static_cast<DWORD>(wParam), reinterpret_cast<HWND>(lParam),
                                    GetTickCount64() + kCopyWindowMs};
                CaptureClipboard();
            }
            return 0;
        case WM_CLIPBOARDUPDATE:
            CaptureClipboard();
            return 0;
        case kShowPicker:
            ShowPicker(reinterpret_cast<HWND>(lParam));
            return 0;
        case kMovePicker:
            if (!pickerItems_.empty()) {
                const int count = static_cast<int>(pickerItems_.size());
                SelectPicker((selected_ + static_cast<int>(wParam) % count + count) % count);
            }
            return 0;
        case kPickerHover:
            if (wParam < pickerItems_.size()) {
                SelectPicker(static_cast<int>(wParam));
            }
            return 0;
        case kFinishPicker:
            FinishPicker(wParam != 0);
            return 0;
        case kPasteSlot:
            if (wParam < 3 && GetForegroundWindow() == reinterpret_cast<HWND>(lParam)) {
                QueuePaste(LoadSlot(static_cast<int>(wParam)), reinterpret_cast<HWND>(lParam));
            }
            return 0;
        case kRequestHistory:
            SendHistory(reinterpret_cast<HWND>(wParam));
            return 0;
        case kDeleteHistoryItem:
            if (history_.Remove(static_cast<UINT>(wParam))) {
                FinishPicker(false);
                NotifyHistoryChanged();
            }
            return 0;
        case kClearHistory:
            ClearHistory();
            return 0;
        case kResetHistory:
            ClearHistory();
            paste_.reset();
            KillTimer(window_, kPasteTimer);
            ReloadSettings();
            return 0;
        case kReloadSettings:
            ReloadSettings();
            NotifyHistoryChanged();
            return 0;
        case WM_TIMER:
            if (wParam == kClipboardTimer) CaptureClipboard();
            if (wParam == kPasteTimer) TryPaste();
            return 0;
        case kTrayEvent:
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) ShowTrayMenu();
            if (lParam == WM_LBUTTONDBLCLK) OpenSettings();
            return 0;
        case WM_DESTROY:
            input_.Stop();
            RemoveClipboardFormatListener(window_);
            UpdateTray(NIM_DELETE);
            FinishPicker(false);
            PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY: {
            const LRESULT result = DefWindowProcW(window_, message, wParam, lParam);
            window_ = nullptr;
            return result;
        }
        default:
            return DefWindowProcW(window_, message, wParam, lParam);
        }
    }

    void ReloadSettings() {
        settings_ = LoadSettings();
        unsigned savedSlots = 0;
        for (int index = 0; index < 3; ++index) {
            if (LoadSlot(index)) savedSlots |= 1u << index;
        }
        input_.Configure(settings_, savedSlots);
        if (settings_.paused) {
            copy_.reset();
            KillTimer(window_, kClipboardTimer);
        }
        UpdateTray(NIM_MODIFY);
    }

    void CaptureClipboard() {
        if (!copy_ || GetTickCount64() > copy_->deadline) {
            copy_.reset();
            KillTimer(window_, kClipboardTimer);
            return;
        }
        const DWORD sequence = GetClipboardSequenceNumber();
        if (sequence == copy_->previousSequence || sequence == ownClipboardSequence_ || !ClipboardBelongsTo(copy_->source)) {
            return;
        }

        auto result = ReadClipboard(window_);
        if (result.status == ClipReadStatus::Busy) {
            SetTimer(window_, kClipboardTimer, kRetryIntervalMs, nullptr);
            return;
        }
        copy_.reset();
        KillTimer(window_, kClipboardTimer);
        if (result.status == ClipReadStatus::Found && result.clip) {
            history_.Add(std::move(result.clip));
            NotifyHistoryChanged();
        }
    }

    void ShowPicker(HWND target) {
        if (GetForegroundWindow() != target) return;

        pickerItems_.clear();
        pickerTarget_ = target;
        selected_ = 0;
        const auto& entries = history_.Entries();
        for (const ClipKind kind : {ClipKind::Text, ClipKind::Image}) {
            int number = 1;
            for (const auto& entry : entries) {
                if (entry.clip->kind != kind) continue;
                if (entry.id == entries.front().id) selected_ = static_cast<int>(pickerItems_.size());
                pickerItems_.push_back({entry.clip, number++});
            }
        }
        picker_.Show(pickerItems_, selected_, settings_.previewLimit, settings_.bottomRight, target);
    }

    void SelectPicker(int index) {
        selected_ = index;
        picker_.Select(index);
    }

    void FinishPicker(bool paste) {
        if (paste && !pickerItems_.empty()) {
            const int hovered = picker_.NewHoveredItem();
            if (hovered >= 0) selected_ = hovered;
        }
        picker_.Hide();
        if (paste && !pickerItems_.empty()) {
            QueuePaste(pickerItems_[selected_].clip, pickerTarget_);
        }
        pickerItems_.clear();
        pickerTarget_ = nullptr;
    }

    void QueuePaste(std::shared_ptr<const ClipData> clip, HWND target) {
        if (clip && target == GetForegroundWindow()) {
            paste_ = PasteRequest{std::move(clip), target, GetTickCount64() + kPasteWindowMs};
            TryPaste();
        }
    }

    void TryPaste() {
        if (!paste_ || GetTickCount64() > paste_->deadline || GetForegroundWindow() != paste_->target) {
            paste_.reset();
            KillTimer(window_, kPasteTimer);
            return;
        }
        for (const int key : {VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN}) {
            if (GetAsyncKeyState(key) & 0x8000) {
                SetTimer(window_, kPasteTimer, kRetryIntervalMs, nullptr);
                return;
            }
        }
        if (!WriteClipboard(window_, *paste_->clip)) {
            SetTimer(window_, kPasteTimer, kRetryIntervalMs, nullptr);
            return;
        }
        ownClipboardSequence_ = GetClipboardSequenceNumber();
        if (GetForegroundWindow() == paste_->target) {
            INPUT events[4]{};
            for (auto& event : events) {
                event.type = INPUT_KEYBOARD;
                event.ki.dwExtraInfo = kPasteExtraInfo;
            }
            events[0].ki.wVk = VK_CONTROL;
            events[1].ki.wVk = 'V';
            events[2].ki.wVk = 'V';
            events[2].ki.dwFlags = KEYEVENTF_KEYUP;
            events[3].ki.wVk = VK_CONTROL;
            events[3].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(static_cast<UINT>(std::size(events)), events, sizeof(INPUT));
        }
        paste_.reset();
        KillTimer(window_, kPasteTimer);
    }

    void ClearHistory() {
        history_.Clear();
        copy_.reset();
        KillTimer(window_, kClipboardTimer);
        FinishPicker(false);
        NotifyHistoryChanged();
    }

    void NotifyHistoryChanged() const {
        if (const HWND settings = FindWindowW(kSettingsWindowClass, nullptr)) {
            PostMessageW(settings, kHistoryChanged, 0, 0);
        }
    }

    void SendHistory(HWND recipient) const {
        if (!recipient || recipient != FindWindowW(kSettingsWindowClass, nullptr)) return;

        HistorySnapshot snapshot{};
        for (const auto& entry : history_.Entries()) {
            auto& row = snapshot.rows[snapshot.count++];
            row.id = entry.id;
            row.kind = entry.clip->kind == ClipKind::Text ? 0 : 1;
            const auto label = row.kind == 0
                ? ClipTextPreview(entry.clip->text, std::min(settings_.previewLimit, 256))
                : L"画像 (" + std::to_wstring((entry.clip->image.size() + 1023) / 1024) + L" KB)";
            size_t count = std::min(label.size(), std::size(row.label) - 1);
            if (count < label.size() && label[count - 1] >= 0xd800 && label[count - 1] <= 0xdbff) {
                --count;
            }
            std::copy_n(label.data(), count, row.label);
        }
        COPYDATASTRUCT payload{kHistoryPayload, sizeof(snapshot), &snapshot};
        SendMessageTimeoutW(recipient, WM_COPYDATA, reinterpret_cast<WPARAM>(window_),
                            reinterpret_cast<LPARAM>(&payload), SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, nullptr);
    }

    void UpdateTray(DWORD operation) const {
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = window_;
        icon.uID = kTrayIcon;
        icon.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
        icon.uCallbackMessage = kTrayEvent;
        icon.hIcon = trayIcon_ ? trayIcon_ : LoadIconW(nullptr, IDI_APPLICATION);
        const std::wstring_view tooltip = settings_.paused ? L"Multi-Copy-System（履歴の記録を一時停止中）"
                                                           : L"Multi-Copy-System";
        std::copy(tooltip.begin(), tooltip.end(), icon.szTip);
        Shell_NotifyIconW(operation, &icon);
    }

    void OpenSettings() const {
        if (const HWND settings = FindWindowW(kSettingsWindowClass, nullptr)) {
            if (IsIconic(settings)) ShowWindow(settings, SW_RESTORE);
            SetForegroundWindow(settings);
            return;
        }
        wchar_t path[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        if (length && length < std::size(path)) {
            const auto executable = std::filesystem::path(path).parent_path() / kSettingsExeName;
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", executable.c_str(), nullptr,
                                                        nullptr, SW_SHOWNORMAL)) > 32) {
                return;
            }
        }
        MessageBoxW(window_, L"MultiCopySettings.exe を常駐アプリと同じフォルダーに置いてください。",
                    L"Multi-Copy-System", MB_OK | MB_ICONWARNING);
    }

    void ShowTrayMenu() {
        const HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING, kOpenSettings, L"設定を開く");
        AppendMenuW(menu, MF_STRING, kTogglePause, settings_.paused ? L"履歴の記録を再開" : L"履歴の記録を一時停止");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kQuit, L"終了");
        POINT position{};
        GetCursorPos(&position);
        SetForegroundWindow(window_);
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                            position.x, position.y, 0, window_, nullptr);
        PostMessageW(window_, WM_NULL, 0, 0);
        DestroyMenu(menu);
        if (command == kOpenSettings) OpenSettings();
        if (command == kTogglePause) {
            Settings updated = LoadSettings();
            updated.paused = !updated.paused;
            if (SaveSettings(updated)) ReloadSettings();
        }
        if (command == kQuit) DestroyWindow(window_);
    }

    HWND window_ = nullptr;
    HICON trayIcon_ = nullptr;
    UINT taskbarCreated_ = 0;
    Settings settings_;
    Picker picker_;
    InputMonitor input_;
    History history_;
    std::vector<PickerItem> pickerItems_;
    HWND pickerTarget_ = nullptr;
    int selected_ = 0;
    DWORD ownClipboardSequence_ = 0;
    std::optional<CopyRequest> copy_;
    std::optional<PasteRequest> paste_;
};

}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HANDLE lock = CreateMutexW(nullptr, FALSE, L"Local\\MultiCopySystem.Resident");
    if (!lock) return 1;
    const bool alreadyRunning = GetLastError() == ERROR_ALREADY_EXISTS;
    int result = 0;
    if (!alreadyRunning) {
        mcs::Resident resident;
        if (resident.Initialize(instance)) {
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        } else {
            MessageBoxW(nullptr, L"常駐アプリを起動できませんでした。", L"Multi-Copy-System", MB_OK | MB_ICONERROR);
            result = 1;
        }
    }
    CloseHandle(lock);
    return result;
}
