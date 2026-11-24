#pragma once

#include <windows.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace mcs {

struct ClipData;

struct PickerItem {
    std::shared_ptr<const ClipData> clip;
    int number;
};

class Picker {
public:
    Picker() = default;
    ~Picker();
    Picker(const Picker&) = delete;
    Picker& operator=(const Picker&) = delete;

    bool Initialize(HINSTANCE instance, HWND owner, UINT hoverMessage);
    void Show(const std::vector<PickerItem>& items, int selected, int previewLimit,
              bool bottomRight, HWND targetWindow);
    void Select(int selected);
    int NewHoveredItem() const;
    void Hide();

private:
    static constexpr int kSlotCount = 5;

    struct Slot {
        int itemIndex = -1;
        RECT bounds{};
        std::wstring preview;
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    int Scale(int value) const;
    void SetFonts(UINT dpi);
    int PreferredWidth() const;
    int Layout(int width);
    void Paint(HDC dc) const;
    void PaintTextSlots(HDC dc) const;
    void PaintImageSlots(HDC dc) const;
    void Highlight(HDC dc, const Slot& slot) const;
    int ItemAt(POINT point) const;
    void MouseMove(POINT point);

    HWND window_ = nullptr;
    HWND owner_ = nullptr;
    UINT hoverMessage_ = 0;
    UINT dpi_ = 0;
    HFONT textFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    std::array<Slot, kSlotCount> textSlots_{};
    std::array<Slot, kSlotCount> imageSlots_{};
    RECT divider_{};
    RECT emptyRow_{};
    bool hasText_ = false;
    bool hasImages_ = false;
    int selected_ = -1;
    POINT lastMousePosition_{};
    bool hasMousePosition_ = false;
};

}  // namespace mcs
