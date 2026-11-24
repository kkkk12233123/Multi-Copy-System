#include "clipboard.h"

#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>
#include <optional>
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

class ClipboardMemory {
public:
    explicit ClipboardMemory(std::size_t bytes) : memory_(GlobalAlloc(GMEM_MOVEABLE, bytes)) {}
    ~ClipboardMemory() {
        if (memory_) {
            GlobalFree(memory_);
        }
    }
    ClipboardMemory(const ClipboardMemory&) = delete;
    ClipboardMemory& operator=(const ClipboardMemory&) = delete;
    HGLOBAL Get() const { return memory_; }

    bool Publish(UINT format) {
        if (!SetClipboardData(format, memory_)) {
            return false;
        }
        memory_ = nullptr;
        return true;
    }

private:
    HGLOBAL memory_;
};

bool IsPathSeparator(wchar_t character) {
    return character == L'\\' || character == L'/';
}

bool IsAbsoluteFilePath(std::wstring_view path) {
    if (path.size() < 3 || path.find(L'\0') != std::wstring_view::npos) {
        return false;
    }
    const bool drive = (path.front() >= L'A' && path.front() <= L'Z')
        || (path.front() >= L'a' && path.front() <= L'z');
    return (drive && path[1] == L':' && IsPathSeparator(path[2])) ||
        (IsPathSeparator(path[0]) && IsPathSeparator(path[1]) && !IsPathSeparator(path[2]));
}

struct DropSizes {
    std::size_t bufferBytes = sizeof(DROPFILES) + sizeof(wchar_t);
    std::size_t retainedBytes = bufferBytes;

    bool AddPath(std::size_t characters) {
        if (characters >= (kMaxClipBytes - retainedBytes) / sizeof(wchar_t)) {
            return false;
        }
        const auto bytes = (characters + 1) * sizeof(wchar_t);
        if (sizeof(std::wstring) > kMaxClipBytes - retainedBytes - bytes) {
            return false;
        }
        bufferBytes += bytes;
        retainedBytes += bytes + sizeof(std::wstring);
        return true;
    }
};

std::optional<DropSizes> FileDropSizes(const ClipData& clip) {
    if (clip.files.empty()) {
        return {};
    }
    DropSizes sizes;
    for (const auto& path : clip.files) {
        if (!IsAbsoluteFilePath(path) || !sizes.AddPath(path.size())) {
            return {};
        }
    }
    return sizes;
}

std::wstring WidePath(std::wstring_view path) {
    return std::wstring(path);
}

std::wstring WidePath(std::string_view path) {
    const auto bytes = static_cast<int>(path.size());
    const int length = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, path.data(), bytes, nullptr, 0);
    if (!length) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, path.data(), bytes, result.data(), length) != length) {
        return {};
    }
    return result;
}

template <class Char>
std::shared_ptr<const ClipData> ReadDropNames(std::basic_string_view<Char> names) {
    auto clip = std::make_shared<ClipData>();
    clip->kind = ClipKind::Files;
    DropSizes sizes;
    while (!names.empty() && names.front() != Char{}) {
        const auto end = names.find(Char{});
        if (end == std::basic_string_view<Char>::npos) {
            return {};
        }
        auto path = WidePath(names.substr(0, end));
        if (!IsAbsoluteFilePath(path) || !sizes.AddPath(path.size())) {
            return {};
        }
        clip->files.push_back(std::move(path));
        names.remove_prefix(end + 1);
    }
    if (names.empty() || clip->files.empty()) {
        return {};
    }
    return clip;
}

std::shared_ptr<const ClipData> ReadFiles() {
    if (!IsClipboardFormatAvailable(CF_HDROP)) {
        return {};
    }
    const HGLOBAL memory = GetClipboardData(CF_HDROP);
    if (!memory) {
        return {};
    }
    const auto length = std::min<std::size_t>(GlobalSize(memory), kMaxClipBytes);
    const GlobalMemoryView view(memory);
    if (!view.Data() || length < sizeof(DROPFILES)) {
        return {};
    }
    DROPFILES drop{};
    std::memcpy(&drop, view.Data(), sizeof(drop));
    if (drop.pFiles < sizeof(DROPFILES) || drop.pFiles >= length) {
        return {};
    }
    const auto* names = static_cast<const char*>(view.Data()) + drop.pFiles;
    const auto nameBytes = length - drop.pFiles;
    if (!drop.fWide) {
        return ReadDropNames(std::string_view(names, nameBytes));
    }
    if (drop.pFiles % alignof(wchar_t) != 0) {
        return {};
    }
    return ReadDropNames(std::wstring_view(reinterpret_cast<const wchar_t*>(names), nameBytes / sizeof(wchar_t)));
}

bool WriteFiles(HWND window, const ClipData& clip) {
    const auto sizes = FileDropSizes(clip);
    static const UINT preferredEffect = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    if (!sizes || !preferredEffect) {
        return false;
    }
    ClipboardMemory files(sizes->bufferBytes);
    ClipboardMemory effect(sizeof(DWORD));
    if (!files.Get() || !effect.Get()) {
        return false;
    }
    {
        const GlobalMemoryView fileView(files.Get());
        const GlobalMemoryView effectView(effect.Get());
        if (!fileView.Data() || !effectView.Data()) {
            return false;
        }
        DROPFILES drop{};
        drop.pFiles = sizeof(drop);
        drop.fWide = TRUE;
        std::memcpy(fileView.Data(), &drop, sizeof(drop));
        auto* output = static_cast<char*>(fileView.Data()) + sizeof(drop);
        for (const auto& path : clip.files) {
            const auto bytes = (path.size() + 1) * sizeof(wchar_t);
            std::memcpy(output, path.c_str(), bytes);
            output += bytes;
        }
        std::memset(output, 0, sizeof(wchar_t));
        constexpr DWORD copy = DROPEFFECT_COPY;
        std::memcpy(effectView.Data(), &copy, sizeof(copy));
    }

    const OpenClipboardScope clipboard(window);
    return clipboard && EmptyClipboard() && effect.Publish(preferredEffect) && files.Publish(CF_HDROP);
}

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
    if (auto clip = ReadFiles()) {
        return {ClipReadStatus::Found, std::move(clip)};
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
    if (!window) {
        return false;
    }
    if (clip.kind == ClipKind::Files) {
        return WriteFiles(window, clip);
    }
    if (!IsValidClip(clip)) {
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
    if (clip.kind == ClipKind::Files) {
        const auto sizes = FileDropSizes(clip);
        return sizes ? sizes->retainedBytes : 0;
    }
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
    if (clip.kind == ClipKind::Files) {
        return true;
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

std::wstring ClipFilesPreview(const ClipData& clip, int characterLimit) {
    if (clip.files.empty() || characterLimit <= 0) {
        return {};
    }
    std::wstring_view name = clip.files.front();
    const auto last = name.find_last_not_of(L"\\/");
    name = name.substr(0, last == std::wstring_view::npos ? 0 : last + 1);
    const auto separator = name.find_last_of(L"\\/");
    if (separator != std::wstring_view::npos) {
        name.remove_prefix(separator + 1);
    }
    if (clip.files.size() == 1) {
        return ClipTextPreview(name, characterLimit);
    }
    const auto suffix = L" 他 " + std::to_wstring(clip.files.size() - 1) + L" 件";
    if (static_cast<std::size_t>(characterLimit) <= suffix.size()) {
        return ClipTextPreview(std::to_wstring(clip.files.size()) + L"件", characterLimit);
    }
    return ClipTextPreview(name, characterLimit - static_cast<int>(suffix.size())) + suffix;
}

} // namespace mcs
