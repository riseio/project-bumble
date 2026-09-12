#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace bumble::controller_pak {

constexpr std::size_t kSlotCount = 16;
constexpr std::size_t kGameNameSize = 16;
constexpr std::size_t kExtensionSize = 4;
constexpr std::size_t kIdentitySize = 16;
constexpr std::size_t kPageSize = 256;
constexpr std::size_t kBlockSize = 32;
constexpr std::size_t kUsableBytes = 123 * kPageSize;

enum class Result : std::int32_t {
    Ok = 0,
    NoPack = 1,
    NewPack = 2,
    Inconsistent = 3,
    ControllerFailure = 4,
    Invalid = 5,
    BadData = 6,
    DataFull = 7,
    DirectoryFull = 8,
    Exists = 9,
    IdFatal = 10,
    Device = 11,
};

struct FileKey {
    std::uint16_t company_code = 0;
    std::uint32_t game_code = 0;
    std::array<std::uint8_t, kGameNameSize> game_name{};
    std::array<std::uint8_t, kExtensionSize> extension{};

    bool operator==(const FileKey&) const = default;
};

struct FileState {
    std::uint32_t file_size = 0;
    FileKey key{};
    bool written = false;
};

struct OpenState {
    Result result = Result::Ok;
    bool created = false;
    bool recovered_backup = false;
};

class Storage {
public:
    Storage(std::filesystem::path root, std::uint32_t channel);

    OpenState open();
    Result reload();

    Result find(const FileKey& key, std::int32_t& file_no) const;
    Result find_free(std::int32_t& file_no) const;
    Result allocate(const FileKey& key, std::uint32_t size, std::int32_t& file_no);
    Result erase(const FileKey& key);
    Result state(std::int32_t file_no, FileState& state) const;
    Result read(
        std::int32_t file_no,
        std::uint32_t offset,
        std::span<std::uint8_t> output
    ) const;
    Result write(
        std::int32_t file_no,
        std::uint32_t offset,
        std::span<const std::uint8_t> input
    );

    std::uint32_t free_bytes() const;
    std::uint32_t used_files() const;
    bool loaded() const;
    const std::array<std::uint8_t, kIdentitySize>& identity() const;
    const std::filesystem::path& path() const;

private:
    struct Entry {
        bool occupied = false;
        bool written = false;
        FileKey key{};
        std::array<std::uint8_t, kUsableBytes> data{};
        std::uint32_t size = 0;
    };

    Result load_file(const std::filesystem::path& path);
    bool persist(bool preserve_existing_backup = false) const;
    void clear();

    std::filesystem::path root_;
    std::filesystem::path path_;
    std::uint32_t channel_ = 0;
    std::array<Entry, kSlotCount> entries_{};
    std::array<std::uint8_t, kIdentitySize> identity_{};
    bool loaded_ = false;
};

} // namespace bumble::controller_pak
