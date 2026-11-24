#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mcs {

enum class ClipKind { Text, Image, Files = 2 };
enum class ImageFormat { Dib, DibV5, Png };

struct ClipData {
    ClipKind kind = ClipKind::Text;
    std::wstring text;
    ImageFormat imageFormat = ImageFormat::Dib;
    std::vector<std::uint8_t> image;
    std::vector<std::wstring> files;
};

inline constexpr std::size_t kMaxClipBytes = 16 * 1024 * 1024;

enum class ClipReadStatus { Found, Empty, Busy };

struct ClipReadResult {
    ClipReadStatus status = ClipReadStatus::Empty;
    std::shared_ptr<const ClipData> clip;
};

ClipReadResult ReadClipboard(HWND window, bool honorExclusions = true);
bool WriteClipboard(HWND window, const ClipData& clip);
std::size_t ClipBytes(const ClipData& clip);
bool IsValidClip(const ClipData& clip);
std::wstring ClipTextPreview(std::wstring_view text, int characterLimit);
std::wstring ClipFilesPreview(const ClipData& clip, int characterLimit);

} // namespace mcs
