#include "picker.h"

#include "caret.h"
#include "clipboard.h"

#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <string_view>
#include <utility>

namespace mcs {
namespace {

constexpr wchar_t kWindowClass[] = L"MultiCopySystem.HistoryPicker";
constexpr COLORREF kForeground = RGB(32, 33, 36);
constexpr COLORREF kPlaceholder = RGB(112, 114, 121);
constexpr COLORREF kBorder = RGB(172, 174, 179);
constexpr COLORREF kDivider = RGB(219, 220, 224);
constexpr COLORREF kSelection = RGB(255, 247, 216);
constexpr UINT kLabelFlags = DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
constexpr UINT kAnchorReady = WM_APP + 55;
constexpr UINT_PTR kAnchorFallbackTimer = 1;
constexpr UINT kFallbackDelayMs = 75;
constexpr ULONGLONG kLateAnchorLimitMs = 300;

POINT WindowAnchor(HWND target, const RECT& bounds) {
    POINT pointer{};
    if (GetPhysicalCursorPos(&pointer)) {
        const HWND hovered = WindowFromPoint(pointer);
        if (hovered && GetAncestor(hovered, GA_ROOT) == target && PtInRect(&bounds, pointer)) {
            return pointer;
        }
        if (!PtInRect(&bounds, pointer)) {
            return {std::clamp(pointer.x, bounds.left, bounds.right - 1),
                    std::clamp(pointer.y, bounds.top, bounds.bottom - 1)};
        }
    }
    return {bounds.left + (bounds.right - bounds.left) / 2, bounds.top + (bounds.bottom - bounds.top) / 2};
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

struct Picker::AnchorQuery {
    HWND window;
    HWND target;
    UINT_PTR generation;
    ULONGLONG started;
    std::optional<RECT> caret;
    std::atomic<bool> finished{false};
};

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
                  bool bottomRight, HWND targetWindow, bool filesOnly) {
    if (!window_) {
        return;
    }

    Hide();
    filesOnly_ = filesOnly;
    bottomRight_ = bottomRight;
    targetWindow_ = targetWindow ? targetWindow : GetForegroundWindow();
    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        if (!item.clip || item.number < 1 || item.number > kSlotCount
            || (item.clip->kind == ClipKind::Files) != filesOnly_) {
            continue;
        }

        auto& slot = (item.clip->kind == ClipKind::Image ? imageSlots_ : listSlots_)[item.number - 1];
        slot.itemIndex = static_cast<int>(index);
        switch (item.clip->kind) {
        case ClipKind::Text:
            hasList_ = true;
            slot.preview = ClipTextPreview(item.clip->text, previewLimit);
            if (slot.preview.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
                slot.preview = L"（空白）";
            }
            break;
        case ClipKind::Image:
            hasImages_ = true;
            break;
        case ClipKind::Files:
            hasList_ = true;
            slot.preview = ClipFilesPreview(*item.clip, previewLimit);
            break;
        }
    }
    selected_ = selected;

    const auto caret = bottomRight_ ? std::nullopt : Win32CaretBounds(targetWindow_);
    if (!caret && !bottomRight_ && targetWindow_ && RequestAnchor()) {
        if (SetTimer(window_, kAnchorFallbackTimer, kFallbackDelayMs, nullptr)) {
            return;
        }
    }
    Present(caret);
}

bool Picker::RequestAnchor() {
    if (const auto worker = anchorWorker_.lock(); worker && !worker->finished.load(std::memory_order_acquire)) {
        return false;
    }
    auto query = std::make_shared<AnchorQuery>(window_, targetWindow_, ++anchorGeneration_, GetTickCount64());
    auto* context = new std::shared_ptr<AnchorQuery>(query);
    if (!TrySubmitThreadpoolCallback(ResolveAnchor, context, nullptr)) {
        delete context;
        return false;
    }
    anchorWorker_ = query;
    anchorQuery_ = std::move(query);
    return true;
}

void CALLBACK Picker::ResolveAnchor(PTP_CALLBACK_INSTANCE, void* context) {
    const std::unique_ptr<std::shared_ptr<AnchorQuery>> argument(static_cast<std::shared_ptr<AnchorQuery>*>(context));
    const auto& query = *argument;
    query->caret = AutomationCaretBounds(query->target);
    query->finished.store(true, std::memory_order_release);
    PostMessageW(query->window, kAnchorReady, query->generation, 0);
}

void Picker::AnchorReady(UINT_PTR generation) {
    if (!anchorQuery_ || anchorQuery_->generation != generation || !anchorQuery_->finished.load(std::memory_order_acquire)) {
        return;
    }
    const auto query = std::exchange(anchorQuery_, {});
    KillTimer(window_, kAnchorFallbackTimer);
    if (GetForegroundWindow() != targetWindow_) {
        Hide();
        return;
    }
    if (!IsWindowVisible(window_)) {
        Present(query->caret);
        return;
    }
    POINT mouse{};
    const bool unchanged = !hasMousePosition_ || (GetPhysicalCursorPos(&mouse)
        && mouse.x == lastMousePosition_.x && mouse.y == lastMousePosition_.y);
    if (query->caret && unchanged && GetTickCount64() - query->started <= kLateAnchorLimitMs) {
        Present(query->caret);
    }
}

void Picker::Present(std::optional<RECT> caret) {
    if (targetWindow_ && GetForegroundWindow() != targetWindow_) {
        return;
    }
    RECT targetBounds{};
    const bool hasTarget = GetWindowRect(targetWindow_, &targetBounds) && !IsRectEmpty(&targetBounds);
    const bool useTargetAnchor = !bottomRight_ && !caret && hasTarget;
    auto anchor = caret;
    if (useTargetAnchor) {
        const POINT point = WindowAnchor(targetWindow_, targetBounds);
        anchor = RECT{point.x, point.y, point.x, point.y};
    }
    HMONITOR monitor = anchor ? MonitorFromPoint({anchor->left, anchor->top}, MONITOR_DEFAULTTONULL) : nullptr;
    if (!monitor) {
        monitor = MonitorFromWindow(targetWindow_, MONITOR_DEFAULTTOPRIMARY);
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
    RECT limits = work;
    if (useTargetAnchor) {
        RECT visible{};
        if (IntersectRect(&visible, &targetBounds, &work)) {
            if (visible.right - visible.left >= width) {
                limits.left = visible.left;
                limits.right = visible.right;
            }
            if (visible.bottom - visible.top >= height) {
                limits.top = visible.top;
                limits.bottom = visible.bottom;
            }
        }
    }
    if (anchor) {
        const int gap = Scale(5);
        const LONG below = anchor->bottom + gap;
        const LONG above = anchor->top - gap - height;
        position = {anchor->left, below + height > limits.bottom && above >= limits.top ? above : below};
    }
    position.x = std::clamp(position.x, limits.left, limits.right - width);
    position.y = std::clamp(position.y, limits.top, limits.bottom - height);

    const int rounding = Scale(20);
    if (const HRGN region = CreateRoundRectRgn(0, 0, width, height, rounding, rounding)) {
        if (!SetWindowRgn(window_, region, FALSE)) {
            DeleteObject(region);
        }
    }

    hasMousePosition_ = GetPhysicalCursorPos(&lastMousePosition_) != FALSE;
    SetWindowPos(window_, HWND_TOPMOST, position.x, position.y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(window_, nullptr, FALSE);
}

void Picker::Select(int selected) {
    if (selected_ != selected) {
        selected_ = selected;
        if (window_) {
            if (IsWindowVisible(window_)) {
                anchorQuery_.reset();
            }
            InvalidateRect(window_, nullptr, FALSE);
        }
    }
}

int Picker::NewHoveredItem() const {
    POINT position{};
    if (!window_ || !IsWindowVisible(window_) || !GetPhysicalCursorPos(&position) ||
        (hasMousePosition_ && position.x == lastMousePosition_.x && position.y == lastMousePosition_.y) ||
        !ScreenToClient(window_, &position)) {
        return -1;
    }
    return ItemAt(position);
}

void Picker::Hide() {
    if (window_) {
        KillTimer(window_, kAnchorFallbackTimer);
    }
    if (window_ && IsWindowVisible(window_)) {
        ShowWindow(window_, SW_HIDE);
    }
    targetWindow_ = nullptr;
    anchorQuery_.reset();
    listSlots_ = {};
    imageSlots_ = {};
    divider_ = {};
    emptyRow_ = {};
    hasList_ = false;
    hasImages_ = false;
    filesOnly_ = false;
    bottomRight_ = false;
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
    if (hasList_) {
        if (const HDC dc = GetDC(window_)) {
            const HGDIOBJ previousFont = SelectPickerFont(dc, textFont_);
            for (const auto& slot : listSlots_) {
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

    if (hasList_) {
        for (auto& slot : listSlots_) {
            slot.bounds = {padding, y, width - padding, y + rowHeight};
            y += rowHeight;
        }
    }

    if (hasImages_) {
        if (hasList_) {
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

    if (!hasList_ && !hasImages_) {
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
    if (hasList_) {
        PaintListSlots(dc);
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
    if (!hasList_ && !hasImages_) {
        const HGDIOBJ previousFont = SelectPickerFont(dc, textFont_);
        SetTextColor(dc, kPlaceholder);
        DrawLabel(dc, filesOnly_ ? L"ファイルの履歴はまだありません" : L"履歴はまだありません", emptyRow_, DT_CENTER);
        SelectObject(dc, previousFont);
    }
}

void Picker::PaintListSlots(HDC dc) const {
    const HGDIOBJ previousFont = SelectPickerFont(dc, textFont_);
    for (int index = 0; index < kSlotCount; ++index) {
        const auto& slot = listSlots_[index];
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
    for (const auto* slots : {&listSlots_, &imageSlots_}) {
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
    case kAnchorReady:
        picker->AnchorReady(wParam);
        return 0;
    case WM_TIMER:
        if (wParam == kAnchorFallbackTimer) {
            KillTimer(window, kAnchorFallbackTimer);
            if (picker->anchorQuery_ && !IsWindowVisible(window)) {
                const auto& query = *picker->anchorQuery_;
                if (query.finished.load(std::memory_order_acquire)) {
                    picker->AnchorReady(query.generation);
                } else {
                    picker->Present(std::nullopt);
                }
            }
            return 0;
        }
        break;
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
