#pragma once

#include <cstdint>
#include <filesystem>

#include "recomp.h"

namespace bumble::controller_pak {

void configure_storage_root(const std::filesystem::path& root);
void reset_runtime_state();
bool clear_all_storage();
std::filesystem::path storage_path(std::uint32_t channel);

} // namespace bumble::controller_pak

extern "C" {
void buck_osPfsInitPak_recomp(std::uint8_t* rdram, recomp_context* ctx);
void buck_osPfsFreeBlocks_recomp(std::uint8_t* rdram, recomp_context* ctx);
void buck_osPfsAllocateFile_recomp(std::uint8_t* rdram, recomp_context* ctx);
void buck_osPfsDeleteFile_recomp(std::uint8_t* rdram, recomp_context* ctx);
void buck_osPfsFileState_recomp(std::uint8_t* rdram, recomp_context* ctx);
void buck_osPfsFindFile_recomp(std::uint8_t* rdram, recomp_context* ctx);
void buck_osPfsReadWriteFile_recomp(std::uint8_t* rdram, recomp_context* ctx);
void buck_osPfsNumFiles_recomp(std::uint8_t* rdram, recomp_context* ctx);
void buck_controller_pak_validate_recomp(std::uint8_t* rdram, recomp_context* ctx);
}
