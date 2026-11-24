#include "picker.h"

#include "clipboard.h"

#include <windowsx.h>

#include <algorithm>
#include <iterator>
#include <optional>
#include <string_view>

namespace mcs {
namespace {

constexpr wchar_t kWindowClass[] = L"MultiCopySystem.HistoryPicker";
constexpr COLORREF kForeground = RGB(32, 33, 36);
constexpr COLORREF kPlaceholder = RGB(112, 114, 121);
constexpr COLORREF kBorder = RGB(172, 174, 179);
constexpr COLORREF kDivider = RGB(219, 220, 224);
constexpr COLORREF kSelection = RGB(255, 247, 216);
constexpr UINT kLabelFlags = DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;

std::optional<RECT> CaretBounds(HWND targetWindow) {
    const DWORD thread = GetWindowThreadProcessId(targetWindow, nullptr);
    GUITHREADINFO gui{};
    gui.cbSize = sizeof(gui);
    if (!thread || !GetGUIThreadInfo(thread, &gui) || !gui.hwndCaret ||
        gui.rcCaret.bottom <= gui.rcCaret.top) {
        return std::nullopt;
    }

    POINT topLeft{gui.rcCaret.left, gui.rcCaret.top};
    POINT bottomLeft{gui.rcCaret.left, gui.rcCaret.bottom};
    const DPI_AWARENESS_CONTEXT context = GetWindowDpiAwarenessContext(gui.hwndCaret);
    const DPI_AWARENESS_CONTEXT previousContext = SetThreadDpiAwarenessContext(context);
    if (!previousContext) {
        return std::nullopt;
    }

    // GetGUIThreadInfo reports coordinates in the caret window's own DPI context.
    const bool mapped = ClientToScreen(gui.hwndCaret, &topLeft) &&
                        ClientToScreen(gui.hwndCaret, &bottomLeft);
    SetThreadDpiAwarenessContext(previousContext);
    if (!mapped ||
        (GetAwarenessFromDpiAwarenessContext(context) != DPI_AWARENESS_PER_MONITOR_AWARE &&
         (!LogicalToPhysicalPointForPerMonitorDPI(gui.hwndCaret, &topLeft) ||
          !LogicalToPhysicalPointForPerMonitorDPI(gui.hwndCaret, &bottomLeft)))) {
        return std::nullopt;
    }

    return RECT{topLeft.x, topLeft.y, topLeft.x, bottomLeft.y};
}

HFONT CreatePickerFont(int height) {
    return CreateFontW(-height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

HGDIOBJ SelectPickerFont(HDC dc, HFONT font) {
    return SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
}

void DrawLabel(HDC dc, std::wstring_view label, RECT bounds, UINT flags = 0) {
    DrawTextW(dc, label.data(), static_cast<int>(label.size()), &bounds, kLabelFlags | flags);
}

void DrawImageGlyph(HDC dc, RECT bounds, COLORREF color, int stroke, int rounding) {
    const HPEN pen = CreatePen(PS_SOLID, stroke, color);
    if (!pen) {
        return;
    }

    const HGDIOBJ previousPen = SelectObject(dc, pen);
    const HGDIOBJ previousBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom, rounding, rounding);

    const LONG width = bounds.right - bounds.left;
    const LONG height = bounds.bottom - bounds.top;
    const POINT mountains[] = {
        {bounds.left + width / 8, bounds.bottom - height / 5},
        {bounds.left + width / 3, bounds.top + height / 2},
        {bounds.left + width / 2, bounds.bottom - height / 4},
        {bounds.left + width * 2 / 3, bounds.top + height * 3 / 5},
        {bounds.right - width / 8, bounds.bottom - height / 5},
    };
    Polyline(dc, mountains, static_cast<int>(std::size(mountains)));
    const LONG sunSize = std::max(3L, height / 4);
    const LONG sunLeft = bounds.right - width / 3;
    const LONG sunTop = bounds.top + height / 6;
    Ellipse(dc, sunLeft, sunTop, sunLeft + sunSize, sunTop + sunSize);

    SelectObject(dc, previousBrush);
    SelectObject(dc, previousPen);
    DeleteObject(pen);
}

}  // namespace

Picker::~Picker() {
    if (window_) {
        DestroyWindow(window_);
    }
    if (textFont_) {
        DeleteObject(textFont_);
    }
    if (smallFont_) {
        DeleteObject(smallFont_);
    }
}

bool Picker::Initialize(HINSTANCE instance, HWND owner, UINT hoverMessage) {
    owner_ = owner;
    hoverMessage_ = hoverMessage;
    if (window_) {
        return true;
    }

    WNDCLASSW windowClass{};
    windowClass.style = CS_DROPSHADOW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    window_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                              kWindowClass, L"コピー履歴", WS_POPUP, 0, 0, 1, 1,
                              nullptr, nullptr, instance, this);
    return window_ != nullptr;
}

void Picker::Show(const std::vector<PickerItem>& items, int selected, int previewLimit,
                  bool bottomRight, HWND targetWindow) {
    if (!window_) {
        return;
    }

    Hide();
    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        if (!item.clip || item.number < 1 || item.number > kSlotCount) {
            continue;
        }

        auto& slot = (item.clip->kind == ClipKind::Text ? textSlots_ : imageSlots_)[item.number - 1];
        slot.itemIndex = static_cast<int>(index);
        if (item.clip->kind == ClipKind::Text) {
            hasText_ = true;
            slot.preview = ClipTextPreview(item.clip->text, previewLimit);
            if (slot.preview.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
                slot.preview = L"（空白）";
            }
        } else {
            hasImages_ = true;
        }
    }
    selected_ = selected;

    if (!targetWindow) {
        targetWindow = GetForegroundWindow();
    }
    auto caret = bottomRight ? std::nullopt : CaretBounds(targetWindow);
    HMONITOR monitor = caret ? MonitorFromPoint({caret->left, caret->top}, MONITOR_DEFAULTTONULL) : nullptr;
    if (!monitor) {
        caret.reset();
        monitor = MonitorFromWindow(targetWindow, MONITOR_DEFAULTTOPRIMARY);
    }

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) {
        monitorInfo.rcWork = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    const RECT work = monitorInfo.rcWork;

    // Measure the popup at the destination monitor's DPI, independently of the target app.
    if (monitor && MonitorFromWindow(window_, MONITOR_DEFAULTTONULL) != monitor) {
        SetWindowPos(window_, nullptr, work.left, work.top, 1, 1,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW);
    }
    const UINT dpi = GetDpiForWindow(window_);
    SetFonts(dpi ? dpi : USER_DEFAULT_SCREEN_DPI);

    const int width = std::min(PreferredWidth(), static_cast<int>(work.right - work.left));
    const int height = std::min(Layout(width), static_cast<int>(work.bottom - work.top));
    const int inset = Scale(12);
    POINT position{work.right - width - inset, work.bottom - height - inset};
    if (caret) {
        const int gap = Scale(5);
        const LONG below = caret->bottom + gap;
        const LONG above = caret->top - gap - height;
        position = {caret->left, below + height > work.bottom && above >= work.top ? above : below};
    }
    position.x = std::clamp(position.x, work.left, work.right - width);
    position.y = std::clamp(position.y, work.top, work.bottom - height);

    const int rounding = Scale(20);
    if (const HRGN region = CreateRoundRectRgn(0, 0, width, height, rounding, rounding)) {
        if (!SetWindowRgn(window_, region, FALSE)) {
            DeleteObject(region);
        }
    }

    hasMousePosition_ = GetCursorPos(&lastMousePosition_) != FALSE;
    SetWindowPos(window_, HWND_TOPMOST, position.x, position.y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(window_, nullptr, FALSE);
}

void Picker::Select(int selected) {
    if (selected_ != selected) {
        selected_ = selected;
        if (window_) {
            InvalidateRect(window_, nullptr, FALSE);
        }
    }
}

int Picker::NewHoveredItem() const {
    POINT position{};
    if (!window_ || !IsWindowVisible(window_) || !GetCursorPos(&position) ||
        (hasMousePosition_ && position.x == lastMousePosition_.x && position.y == lastMousePosition_.y) ||
        !ScreenToClient(window_, &position)) {
        return -1;
    }
    return ItemAt(position);
}

void Picker::Hide() {
    if (window_ && IsWindowVisible(window_)) {
        ShowWindow(window_, SW_HIDE);
    }
    textSlots_ = {};
    imageSlots_ = {};
    divider_ = {};
    emptyRow_ = {};
    hasText_ = false;
    hasImages_ = false;
    hasMousePosition_ = false;
    selected_ = -1;
}

int Picker::Scale(int value) const {
    return MulDiv(value, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI);
}

void Picker::SetFonts(UINT dpi) {
    if (dpi_ == dpi) {
        return;
    }

    dpi_ = dpi;
    if (textFont_) {
        DeleteObject(textFont_);
    }
    if (smallFont_) {
        DeleteObject(smallFont_);
    }
    textFont_ = CreatePickerFont(Scale(16));
    smallFont_ = CreatePickerFont(Scale(12));
}

int Picker::PreferredWidth() const {
    const int minimum = Scale(hasImages_ ? 256 : 204);
    int textWidth = 0;
    if (hasText_) {
        if (const HDC dc = GetDC(window_)) {
            const HGDIOBJ previousFont = SelectPickerFont(dc, textFont_);
            for (const auto& slot : textSlots_) {
                SIZE size{};
                if (!slot.preview.empty() && GetTextExtentPoint32W(
                        dc, slot.preview.data(), static_cast<int>(slot.preview.size()), &size)) {
                    textWidth = std::max(textWidth, static_cast<int>(size.cx));
                }
            }
            SelectObject(dc, previousFont);
            ReleaseDC(window_, dc);
        }
    }
    return std::clamp(textWidth + Scale(50), minimum, Scale(352));
}

int Picker::Layout(int width) {
    const int padding = Scale(6);
    const int rowHeight = Scale(28);
    int y = padding;

    if (hasText_) {
        for (auto& slot : textSlots_) {
            slot.bounds = {padding, y, width - padding, y + rowHeight};
            y += rowHeight;
        }
    }

    if (hasImages_) {
        if (hasText_) {
            y += Scale(4);
            divider_ = {0, y, width, y + Scale(1)};
            y = divider_.bottom + Scale(4);
        }

        const int imageHeight = Scale(49);
        const int cellPadding = Scale(2);
        const int stripWidth = width - padding * 2;
        for (int index = 0; index < kSlotCount; ++index) {
            const int left = padding + stripWidth * index / kSlotCount;
            const int right = padding + stripWidth * (index + 1) / kSlotCount;
            imageSlots_[index].bounds = {left + cellPadding, y, right - cellPadding, y + imageHeight};
        }
        y += imageHeight;
    }

    if (!hasText_ && !hasImages_) {
        emptyRow_ = {padding, y, width - padding, y + rowHeight};
        y += rowHeight;
    }

    return y + padding;
}

void Picker::Paint(HDC dc) const {
    RECT client{};
    GetClientRect(window_, &client);
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));

    if (const HPEN border = CreatePen(PS_SOLID, Scale(1), kBorder)) {
        const HGDIOBJ previousPen = SelectObject(dc, border);
        const HGDIOBJ previousBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        const int rounding = Scale(20);
        RoundRect(dc, client.left, client.top, client.right, client.bottom, rounding, rounding);
        SelectObject(dc, previousBrush);
        SelectObject(dc, previousPen);
        DeleteObject(border);
    }

    SetBkMode(dc, TRANSPARENT);
    if (hasText_) {
        PaintTextSlots(dc);
    }
    if (!IsRectEmpty(&divider_)) {
        if (const HBRUSH brush = CreateSolidBrush(kDivider)) {
            FillRect(dc, &divider_, brush);
            DeleteObject(brush);
        }
    }
    if (hasImages_) {
        PaintImageSlots(dc);
    }
    if (!hasText_ && !hasImages_) {
        const HGDIOBJ previousFont = SelectPickerFont(dc, textFont_);
        SetTextColor(dc, kPlaceholder);
        DrawLabel(dc, L"履歴はまだありません", emptyRow_, DT_CENTER);
        SelectObject(dc, previousFont);
    }
}

void Picker::PaintTextSlots(HDC dc) const {
    const HGDIOBJ previousFont = SelectPickerFont(dc, textFont_);
    for (int index = 0; index < kSlotCount; ++index) {
        const auto& slot = textSlots_[index];
        Highlight(dc, slot);
        SetTextColor(dc, slot.itemIndex >= 0 ? kForeground : kPlaceholder);

        RECT numberBounds = slot.bounds;
        numberBounds.left += Scale(10);
        numberBounds.right = numberBounds.left + Scale(20);
        const wchar_t number = static_cast<wchar_t>(L'1' + index);
        DrawLabel(dc, {&number, 1}, numberBounds);

        RECT labelBounds = slot.bounds;
        labelBounds.left = numberBounds.right;
        labelBounds.right -= Scale(8);
        const std::wstring_view preview = slot.itemIndex >= 0 ? std::wstring_view(slot.preview) : L"—";
        DrawLabel(dc, preview, labelBounds, DT_END_ELLIPSIS);
    }
    SelectObject(dc, previousFont);
}

void Picker::PaintImageSlots(HDC dc) const {
    const HGDIOBJ previousFont = SelectPickerFont(dc, smallFont_);
    for (int index = 0; index < kSlotCount; ++index) {
        const auto& slot = imageSlots_[index];
        Highlight(dc, slot);
        const COLORREF color = slot.itemIndex >= 0 ? kForeground : kPlaceholder;
        const int iconWidth = Scale(25);
        const LONG iconLeft = (slot.bounds.left + slot.bounds.right - iconWidth) / 2;
        const LONG iconTop = slot.bounds.top + Scale(7);
        DrawImageGlyph(dc, {iconLeft, iconTop, iconLeft + iconWidth, iconTop + Scale(20)},
                       color, Scale(1), Scale(3));

        RECT labelBounds = slot.bounds;
        labelBounds.top += Scale(28);
        labelBounds.bottom -= Scale(3);
        SetTextColor(dc, color);
        const wchar_t number = static_cast<wchar_t>(L'1' + index);
        DrawLabel(dc, {&number, 1}, labelBounds, DT_CENTER);
    }
    SelectObject(dc, previousFont);
}

void Picker::Highlight(HDC dc, const Slot& slot) const {
    if (slot.itemIndex < 0 || slot.itemIndex != selected_) {
        return;
    }

    if (const HBRUSH brush = CreateSolidBrush(kSelection)) {
        const HGDIOBJ previousBrush = SelectObject(dc, brush);
        const HGDIOBJ previousPen = SelectObject(dc, GetStockObject(NULL_PEN));
        const int rounding = Scale(12);
        const RECT& bounds = slot.bounds;
        RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom, rounding, rounding);
        SelectObject(dc, previousPen);
        SelectObject(dc, previousBrush);
        DeleteObject(brush);
    }
}

int Picker::ItemAt(POINT point) const {
    for (const auto* slots : {&textSlots_, &imageSlots_}) {
        for (const auto& slot : *slots) {
            if (slot.itemIndex >= 0 && PtInRect(&slot.bounds, point)) {
                return slot.itemIndex;
            }
        }
    }
    return -1;
}

void Picker::MouseMove(POINT point) {
    if (!IsWindowVisible(window_)) {
        return;
    }

    POINT screenPoint = point;
    if (ClientToScreen(window_, &screenPoint)) {
        if (hasMousePosition_ && screenPoint.x == lastMousePosition_.x && screenPoint.y == lastMousePosition_.y) {
            return;
        }
        lastMousePosition_ = screenPoint;
        hasMousePosition_ = true;
    }

    const int index = ItemAt(point);
    if (index >= 0 && index != selected_) {
        SendMessageW(owner_, hoverMessage_, static_cast<WPARAM>(index), 0);
    }
}

LRESULT CALLBACK Picker::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    const auto picker = reinterpret_cast<Picker*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!picker) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
        picker->MouseMove({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        if (const HDC dc = BeginPaint(window, &paint)) {
            picker->Paint(dc);
            EndPaint(window, &paint);
        }
        return 0;
    }
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        picker->window_ = nullptr;
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace mcs
