#include "cart_rom_mirror.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

#ifdef _WIN32
#include <Windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"

namespace {

constexpr size_t kExpectedRomSize = 0x00C00000u;
constexpr size_t kCartMirrorOffset = 0x30000000u;

struct KnownWord {
    size_t rom_offset;
    uint32_t value;
};

constexpr std::array<KnownWord, 5> kKnownWords{{
    {0x00000000u, 0x80371240u},
    {0x00222BA0u, 0x000000D8u},
    {0x00222C20u, 0x000F3378u},
    {0x00222C78u, 0x4E363420u},
    {0x00BFFFFCu, 0x00000000u},
}};

static_assert(kCartMirrorOffset >= recomp::mem_size);
static_assert(kCartMirrorOffset + kExpectedRomSize <= recomp::allocation_size);

[[noreturn]] void fail_mirror(const char* reason, unsigned long error = 0) {
    std::fprintf(
        stderr,
        "BUMBLE_RUNTIME fatal=cart_rom_mirror reason=%s error=%lu\n",
        reason,
        error
    );
    std::fflush(stderr);
    std::abort();
}

size_t protection_size(size_t byte_count) {
#ifdef _WIN32
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const size_t page_size = system_info.dwPageSize;
#else
    const long queried_page_size = sysconf(_SC_PAGESIZE);
    if (queried_page_size <= 0) {
        fail_mirror("page_size_query_failed", static_cast<unsigned long>(errno));
    }
    const size_t page_size = static_cast<size_t>(queried_page_size);
#endif
    if (page_size == 0 || (page_size & (page_size - 1u)) != 0) {
        fail_mirror("invalid_page_size");
    }
    return (byte_count + page_size - 1u) & ~(page_size - 1u);
}

void set_protection(uint8_t* address, size_t size, bool read_only) {
#ifdef _WIN32
    DWORD old_protection = 0;
    const DWORD protection = read_only ? PAGE_READONLY : PAGE_READWRITE;
    if (VirtualProtect(address, size, protection, &old_protection) == 0) {
        fail_mirror(
            read_only ? "read_only_protection_failed" : "write_protection_failed",
            GetLastError()
        );
    }
#else
    const int protection = read_only ? PROT_READ : (PROT_READ | PROT_WRITE);
    if (mprotect(address, size, protection) != 0) {
        fail_mirror(
            read_only ? "read_only_protection_failed" : "write_protection_failed",
            static_cast<unsigned long>(errno)
        );
    }
#endif
}

uint32_t mirror_word(const uint8_t* mirror, size_t rom_offset) {
    uint32_t value = 0;
    std::memcpy(&value, mirror + rom_offset, sizeof(value));
    return value;
}

} // namespace

void install_buck_cart_rom_mirror(uint8_t* rdram) {
    const std::span<const uint8_t> rom = recomp::get_rom();
    if (rdram == nullptr || rom.size() != kExpectedRomSize) {
        fail_mirror("unexpected_runtime_input");
    }

    const size_t protected_size = protection_size(rom.size());
    if (kCartMirrorOffset + protected_size > recomp::allocation_size) {
        fail_mirror("mirror_exceeds_reserved_address_space");
    }

    uint8_t* const mirror = rdram + kCartMirrorOffset;
    set_protection(mirror, protected_size, false);
    for (size_t rom_offset = 0; rom_offset < rom.size(); ++rom_offset) {
        mirror[rom_offset ^ 3u] = rom[rom_offset];
    }

    for (const KnownWord& probe : kKnownWords) {
        if (mirror_word(mirror, probe.rom_offset) != probe.value) {
            fail_mirror("known_word_projection_mismatch");
        }
    }
    set_protection(mirror, protected_size, true);
}
