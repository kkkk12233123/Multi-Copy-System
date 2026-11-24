#include "storage.h"

#include <atomic>
#include <span>
#include <system_error>

namespace mcs {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"MultiCopySystem";
constexpr wchar_t kSettingsName[] = L"settings.bin";
constexpr std::array<const wchar_t*, 3> kSlotNames{L"slot1.bin", L"slot2.bin", L"slot3.bin"};
constexpr std::uint32_t kSettingsMagic = 0x5343534d; // MCSS
constexpr std::uint32_t kSlotMagic = 0x4c53434d; // MCSL
constexpr std::uint32_t kVersion = 1;

enum class StoredFormat : std::uint32_t { Text = 1, Dib, DibV5, Png };
using SettingsRecord = std::array<std::uint32_t, 12>;
using SlotHeader = std::array<std::uint32_t, 4>;

class FileHandle {
public:
    explicit FileHandle(HANDLE handle) : handle_(handle) {}
    ~FileHandle() {
        if (*this) {
            CloseHandle(handle_);
        }
    }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    explicit operator bool() const { return handle_ != INVALID_HANDLE_VALUE; }
    HANDLE Get() const { return handle_; }

private:
    HANDLE handle_;
};

std::filesystem::path DataDirectory() {
    constexpr wchar_t variable[] = L"LOCALAPPDATA";
    const DWORD size = GetEnvironmentVariableW(variable, nullptr, 0);
    if (!size) {
        return {};
    }
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(variable, value.data(), size);
    if (!written || written >= size) {
        return {};
    }
    value.resize(written);
    const std::filesystem::path base(value);
    return base.is_absolute() ? base / L"MultiCopySystem" : std::filesystem::path{};
}

FileHandle OpenForReading(const std::filesystem::path& path) {
    return FileHandle(CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
}

bool HasSize(const FileHandle& file, std::size_t expected) {
    LARGE_INTEGER size{};
    return GetFileSizeEx(file.Get(), &size) && size.QuadPart == static_cast<LONGLONG>(expected);
}

bool ReadBytes(const FileHandle& file, std::span<std::byte> data) {
    DWORD read = 0;
    const auto size = static_cast<DWORD>(data.size());
    return ReadFile(file.Get(), data.data(), size, &read, nullptr) && read == size;
}

bool WriteBytes(const FileHandle& file, std::span<const std::byte> data) {
    DWORD written = 0;
    const auto size = static_cast<DWORD>(data.size());
    return WriteFile(file.Get(), data.data(), size, &written, nullptr) && written == size;
}

bool WriteAtomic(const std::filesystem::path& path, std::span<const std::byte> header,
    std::span<const std::byte> contents = {}) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }

    static std::atomic_uint32_t sequence{};
    auto temp = path;
    temp += L"." + std::to_wstring(GetCurrentProcessId()) + L"."
        + std::to_wstring(GetTickCount64()) + L"."
        + std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)) + L".tmp";

    bool written = false;
    {
        const FileHandle file(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file) {
            return false;
        }
        written = WriteBytes(file, header) && (contents.empty() || WriteBytes(file, contents))
            && FlushFileBuffers(file.Get());
    }
    if (written && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    DeleteFileW(temp.c_str());
    return false;
}

bool RemoveFile(const std::filesystem::path& path) {
    if (DeleteFileW(path.c_str())) {
        return true;
    }
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool IsValidSettings(const Settings& settings) {
    if (settings.previewLimit < 1 || settings.previewLimit > 512) {
        return false;
    }
    constexpr UINT allowedModifiers = MOD_CONTROL | MOD_SHIFT | MOD_ALT;
    for (std::size_t index = 0; index < settings.shortcuts.size(); ++index) {
        const auto shortcut = settings.shortcuts[index];
        if (!shortcut.key && !shortcut.modifiers) {
            continue;
        }
        if (!shortcut.key || !shortcut.modifiers || shortcut.key > 0xfe
            || (shortcut.modifiers & ~allowedModifiers) != 0) {
            return false;
        }
        if (shortcut.modifiers == MOD_CONTROL && (shortcut.key == 'C' || shortcut.key == 'X' || shortcut.key == 'V')) {
            return false;
        }
        if (index == 0 && shortcut.key == VK_ESCAPE) {
            return false;
        }
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            const auto other = settings.shortcuts[earlier];
            if (shortcut.key == other.key && shortcut.modifiers == other.modifiers) {
                return false;
            }
        }
    }
    return true;
}

StoredFormat FormatFor(const ClipData& clip) {
    if (clip.kind == ClipKind::Text) {
        return StoredFormat::Text;
    }
    switch (clip.imageFormat) {
    case ImageFormat::Dib:
        return StoredFormat::Dib;
    case ImageFormat::DibV5:
        return StoredFormat::DibV5;
    case ImageFormat::Png:
        return StoredFormat::Png;
    }
    return {};
}

bool ReadSlotBytes(const FileHandle& file, ClipData& clip, StoredFormat format, std::size_t bytes) {
    if (format == StoredFormat::Text) {
        if (bytes % sizeof(wchar_t)) {
            return false;
        }
        clip.text.resize(bytes / sizeof(wchar_t));
        return ReadBytes(file, std::as_writable_bytes(std::span{clip.text}));
    }

    clip.kind = ClipKind::Image;
    switch (format) {
    case StoredFormat::Dib:
        clip.imageFormat = ImageFormat::Dib;
        break;
    case StoredFormat::DibV5:
        clip.imageFormat = ImageFormat::DibV5;
        break;
    case StoredFormat::Png:
        clip.imageFormat = ImageFormat::Png;
        break;
    default:
        return false;
    }
    clip.image.resize(bytes);
    return ReadBytes(file, std::as_writable_bytes(std::span{clip.image}));
}

std::filesystem::path SlotPath(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= kSlotNames.size()) {
        return {};
    }
    const auto dir = DataDirectory();
    return dir.empty() ? dir : dir / kSlotNames[static_cast<std::size_t>(index)];
}

} // namespace

Settings LoadSettings() {
    const auto dir = DataDirectory();
    if (dir.empty()) {
        return {};
    }
    const auto file = OpenForReading(dir / kSettingsName);
    SettingsRecord record{};
    if (!file || !HasSize(file, sizeof(record)) || !ReadBytes(file, std::as_writable_bytes(std::span{record}))
        || record[0] != kSettingsMagic || record[1] != kVersion || record[2] < 1 || record[2] > 512
        || record[3] > 3) {
        return {};
    }

    Settings settings;
    settings.previewLimit = static_cast<int>(record[2]);
    settings.bottomRight = (record[3] & 1) != 0;
    settings.paused = (record[3] & 2) != 0;
    for (std::size_t index = 0; index < settings.shortcuts.size(); ++index) {
        settings.shortcuts[index] = {record[4 + 2 * index], record[5 + 2 * index]};
    }
    return IsValidSettings(settings) ? settings : Settings{};
}

bool SaveSettings(const Settings& settings) {
    const auto dir = DataDirectory();
    if (dir.empty() || !IsValidSettings(settings)) {
        return false;
    }
    const std::uint32_t flags = (settings.bottomRight ? 1u : 0u) | (settings.paused ? 2u : 0u);
    SettingsRecord record{kSettingsMagic, kVersion, static_cast<std::uint32_t>(settings.previewLimit), flags};
    for (std::size_t index = 0; index < settings.shortcuts.size(); ++index) {
        record[4 + 2 * index] = settings.shortcuts[index].modifiers;
        record[5 + 2 * index] = settings.shortcuts[index].key;
    }
    return WriteAtomic(dir / kSettingsName, std::as_bytes(std::span{record}));
}

std::shared_ptr<const ClipData> LoadSlot(int index) {
    const auto path = SlotPath(index);
    if (path.empty()) {
        return {};
    }
    const auto file = OpenForReading(path);
    SlotHeader header{};
    if (!file || !ReadBytes(file, std::as_writable_bytes(std::span{header}))
        || header[0] != kSlotMagic || header[1] != kVersion
        || !header[3] || header[3] > kMaxClipBytes || !HasSize(file, sizeof(header) + header[3])) {
        return {};
    }
    auto clip = std::make_shared<ClipData>();
    if (!ReadSlotBytes(file, *clip, static_cast<StoredFormat>(header[2]), header[3]) || !IsValidClip(*clip)) {
        return {};
    }
    return clip;
}

bool SaveSlot(int index, const ClipData* clip) {
    const auto path = SlotPath(index);
    if (path.empty()) {
        return false;
    }
    if (!clip) {
        return RemoveFile(path);
    }
    if (!IsValidClip(*clip)) {
        return false;
    }

    const SlotHeader header{kSlotMagic, kVersion, static_cast<std::uint32_t>(FormatFor(*clip)),
        static_cast<std::uint32_t>(ClipBytes(*clip))};
    const auto contents = clip->kind == ClipKind::Text ? std::as_bytes(std::span{clip->text})
        : std::as_bytes(std::span{clip->image});
    return WriteAtomic(path, std::as_bytes(std::span{header}), contents);
}

bool ClearStoredData() {
    const auto dir = DataDirectory();
    if (dir.empty()) {
        return false;
    }
    bool success = RemoveFile(dir / kSettingsName);
    for (const auto* name : kSlotNames) {
        success = RemoveFile(dir / name) && success;
    }

    // An interrupted write can leave a temporary copy beside the saved file.
    std::error_code error;
    for (std::filesystem::directory_iterator entry(dir, error), end; !error && entry != end; entry.increment(error)) {
        const std::wstring name = entry->path().filename().native();
        const std::wstring_view filename(name);
        if (filename.ends_with(L".tmp") && (filename.starts_with(L"settings.bin.") ||
            filename.starts_with(L"slot1.bin.") || filename.starts_with(L"slot2.bin.") || filename.starts_with(L"slot3.bin."))) {
            success = RemoveFile(entry->path()) && success;
        }
    }
    return success && (!error || error == std::errc::no_such_file_or_directory);
}

bool IsStartOnSignInEnabled() {
    DWORD size = 0;
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, &size)
        == ERROR_SUCCESS && size > sizeof(wchar_t);
}

bool SetStartOnSignIn(bool enabled, const std::filesystem::path& residentExe) {
    if (!enabled) {
        const LSTATUS status = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
    }

    std::error_code error;
    if (!residentExe.is_absolute() || !std::filesystem::is_regular_file(residentExe, error)) {
        return false;
    }
    const std::wstring command = L"\"" + residentExe.native() + L"\"";
    if (command.size() > MAX_PATH) {
        return false;
    }
    const DWORD bytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
    return RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, command.c_str(), bytes) == ERROR_SUCCESS;
}

} // namespace mcs
