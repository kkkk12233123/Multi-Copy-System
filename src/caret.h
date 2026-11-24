#pragma once

#include <windows.h>

#include <optional>

namespace mcs {

std::optional<RECT> Win32CaretBounds(HWND target);
std::optional<RECT> AutomationCaretBounds(HWND target);

} // namespace mcs
