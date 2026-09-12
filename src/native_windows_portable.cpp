#include "native_windows_portable.hpp"
#include "bumble_portable_payload.hpp"
#include "common/rt64_dynamic_libraries.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bumble::portable {
namespace {
class Handle {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() { if (valid()) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    HANDLE get() const { return value_; }
    bool valid() const { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }
private:
    HANDLE value_;
};

[[noreturn]] void fail(const char* operation) {
    throw std::runtime_error(std::string(operation) + " (Windows error " +
        std::to_string(GetLastError()) + ")");
}

void require_regular(HANDLE file, bool directory) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) fail("Inspect runtime cache");
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory ||
        (!directory && info.nNumberOfLinks != 1)) {
        throw std::runtime_error("Runtime cache contains a link or unexpected file type");
    }
}

void ensure_directory(const std::filesystem::path& path, std::vector<Handle>& pins) {
    std::filesystem::path current = path.root_path();
    for (const auto& component : path.relative_path()) {
        current /= component;
        if (!CreateDirectoryW(current.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            fail("Create runtime cache directory");
        Handle pin(CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!pin.valid()) fail("Open runtime cache directory");
        require_regular(pin.get(), true);
        pins.push_back(std::move(pin));
    }
}

Handle verified_file(const std::filesystem::path& path, const void* bytes, DWORD size) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file.valid()) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return Handle();
        fail("Open runtime payload");
    }
    require_regular(file.get(), false);
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file.get(), &length)) fail("Read runtime payload size");
    if (length.QuadPart != size) return Handle();
    std::array<unsigned char, 65536> buffer;
    for (DWORD offset = 0; offset < size;) {
        const DWORD count = (std::min)(static_cast<DWORD>(buffer.size()), size - offset);
        DWORD read = 0;
        if (!ReadFile(file.get(), buffer.data(), count, &read, nullptr)) fail("Read runtime payload");
        if (read != count || std::memcmp(buffer.data(),
                static_cast<const unsigned char*>(bytes) + offset, count) != 0) return Handle();
        offset += count;
    }
    return file;
}

void publish(const std::filesystem::path& path, const void* bytes, DWORD size) {
    const auto staging = std::filesystem::path(path.wstring() + L".partial");
    try {
        {
            Handle file(CreateFileW(staging.c_str(), GENERIC_WRITE, 0, nullptr,
                OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!file.valid()) fail("Stage runtime payload");
            require_regular(file.get(), false);
            if (!SetEndOfFile(file.get())) fail("Truncate runtime staging file");
            DWORD written = 0;
            if (!WriteFile(file.get(), bytes, size, &written, nullptr) || written != size ||
                !FlushFileBuffers(file.get())) fail("Write runtime payload");
        }
        {
            auto verified = verified_file(staging, bytes, size);
            if (!verified.valid()) throw std::runtime_error("Runtime staging verification failed");
        }
        if (!MoveFileExW(staging.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) fail("Publish runtime payload");
    } catch (...) {
        // Delete only our staging file.
        DeleteFileW(staging.c_str());
        throw;
    }
}
} // namespace

bool prepare(const std::filesystem::path& data_root, bool diagnostics) {
    try {
        // Allow a redirected data root, but reject links inside our cache subtree.
        std::filesystem::create_directories(data_root);
        const auto cache = std::filesystem::canonical(data_root) /
            "cache" / "runtime" / kCacheIdentity;
        std::vector<Handle> directories;
        ensure_directory(cache, directories);
        Handle lock(CreateFileW((cache / "prepare.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!lock.valid()) fail("Lock runtime preparation (another launch may be preparing)");
        require_regular(lock.get(), false);

        static std::vector<Handle> payload_pins;
        unsigned repaired = 0;
        for (const auto& payload : kPayloads) {
            const HMODULE executable = GetModuleHandleW(nullptr);
            const HRSRC resource = FindResourceW(executable, MAKEINTRESOURCEW(payload.id), MAKEINTRESOURCEW(10));
            if (!resource || SizeofResource(executable, resource) != payload.size)
                throw std::runtime_error("Embedded runtime payload is missing or truncated");
            const void* bytes = LockResource(LoadResource(executable, resource));
            if (!bytes) fail("Read embedded runtime payload");
            const auto path = cache / payload.name;
            auto pin = verified_file(path, bytes, payload.size);
            if (pin.valid()) {
                payload_pins.push_back(std::move(pin));
            } else {
                publish(path, bytes, payload.size);
                auto published = verified_file(path, bytes, payload.size);
                if (!published.valid()) throw std::runtime_error("Published runtime verification failed");
                payload_pins.push_back(std::move(published));
                ++repaired;
            }
        }
        if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS) ||
            !AddDllDirectory(cache.c_str())) fail("Configure runtime DLL search");
        for (const auto* name : {L"dxil.dll", L"dxcompiler.dll", L"SDL2.dll"}) {
            if (GetModuleHandleW(name))
                throw std::runtime_error("Runtime dependency loaded before verified preparation");
            if (!LoadLibraryExW((cache / name).c_str(), nullptr,
                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32))
                fail("Load verified runtime DLL");
        }
        RT64::DynamicLibraries::usePreloadedLibraries();
        std::fprintf(stderr, "BUMBLE_PORTABLE stage=ready files=%zu repaired=%u cache=%ls\n",
            std::size(kPayloads), repaired, cache.c_str());
        return true;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "BUMBLE_PORTABLE stage=failed error=%s\n", error.what());
        if (!diagnostics) {
            const std::string message = std::string("Bumble could not prepare its bundled runtime.\n\n") +
                error.what() + "\n\nCheck that your user-data location is writable and has free space.";
            MessageBoxA(nullptr, message.c_str(), "Bumble startup error", MB_OK | MB_ICONERROR);
        }
        return false;
    }
}
} // namespace bumble::portable
