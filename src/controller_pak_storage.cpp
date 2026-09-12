#include "controller_pak_storage.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace bumble::controller_pak {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic{
    'B', 'U', 'M', 'B', 'P', 'A', 'K', '1'
};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kHeaderSize = 32 + kIdentitySize;
constexpr std::size_t kEntryMetadataSize = 32;
constexpr std::uint32_t kCrcPolynomial = 0xEDB88320u;

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

bool read_u16(
    std::span<const std::uint8_t> input,
    std::size_t& cursor,
    std::uint16_t& value
) {
    if (cursor + 2 > input.size()) {
        return false;
    }
    value = static_cast<std::uint16_t>(input[cursor]) |
        static_cast<std::uint16_t>(input[cursor + 1] << 8);
    cursor += 2;
    return true;
}

bool read_u32(
    std::span<const std::uint8_t> input,
    std::size_t& cursor,
    std::uint32_t& value
) {
    if (cursor + 4 > input.size()) {
        return false;
    }
    value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(input[cursor++]) << shift;
    }
    return true;
}

std::uint32_t crc32(std::span<const std::uint8_t> input) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const std::uint8_t byte : input) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (kCrcPolynomial & mask);
        }
    }
    return ~crc;
}

std::filesystem::path with_suffix(
    const std::filesystem::path& path,
    const char* suffix
) {
    std::filesystem::path result = path;
    result += suffix;
    return result;
}

bool valid_transfer(std::uint32_t offset, std::size_t size, std::uint32_t file_size) {
    return size > 0 &&
        size % kBlockSize == 0 &&
        offset % kBlockSize == 0 &&
        offset <= file_size &&
        size <= static_cast<std::size_t>(file_size - offset);
}

bool write_temporary_file(
    const std::filesystem::path& path,
    std::span<const std::uint8_t> bytes
) {
#ifdef _WIN32
    const HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr
    );
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    bool succeeded = true;
    std::size_t cursor = 0;
    while (cursor < bytes.size()) {
        const std::size_t remaining = bytes.size() - cursor;
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            remaining,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max())
        ));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + cursor, requested, &written, nullptr) ||
            written != requested) {
            succeeded = false;
            break;
        }
        cursor += written;
    }
    if (succeeded && !FlushFileBuffers(file)) {
        succeeded = false;
    }
    if (!CloseHandle(file)) {
        succeeded = false;
    }
    return succeeded;
#else
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );
    file.flush();
    return static_cast<bool>(file);
#endif
}

bool replace_primary(
    const std::filesystem::path& temporary,
    const std::filesystem::path& primary,
    const std::filesystem::path& backup,
    bool primary_exists,
    bool preserve_existing_backup
) {
#ifdef _WIN32
    if (!primary_exists) {
        return MoveFileExW(
            temporary.c_str(),
            primary.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
        ) != 0;
    }

    const wchar_t* backup_path = nullptr;
    if (!preserve_existing_backup) {
        std::error_code error;
        std::filesystem::remove(backup, error);
        if (error) {
            return false;
        }
        backup_path = backup.c_str();
    }
    return ReplaceFileW(
        primary.c_str(),
        temporary.c_str(),
        backup_path,
        REPLACEFILE_WRITE_THROUGH,
        nullptr,
        nullptr
    ) != 0;
#else
    std::error_code error;
    if (primary_exists && !preserve_existing_backup) {
        std::filesystem::copy_file(
            primary,
            backup,
            std::filesystem::copy_options::overwrite_existing,
            error
        );
        if (error) {
            return false;
        }
    }
    std::filesystem::rename(temporary, primary, error);
    return !error;
#endif
}

} // namespace

Storage::Storage(std::filesystem::path root, std::uint32_t channel)
    : root_(std::move(root)),
      path_(root_ / ("controller-" + std::to_string(channel + 1) + ".bpak")),
      channel_(channel) {
}

void Storage::clear() {
    entries_ = {};
    identity_ = {};
    loaded_ = false;
}

OpenState Storage::open() {
    clear();
    std::error_code error;
    const bool primary_exists = std::filesystem::is_regular_file(path_, error);
    if (!error && primary_exists && load_file(path_) == Result::Ok) {
        return {.result = Result::Ok};
    }

    const std::filesystem::path backup = with_suffix(path_, ".bak");
    error.clear();
    const bool backup_exists = std::filesystem::is_regular_file(backup, error);
    if (!error && backup_exists && load_file(backup) == Result::Ok) {
        if (!persist(true)) {
            clear();
            return {.result = Result::ControllerFailure};
        }
        return {.result = Result::Ok, .recovered_backup = true};
    }

    if (primary_exists || backup_exists) {
        clear();
        return {.result = Result::Inconsistent};
    }

    clear();
    loaded_ = true;
    std::random_device random;
    for (std::uint8_t& byte : identity_) {
        byte = static_cast<std::uint8_t>(random());
    }
    if (std::all_of(identity_.begin(), identity_.end(), [](auto byte) { return byte == 0; })) {
        identity_[0] = 1;
    }
    if (!persist()) {
        clear();
        return {.result = Result::ControllerFailure};
    }
    return {.result = Result::Ok, .created = true};
}

Result Storage::reload() {
    return open().result;
}

Result Storage::find(const FileKey& key, std::int32_t& file_no) const {
    if (!loaded_ || key.company_code == 0 || key.game_code == 0) {
        return Result::Invalid;
    }
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        const Entry& entry = entries_[index];
        if (entry.occupied && entry.key == key) {
            file_no = static_cast<std::int32_t>(index);
            return Result::Ok;
        }
    }
    return Result::Invalid;
}

Result Storage::find_free(std::int32_t& file_no) const {
    if (!loaded_) {
        return Result::Invalid;
    }
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (!entries_[index].occupied) {
            file_no = static_cast<std::int32_t>(index);
            return Result::Ok;
        }
    }
    file_no = -1;
    return Result::Invalid;
}

Result Storage::allocate(
    const FileKey& key,
    std::uint32_t size,
    std::int32_t& file_no
) {
    if (!loaded_ || key.company_code == 0 || key.game_code == 0 ||
        size == 0 || size > kUsableBytes) {
        return Result::Invalid;
    }
    const std::uint32_t rounded_size = static_cast<std::uint32_t>(
        (size + kPageSize - 1) / kPageSize * kPageSize
    );

    std::int32_t existing = -1;
    if (find(key, existing) == Result::Ok) {
        return Result::Exists;
    }
    if (rounded_size > free_bytes()) {
        return Result::DataFull;
    }

    const auto available = std::find_if(
        entries_.begin(), entries_.end(),
        [](const Entry& entry) { return !entry.occupied; }
    );
    if (available == entries_.end()) {
        return Result::DirectoryFull;
    }

    const Entry previous = *available;
    available->occupied = true;
    available->written = false;
    available->key = key;
    available->size = rounded_size;
    std::fill_n(available->data.begin(), rounded_size, 0);
    if (!persist()) {
        *available = previous;
        return Result::ControllerFailure;
    }
    file_no = static_cast<std::int32_t>(available - entries_.begin());
    return Result::Ok;
}

Result Storage::erase(const FileKey& key) {
    std::int32_t file_no = -1;
    const Result found = find(key, file_no);
    if (found != Result::Ok) {
        return found;
    }
    Entry& entry = entries_[static_cast<std::size_t>(file_no)];
    const Entry previous = entry;
    entry = {};
    if (!persist()) {
        entry = previous;
        return Result::ControllerFailure;
    }
    return Result::Ok;
}

Result Storage::state(std::int32_t file_no, FileState& state_out) const {
    if (!loaded_ || file_no < 0 ||
        file_no >= static_cast<std::int32_t>(entries_.size())) {
        return Result::Invalid;
    }
    const Entry& entry = entries_[static_cast<std::size_t>(file_no)];
    if (!entry.occupied) {
        return Result::Invalid;
    }
    state_out = {
        .file_size = entry.size,
        .key = entry.key,
        .written = entry.written,
    };
    return Result::Ok;
}

Result Storage::read(
    std::int32_t file_no,
    std::uint32_t offset,
    std::span<std::uint8_t> output
) const {
    if (!loaded_ || file_no < 0 ||
        file_no >= static_cast<std::int32_t>(entries_.size())) {
        return Result::Invalid;
    }
    const Entry& entry = entries_[static_cast<std::size_t>(file_no)];
    if (!entry.occupied || !valid_transfer(offset, output.size(), entry.size)) {
        return Result::Invalid;
    }
    if (!entry.written) {
        return Result::BadData;
    }
    std::copy_n(entry.data.begin() + offset, output.size(), output.begin());
    return Result::Ok;
}

Result Storage::write(
    std::int32_t file_no,
    std::uint32_t offset,
    std::span<const std::uint8_t> input
) {
    if (!loaded_ || file_no < 0 ||
        file_no >= static_cast<std::int32_t>(entries_.size())) {
        return Result::Invalid;
    }
    Entry& entry = entries_[static_cast<std::size_t>(file_no)];
    if (!entry.occupied || !valid_transfer(offset, input.size(), entry.size)) {
        return Result::Invalid;
    }
    const Entry previous = entry;
    std::copy(input.begin(), input.end(), entry.data.begin() + offset);
    entry.written = true;
    if (!persist()) {
        entry = previous;
        return Result::ControllerFailure;
    }
    return Result::Ok;
}

std::uint32_t Storage::free_bytes() const {
    std::uint32_t used = 0;
    for (const Entry& entry : entries_) {
        if (entry.occupied) {
            used += entry.size;
        }
    }
    return static_cast<std::uint32_t>(kUsableBytes) - used;
}

std::uint32_t Storage::used_files() const {
    return static_cast<std::uint32_t>(std::count_if(
        entries_.begin(), entries_.end(),
        [](const Entry& entry) { return entry.occupied; }
    ));
}

bool Storage::loaded() const {
    return loaded_;
}

const std::array<std::uint8_t, kIdentitySize>& Storage::identity() const {
    return identity_;
}

const std::filesystem::path& Storage::path() const {
    return path_;
}

Result Storage::load_file(const std::filesystem::path& input_path) {
    std::ifstream input(input_path, std::ios::binary);
    if (!input) {
        return Result::ControllerFailure;
    }
    const std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
    if (bytes.size() < kHeaderSize + kSlotCount * kEntryMetadataSize ||
        !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        return Result::Inconsistent;
    }

    std::size_t cursor = kMagic.size();
    std::uint32_t version = 0;
    std::uint32_t capacity = 0;
    std::uint32_t slots = 0;
    std::uint32_t payload_size = 0;
    std::uint32_t expected_crc = 0;
    std::uint32_t channel = 0;
    if (!read_u32(bytes, cursor, version) ||
        !read_u32(bytes, cursor, capacity) ||
        !read_u32(bytes, cursor, slots) ||
        !read_u32(bytes, cursor, payload_size) ||
        !read_u32(bytes, cursor, expected_crc) ||
        !read_u32(bytes, cursor, channel) ||
        version != kFormatVersion || capacity != kUsableBytes ||
        slots != kSlotCount || channel != channel_ ||
        bytes.size() != kHeaderSize + kSlotCount * kEntryMetadataSize + payload_size ||
        crc32(std::span(bytes).subspan(32)) != expected_crc) {
        return Result::Inconsistent;
    }

    std::array<std::uint8_t, kIdentitySize> decoded_identity{};
    std::copy_n(bytes.begin() + cursor, decoded_identity.size(), decoded_identity.begin());
    cursor += decoded_identity.size();
    if (std::all_of(
        decoded_identity.begin(), decoded_identity.end(),
        [](std::uint8_t byte) { return byte == 0; }
    )) {
        return Result::Inconsistent;
    }

    std::array<Entry, kSlotCount> decoded{};
    std::uint32_t declared_payload = 0;
    for (Entry& entry : decoded) {
        if (cursor + kEntryMetadataSize > bytes.size()) {
            return Result::Inconsistent;
        }
        const std::uint8_t occupied = bytes[cursor++];
        const std::uint8_t written = bytes[cursor++];
        if (occupied > 1 || written > 1 || (occupied == 0 && written != 0) ||
            !read_u16(bytes, cursor, entry.key.company_code) ||
            !read_u32(bytes, cursor, entry.key.game_code) ||
            !read_u32(bytes, cursor, entry.size)) {
            return Result::Inconsistent;
        }
        std::copy_n(bytes.begin() + cursor, kGameNameSize, entry.key.game_name.begin());
        cursor += kGameNameSize;
        std::copy_n(bytes.begin() + cursor, kExtensionSize, entry.key.extension.begin());
        cursor += kExtensionSize;
        entry.occupied = occupied != 0;
        entry.written = written != 0;
        if (!entry.occupied) {
            if (entry.key.company_code != 0 || entry.key.game_code != 0 || entry.size != 0 ||
                std::any_of(entry.key.game_name.begin(), entry.key.game_name.end(), [](auto b) { return b != 0; }) ||
                std::any_of(entry.key.extension.begin(), entry.key.extension.end(), [](auto b) { return b != 0; })) {
                return Result::Inconsistent;
            }
            continue;
        }
        if (entry.key.company_code == 0 || entry.key.game_code == 0 ||
            entry.size == 0 || entry.size % kPageSize != 0 ||
            entry.size > kUsableBytes || declared_payload > kUsableBytes - entry.size) {
            return Result::Inconsistent;
        }
        declared_payload += entry.size;
    }
    if (declared_payload != payload_size) {
        return Result::Inconsistent;
    }

    for (Entry& entry : decoded) {
        if (!entry.occupied) {
            continue;
        }
        if (cursor + entry.size > bytes.size()) {
            return Result::Inconsistent;
        }
        std::copy_n(bytes.begin() + cursor, entry.size, entry.data.begin());
        cursor += entry.size;
    }
    if (cursor != bytes.size()) {
        return Result::Inconsistent;
    }
    for (std::size_t left = 0; left < decoded.size(); ++left) {
        if (!decoded[left].occupied) {
            continue;
        }
        for (std::size_t right = left + 1; right < decoded.size(); ++right) {
            if (decoded[right].occupied && decoded[left].key == decoded[right].key) {
                return Result::Inconsistent;
            }
        }
    }

    entries_ = std::move(decoded);
    identity_ = decoded_identity;
    loaded_ = true;
    return Result::Ok;
}

bool Storage::persist(bool preserve_existing_backup) const {
    if (!loaded_) {
        return false;
    }

    std::vector<std::uint8_t> body;
    body.reserve(kSlotCount * kEntryMetadataSize + kUsableBytes);
    std::uint32_t payload_size = 0;
    for (const Entry& entry : entries_) {
        body.push_back(entry.occupied ? 1 : 0);
        body.push_back(entry.written ? 1 : 0);
        append_u16(body, entry.key.company_code);
        append_u32(body, entry.key.game_code);
        append_u32(body, entry.size);
        body.insert(body.end(), entry.key.game_name.begin(), entry.key.game_name.end());
        body.insert(body.end(), entry.key.extension.begin(), entry.key.extension.end());
        if (entry.occupied) {
            payload_size += entry.size;
        }
    }
    for (const Entry& entry : entries_) {
        if (entry.occupied) {
            body.insert(body.end(), entry.data.begin(), entry.data.begin() + entry.size);
        }
    }

    std::vector<std::uint8_t> output;
    std::vector<std::uint8_t> checksum_input;
    checksum_input.reserve(identity_.size() + body.size());
    checksum_input.insert(checksum_input.end(), identity_.begin(), identity_.end());
    checksum_input.insert(checksum_input.end(), body.begin(), body.end());

    output.reserve(kHeaderSize + body.size());
    output.insert(output.end(), kMagic.begin(), kMagic.end());
    append_u32(output, kFormatVersion);
    append_u32(output, static_cast<std::uint32_t>(kUsableBytes));
    append_u32(output, static_cast<std::uint32_t>(kSlotCount));
    append_u32(output, payload_size);
    append_u32(output, crc32(checksum_input));
    append_u32(output, channel_);
    output.insert(output.end(), identity_.begin(), identity_.end());
    output.insert(output.end(), body.begin(), body.end());

    std::error_code error;
    std::filesystem::create_directories(root_, error);
    if (error) {
        return false;
    }
    const std::filesystem::path temporary = with_suffix(path_, ".temp");
    const std::filesystem::path backup = with_suffix(path_, ".bak");
    std::filesystem::remove(temporary, error);
    if (error || !write_temporary_file(temporary, output)) {
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
        return false;
    }

    error.clear();
    const std::filesystem::file_status primary_status =
        std::filesystem::status(path_, error);
    if (error == std::errc::no_such_file_or_directory) {
        error.clear();
    }
    const bool primary_exists = !error && std::filesystem::exists(primary_status);
    if (error || (primary_exists && !std::filesystem::is_regular_file(primary_status)) ||
        !replace_primary(
        temporary,
        path_,
        backup,
        primary_exists,
        preserve_existing_backup
    )) {
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
        return false;
    }
    std::error_code remove_error;
    std::filesystem::remove(temporary, remove_error);
    return true;
}

} // namespace bumble::controller_pak
