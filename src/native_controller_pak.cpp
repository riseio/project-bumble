#include "native_controller_pak.hpp"

#include <array>
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "controller_pak_storage.hpp"
#include "librecomp/helpers.hpp"
#include "recomp.h"
#include "ultramodern/input.hpp"
#include "ultramodern/ultra64.h"

namespace bumble::controller_pak {
namespace {

constexpr std::int32_t kPfsInitialized = 1;

std::mutex g_mutex;
std::filesystem::path g_storage_root;
std::array<std::unique_ptr<Storage>, 4> g_cards{};

const char* result_name(Result result) {
    switch (result) {
    case Result::Ok: return "ok";
    case Result::NoPack: return "no_pack";
    case Result::NewPack: return "new_pack";
    case Result::Inconsistent: return "inconsistent";
    case Result::ControllerFailure: return "controller_failure";
    case Result::Invalid: return "invalid";
    case Result::BadData: return "bad_data";
    case Result::DataFull: return "data_full";
    case Result::DirectoryFull: return "directory_full";
    case Result::Exists: return "exists";
    case Result::IdFatal: return "id_fatal";
    case Result::Device: return "device";
    }
    return "unknown";
}

void log_operation(
    const char* operation,
    std::int32_t channel,
    Result result,
    std::int32_t file_no = -1,
    std::uint32_t size = 0,
    std::int32_t offset = -1
) {
    std::fprintf(
        stderr,
        "BUMBLE_CONTROLLER_PAK stage=%s channel=%" PRId32
        " result=%s code=%" PRId32 " file=%" PRId32 " offset=%" PRId32
        " size=%" PRIu32 "\n",
        operation,
        channel,
        result_name(result),
        static_cast<std::int32_t>(result),
        file_no,
        offset,
        size
    );
    std::fflush(stderr);
}

Result connected_controller_pak(std::int32_t channel, bool initializing) {
    if (channel < 0 || channel >= static_cast<std::int32_t>(g_cards.size())) {
        return Result::Invalid;
    }
    const auto info = ultramodern::input::get_connected_device_info(channel);
    if (info.connected_device != ultramodern::input::Device::Controller) {
        return Result::ControllerFailure;
    }
    switch (info.connected_pak) {
    case ultramodern::input::Pak::None:
        return Result::NoPack;
    case ultramodern::input::Pak::RumblePak:
        return initializing ? Result::IdFatal : Result::Device;
    case ultramodern::input::Pak::ControllerPak:
        return Result::Ok;
    }
    return Result::Device;
}

Storage* initialized_storage(OSPfs* pfs, Result& result) {
    if (pfs == nullptr || !(pfs->status & kPfsInitialized)) {
        result = Result::Invalid;
        return nullptr;
    }
    result = connected_controller_pak(pfs->channel, false);
    if (result != Result::Ok) {
        return nullptr;
    }
    if (g_storage_root.empty()) {
        result = Result::ControllerFailure;
        return nullptr;
    }
    std::unique_ptr<Storage>& card = g_cards[static_cast<std::size_t>(pfs->channel)];
    if (!card || !card->loaded()) {
        card = std::make_unique<Storage>(g_storage_root, pfs->channel);
        result = card->open().result;
        if (result != Result::Ok) {
            card.reset();
            return nullptr;
        }
    }
    result = Result::Ok;
    return card.get();
}

gpr guest_address(std::int32_t value) {
    return static_cast<gpr>(static_cast<std::int64_t>(value));
}

template <std::size_t Size>
std::array<std::uint8_t, Size> read_guest_bytes(uint8_t* rdram, gpr address) {
    std::array<std::uint8_t, Size> output{};
    if (address == 0) {
        return output;
    }
    for (std::size_t index = 0; index < Size; ++index) {
        output[index] = MEM_BU(index, address);
    }
    return output;
}

FileKey read_key(
    uint8_t* rdram,
    std::uint16_t company_code,
    std::uint32_t game_code,
    gpr game_name,
    gpr extension
) {
    return {
        .company_code = company_code,
        .game_code = game_code,
        .game_name = read_guest_bytes<kGameNameSize>(rdram, game_name),
        .extension = read_guest_bytes<kExtensionSize>(rdram, extension),
    };
}

void return_result(recomp_context* ctx, Result result) {
    _return<std::int32_t>(ctx, static_cast<std::int32_t>(result));
}

} // namespace

void configure_storage_root(const std::filesystem::path& root) {
    std::lock_guard lock(g_mutex);
    g_storage_root = std::filesystem::absolute(root);
    for (auto& card : g_cards) {
        card.reset();
    }
    std::fprintf(
        stderr,
        "BUMBLE_CONTROLLER_PAK stage=storage_configured root=%s\n",
        g_storage_root.string().c_str()
    );
    std::fflush(stderr);
}

void reset_runtime_state() {
    std::lock_guard lock(g_mutex);
    for (auto& card : g_cards) {
        card.reset();
    }
}

bool clear_all_storage() {
    std::lock_guard lock(g_mutex);
    if (g_storage_root.empty()) {
        return false;
    }

    for (auto& card : g_cards) {
        card.reset();
    }

    bool succeeded = true;
    std::uint32_t removed = 0u;
    for (std::uint32_t channel = 0u; channel < g_cards.size(); ++channel) {
        const std::filesystem::path primary =
            Storage(g_storage_root, channel).path();
        for (const char* suffix : {"", ".bak", ".temp"}) {
            std::filesystem::path candidate = primary;
            candidate += suffix;
            std::error_code error;
            if (std::filesystem::remove(candidate, error)) {
                ++removed;
            }
            succeeded = !error && succeeded;
        }
    }

    std::fprintf(
        stderr,
        "BUMBLE_CONTROLLER_PAK stage=all_storage_cleared"
        " removed=%" PRIu32 " success=%d\n",
        removed,
        succeeded ? 1 : 0
    );
    std::fflush(stderr);
    return succeeded;
}

std::filesystem::path storage_path(std::uint32_t channel) {
    std::lock_guard lock(g_mutex);
    return Storage(g_storage_root, channel).path();
}

} // namespace bumble::controller_pak

extern "C" void buck_osPfsInitPak_recomp(uint8_t* rdram, recomp_context* ctx) {
    using namespace bumble::controller_pak;
    const PTR(OSMesgQueue) queue = _arg<0, PTR(OSMesgQueue)>(rdram, ctx);
    OSPfs* pfs = _arg<1, OSPfs*>(rdram, ctx);
    const std::int32_t channel = _arg<2, std::int32_t>(rdram, ctx);

    std::lock_guard lock(g_mutex);
    if (pfs == nullptr) {
        return_result(ctx, Result::Invalid);
        return;
    }
    pfs->queue = queue;
    pfs->channel = channel;
    pfs->activebank = 0xFF;
    pfs->status = 0;

    Result result = connected_controller_pak(channel, true);
    OpenState opened{};
    if (result == Result::Ok) {
        if (g_storage_root.empty()) {
            result = Result::ControllerFailure;
        } else {
            std::unique_ptr<Storage>& card = g_cards[static_cast<std::size_t>(channel)];
            if (!card) {
                card = std::make_unique<Storage>(g_storage_root, channel);
            }
            opened = card->open();
            result = opened.result;
            if (result != Result::Ok) {
                card.reset();
            }
        }
    }
    if (result == Result::Ok) {
        pfs->status = kPfsInitialized;
        pfs->version = 0x0200;
        pfs->dir_size = static_cast<std::int32_t>(kSlotCount);
        pfs->inode_table = 8;
        pfs->minode_table = 16;
        pfs->dir_table = 24;
        pfs->inode_start_page = 5;
        pfs->activebank = 0;
        pfs->banks = 1;
        std::fill(std::begin(pfs->id), std::end(pfs->id), 0);
        std::fill(std::begin(pfs->label), std::end(pfs->label), 0);
        std::fprintf(
            stderr,
            "BUMBLE_CONTROLLER_PAK stage=initialized channel=%" PRId32
            " created=%d recovered_backup=%d path=%s\n",
            channel,
            opened.created ? 1 : 0,
            opened.recovered_backup ? 1 : 0,
            g_cards[static_cast<std::size_t>(channel)]->path().string().c_str()
        );
        std::fflush(stderr);
    } else {
        log_operation("init_failed", channel, result);
    }
    return_result(ctx, result);
}

extern "C" void buck_osPfsFreeBlocks_recomp(uint8_t* rdram, recomp_context* ctx) {
    using namespace bumble::controller_pak;
    OSPfs* pfs = _arg<0, OSPfs*>(rdram, ctx);
    const gpr output = guest_address(_arg<1, PTR(s32)>(rdram, ctx));
    std::lock_guard lock(g_mutex);
    Result result = Result::Ok;
    Storage* card = initialized_storage(pfs, result);
    if (card && output != 0) {
        MEM_W(0, output) = static_cast<std::int32_t>(card->free_bytes());
    } else if (card && output == 0) {
        result = Result::Invalid;
    }
    log_operation("free_blocks", pfs ? pfs->channel : -1, result);
    return_result(ctx, result);
}

extern "C" void buck_osPfsAllocateFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    using namespace bumble::controller_pak;
    OSPfs* pfs = _arg<0, OSPfs*>(rdram, ctx);
    const auto company = _arg<1, std::uint16_t>(rdram, ctx);
    const auto game = _arg<2, std::uint32_t>(rdram, ctx);
    const gpr name = guest_address(_arg<3, PTR(u8)>(rdram, ctx));
    const gpr extension = guest_address(_arg<4, PTR(u8)>(rdram, ctx));
    const auto size = _arg<5, std::int32_t>(rdram, ctx);
    const gpr file_no_output = guest_address(_arg<6, PTR(s32)>(rdram, ctx));

    std::lock_guard lock(g_mutex);
    Result result = Result::Ok;
    Storage* card = initialized_storage(pfs, result);
    std::int32_t file_no = -1;
    if (card && name != 0 && extension != 0 && file_no_output != 0 && size > 0) {
        result = card->allocate(
            read_key(rdram, company, game, name, extension),
            static_cast<std::uint32_t>(size),
            file_no
        );
        if (result == Result::Ok) {
            MEM_W(0, file_no_output) = file_no;
        }
    } else if (card) {
        result = Result::Invalid;
    }
    log_operation("allocate", pfs ? pfs->channel : -1, result, file_no, size > 0 ? size : 0);
    return_result(ctx, result);
}

extern "C" void buck_osPfsDeleteFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    using namespace bumble::controller_pak;
    OSPfs* pfs = _arg<0, OSPfs*>(rdram, ctx);
    const auto company = _arg<1, std::uint16_t>(rdram, ctx);
    const auto game = _arg<2, std::uint32_t>(rdram, ctx);
    const gpr name = guest_address(_arg<3, PTR(u8)>(rdram, ctx));
    const gpr extension = guest_address(_arg<4, PTR(u8)>(rdram, ctx));
    std::lock_guard lock(g_mutex);
    Result result = Result::Ok;
    Storage* card = initialized_storage(pfs, result);
    if (card && name != 0 && extension != 0) {
        result = card->erase(read_key(rdram, company, game, name, extension));
    } else if (card) {
        result = Result::Invalid;
    }
    log_operation("delete", pfs ? pfs->channel : -1, result);
    return_result(ctx, result);
}

extern "C" void buck_osPfsFileState_recomp(uint8_t* rdram, recomp_context* ctx) {
    using namespace bumble::controller_pak;
    OSPfs* pfs = _arg<0, OSPfs*>(rdram, ctx);
    const auto file_no = _arg<1, std::int32_t>(rdram, ctx);
    const gpr output = guest_address(_arg<2, PTR(void)>(rdram, ctx));
    std::lock_guard lock(g_mutex);
    Result result = Result::Ok;
    Storage* card = initialized_storage(pfs, result);
    FileState state{};
    if (card && output != 0) {
        result = card->state(file_no, state);
        if (result == Result::Ok) {
            MEM_W(0x00, output) = static_cast<std::int32_t>(state.file_size);
            MEM_W(0x04, output) = static_cast<std::int32_t>(state.key.game_code);
            MEM_H(0x08, output) = static_cast<std::int16_t>(state.key.company_code);
            for (std::size_t index = 0; index < state.key.extension.size(); ++index) {
                MEM_B(0x0A + index, output) = state.key.extension[index];
            }
            for (std::size_t index = 0; index < state.key.game_name.size(); ++index) {
                MEM_B(0x0E + index, output) = state.key.game_name[index];
            }
        }
    } else if (card) {
        result = Result::Invalid;
    }
    log_operation("file_state", pfs ? pfs->channel : -1, result, file_no);
    return_result(ctx, result);
}

extern "C" void buck_osPfsFindFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    using namespace bumble::controller_pak;
    OSPfs* pfs = _arg<0, OSPfs*>(rdram, ctx);
    const auto company = _arg<1, std::uint16_t>(rdram, ctx);
    const auto game = _arg<2, std::uint32_t>(rdram, ctx);
    const gpr name = guest_address(_arg<3, PTR(u8)>(rdram, ctx));
    const gpr extension = guest_address(_arg<4, PTR(u8)>(rdram, ctx));
    const gpr file_no_output = guest_address(_arg<5, PTR(s32)>(rdram, ctx));
    std::lock_guard lock(g_mutex);
    Result result = Result::Ok;
    Storage* card = initialized_storage(pfs, result);
    std::int32_t file_no = -1;
    if (card && file_no_output != 0) {
        if (company == 0 && game == 0 && name == 0 && extension == 0) {
            result = card->find_free(file_no);
        } else if (name != 0 && extension != 0) {
            result = card->find(read_key(rdram, company, game, name, extension), file_no);
        } else {
            result = Result::Invalid;
        }
        MEM_W(0, file_no_output) = file_no;
    } else if (card) {
        result = Result::Invalid;
    }
    log_operation("find", pfs ? pfs->channel : -1, result, file_no);
    return_result(ctx, result);
}

extern "C" void buck_osPfsReadWriteFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    using namespace bumble::controller_pak;
    OSPfs* pfs = _arg<0, OSPfs*>(rdram, ctx);
    const auto file_no = _arg<1, std::int32_t>(rdram, ctx);
    const auto flag = _arg<2, std::uint8_t>(rdram, ctx);
    const auto offset = _arg<3, std::int32_t>(rdram, ctx);
    const auto size = _arg<4, std::int32_t>(rdram, ctx);
    const gpr data = guest_address(_arg<5, PTR(u8)>(rdram, ctx));
    std::lock_guard lock(g_mutex);
    Result result = Result::Ok;
    Storage* card = initialized_storage(pfs, result);
    if (card && data != 0 && offset >= 0 && size > 0 &&
        static_cast<std::uint32_t>(size) <= kUsableBytes) {
        std::vector<std::uint8_t> transfer(static_cast<std::size_t>(size));
        if (flag == 0) {
            result = card->read(
                file_no,
                static_cast<std::uint32_t>(offset),
                transfer
            );
            if (result == Result::Ok) {
                for (std::size_t index = 0; index < transfer.size(); ++index) {
                    MEM_B(index, data) = transfer[index];
                }
            }
        } else if (flag == 1 || flag == 2) {
            for (std::size_t index = 0; index < transfer.size(); ++index) {
                transfer[index] = MEM_BU(index, data);
            }
            result = card->write(
                file_no,
                static_cast<std::uint32_t>(offset),
                transfer
            );
        } else {
            result = Result::Invalid;
        }
    } else if (card) {
        result = Result::Invalid;
    }
    log_operation(
        flag == 0 ? "read" : "write",
        pfs ? pfs->channel : -1,
        result,
        file_no,
        size > 0 ? size : 0,
        offset
    );
    return_result(ctx, result);
}

extern "C" void buck_osPfsNumFiles_recomp(uint8_t* rdram, recomp_context* ctx) {
    using namespace bumble::controller_pak;
    OSPfs* pfs = _arg<0, OSPfs*>(rdram, ctx);
    const gpr max_output = guest_address(_arg<1, PTR(s32)>(rdram, ctx));
    const gpr used_output = guest_address(_arg<2, PTR(s32)>(rdram, ctx));
    std::lock_guard lock(g_mutex);
    Result result = Result::Ok;
    Storage* card = initialized_storage(pfs, result);
    if (card && max_output != 0 && used_output != 0) {
        MEM_W(0, max_output) = static_cast<std::int32_t>(kSlotCount);
        MEM_W(0, used_output) = static_cast<std::int32_t>(card->used_files());
    } else if (card) {
        result = Result::Invalid;
    }
    log_operation("num_files", pfs ? pfs->channel : -1, result);
    return_result(ctx, result);
}

extern "C" void buck_controller_pak_validate_recomp(
    uint8_t* rdram,
    recomp_context* ctx
) {
    using namespace bumble::controller_pak;
    OSPfs* pfs = _arg<0, OSPfs*>(rdram, ctx);
    std::lock_guard lock(g_mutex);
    Result result = Result::Ok;
    Storage* card = initialized_storage(pfs, result);
    if (card) {
        const auto prior_identity = card->identity();
        result = card->reload();
        if (result == Result::Ok && card->identity() != prior_identity) {
            result = Result::NewPack;
            pfs->status = 0;
        } else if (result != Result::Ok) {
            pfs->status = 0;
        }
    }
    log_operation("validate_identity", pfs ? pfs->channel : -1, result);
    return_result(ctx, result);
}
