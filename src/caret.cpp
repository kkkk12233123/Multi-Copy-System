#include "caret.h"

#include <ole2.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <memory>

namespace mcs {
namespace {

using Microsoft::WRL::ComPtr;

bool BoundsOnTarget(HWND target, const RECT& bounds) {
    RECT window{};
    return bounds.bottom > bounds.top && bounds.right >= bounds.left && GetWindowRect(target, &window)
        && bounds.left >= window.left && bounds.left <= window.right
        && bounds.top < window.bottom && bounds.bottom > window.top;
}

std::optional<RECT> RangeBounds(IUIAutomation2& automation, IUIAutomationTextRange& range, bool trailing) {
    SAFEARRAY* array = nullptr;
    const HRESULT read = range.GetBoundingRectangles(&array);
    const std::unique_ptr<SAFEARRAY, decltype(&SafeArrayDestroy)> rectangles(array, SafeArrayDestroy);
    if (FAILED(read) || !rectangles) {
        return std::nullopt;
    }

    RECT* result = nullptr;
    int count = 0;
    const HRESULT converted = automation.SafeArrayToRectNativeArray(rectangles.get(), &result, &count);
    const std::unique_ptr<RECT, decltype(&CoTaskMemFree)> bounds(result, CoTaskMemFree);
    if (FAILED(converted) || !bounds || count <= 0) {
        return std::nullopt;
    }
    const RECT rectangle = result[trailing ? count - 1 : 0];
    const LONG edge = trailing ? rectangle.right : rectangle.left;
    return RECT{edge, rectangle.top, edge, rectangle.bottom};
}

std::optional<RECT> TextPatternBounds(IUIAutomation2& automation, IUIAutomationElement& element) {
    ComPtr<IUIAutomationTextPattern2> pattern;
    if (FAILED(element.GetCurrentPatternAs(UIA_TextPattern2Id, IID_PPV_ARGS(&pattern))) || !pattern) {
        return std::nullopt;
    }

    BOOL active = FALSE;
    ComPtr<IUIAutomationTextRange> range;
    if (FAILED(pattern->GetCaretRange(&active, &range)) || !active || !range) {
        return std::nullopt;
    }
    if (auto bounds = RangeBounds(automation, *range.Get(), false)) {
        return bounds;
    }

    // Providers may omit rectangle data for a zero-length range.
    int moved = 0;
    if (SUCCEEDED(range->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Character, 1, &moved))
        && moved > 0) {
        return RangeBounds(automation, *range.Get(), false);
    }
    if (SUCCEEDED(range->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Character, -1, &moved))
        && moved < 0) {
        return RangeBounds(automation, *range.Get(), true);
    }
    return std::nullopt;
}

std::optional<RECT> FocusedBounds(HWND target) {
    constexpr DWORD providerTimeoutMs = 150;
    ComPtr<IUIAutomation2> automation;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&automation))) || !automation) {
        return std::nullopt;
    }
    automation->put_ConnectionTimeout(providerTimeoutMs);
    automation->put_TransactionTimeout(providerTimeoutMs);

    ComPtr<IUIAutomationElement> focused;
    if (FAILED(automation->GetFocusedElement(&focused)) || !focused) {
        return std::nullopt;
    }
    if (auto caret = TextPatternBounds(*automation.Get(), *focused.Get()); caret && BoundsOnTarget(target, *caret)) {
        return caret;
    }

    CONTROLTYPEID type{};
    RECT bounds{};
    if (FAILED(focused->get_CurrentControlType(&type)) || type != UIA_EditControlTypeId
        || FAILED(focused->get_CurrentBoundingRectangle(&bounds)) || !BoundsOnTarget(target, bounds)) {
        return std::nullopt;
    }

    // For a multiline edit without a caret rectangle, anchor by its first line.
    const UINT dpi = GetDpiForWindow(target);
    bounds.bottom = std::min(bounds.bottom, bounds.top + MulDiv(28, dpi ? dpi : USER_DEFAULT_SCREEN_DPI,
                                                               USER_DEFAULT_SCREEN_DPI));
    return bounds;
}

} // namespace

std::optional<RECT> Win32CaretBounds(HWND target) {
    const DWORD thread = GetWindowThreadProcessId(target, nullptr);
    GUITHREADINFO gui{};
    gui.cbSize = sizeof(gui);
    if (!thread || !GetGUIThreadInfo(thread, &gui) || !gui.hwndCaret || gui.rcCaret.bottom <= gui.rcCaret.top
        || (gui.hwndCaret != target && !IsChild(target, gui.hwndCaret))) {
        return std::nullopt;
    }

    POINT top{gui.rcCaret.left, gui.rcCaret.top};
    POINT bottom{gui.rcCaret.left, gui.rcCaret.bottom};
    const auto context = GetWindowDpiAwarenessContext(gui.hwndCaret);
    const auto previous = SetThreadDpiAwarenessContext(context);
    if (!previous) {
        return std::nullopt;
    }
    // The caret rectangle uses the caret window's logical client coordinates.
    const bool mapped = ClientToScreen(gui.hwndCaret, &top) && ClientToScreen(gui.hwndCaret, &bottom);
    SetThreadDpiAwarenessContext(previous);
    if (!mapped || (GetAwarenessFromDpiAwarenessContext(context) != DPI_AWARENESS_PER_MONITOR_AWARE
        && (!LogicalToPhysicalPointForPerMonitorDPI(gui.hwndCaret, &top)
            || !LogicalToPhysicalPointForPerMonitorDPI(gui.hwndCaret, &bottom)))) {
        return std::nullopt;
    }

    const RECT bounds{top.x, top.y, top.x, bottom.y};
    return BoundsOnTarget(target, bounds) ? std::optional{bounds} : std::nullopt;
}

std::optional<RECT> AutomationCaretBounds(HWND target) {
    if (!target || GetForegroundWindow() != target || FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        return std::nullopt;
    }
    const auto bounds = FocusedBounds(target);
    CoUninitialize();
    return GetForegroundWindow() == target ? bounds : std::nullopt;
}

} // namespace mcs
