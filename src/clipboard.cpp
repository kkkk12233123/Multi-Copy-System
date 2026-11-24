#include "clipboard.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>
#include <span>
#include <utility>

namespace mcs {
namespace {

using Bytes = std::span<const std::uint8_t>;

class OpenClipboardScope {
public:
    explicit OpenClipboardScope(HWND window) : open_(::OpenClipboard(window) != FALSE) {}
    ~OpenClipboardScope() {
        if (open_) {
            CloseClipboard();
        }
    }
    explicit operator bool() const { return open_; }

private:
    bool open_;
};

class GlobalMemoryView {
public:
    explicit GlobalMemoryView(HGLOBAL memory) : memory_(memory), data_(GlobalLock(memory)) {}
    ~GlobalMemoryView() {
        if (data_) {
            GlobalUnlock(memory_);
        }
    }
    void* Data() const { return data_; }

private:
    HGLOBAL memory_;
    void* data_;
};

std::uint32_t ReadBigEndian32(Bytes data, std::size_t offset) {
    return (std::uint32_t{data[offset]} << 24) | (std::uint32_t{data[offset + 1]} << 16)
        | (std::uint32_t{data[offset + 2]} << 8) | std::uint32_t{data[offset + 3]};
}

std::size_t PngSize(Bytes data) {
    constexpr std::array<std::uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    constexpr std::uint32_t ihdr = 0x49484452;
    constexpr std::uint32_t idat = 0x49444154;
    constexpr std::uint32_t iend = 0x49454e44;
    if (data.size() < 33 || !std::equal(signature.begin(), signature.end(), data.begin())
        || ReadBigEndian32(data, 8) != 13 || ReadBigEndian32(data, 12) != ihdr) {
        return 0;
    }

    const auto width = ReadBigEndian32(data, 16);
    const auto height = ReadBigEndian32(data, 20);
    if (!width || !height || width > 0x7fffffff || height > 0x7fffffff) {
        return 0;
    }

    bool hasImageData = false;
    for (std::size_t offset = 33; data.size() - offset >= 12;) {
        const auto length = ReadBigEndian32(data, offset);
        if (length > data.size() - offset - 12) {
            return 0;
        }
        const auto type = ReadBigEndian32(data, offset + 4);
        offset += 12 + length;
        if (type == iend) {
            return hasImageData && length == 0 ? offset : 0;
        }
        if (type == idat && length > 0) {
            hasImageData = true;
        }
    }
    return 0;
}

std::size_t PackedBitmapSize(Bytes data, std::uint64_t offset, std::uint64_t width,
    std::uint64_t rows, unsigned bitsPerPixel) {
    const std::uint64_t stride = ((width * bitsPerPixel + 31) / 32) * 4;
    if (!stride || offset > data.size() || rows > (data.size() - offset) / stride) {
        return 0;
    }
    return static_cast<std::size_t>(offset + rows * stride);
}

std::size_t CoreBitmapSize(Bytes data) {
    BITMAPCOREHEADER header{};
    std::memcpy(&header, data.data(), sizeof(header));
    const WORD depth = header.bcBitCount;
    if (!header.bcWidth || !header.bcHeight || header.bcPlanes != 1
        || (depth != 1 && depth != 4 && depth != 8 && depth != 24)) {
        return 0;
    }
    const std::uint64_t paletteBytes = depth <= 8 ? (std::uint64_t{1} << depth) * sizeof(RGBTRIPLE) : 0;
    return PackedBitmapSize(data, sizeof(header) + paletteBytes, header.bcWidth,
        header.bcHeight, depth);
}

std::size_t DibSize(Bytes data, bool needsV5Header) {
    if (data.size() < sizeof(BITMAPCOREHEADER)) {
        return 0;
    }
    DWORD headerSize{};
    std::memcpy(&headerSize, data.data(), sizeof(headerSize));
    if (headerSize > data.size() || (needsV5Header && headerSize != sizeof(BITMAPV5HEADER))) {
        return 0;
    }
    if (headerSize == sizeof(BITMAPCOREHEADER)) {
        return CoreBitmapSize(data);
    }
    if (headerSize != sizeof(BITMAPINFOHEADER) && headerSize != 52 && headerSize != 56
        && headerSize != sizeof(BITMAPV4HEADER) && headerSize != sizeof(BITMAPV5HEADER)) {
        return 0;
    }

    BITMAPINFOHEADER header{};
    std::memcpy(&header, data.data(), sizeof(header));
    if (header.biWidth <= 0 || !header.biHeight || header.biPlanes != 1) {
        return 0;
    }

    constexpr DWORD alphaBitfields = 6;
    const DWORD compression = header.biCompression;
    const WORD depth = header.biBitCount;
    const bool bitfields = compression == BI_BITFIELDS || compression == alphaBitfields;
    const bool compressed = compression == BI_RLE4 || compression == BI_RLE8
        || compression == BI_JPEG || compression == BI_PNG;
    const bool indexed = depth == 1 || depth == 4 || depth == 8;
    const bool validDepth = indexed || depth == 16 || depth == 24 || depth == 32;
    if ((!compressed && !validDepth) || (bitfields && depth != 16 && depth != 32)
        || (!compressed && !bitfields && compression != BI_RGB)
        || (compressed && header.biHeight < 0) || (compression == BI_RLE4 && depth != 4)
        || (compression == BI_RLE8 && depth != 8)) {
        return 0;
    }

    const DWORD defaultPaletteSize = indexed ? 1u << depth : 0;
    if (indexed && header.biClrUsed > defaultPaletteSize) {
        return 0;
    }
    const DWORD paletteSize = header.biClrUsed ? header.biClrUsed : defaultPaletteSize;
    const std::uint64_t maskBytes = bitfields && headerSize == sizeof(BITMAPINFOHEADER)
        ? (compression == alphaBitfields ? 4 : 3) * sizeof(DWORD) : 0;
    const std::uint64_t pixelOffset = headerSize + maskBytes + std::uint64_t{paletteSize} * sizeof(RGBQUAD);
    if (pixelOffset > data.size()) {
        return 0;
    }

    std::size_t needed{};
    if (compressed) {
        if (!header.biSizeImage || header.biSizeImage > data.size() - pixelOffset) {
            return 0;
        }
        needed = static_cast<std::size_t>(pixelOffset + header.biSizeImage);
    } else {
        const auto height = static_cast<std::int64_t>(header.biHeight);
        needed = PackedBitmapSize(data, pixelOffset, static_cast<std::uint64_t>(header.biWidth),
            static_cast<std::uint64_t>(height < 0 ? -height : height), depth);
        if (!needed) {
            return 0;
        }
    }

    if (headerSize == sizeof(BITMAPV5HEADER)) {
        BITMAPV5HEADER v5{};
        std::memcpy(&v5, data.data(), sizeof(v5));
        if (v5.bV5CSType == PROFILE_LINKED || v5.bV5CSType == PROFILE_EMBEDDED) {
            const auto offset = v5.bV5ProfileData;
            if (!v5.bV5ProfileSize || offset < needed || offset > data.size()
                || v5.bV5ProfileSize > data.size() - offset) {
                return 0;
            }
            needed = static_cast<std::size_t>(offset) + v5.bV5ProfileSize;
        }
    }
    return needed;
}

std::size_t ImageSize(Bytes data, ImageFormat format) {
    switch (format) {
    case ImageFormat::Dib:
        return DibSize(data, false);
    case ImageFormat::DibV5:
        return DibSize(data, true);
    case ImageFormat::Png:
        return PngSize(data);
    }
    return 0;
}

UINT PngFormat() {
    static const UINT format = RegisterClipboardFormatW(L"PNG");
    return format;
}

bool IsExcluded() {
    static const UINT exclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    static const std::array<UINT, 2> historyFormats{
        RegisterClipboardFormatW(L"CanIncludeInClipboardHistory"),
        RegisterClipboardFormatW(L"IncludeClipboardFormatsInClipboardHistory"),
    };
    if (exclude && IsClipboardFormatAvailable(exclude)) {
        return true;
    }
    for (const UINT format : historyFormats) {
        if (!format || !IsClipboardFormatAvailable(format)) {
            continue;
        }
        const HGLOBAL memory = GetClipboardData(format);
        if (!memory || GlobalSize(memory) < sizeof(DWORD)) {
            continue;
        }
        const GlobalMemoryView view(memory);
        DWORD canInclude{};
        if (view.Data()) {
            std::memcpy(&canInclude, view.Data(), sizeof(canInclude));
            if (!canInclude) {
                return true;
            }
        }
    }
    return false;
}

std::shared_ptr<const ClipData> ReadText() {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        return {};
    }
    const HGLOBAL memory = GetClipboardData(CF_UNICODETEXT);
    if (!memory) {
        return {};
    }
    constexpr std::size_t maxCharacters = kMaxClipBytes / sizeof(wchar_t);
    const auto capacity = std::min(GlobalSize(memory) / sizeof(wchar_t), maxCharacters + 1);
    const GlobalMemoryView view(memory);
    const auto* text = static_cast<const wchar_t*>(view.Data());
    if (!text || !capacity) {
        return {};
    }
    const auto* end = std::wmemchr(text, L'\0', capacity);
    if (!end || end == text) {
        return {};
    }
    auto clip = std::make_shared<ClipData>();
    clip->text.assign(text, end);
    return clip;
}

std::shared_ptr<const ClipData> ReadImage(UINT clipboardFormat, ImageFormat imageFormat) {
    if (!clipboardFormat || !IsClipboardFormatAvailable(clipboardFormat)) {
        return {};
    }
    const HGLOBAL memory = GetClipboardData(clipboardFormat);
    if (!memory) {
        return {};
    }
    const std::size_t length = std::min<std::size_t>(GlobalSize(memory), kMaxClipBytes);
    const GlobalMemoryView view(memory);
    const auto* image = static_cast<const std::uint8_t*>(view.Data());
    if (!image || !length) {
        return {};
    }
    const std::size_t imageLength = ImageSize(Bytes{image, length}, imageFormat);
    if (!imageLength) {
        return {};
    }
    auto clip = std::make_shared<ClipData>();
    clip->kind = ClipKind::Image;
    clip->imageFormat = imageFormat;
    clip->image.assign(image, image + imageLength);
    return clip;
}

UINT WriteFormat(const ClipData& clip) {
    if (clip.kind == ClipKind::Text) {
        return CF_UNICODETEXT;
    }
    switch (clip.imageFormat) {
    case ImageFormat::Dib:
        return CF_DIB;
    case ImageFormat::DibV5:
        return CF_DIBV5;
    case ImageFormat::Png:
        return PngFormat();
    }
    return 0;
}

} // namespace

ClipReadResult ReadClipboard(HWND window, bool honorExclusions) {
    const OpenClipboardScope clipboard(window);
    if (!clipboard) {
        return {ClipReadStatus::Busy, {}};
    }
    if (honorExclusions && IsExcluded()) {
        return {};
    }
    if (auto clip = ReadText()) {
        return {ClipReadStatus::Found, std::move(clip)};
    }
    if (auto clip = ReadImage(CF_DIBV5, ImageFormat::DibV5)) {
        return {ClipReadStatus::Found, std::move(clip)};
    }
    if (auto clip = ReadImage(CF_DIB, ImageFormat::Dib)) {
        return {ClipReadStatus::Found, std::move(clip)};
    }
    if (auto clip = ReadImage(PngFormat(), ImageFormat::Png)) {
        return {ClipReadStatus::Found, std::move(clip)};
    }
    return {};
}

bool WriteClipboard(HWND window, const ClipData& clip) {
    if (!window || !IsValidClip(clip)) {
        return false;
    }
    const UINT format = WriteFormat(clip);
    if (!format) {
        return false;
    }
    const bool text = clip.kind == ClipKind::Text;
    const void* source = text ? static_cast<const void*>(clip.text.c_str()) : clip.image.data();
    const auto length = ClipBytes(clip) + (text ? sizeof(wchar_t) : 0);
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, length);
    if (!memory) {
        return false;
    }
    bool copied = false;
    {
        const GlobalMemoryView view(memory);
        if (view.Data()) {
            std::memcpy(view.Data(), source, length);
            copied = true;
        }
    }
    if (copied) {
        const OpenClipboardScope clipboard(window);
        if (clipboard && EmptyClipboard() && SetClipboardData(format, memory)) {
            return true;
        }
    }
    GlobalFree(memory);
    return false;
}

std::size_t ClipBytes(const ClipData& clip) {
    return clip.kind == ClipKind::Text ? clip.text.size() * sizeof(wchar_t) : clip.image.size();
}

bool IsValidClip(const ClipData& clip) {
    const auto bytes = ClipBytes(clip);
    if (!bytes || bytes > kMaxClipBytes) {
        return false;
    }
    if (clip.kind == ClipKind::Text) {
        return clip.text.find(L'\0') == std::wstring::npos;
    }
    return clip.kind == ClipKind::Image && ImageSize(clip.image, clip.imageFormat) == bytes;
}

std::wstring ClipTextPreview(std::wstring_view text, int characterLimit) {
    if (characterLimit <= 0) {
        return {};
    }
    std::wstring preview;
    preview.reserve(std::min(text.size(), static_cast<std::size_t>(characterLimit)));
    std::size_t offset = 0;
    std::size_t lastCharacter = 0;
    for (int count = 0; count < characterLimit && offset < text.size(); ++count) {
        lastCharacter = preview.size();
        const wchar_t value = text[offset++];
        if (value == L'\r' && offset < text.size() && text[offset] == L'\n') {
            ++offset;
        }
        if (value < L' ' || value == L'\u0085' || value == L'\u2028' || value == L'\u2029') {
            preview.push_back(L' ');
        } else {
            preview.push_back(value);
            if (value >= 0xd800 && value <= 0xdbff && offset < text.size()
                && text[offset] >= 0xdc00 && text[offset] <= 0xdfff) {
                preview.push_back(text[offset++]);
            }
        }
    }
    if (offset < text.size()) {
        preview.resize(lastCharacter);
        preview.push_back(L'\u2026');
    }
    return preview;
}

} // namespace mcs
