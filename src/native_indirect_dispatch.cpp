#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>

#include "funcs.h"

namespace {

using RecompFunction = void (*)(uint8_t*, recomp_context*);

constexpr std::array<RecompFunction, 45> kSoundCommandHandlers = {{
    &func_800512B4,
    &func_80051284,
    &func_80051260,
    &func_80051254,
    &func_8004C5D0,
    &func_80051188,
    &func_80051150,
    &func_80051174,
    &func_80051148,
    &func_80051140,
    &func_80051138,
    &func_800510FC,
    &func_800510EC,
    &func_800510DC,
    &func_800510CC,
    &func_800510C0,
    &func_80051060,
    &func_80051050,
    &func_80051044,
    &func_80051034,
    &func_80051028,
    &func_80050F88,
    &func_80050EF0,
    &func_80050EE8,
    &func_80050EE0,
    &func_80050ED0,
    &func_80050EC4,
    &func_80050EB0,
    &func_80050E9C,
    &func_80050E94,
    &func_80050E6C,
    &func_80050E60,
    &func_80050E58,
    &func_80050DE8,
    &func_80050DD8,
    &func_80050D60,
    &func_80050CE4,
    &func_80050C6C,
    &func_80050C58,
    &func_80050BDC,
    &func_80050BA4,
    &func_80050B9C,
    &func_80050B8C,
    &func_80050B3C,
    &func_80050B20,
}};

struct ResourceCommandHandler {
    uint32_t guest_address;
    RecompFunction function;
};

constexpr std::array<ResourceCommandHandler, 81> kResourceCommandHandlers = {{
    {0x80092144u, &func_80092144},
    {0x800923D8u, &func_800923D8},
    {0x800926ACu, &func_800926AC},
    {0x80092774u, &func_80092774},
    {0x800928BCu, &func_800928BC},
    {0x80092CE8u, &func_80092CE8},
    {0x80092F88u, &func_80092F88},
    {0x80093468u, &func_80093468},
    {0x800935D4u, &func_800935D4},
    {0x80093C2Cu, &func_80093C2C},
    {0x80093DB0u, &func_80093DB0},
    {0x80093FACu, &func_80093FAC},
    {0x800941ACu, &func_800941AC},
    {0x8009433Cu, &func_8009433C},
    {0x80094544u, &func_80094544},
    {0x800946ACu, &func_800946AC},
    {0x800947FCu, &func_800947FC},
    {0x80094928u, &func_80094928},
    {0x800949B4u, &func_800949B4},
    {0x80094B08u, &func_80094B08},
    {0x80094E5Cu, &func_80094E5C},
    {0x80094FD0u, &func_80094FD0},
    {0x800950E4u, &func_800950E4},
    {0x80095188u, &func_80095188},
    {0x80095194u, &func_80095194},
    {0x800951A4u, &func_800951A4},
    {0x800951B4u, &func_800951B4},
    {0x80095218u, &func_80095218},
    {0x80095224u, &func_80095224},
    {0x80095288u, &func_80095288},
    {0x800952ECu, &func_800952EC},
    {0x800952F8u, &func_800952F8},
    {0x80095308u, &func_80095308},
    {0x8009536Cu, &func_8009536C},
    {0x8009538Cu, &func_8009538C},
    {0x800953ECu, &func_800953EC},
    {0x80095438u, &func_80095438},
    {0x80095460u, &func_80095460},
    {0x80095478u, &func_80095478},
    {0x80095490u, &func_80095490},
    {0x800954A8u, &func_800954A8},
    {0x800954C0u, &func_800954C0},
    {0x800954D8u, &func_800954D8},
    {0x8009553Cu, &func_8009553C},
    {0x80095558u, &func_80095558},
    {0x800955B0u, &func_800955B0},
    {0x800955F0u, &func_800955F0},
    {0x8009565Cu, &func_8009565C},
    {0x800956C0u, &func_800956C0},
    {0x80095740u, &func_80095740},
    {0x80095788u, &func_80095788},
    {0x800957FCu, &func_800957FC},
    {0x80095894u, &func_80095894},
    {0x80095918u, &func_80095918},
    {0x80095954u, &func_80095954},
    {0x800959C8u, &func_800959C8},
    {0x80095B30u, &func_80095B30},
    {0x80095B70u, &func_80095B70},
    {0x80095C08u, &func_80095C08},
    {0x80095CA0u, &func_80095CA0},
    {0x80095D38u, &func_80095D38},
    {0x80095FFCu, &func_80095FFC},
    {0x80096064u, &func_80096064},
    {0x800960A4u, &func_800960A4},
    {0x800960D4u, &func_800960D4},
    {0x8009615Cu, &func_8009615C},
    {0x8009618Cu, &func_8009618C},
    {0x800961BCu, &func_800961BC},
    {0x80096214u, &func_80096214},
    {0x80096230u, &func_80096230},
    {0x8009624Cu, &func_8009624C},
    {0x80096324u, &func_80096324},
    {0x80096394u, &func_80096394},
    {0x800965CCu, &func_800965CC},
    {0x800965FCu, &func_800965FC},
    {0x8009662Cu, &func_8009662C},
    {0x80096774u, &func_80096774},
    {0x800967D4u, &func_800967D4},
    {0x80096898u, &func_80096898},
    {0x800968D8u, &func_800968D8},
    {0x80096918u, &func_80096918},
}};

const ResourceCommandHandler* find_resource_handler(uint32_t guest_address) {
    std::size_t first = 0;
    std::size_t last = kResourceCommandHandlers.size();
    while (first < last) {
        const std::size_t middle = first + (last - first) / 2;
        if (kResourceCommandHandlers[middle].guest_address < guest_address) {
            first = middle + 1;
        }
        else {
            last = middle;
        }
    }
    if (first == kResourceCommandHandlers.size() ||
        kResourceCommandHandlers[first].guest_address != guest_address) {
        return nullptr;
    }
    return &kResourceCommandHandlers[first];
}

} // namespace

extern "C" void buck_sound_command_dispatch_recomp(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t command = static_cast<uint32_t>(context->r6) & 0x7Fu;
    if (command >= kSoundCommandHandlers.size()) {
        std::fprintf(
            stderr,
            "BUMBLE_INDIRECT_DISPATCH stage=sound_command_rejected command=0x%02" PRIX32
            " site=0x8004D238 action=terminate_stream\n",
            command
        );
        std::fflush(stderr);
        context->r2 = 0;
        return;
    }
    kSoundCommandHandlers[command](rdram, context);
}

extern "C" void buck_resource_command_dispatch_recomp(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t guest_address = static_cast<uint32_t>(context->r4);
    const ResourceCommandHandler* handler = find_resource_handler(guest_address);
    if (handler == nullptr) {
        std::fprintf(
            stderr,
            "BUMBLE_INDIRECT_DISPATCH stage=resource_command_rejected target=0x%08" PRIX32
            " sites=0x80090FD8,0x80096A64 action=terminate_stream\n",
            guest_address
        );
        std::fflush(stderr);
        if (context->r3 != 0) {
            MEM_W(0, context->r3) = 0;
        }
        context->r2 = 0;
        return;
    }
    handler->function(rdram, context);
}
