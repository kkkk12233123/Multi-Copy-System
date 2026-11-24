#include "app_resources.h"
#include "clipboard.h"
#include "ipc.h"
#include "storage.h"

#include <commctrl.h>
#include <shellapi.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#ifdef _MSC_VER
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace mcs {
namespace {

constexpr wchar_t kTitle[] = L"Multi-Copy-System 設定";
constexpr wchar_t kSurfaceClass[] = L"MultiCopySystem.SettingsSurface";
constexpr int kClientWidth = 528;
constexpr int kClientHeight = 390;
constexpr int kEditText = 200;
constexpr int kSlotFirst = 300;
constexpr int kHistoryList = 400;
constexpr int kDeleteHistory = 401;
constexpr int kDeleteAllHistory = 402;
constexpr int kStartResident = 403;
constexpr int kPreviewLimit = 500;
constexpr int kSavePreferences = 501;
constexpr int kClearData = 502;
constexpr int kHotkeyEnabled = 520;
constexpr int kHotkey = 530;
constexpr UINT_PTR kWaitForResident = 1;
constexpr int kWaitAttempts = 20;
constexpr auto kShortcutCount = Settings{}.shortcuts.size();
constexpr std::array kHistoryKinds{L"テキスト", L"画像", L"ファイル"};

enum class Page { Slots, History, Preferences };

struct ControlLayout {
    HWND window;
    int x;
    int y;
    int width;
    int height;
};

struct LegacyHistorySnapshot {
    UINT count;
    HistoryRow rows[10];
};

static_assert(offsetof(LegacyHistorySnapshot, rows) == offsetof(HistorySnapshot, rows));
static_assert(sizeof(LegacyHistorySnapshot) <= sizeof(HistorySnapshot));

struct TextDialog {
    HFONT font;
    std::wstring value;
};

struct alignas(DWORD) DialogTemplate {
    DLGTEMPLATE header;
    WORD menu;
    WORD windowClass;
    WORD title;
};

static_assert(offsetof(DialogTemplate, menu) == sizeof(DLGTEMPLATE));

int Scale(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

HMENU ToControlId(int id) {
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

HWND AddControl(HWND parent, LPCWSTR windowClass, LPCWSTR text, DWORD style,
                DWORD exStyle, int id) {
    const auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));
    return CreateWindowExW(exStyle, windowClass, text, WS_CHILD | WS_CLIPSIBLINGS | style, 0, 0, 0, 0,
                           parent, id ? ToControlId(id) : nullptr, instance, nullptr);
}

void Place(HWND window, UINT dpi, int x, int y, int width, int height, bool repaint = true) {
    MoveWindow(window, Scale(x, dpi), Scale(y, dpi), Scale(width, dpi),
               Scale(height, dpi), repaint);
}

void PaintSurfaceFrame(HWND window, HDC dc) {
    wchar_t title[160]{};
    const int titleLength = GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    if (!titleLength) {
        return;
    }

    const UINT dpi = GetDpiForWindow(window);
    RECT bounds{};
    GetClientRect(window, &bounds);
    const auto oldPen = SelectObject(dc, GetStockObject(DC_PEN));
    const auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    SetDCPenColor(dc, GetSysColor(COLOR_3DSHADOW));
    const int corner = Scale(4, dpi);
    RoundRect(dc, bounds.left, bounds.top + Scale(8, dpi), bounds.right, bounds.bottom,
              corner, corner);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);

    const auto font = reinterpret_cast<HFONT>(GetWindowLongPtrW(window, 0));
    const auto oldFont = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    constexpr UINT labelFlags = DT_SINGLELINE | DT_NOPREFIX;
    RECT label{Scale(9, dpi), 0, bounds.right - Scale(9, dpi), 0};
    DrawTextW(dc, title, titleLength, &label, labelFlags | DT_CALCRECT);
    RECT backing = label;
    InflateRect(&backing, Scale(3, dpi), 0);
    FillRect(dc, &backing, GetSysColorBrush(COLOR_WINDOW));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    DrawTextW(dc, title, titleLength, &label, labelFlags);
    SelectObject(dc, oldFont);
}

LRESULT CALLBACK SurfaceProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_COMMAND:
        return SendMessageW(GetAncestor(window, GA_ROOT), message, wParam, lParam);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        const auto dc = reinterpret_cast<HDC>(wParam);
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        SetTextColor(dc, GetSysColor(IsWindowEnabled(reinterpret_cast<HWND>(lParam))
                                    ? COLOR_WINDOWTEXT : COLOR_GRAYTEXT));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_SETFONT:
        SetWindowLongPtrW(window, 0, static_cast<LONG_PTR>(wParam));
        if (lParam) InvalidateRect(window, nullptr, TRUE);
        return 0;
    case WM_GETFONT:
        return GetWindowLongPtrW(window, 0);
    case WM_SETTEXT: {
        const LRESULT result = DefWindowProcW(window, message, wParam, lParam);
        InvalidateRect(window, nullptr, TRUE);
        return result;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        if (const HDC dc = BeginPaint(window, &paint)) {
            PaintSurfaceFrame(window, dc);
            EndPaint(window, &paint);
        }
        return 0;
    }
    case WM_PRINTCLIENT: {
        const auto dc = reinterpret_cast<HDC>(wParam);
        RECT bounds{};
        GetClientRect(window, &bounds);
        FillRect(dc, &bounds, GetSysColorBrush(COLOR_WINDOW));
        PaintSurfaceFrame(window, dc);
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

void Error(HWND parent, LPCWSTR message) {
    MessageBoxW(parent, message, kTitle, MB_OK | MB_ICONERROR);
}

bool Confirm(HWND parent, LPCWSTR message) {
    return MessageBoxW(parent, message, kTitle, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2)
        == IDYES;
}

std::filesystem::path ResidentPath() {
    std::wstring modulePath(512, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(),
                                               static_cast<DWORD>(modulePath.size()));
        if (length == 0) {
            return {};
        }
        if (length < modulePath.size()) {
            modulePath.resize(length);
            return std::filesystem::path(modulePath).parent_path() / kResidentExeName;
        }
        modulePath.resize(modulePath.size() * 2);
    }
}

BYTE HotkeyFlags(UINT modifiers, UINT key) {
    BYTE flags = 0;
    if (modifiers & MOD_CONTROL) {
        flags |= HOTKEYF_CONTROL;
    }
    if (modifiers & MOD_ALT) {
        flags |= HOTKEYF_ALT;
    }
    if (modifiers & MOD_SHIFT) {
        flags |= HOTKEYF_SHIFT;
    }
    if (MapVirtualKeyW(key, MAPVK_VK_TO_VSC_EX) & 0xFF00) {
        flags |= HOTKEYF_EXT;
    }
    return flags;
}

Shortcut ReadHotkey(HWND control) {
    const auto keyAndFlags = static_cast<WORD>(SendMessageW(control, HKM_GETHOTKEY, 0, 0));
    const BYTE flags = HIBYTE(keyAndFlags);
    UINT modifiers = 0;
    if (flags & HOTKEYF_CONTROL) {
        modifiers |= MOD_CONTROL;
    }
    if (flags & HOTKEYF_ALT) {
        modifiers |= MOD_ALT;
    }
    if (flags & HOTKEYF_SHIFT) {
        modifiers |= MOD_SHIFT;
    }
    return {modifiers, LOBYTE(keyAndFlags)};
}

std::wstring ShortcutLabel(const Shortcut& shortcut) {
    if (shortcut.key == 0) {
        return {};
    }

    std::wstring label;
    if (shortcut.modifiers & MOD_CONTROL) {
        label += L"Ctrl+";
    }
    if (shortcut.modifiers & MOD_SHIFT) {
        label += L"Shift+";
    }
    if (shortcut.modifiers & MOD_ALT) {
        label += L"Alt+";
    }
    const UINT scanCode = MapVirtualKeyW(shortcut.key, MAPVK_VK_TO_VSC_EX);
    const LONG keyParameter = static_cast<LONG>(((scanCode & 0xff) << 16)
                                               | (scanCode & 0xff00 ? 1 << 24 : 0));
    wchar_t keyName[64]{};
    if (GetKeyNameTextW(keyParameter, keyName, static_cast<int>(std::size(keyName)))) {
        label += keyName;
    } else {
        label += std::to_wstring(shortcut.key);
    }
    return label;
}

INT_PTR CALLBACK TextDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG: {
        const auto state = reinterpret_cast<TextDialog*>(lParam);
        SetWindowLongPtrW(window, DWLP_USER, lParam);
        SetWindowTextW(window, L"永久保存のテキストを入力");
        const UINT dpi = GetDpiForWindow(window);
        RECT rect{0, 0, Scale(422, dpi), Scale(224, dpi)};
        const auto style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
        const auto exStyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
        AdjustWindowRectExForDpi(&rect, style, FALSE, exStyle, dpi);
        RECT owner{};
        GetWindowRect(GetParent(window), &owner);
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        SetWindowPos(window, nullptr, (owner.left + owner.right - width) / 2,
                     (owner.top + owner.bottom - height) / 2, width, height,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        const HWND edit = AddControl(window, L"EDIT", state->value.c_str(),
                                     WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE
                                     | ES_AUTOVSCROLL | ES_WANTRETURN, WS_EX_CLIENTEDGE,
                                     kEditText);
        const HWND store = AddControl(window, L"BUTTON", L"登録",
                                      WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, IDOK);
        const HWND cancel = AddControl(window, L"BUTTON", L"キャンセル",
                                       WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, IDCANCEL);
        if (!edit || !store || !cancel) {
            EndDialog(window, -1);
            return TRUE;
        }
        for (const HWND control : {edit, store, cancel}) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), FALSE);
        }
        Place(edit, dpi, 12, 12, 398, 164);
        Place(store, dpi, 230, 188, 84, 26);
        Place(cancel, dpi, 326, 188, 84, 26);
        SendMessageW(edit, EM_SETLIMITTEXT, kMaxClipBytes / sizeof(wchar_t) - 1, 0);
        SendMessageW(window, DM_SETDEFID, IDOK, 0);
        SetFocus(edit);
        return FALSE;
    }
    case WM_COMMAND: {
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(window, IDCANCEL);
            return TRUE;
        }
        if (LOWORD(wParam) != IDOK) {
            return FALSE;
        }
        const HWND edit = GetDlgItem(window, kEditText);
        const int textSize = GetWindowTextLengthW(edit);
        if (textSize <= 0) {
            Error(window, L"登録するテキストを入力してください。");
            SetFocus(edit);
            return TRUE;
        }
        auto* state = reinterpret_cast<TextDialog*>(GetWindowLongPtrW(window, DWLP_USER));
        std::wstring text(static_cast<std::size_t>(textSize) + 1, L'\0');
        text.resize(static_cast<std::size_t>(GetWindowTextW(edit, text.data(), textSize + 1)));
        state->value = std::move(text);
        EndDialog(window, IDOK);
        return TRUE;
    }
    default:
        return FALSE;
    }
}

class SettingsWindow {
public:
    explicit SettingsWindow(HINSTANCE instance) : instance_(instance) {}

    ~SettingsWindow() {
        if (ownedFont_) {
            DeleteObject(ownedFont_);
        }
    }

    bool Open(int showCommand) {
        WNDCLASSEXW definition{};
        definition.cbSize = sizeof(definition);
        definition.lpfnWndProc = WindowProc;
        definition.hInstance = instance_;
        definition.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        definition.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
        definition.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        definition.lpszClassName = kSettingsWindowClass;
        if (!RegisterClassExW(&definition)) {
            return false;
        }

        WNDCLASSW surface{};
        surface.style = CS_HREDRAW | CS_VREDRAW;
        surface.lpfnWndProc = SurfaceProc;
        surface.cbWndExtra = sizeof(LONG_PTR);
        surface.hInstance = instance_;
        surface.hCursor = definition.hCursor;
        surface.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        surface.lpszClassName = kSurfaceClass;
        if (!RegisterClassW(&surface)) {
            return false;
        }

        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX
                              | WS_CLIPCHILDREN;
        window_ = CreateWindowExW(WS_EX_CONTROLPARENT, kSettingsWindowClass, kTitle, style,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 540, 430, nullptr, nullptr,
                                  instance_, this);
        if (!window_) {
            return false;
        }
        ResizeForDpi(GetDpiForWindow(window_), nullptr);
        ShowWindow(window_, showCommand);
        UpdateWindow(window_);
        if (GetActiveWindow() == window_) {
            SetFocus(tab_);
        }
        return true;
    }

    int Run() const {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(window_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return static_cast<int>(message.wParam);
    }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            auto* self = static_cast<SettingsWindow*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (auto* self = reinterpret_cast<SettingsWindow*>(
                GetWindowLongPtrW(window, GWLP_USERDATA))) {
            return self->HandleMessage(message, wParam, lParam);
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            return Initialize() ? 0 : -1;
        case WM_COMMAND:
            ProcessCommand(LOWORD(wParam), HIWORD(wParam));
            return 0;
        case WM_NOTIFY: {
            const auto& header = *reinterpret_cast<const NMHDR*>(lParam);
            if (header.hwndFrom == tab_ && header.code == TCN_SELCHANGE) {
                ShowPage();
                return 0;
            }
            break;
        }
        case WM_COPYDATA:
            return OnHistorySnapshot(reinterpret_cast<HWND>(wParam),
                                     reinterpret_cast<const COPYDATASTRUCT*>(lParam));
        case kHistoryChanged:
            if (page_ == Page::History) {
                RequestHistory();
            }
            return 0;
        case WM_DPICHANGED:
            ResizeForDpi(HIWORD(wParam), reinterpret_cast<const RECT*>(lParam));
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wParam) != WA_INACTIVE && page_ == Page::History) {
                RequestHistory();
            }
            break;
        case WM_TIMER:
            if (wParam == kWaitForResident) {
                if (FindResident() || --waitAttempts_ == 0) {
                    KillTimer(window_, kWaitForResident);
                    waitAttempts_ = 0;
                    RequestHistory();
                }
                return 0;
            }
            break;
        case WM_DESTROY:
            KillTimer(window_, kWaitForResident);
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    HWND PageWindow(Page page) const {
        return pages_[static_cast<std::size_t>(page)];
    }

    HWND Make(HWND parent, LPCWSTR windowClass, LPCWSTR caption, DWORD style, int id,
              int x, int y, int width, int height, DWORD exStyle = 0) {
        const HWND control = AddControl(parent, windowClass, caption, style | WS_VISIBLE, exStyle, id);
        controls_.push_back({control, x, y, width, height});
        return control;
    }

    HWND Button(HWND parent, LPCWSTR caption, int id, int x, int y, int width) {
        return Make(parent, L"BUTTON", caption, BS_PUSHBUTTON | WS_TABSTOP, id, x, y, width, 26);
    }

    HWND Label(HWND parent, LPCWSTR caption, int x, int y, int width, DWORD flags = 0) {
        return Make(parent, L"STATIC", caption, SS_LEFT | SS_NOPREFIX | flags, 0,
                    x, y, width, 22);
    }

    HWND Check(HWND parent, LPCWSTR caption, int id, int x, int y, int width) {
        return Make(parent, L"BUTTON", caption, BS_AUTOCHECKBOX | WS_TABSTOP, id, x, y, width, 22);
    }

    bool Initialize() {
        tab_ = Make(window_, WC_TABCONTROLW, L"", WS_TABSTOP, 100,
                    10, 10, 508, 370);
        if (!tab_) {
            return false;
        }

        wchar_t slotsLabel[] = L"永久保存";
        wchar_t historyLabel[] = L"履歴";
        wchar_t preferencesLabel[] = L"設定";
        for (auto* caption : {slotsLabel, historyLabel, preferencesLabel}) {
            TCITEMW entry{};
            entry.mask = TCIF_TEXT;
            entry.pszText = caption;
            if (TabCtrl_InsertItem(tab_, TabCtrl_GetItemCount(tab_), &entry) < 0) {
                return false;
            }
        }

        for (auto& page : pages_) {
            page = AddControl(window_, kSurfaceClass, L"", WS_CLIPCHILDREN, WS_EX_CONTROLPARENT, 0);
            if (!page) {
                return false;
            }
        }

        CreateSlots();
        CreateHistory();
        CreatePreferences();
        for (const auto& layout : controls_) {
            if (!layout.window) {
                return false;
            }
        }
        SendMessageW(previewLimit_, EM_SETLIMITTEXT, 3, 0);
        SendMessageW(position_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"入力位置の近く"));
        SendMessageW(position_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"画面右下"));
        LoadPreferences();
        ShowPage();
        return true;
    }

    void CreateSlots() {
        const HWND page = PageWindow(Page::Slots);
        for (int index = 0; index < 3; ++index) {
            const int y = 14 + index * 104;
            const int firstId = kSlotFirst + index * 3;
            const HWND group = Make(page, kSurfaceClass, L"", WS_CLIPCHILDREN, 0,
                                    8, y, 484, 92, WS_EX_CONTROLPARENT);
            slotGroup_[index] = group;
            slotLabel_[index] = Label(group, L"", 12, 21, 460, SS_ENDELLIPSIS);
            Button(group, L"テキスト入力…", firstId, 12, 51, 132);
            Button(group, L"クリップボードから登録", firstId + 1, 154, 51, 180);
            slotDelete_[index] = Button(group, L"削除", firstId + 2, 344, 51, 78);
        }
        Label(page, L"ファイルは永久保存に登録できません。ファイル履歴から貼り付けられます。",
              8, 319, 484);
    }

    void CreateHistory() {
        const HWND page = PageWindow(Page::History);
        historyStatus_ = Label(page, L"", 9, 15, 482);
        historyList_ = Make(page, L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL
                            | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, kHistoryList,
                            9, 43, 482, 220, WS_EX_CLIENTEDGE);
        deleteHistory_ = Button(page, L"選択した項目を削除", kDeleteHistory, 9, 275, 180);
        deleteAllHistory_ = Button(page, L"履歴を全削除…", kDeleteAllHistory, 199, 275, 130);
        startResident_ = Button(page, L"常駐アプリを起動", kStartResident, 341, 275, 150);
    }

    void CreatePreferences() {
        const HWND page = PageWindow(Page::Preferences);
        Label(page, L"候補の表示文字数（1～512）", 11, 15, 235);
        previewLimit_ = Make(page, L"EDIT", L"", WS_TABSTOP | ES_NUMBER,
                             kPreviewLimit, 256, 12, 62, 24, WS_EX_CLIENTEDGE);
        Label(page, L"候補の表示位置", 11, 46, 235);
        position_ = Make(page, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST,
                         0, 256, 42, 213, 130);
        autoStart_ = Check(page, L"Windows サインイン時に常駐アプリを起動", 0, 11, 73, 465);
        paused_ = Check(page, L"履歴の記録を一時停止", 0, 11, 101, 465);

        const HWND shortcuts = Make(page, kSurfaceClass, L"ショートカット", WS_CLIPCHILDREN, 0,
                                    8, 129, 484, 163, WS_EX_CONTROLPARENT);
        constexpr std::array<LPCWSTR, kShortcutCount> actions{
            L"履歴から貼り付け", L"永久保存 1", L"永久保存 2", L"永久保存 3", L"ファイル履歴から貼り付け"
        };
        for (int index = 0; index < static_cast<int>(actions.size()); ++index) {
            const int y = 23 + index * 27;
            Label(shortcuts, actions[index], 12, y + 3, 172);
            hotkeyControl_[index] = Make(shortcuts, HOTKEY_CLASSW, L"", WS_TABSTOP,
                                         kHotkey + index, 189, y, 181, 24, WS_EX_CLIENTEDGE);
            hotkeyEnabled_[index] = Check(shortcuts, L"有効", kHotkeyEnabled + index, 385, y, 81);
        }
        Button(page, L"保存データをすべて消去…", kClearData, 9, 303, 208);
        preferenceStatus_ = Label(page, L"", 223, 309, 144);
        Button(page, L"設定を保存", kSavePreferences, 371, 303, 120);
    }

    void ResizeForDpi(UINT dpi, const RECT* suggested) {
        dpi = dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        HFONT newFont = nullptr;
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0,
                                       dpi)) {
            newFont = CreateFontIndirectW(&metrics.lfMessageFont);
        }
        font_ = newFont ? newFont : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        for (const auto& control : controls_) {
            SendMessageW(control.window, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
            Place(control.window, dpi, control.x, control.y, control.width, control.height, false);
        }
        RECT pageBounds{};
        GetClientRect(tab_, &pageBounds);
        TabCtrl_AdjustRect(tab_, FALSE, &pageBounds);
        MapWindowPoints(tab_, window_, reinterpret_cast<POINT*>(&pageBounds), 2);
        for (const HWND page : pages_) {
            MoveWindow(page, pageBounds.left, pageBounds.top, pageBounds.right - pageBounds.left,
                       pageBounds.bottom - pageBounds.top, FALSE);
        }
        if (ownedFont_) {
            DeleteObject(ownedFont_);
        }
        ownedFont_ = newFont;
        RECT rect{0, 0, Scale(kClientWidth, dpi), Scale(kClientHeight, dpi)};
        const auto style = static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE));
        const auto exStyle = static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_EXSTYLE));
        AdjustWindowRectExForDpi(&rect, style, FALSE, exStyle, dpi);
        SetWindowPos(window_, nullptr, suggested ? suggested->left : 0,
                     suggested ? suggested->top : 0, rect.right - rect.left,
                     rect.bottom - rect.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | (suggested ? 0 : SWP_NOMOVE));
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }

    void ShowPage() {
        const int selection = TabCtrl_GetCurSel(tab_);
        if (selection < 0 || selection >= static_cast<int>(pages_.size())) {
            return;
        }
        const Page next = static_cast<Page>(selection);
        if (next != page_ && IsChild(PageWindow(page_), GetFocus())) {
            SetFocus(tab_);
        }
        page_ = next;
        const HWND active = PageWindow(page_);
        for (const HWND page : pages_) {
            if (page != active) {
                ShowWindow(page, SW_HIDE);
            }
        }
        if (page_ == Page::Slots) {
            RefreshSlots();
        } else if (page_ == Page::History) {
            RequestHistory();
        }
        SetWindowPos(active, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        RedrawWindow(window_, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }

    void ProcessCommand(int id, int code) {
        if (id == kHistoryList && code == LBN_SELCHANGE) {
            UpdateHistoryControls();
            return;
        }
        if (code != BN_CLICKED || id == 0) {
            return;
        }
        if (id >= kSlotFirst && id < kSlotFirst + 9) {
            SlotAction((id - kSlotFirst) / 3, (id - kSlotFirst) % 3);
            return;
        }
        if (id >= kHotkeyEnabled && id < kHotkeyEnabled + static_cast<int>(kShortcutCount)) {
            SetHotkeyEnabled(id - kHotkeyEnabled);
            return;
        }
        switch (id) {
        case kSavePreferences:
            SavePreferences();
            break;
        case kClearData:
            ClearData();
            break;
        case kStartResident:
            StartResident();
            break;
        case kDeleteHistory:
            DeleteHistory();
            break;
        case kDeleteAllHistory:
            if (Confirm(window_, L"すべての履歴を削除しますか？")) {
                if (!NotifyResident(kClearHistory)) {
                    RequestHistory();
                }
            }
            break;
        default:
            break;
        }
    }

    void RefreshSlots() {
        for (int index = 0; index < 3; ++index) {
            auto title = L"保存枠 " + std::to_wstring(index + 1);
            const auto shortcut = ShortcutLabel(settings_.shortcuts[index + 1]);
            if (!shortcut.empty()) {
                title += L"  " + shortcut;
            }
            SetWindowTextW(slotGroup_[index], title.c_str());
            const auto clip = LoadSlot(index);
            std::wstring preview = L"未登録";
            if (clip) {
                if (clip->kind == ClipKind::Text) {
                    preview = ClipTextPreview(clip->text, settings_.previewLimit);
                } else {
                    const auto size = (clip->image.size() + 1023) / 1024;
                    const LPCWSTR format = clip->imageFormat == ImageFormat::Png ? L"PNG" : L"DIB";
                    preview = std::wstring(L"画像（") + format + L"・"
                            + std::to_wstring(size) + L" KB）";
                }
            }
            SetWindowTextW(slotLabel_[index], preview.c_str());
            EnableWindow(slotDelete_[index], clip != nullptr);
        }
    }

    void SlotAction(int index, int action) {
        if (action == 0) {
            TextDialog dialog{font_, {}};
            if (const auto clip = LoadSlot(index); clip && clip->kind == ClipKind::Text) {
                dialog.value = clip->text;
            }
            DialogTemplate definition{};
            definition.header.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
            definition.header.cx = 260;
            definition.header.cy = 148;
            const auto result = DialogBoxIndirectParamW(instance_, &definition.header, window_,
                                                        TextDialogProc,
                                                        reinterpret_cast<LPARAM>(&dialog));
            if (result == -1) {
                Error(window_, L"入力画面を開けませんでした。");
            } else if (result == IDOK) {
                ClipData clip;
                clip.text = std::move(dialog.value);
                StoreSlot(index, &clip);
            }
        } else if (action == 1) {
            const auto result = ReadClipboard(window_, false);
            if (result.status == ClipReadStatus::Busy) {
                Error(window_, L"クリップボードを使用中です。もう一度お試しください。");
            } else if (!result.clip) {
                Error(window_, L"登録できるテキストまたは画像がありません。");
            } else if (result.clip->kind == ClipKind::Files) {
                Error(window_, L"ファイルは永久保存に登録できません。ファイル履歴から貼り付けられます。");
            } else if (!LoadSlot(index) || Confirm(window_, L"この保存枠を上書きしますか？")) {
                StoreSlot(index, result.clip.get());
            }
        } else if (Confirm(window_, L"この保存枠の内容を削除しますか？")) {
            StoreSlot(index, nullptr);
        }
    }

    void StoreSlot(int index, const ClipData* clip) {
        if (!SaveSlot(index, clip)) {
            Error(window_, L"保存枠を更新できませんでした。");
            return;
        }
        RefreshSlots();
        NotifyResident(kReloadSettings);
    }

    void LoadPreferences() {
        settings_ = LoadSettings();
        SetWindowTextW(preferenceStatus_, L"");
        SetWindowTextW(previewLimit_, std::to_wstring(settings_.previewLimit).c_str());
        SendMessageW(position_, CB_SETCURSEL, settings_.bottomRight ? 1 : 0, 0);
        SendMessageW(autoStart_, BM_SETCHECK, IsStartOnSignInEnabled() ? BST_CHECKED
                                                                    : BST_UNCHECKED, 0);
        SendMessageW(paused_, BM_SETCHECK, settings_.paused ? BST_CHECKED : BST_UNCHECKED, 0);
        for (std::size_t index = 0; index < settings_.shortcuts.size(); ++index) {
            const auto& shortcut = settings_.shortcuts[index];
            const bool enabled = shortcut.key != 0;
            SendMessageW(hotkeyEnabled_[index], BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(hotkeyControl_[index], HKM_SETHOTKEY,
                         MAKEWORD(shortcut.key, HotkeyFlags(shortcut.modifiers, shortcut.key)), 0);
            EnableWindow(hotkeyControl_[index], enabled);
        }
    }

    void SetHotkeyEnabled(int index) {
        const bool enabled = IsChecked(hotkeyEnabled_[index]);
        EnableWindow(hotkeyControl_[index], enabled);
        if (enabled && ReadHotkey(hotkeyControl_[index]).key == 0) {
            const Settings defaults{};
            const auto& shortcut = defaults.shortcuts[index];
            SendMessageW(hotkeyControl_[index], HKM_SETHOTKEY,
                         MAKEWORD(shortcut.key, HotkeyFlags(shortcut.modifiers, shortcut.key)), 0);
        }
    }

    bool ReadPreferences(Settings& settings) const {
        BOOL isNumber = FALSE;
        const UINT limit = GetDlgItemInt(PageWindow(Page::Preferences), kPreviewLimit, &isNumber, FALSE);
        if (!isNumber || limit < 1 || limit > 512) {
            Error(window_, L"表示文字数は1～512の整数で入力してください。");
            SetFocus(previewLimit_);
            return false;
        }
        settings.previewLimit = static_cast<int>(limit);
        settings.bottomRight = SendMessageW(position_, CB_GETCURSEL, 0, 0) == 1;
        settings.paused = IsChecked(paused_);
        for (std::size_t index = 0; index < settings.shortcuts.size(); ++index) {
            if (!IsChecked(hotkeyEnabled_[index])) {
                settings.shortcuts[index] = {0, 0};
                continue;
            }
            const auto shortcut = ReadHotkey(hotkeyControl_[index]);
            if (!shortcut.key || !shortcut.modifiers) {
                Error(window_, L"有効なショートカットは Ctrl、Shift、Alt のいずれかと"
                              L"別のキーを組み合わせてください。");
                SetFocus(hotkeyControl_[index]);
                return false;
            }
            if ((index == 0 || index == kFileShortcutIndex) && shortcut.key == VK_ESCAPE) {
                Error(window_, L"Esc は候補表示の取り消しに使われています。");
                SetFocus(hotkeyControl_[index]);
                return false;
            }
            if (shortcut.modifiers == MOD_CONTROL && (shortcut.key == 'C'
                || shortcut.key == 'X' || shortcut.key == 'V')) {
                Error(window_, L"Ctrl+C、Ctrl+X、Ctrl+V は Windows のコピー・切り取り・"
                              L"貼り付けに使われています。");
                SetFocus(hotkeyControl_[index]);
                return false;
            }
            for (std::size_t other = 0; other < index; ++other) {
                if (settings.shortcuts[other].key == shortcut.key
                    && settings.shortcuts[other].modifiers == shortcut.modifiers) {
                    Error(window_, L"各ショートカットには異なる組み合わせを設定してください。");
                    SetFocus(hotkeyControl_[index]);
                    return false;
                }
            }
            settings.shortcuts[index] = shortcut;
        }
        return true;
    }

    void SavePreferences() {
        SetWindowTextW(preferenceStatus_, L"");
        Settings updated{};
        if (!ReadPreferences(updated)) {
            return;
        }
        const bool previousAutoStart = IsStartOnSignInEnabled();
        const bool newAutoStart = IsChecked(autoStart_);
        const auto residentPath = ResidentPath();
        if (newAutoStart != previousAutoStart
            && !SetStartOnSignIn(newAutoStart, residentPath)) {
            Error(window_, L"自動起動の設定を保存できませんでした。");
            return;
        }
        if (!SaveSettings(updated)) {
            if (newAutoStart != previousAutoStart) {
                SetStartOnSignIn(previousAutoStart, residentPath);
            }
            Error(window_, L"設定を保存できませんでした。");
            return;
        }
        settings_ = updated;
        SetWindowTextW(preferenceStatus_, L"設定を保存しました。");
        NotifyResident(kReloadSettings);
    }

    void ClearData() {
        if (!Confirm(window_, L"履歴と永久保存の内容をすべて消去し、設定を初期状態に戻しますか？"
                              L"\n自動起動も解除されます。")) {
            return;
        }
        const bool filesCleared = ClearStoredData();
        const bool startupCleared = SetStartOnSignIn(false, {});
        NotifyResident(kResetHistory);
        LoadPreferences();
        RefreshSlots();
        if (!filesCleared || !startupCleared) {
            Error(window_, L"一部のデータを消去できませんでした。");
        } else {
            SetWindowTextW(preferenceStatus_, L"消去しました。");
        }
    }

    static bool IsChecked(HWND control) {
        return SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }

    static HWND FindResident() {
        return FindWindowW(kResidentWindowClass, nullptr);
    }

    static bool NotifyResident(UINT message, WPARAM parameter = 0) {
        const HWND resident = FindResident();
        return resident && PostMessageW(resident, message, parameter, 0);
    }

    void RequestHistory() {
        if (!NotifyResident(kRequestHistory, reinterpret_cast<WPARAM>(window_))) {
            snapshot_ = {};
            SendMessageW(historyList_, LB_RESETCONTENT, 0, 0);
        }
        UpdateHistoryControls();
    }

    LRESULT OnHistorySnapshot(HWND sender, const COPYDATASTRUCT* payload) {
        if (!sender || sender != FindResident() || !payload || payload->dwData != kHistoryPayload
            || !payload->lpData) {
            return FALSE;
        }
        HistorySnapshot received{};
        const bool legacy = payload->cbData == sizeof(LegacyHistorySnapshot);
        if (!legacy && payload->cbData != sizeof(received)) {
            return FALSE;
        }
        const auto capacity = legacy ? std::extent_v<decltype(LegacyHistorySnapshot::rows)>
                                     : std::size(received.rows);
        std::memcpy(&received, payload->lpData, payload->cbData);
        if (received.count > capacity) {
            return FALSE;
        }
        for (UINT index = 0; index < received.count; ++index) {
            auto& row = received.rows[index];
            if (!row.id || row.kind >= (legacy ? 2 : kHistoryKinds.size())) {
                return FALSE;
            }
            row.label[std::size(row.label) - 1] = L'\0';
        }

        const UINT selectedId = SelectedHistoryId();
        snapshot_ = received;
        SendMessageW(historyList_, WM_SETREDRAW, FALSE, 0);
        SendMessageW(historyList_, LB_RESETCONTENT, 0, 0);
        for (UINT index = 0; index < snapshot_.count; ++index) {
            const auto& row = snapshot_.rows[index];
            std::wstring_view detail(row.label);
            constexpr std::wstring_view legacyImagePrefix = L"画像 ";
            if (legacy && row.kind == 1 && detail.starts_with(legacyImagePrefix)) {
                detail.remove_prefix(legacyImagePrefix.size());
            }
            std::wstring label = std::wstring(L"［") + kHistoryKinds[row.kind] + L"］ ";
            label += detail;
            const LRESULT position = SendMessageW(historyList_, LB_ADDSTRING, 0,
                                                  reinterpret_cast<LPARAM>(label.c_str()));
            if (position < 0) {
                continue;
            }
            SendMessageW(historyList_, LB_SETITEMDATA, static_cast<WPARAM>(position), row.id);
            if (row.id == selectedId) {
                SendMessageW(historyList_, LB_SETCURSEL, static_cast<WPARAM>(position), 0);
            }
        }
        SendMessageW(historyList_, WM_SETREDRAW, TRUE, 0);
        RedrawWindow(historyList_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
        UpdateHistoryControls();
        return TRUE;
    }

    UINT SelectedHistoryId() const {
        const LRESULT selected = SendMessageW(historyList_, LB_GETCURSEL, 0, 0);
        if (selected == LB_ERR) {
            return 0;
        }
        const LRESULT id = SendMessageW(historyList_, LB_GETITEMDATA, static_cast<WPARAM>(selected), 0);
        return id == LB_ERR ? 0 : static_cast<UINT>(id);
    }

    void UpdateHistoryControls() const {
        const bool running = FindResident() != nullptr;
        EnableWindow(startResident_, !running && waitAttempts_ == 0);
        EnableWindow(deleteHistory_, running && SelectedHistoryId() != 0);
        EnableWindow(deleteAllHistory_, running && snapshot_.count > 0);
        if (waitAttempts_) {
            SetWindowTextW(historyStatus_, L"常駐アプリを起動しています…");
        } else if (!running) {
            SetWindowTextW(historyStatus_, L"常駐アプリは起動していません。");
        } else if (!snapshot_.count) {
            SetWindowTextW(historyStatus_, L"履歴は空です。");
        } else {
            std::array<UINT, kHistoryKinds.size()> counts{};
            for (UINT index = 0; index < snapshot_.count; ++index) {
                ++counts[snapshot_.rows[index].kind];
            }
            const auto text = L"テキスト " + std::to_wstring(counts[0]) + L" 件　画像 "
                            + std::to_wstring(counts[1]) + L" 件　ファイル "
                            + std::to_wstring(counts[2]) + L" 件";
            SetWindowTextW(historyStatus_, text.c_str());
        }
    }

    void DeleteHistory() {
        if (const UINT selectedId = SelectedHistoryId(); selectedId &&
            !NotifyResident(kDeleteHistoryItem, selectedId)) {
            RequestHistory();
        }
    }

    void StartResident() {
        if (FindResident()) {
            RequestHistory();
            return;
        }
        const auto path = ResidentPath();
        if (path.empty() || reinterpret_cast<INT_PTR>(
                ShellExecuteW(window_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL))
                <= 32) {
            Error(window_, L"常駐アプリを起動できませんでした。両方の実行ファイルが"
                          L"同じフォルダーにあるか確認してください。");
            return;
        }
        waitAttempts_ = kWaitAttempts;
        if (!SetTimer(window_, kWaitForResident, 150, nullptr)) {
            waitAttempts_ = 0;
        }
        RequestHistory();
    }

    HINSTANCE instance_;
    HWND window_ = nullptr;
    HWND tab_ = nullptr;
    HFONT font_ = nullptr;
    HFONT ownedFont_ = nullptr;
    Page page_ = Page::Slots;
    std::array<HWND, 3> pages_{};
    std::vector<ControlLayout> controls_;
    Settings settings_{};
    HistorySnapshot snapshot_{};
    int waitAttempts_ = 0;
    std::array<HWND, 3> slotGroup_{};
    std::array<HWND, 3> slotLabel_{};
    std::array<HWND, 3> slotDelete_{};
    HWND historyStatus_ = nullptr;
    HWND historyList_ = nullptr;
    HWND deleteHistory_ = nullptr;
    HWND deleteAllHistory_ = nullptr;
    HWND startResident_ = nullptr;
    HWND previewLimit_ = nullptr;
    HWND position_ = nullptr;
    HWND autoStart_ = nullptr;
    HWND paused_ = nullptr;
    HWND preferenceStatus_ = nullptr;
    std::array<HWND, kShortcutCount> hotkeyControl_{};
    std::array<HWND, kShortcutCount> hotkeyEnabled_{};
};

}  // namespace
}  // namespace mcs

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (const HWND existing = FindWindowW(mcs::kSettingsWindowClass, nullptr)) {
        ShowWindow(existing, IsIconic(existing) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(existing);
        return 0;
    }
    INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX), ICC_TAB_CLASSES | ICC_HOTKEY_CLASS};
    if (!InitCommonControlsEx(&controls)) {
        mcs::Error(nullptr, L"設定画面を初期化できませんでした。");
        return 1;
    }
    mcs::SettingsWindow window(instance);
    if (!window.Open(showCommand)) {
        mcs::Error(nullptr, L"設定画面を開けませんでした。");
        return 1;
    }
    return window.Run();
}
