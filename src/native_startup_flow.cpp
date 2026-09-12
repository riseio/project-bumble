#include "native_startup_flow.hpp"

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>

namespace {

constexpr uint32_t kFrontendObject = 0x800FFF80u;
constexpr uint32_t kIntroState = 0x00000008u;
constexpr uint32_t kMainMenuState = 0x0000000Cu;
constexpr uint32_t kAccessoryChoiceState = 0x00000002u;
constexpr uint32_t kFrontendPreviousStateOffset = 0x80u;
constexpr uint32_t kFrontendPreviousTargetOffset = 0x84u;
constexpr uint32_t kMainMenuDescriptorTableEntry = 0x800FE5A0u;
constexpr uint32_t kMainMenuDescriptor = 0x800FC940u;
constexpr uint32_t kMainMenuNextStateWord = 0x800FC944u;
constexpr uint32_t kMainMenuDefaultItemWord = 0x800FC954u;
constexpr uint32_t kMainMenuFlagsWord = 0x800FC95Cu;
constexpr uint32_t kMainMenuIdleTimeoutWord = 0x800FC960u;
constexpr uint32_t kExpectedMainMenuNextState = 0x0000000Du;
constexpr uint32_t kExpectedMainMenuDefaultItem = 0x800FC820u;
constexpr uint32_t kExpectedMainMenuFlags = 0x0000002Fu;
constexpr uint32_t kExpectedMainMenuIdleTimeout = 0x000004B0u;
constexpr uint32_t kRumblePromptState = 0x0000000Eu;
constexpr uint32_t kTrainingSelectionState = 0x00000027u;
constexpr uint32_t kRumblePromptRuntimeState = 0x0000002Bu;
constexpr uint32_t kRumblePromptSuccessorState = 0x00000018u;
constexpr uint32_t kFrontendDescriptorTable = 0x800FE570u;
constexpr uint32_t kControllerPakRumblePromptDescriptor = 0x800FE508u;
constexpr uint32_t kAccessoryChoiceDescriptor = 0x800FC1E0u;
constexpr uint32_t kIntroDescriptor = 0x800FC668u;
constexpr uint32_t kGameSelectionDescriptor = 0x800FD388u;
constexpr uint32_t kNativeLevelSelectDescriptor = 0x800FD320u;
constexpr uint32_t kTrainingSelectionDescriptor = 0x800FE1A8u;
constexpr uint32_t kRumblePromptSuccessorDescriptor = 0x800FD3F0u;
constexpr uint32_t kRumblePromptFlags = 0x00000203u;
constexpr uint32_t kAccessoryChoiceFlags = 0x00000A03u;
constexpr uint32_t kFrontendTransitionOffset = 0x40u;
constexpr uint32_t kFrontendDescriptorFlagsOffset = 0x1Cu;
constexpr uint32_t kFrontendDescriptorItemOffset = 0x38u;
constexpr uint32_t kFrontendConfirmMask = 0x00008000u;
constexpr uint32_t kExtendedRdramStart = 0x80800000u;
constexpr uint32_t kExtendedRdramEnd = 0x84000000u;
constexpr uint32_t kMenuNodeBytes = 0x28u;
constexpr uint32_t kMenuStringOffset = 4u * kMenuNodeBytes;
constexpr uint32_t kNativeMenuItemColour = 0x7FFF6666u;
constexpr uint32_t kNativeMenuItemFlags = 0x00000019u;
constexpr uint32_t kNativeMenuItemStyle = 0x0000000Au;
constexpr std::array<uint32_t, 8> kStartupPakNoticeDescriptors{
    0x800FC080u,
    0x800FC120u,
    kAccessoryChoiceDescriptor,
    0x800FC2D8u,
    0x800FC410u,
    0x800FC520u,
    0x800FC520u,
    0x800FC5C0u,
};

std::atomic_bool g_skip_intro_enabled{false};
std::atomic_bool g_suppress_attract_enabled{false};
std::atomic_bool g_intro_skip_applied{false};
std::atomic_uint64_t g_intro_skip_count{0};
std::atomic_bool g_attract_suppression_applied{false};
std::atomic_uint64_t g_attract_suppression_count{0};
std::atomic_uint64_t g_attract_transition_preemption_count{0};
std::atomic_bool g_bypass_rumble_prompt_enabled{false};
std::atomic_bool g_rumble_prompt_bypass_latched{false};
std::atomic_uint64_t g_rumble_prompt_bypass_count{0};
thread_local bool g_attract_transition_preemption_pending = false;

gpr guest_address(uint32_t address) {
    return static_cast<gpr>(static_cast<int32_t>(address));
}

uint32_t guest_u32(gpr value) {
    return static_cast<uint32_t>(value);
}

uint32_t read_u32(uint8_t* rdram, uint32_t address) {
    return static_cast<uint32_t>(MEM_W(0, guest_address(address)));
}

void write_u32(uint8_t* rdram, uint32_t address, uint32_t value) {
    MEM_W(0, guest_address(address)) = static_cast<int32_t>(value);
}

bool native_main_menu_default_item_matches(
    uint8_t* rdram,
    uint32_t item
) {
    return item >= kExtendedRdramStart &&
        item <= kExtendedRdramEnd - kMenuNodeBytes &&
        read_u32(rdram, item + 0x00u) == 0u &&
        read_u32(rdram, item + 0x04u) == item + kMenuNodeBytes &&
        read_u32(rdram, item + 0x08u) == kNativeMenuItemColour &&
        read_u32(rdram, item + 0x0Cu) == item + kMenuStringOffset &&
        read_u32(rdram, item + 0x10u) == kNativeMenuItemFlags &&
        read_u32(rdram, item + 0x14u) == kNativeMenuItemStyle &&
        read_u32(rdram, item + 0x1Cu) == 0u &&
        read_u32(rdram, item + 0x20u) == 0u &&
        read_u32(rdram, item + 0x24u) == 0u;
}

bool main_menu_descriptor_matches(uint8_t* rdram) {
    const uint32_t default_item = read_u32(
        rdram,
        kMainMenuDefaultItemWord
    );
    return read_u32(rdram, kMainMenuDescriptorTableEntry) ==
            kMainMenuDescriptor &&
        read_u32(rdram, kMainMenuNextStateWord) ==
            kExpectedMainMenuNextState &&
        (default_item == kExpectedMainMenuDefaultItem ||
            native_main_menu_default_item_matches(rdram, default_item)) &&
        read_u32(rdram, kMainMenuFlagsWord) == kExpectedMainMenuFlags;
}

uint32_t descriptor_for_state(uint8_t* rdram, uint32_t state) {
    return read_u32(
        rdram,
        kFrontendDescriptorTable + state * sizeof(uint32_t)
    );
}

bool selection_descriptor_matches(uint8_t* rdram, uint32_t state) {
    if (state == kRumblePromptState) {
        const uint32_t descriptor = descriptor_for_state(rdram, state);
        return descriptor == kGameSelectionDescriptor ||
            descriptor == kNativeLevelSelectDescriptor;
    }
    if (state == kTrainingSelectionState) {
        return descriptor_for_state(rdram, state) ==
            kTrainingSelectionDescriptor;
    }
    return false;
}

bool startup_pak_notice_descriptor_matches(
    uint8_t* rdram,
    uint32_t state,
    uint32_t descriptor
) {
    return state < kStartupPakNoticeDescriptors.size() &&
        descriptor == kStartupPakNoticeDescriptors[state] &&
        descriptor_for_state(rdram, state) == descriptor;
}

} // namespace

void bumble::startup_flow::configure(bool skip_intro, bool suppress_attract) {
    g_skip_intro_enabled.store(skip_intro, std::memory_order_release);
    g_suppress_attract_enabled.store(
        suppress_attract,
        std::memory_order_release
    );
    g_intro_skip_applied.store(false, std::memory_order_release);
    g_intro_skip_count.store(0, std::memory_order_release);
    g_attract_suppression_applied.store(false, std::memory_order_release);
    g_attract_suppression_count.store(0, std::memory_order_release);
    g_attract_transition_preemption_count.store(0, std::memory_order_release);
    g_attract_transition_preemption_pending = false;
    std::fprintf(
        stderr,
        "BUMBLE_STARTUP_FLOW stage=configured skip_intro=%d suppress_attract=%d\n",
        skip_intro ? 1 : 0,
        suppress_attract ? 1 : 0
    );
    std::fflush(stderr);
}

bool bumble::startup_flow::skip_intro_enabled() {
    return g_skip_intro_enabled.load(std::memory_order_acquire);
}

bool bumble::startup_flow::attract_suppression_enabled() {
    return g_suppress_attract_enabled.load(std::memory_order_acquire);
}

uint64_t bumble::startup_flow::intro_skip_count() {
    return g_intro_skip_count.load(std::memory_order_acquire);
}

uint64_t bumble::startup_flow::attract_suppression_count() {
    return g_attract_suppression_count.load(std::memory_order_acquire);
}

void bumble::startup_flow::configure_rumble_prompt_bypass(
    bool bypass_prompt
) {
    g_bypass_rumble_prompt_enabled.store(
        bypass_prompt,
        std::memory_order_release
    );
    g_rumble_prompt_bypass_latched.store(false, std::memory_order_release);
    g_rumble_prompt_bypass_count.store(0, std::memory_order_release);
    std::fprintf(
        stderr,
        "BUMBLE_STARTUP_FLOW stage=rumble_prompt_configured bypass=%d\n",
        bypass_prompt ? 1 : 0
    );
    std::fflush(stderr);
}

bool bumble::startup_flow::rumble_prompt_bypass_enabled() {
    return g_bypass_rumble_prompt_enabled.load(std::memory_order_acquire);
}

uint64_t bumble::startup_flow::rumble_prompt_bypass_count() {
    return g_rumble_prompt_bypass_count.load(std::memory_order_acquire);
}

extern "C" void bumble_apply_startup_menu_skip(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr ||
        guest_u32(context->r16) != kFrontendObject) {
        return;
    }

    const bool skip_intro =
        g_skip_intro_enabled.load(std::memory_order_acquire);
    const bool bypass_prompt =
        g_bypass_rumble_prompt_enabled.load(std::memory_order_acquire);
    const uint32_t initial_state = read_u32(rdram, kFrontendObject);
    const uint32_t initial_target = read_u32(
        rdram,
        kFrontendObject + kFrontendPreviousTargetOffset
    );
    const uint32_t initial_descriptor = initial_state <
            kStartupPakNoticeDescriptors.size()
        ? descriptor_for_state(rdram, initial_state)
        : 0u;
    if (bypass_prompt && initial_target == initial_state &&
        read_u32(rdram, kFrontendObject + kFrontendTransitionOffset) == 0u &&
        startup_pak_notice_descriptor_matches(
            rdram,
            initial_state,
            initial_descriptor
        ) &&
        descriptor_for_state(rdram, kIntroState) == kIntroDescriptor &&
        (!skip_intro || main_menu_descriptor_matches(rdram))) {
        const uint32_t target_state =
            skip_intro ? kMainMenuState : kIntroState;
        write_u32(rdram, kFrontendObject, target_state);
        write_u32(
            rdram,
            kFrontendObject + kFrontendPreviousTargetOffset,
            target_state
        );
        g_rumble_prompt_bypass_latched.store(true, std::memory_order_release);
        const uint64_t bypass_count =
            g_rumble_prompt_bypass_count.fetch_add(
                1,
                std::memory_order_acq_rel
            ) + 1;
        std::fprintf(
            stderr,
            "BUMBLE_STARTUP_FLOW stage=pak_notice_preempted count=%" PRIu64
            " pc=0x800AABCC object=0x%08" PRIX32
            " source_state=0x%08" PRIX32
            " source_descriptor=0x%08" PRIX32
            " target_state=0x%08" PRIX32 " target_descriptor=0x%08" PRIX32
            "\n",
            bypass_count,
            kFrontendObject,
            initial_state,
            initial_descriptor,
            target_state,
            descriptor_for_state(rdram, target_state)
        );
        std::fflush(stderr);

        if (skip_intro) {
            g_intro_skip_applied.store(true, std::memory_order_release);
            const uint64_t intro_count =
                g_intro_skip_count.fetch_add(1, std::memory_order_acq_rel) + 1;
            std::fprintf(
                stderr,
                "BUMBLE_STARTUP_FLOW stage=intro_skipped count=%" PRIu64
                " pc=0x800AABCC object=0x%08" PRIX32
                " source_state=0x%08" PRIX32 " target_state=0x%08" PRIX32
                " descriptor=0x%08" PRIX32 " default_item=0x%08" PRIX32
                "\n",
                intro_count,
                kFrontendObject,
                initial_state,
                kMainMenuState,
                kMainMenuDescriptor,
                kExpectedMainMenuDefaultItem
            );
            std::fflush(stderr);
        }
        return;
    }

    if (!skip_intro ||
        g_intro_skip_applied.load(std::memory_order_acquire) ||
        initial_state != kIntroState || initial_target != kIntroState ||
        !main_menu_descriptor_matches(rdram)) {
        return;
    }

    bool expected = false;
    if (!g_intro_skip_applied.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel)) {
        return;
    }

    write_u32(rdram, kFrontendObject, kMainMenuState);
    write_u32(
        rdram,
        kFrontendObject + kFrontendPreviousTargetOffset,
        kMainMenuState
    );
    const uint64_t count =
        g_intro_skip_count.fetch_add(1, std::memory_order_acq_rel) + 1;
    std::fprintf(
        stderr,
        "BUMBLE_STARTUP_FLOW stage=intro_skipped count=%" PRIu64
        " pc=0x800AABCC object=0x%08" PRIX32
        " source_state=0x%08" PRIX32 " target_state=0x%08" PRIX32
        " descriptor=0x%08" PRIX32 " default_item=0x%08" PRIX32 "\n",
        count,
        kFrontendObject,
        kIntroState,
        kMainMenuState,
        kMainMenuDescriptor,
        kExpectedMainMenuDefaultItem
    );
    std::fflush(stderr);
}

extern "C" void bumble_bypass_new_game_rumble_prompt(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!g_bypass_rumble_prompt_enabled.load(std::memory_order_acquire) ||
        rdram == nullptr || context == nullptr) {
        return;
    }

    const uint32_t live_object = guest_u32(context->r19);
    const uint32_t descriptor = guest_u32(context->r18);
    const uint32_t previous_target = read_u32(
        rdram,
        kFrontendObject + kFrontendPreviousTargetOffset
    );
    const bool known_prompt_descriptor =
        descriptor == kControllerPakRumblePromptDescriptor ||
        descriptor == kAccessoryChoiceDescriptor;
    const uint32_t descriptor_flags = known_prompt_descriptor
        ? read_u32(rdram, descriptor + kFrontendDescriptorFlagsOffset)
        : 0u;
    const bool exact_post_selection_prompt =
        descriptor == kControllerPakRumblePromptDescriptor &&
        selection_descriptor_matches(rdram, previous_target) &&
        descriptor_flags == kRumblePromptFlags;
    const bool exact_boot_accessory_choice =
        descriptor == kAccessoryChoiceDescriptor &&
        previous_target == kAccessoryChoiceState &&
        descriptor_flags == kAccessoryChoiceFlags;
    const bool exact_post_menu_accessory_choice =
        descriptor == kAccessoryChoiceDescriptor &&
        previous_target == kRumblePromptState &&
        descriptor_flags == kRumblePromptFlags;
    const bool exact_prompt = live_object == kFrontendObject &&
        (exact_post_selection_prompt || exact_boot_accessory_choice ||
            exact_post_menu_accessory_choice) &&
        read_u32(rdram, kFrontendObject + kFrontendTransitionOffset) == 0u &&
        read_u32(
            rdram,
            descriptor + kFrontendDescriptorItemOffset
        ) == 0u;

    if (!exact_prompt) {
        g_rumble_prompt_bypass_latched.store(false, std::memory_order_release);
        return;
    }

    bool expected = false;
    if (!g_rumble_prompt_bypass_latched.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel)) {
        return;
    }

    context->r2 = static_cast<gpr>(kFrontendConfirmMask);
    const uint64_t count =
        g_rumble_prompt_bypass_count.fetch_add(
            1,
            std::memory_order_acq_rel
        ) + 1;
    std::fprintf(
        stderr,
        "BUMBLE_STARTUP_FLOW stage=rumble_prompt_bypassed count=%" PRIu64
        " pc=0x800AB72C object=0x%08" PRIX32
        " state=0x%08" PRIX32 " descriptor=0x%08" PRIX32
        " flags=0x%08" PRIX32 " synthetic_mask=0x%04" PRIX32 "\n",
        count,
        kFrontendObject,
        previous_target,
        descriptor,
        descriptor_flags,
        kFrontendConfirmMask
    );
    std::fflush(stderr);
}

extern "C" void bumble_disable_main_menu_attract(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!g_suppress_attract_enabled.load(std::memory_order_acquire) ||
        rdram == nullptr || context == nullptr) {
        return;
    }

    const uint32_t live_object = guest_u32(context->r19);
    const uint32_t live_state = guest_u32(context->r22);
    const uint32_t live_descriptor = guest_u32(context->r18);
    if (live_object != kFrontendObject || live_state != kMainMenuState ||
        live_descriptor != kMainMenuDescriptor ||
        read_u32(rdram, kFrontendObject) != kMainMenuState ||
        read_u32(
            rdram,
            kFrontendObject + kFrontendPreviousTargetOffset
        ) != kMainMenuState ||
        read_u32(rdram, kFrontendObject + kFrontendTransitionOffset) != 0u ||
        descriptor_for_state(rdram, kMainMenuState) != kMainMenuDescriptor ||
        !main_menu_descriptor_matches(rdram) ||
        read_u32(rdram, kMainMenuIdleTimeoutWord) !=
            kExpectedMainMenuIdleTimeout) {
        return;
    }

    bool expected = false;
    g_attract_suppression_applied.compare_exchange_strong(
        expected,
        true,
        std::memory_order_acq_rel
    );
    write_u32(rdram, kMainMenuIdleTimeoutWord, 0u);
    const uint64_t count = g_attract_suppression_count.fetch_add(
        1,
        std::memory_order_acq_rel
    ) + 1;
    std::fprintf(
        stderr,
        "BUMBLE_STARTUP_FLOW stage=attract_disabled count=%" PRIu64
        " pc=0x800AB72C object=0x%08" PRIX32
        " state=0x%08" PRIX32 " descriptor=0x%08" PRIX32
        " successor=0x%08" PRIX32
        " original_timeout=0x%08" PRIX32 " replacement_timeout=0\n",
        count,
        live_object,
        live_state,
        live_descriptor,
        kExpectedMainMenuNextState,
        kExpectedMainMenuIdleTimeout
    );
    std::fflush(stderr);
}

extern "C" void bumble_preempt_main_menu_attract_transition(
    uint8_t* rdram,
    recomp_context* context
) {
    g_attract_transition_preemption_pending = false;
    if (!g_suppress_attract_enabled.load(std::memory_order_acquire) ||
        rdram == nullptr || context == nullptr) {
        return;
    }

    const uint32_t live_object = guest_u32(context->r19);
    const uint32_t live_state = guest_u32(context->r22);
    const uint32_t live_descriptor = guest_u32(context->r18);
    const uint32_t timeout = read_u32(rdram, kMainMenuIdleTimeoutWord);
    if (live_object != kFrontendObject || live_state != kMainMenuState ||
        live_descriptor != kMainMenuDescriptor ||
        guest_u32(context->r2) != 1u ||
        read_u32(rdram, kFrontendObject + kFrontendPreviousTargetOffset) !=
            kMainMenuState ||
        read_u32(rdram, kFrontendObject + kFrontendTransitionOffset) != 0u ||
        descriptor_for_state(rdram, kMainMenuState) != kMainMenuDescriptor ||
        !main_menu_descriptor_matches(rdram) ||
        (timeout != 0u && timeout != kExpectedMainMenuIdleTimeout)) {
        return;
    }

    context->r2 = 0;
    g_attract_transition_preemption_pending = true;
}

extern "C" void bumble_retain_main_menu_after_attract_transition(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!g_attract_transition_preemption_pending) {
        return;
    }
    g_attract_transition_preemption_pending = false;
    if (!g_suppress_attract_enabled.load(std::memory_order_acquire) ||
        rdram == nullptr || context == nullptr ||
        guest_u32(context->r19) != kFrontendObject ||
        guest_u32(context->r22) != kMainMenuState ||
        guest_u32(context->r18) != kMainMenuDescriptor ||
        guest_u32(context->r2) != kExpectedMainMenuNextState ||
        read_u32(rdram, kFrontendObject + kFrontendPreviousTargetOffset) !=
            kMainMenuState ||
        read_u32(rdram, kFrontendObject + kFrontendTransitionOffset) != 0u ||
        !main_menu_descriptor_matches(rdram)) {
        return;
    }

    context->r2 = static_cast<gpr>(kMainMenuState);
    const uint64_t count = g_attract_transition_preemption_count.fetch_add(
        1,
        std::memory_order_acq_rel
    ) + 1;
    std::fprintf(
        stderr,
        "BUMBLE_STARTUP_FLOW stage=attract_transition_preempted"
        " count=%" PRIu64 " pc=0x800ABC70 object=0x%08" PRIX32
        " retained_state=0x%08" PRIX32
        " rejected_successor=0x%08" PRIX32
        " transition_flag=0 guest_memory_mutated=0\n",
        count,
        kFrontendObject,
        kMainMenuState,
        kExpectedMainMenuNextState
    );
    std::fflush(stderr);
}

extern "C" void bumble_preempt_new_game_rumble_prompt(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!g_bypass_rumble_prompt_enabled.load(std::memory_order_acquire) ||
        rdram == nullptr || context == nullptr) {
        return;
    }

    const uint32_t live_object = guest_u32(context->r16);
    const uint32_t selected_state = guest_u32(context->r5);
    const uint32_t descriptor = guest_u32(context->r6);
    const uint32_t previous_selection_target = read_u32(
        rdram,
        kFrontendObject + kFrontendPreviousTargetOffset
    );
    const bool exact_post_selection_transition =
        live_object == kFrontendObject &&
        selected_state == kRumblePromptRuntimeState &&
        descriptor == kControllerPakRumblePromptDescriptor &&
        read_u32(rdram, kFrontendObject) == kRumblePromptRuntimeState &&
        read_u32(rdram, kFrontendObject + kFrontendTransitionOffset) == 0u &&
        descriptor_for_state(rdram, kRumblePromptRuntimeState) ==
            kControllerPakRumblePromptDescriptor &&
        read_u32(
            rdram,
            descriptor + kFrontendDescriptorFlagsOffset
        ) == kRumblePromptFlags &&
        read_u32(
            rdram,
            descriptor + kFrontendDescriptorItemOffset
        ) == 0u &&
        read_u32(rdram, descriptor + sizeof(uint32_t)) ==
            kRumblePromptSuccessorState &&
        descriptor_for_state(rdram, kRumblePromptSuccessorState) ==
            kRumblePromptSuccessorDescriptor;
    const bool exact_post_menu_accessory_transition =
        live_object == kFrontendObject &&
        selected_state == kAccessoryChoiceState &&
        descriptor == kAccessoryChoiceDescriptor &&
        read_u32(rdram, kFrontendObject) == kAccessoryChoiceState &&
        read_u32(
            rdram,
            kFrontendObject + kFrontendPreviousStateOffset
        ) == kRumblePromptState &&
        read_u32(
            rdram,
            kFrontendObject + kFrontendPreviousTargetOffset
        ) == kRumblePromptState &&
        read_u32(rdram, kFrontendObject + kFrontendTransitionOffset) == 0u &&
        descriptor_for_state(rdram, kAccessoryChoiceState) ==
            kAccessoryChoiceDescriptor &&
        read_u32(rdram, descriptor + sizeof(uint32_t)) ==
            kRumblePromptState &&
        read_u32(
            rdram,
            descriptor + kFrontendDescriptorFlagsOffset
        ) == kRumblePromptFlags &&
        read_u32(
            rdram,
            descriptor + kFrontendDescriptorItemOffset
        ) == 0u &&
        descriptor_for_state(rdram, kRumblePromptState) ==
            kGameSelectionDescriptor;
    const bool exact_controller_pak_notice_transition =
        live_object == kFrontendObject &&
        selected_state < kStartupPakNoticeDescriptors.size() &&
        read_u32(rdram, kFrontendObject) == selected_state &&
        read_u32(rdram, kFrontendObject + kFrontendTransitionOffset) == 0u &&
        startup_pak_notice_descriptor_matches(
            rdram,
            selected_state,
            descriptor
        );
    if (!exact_post_selection_transition &&
        !exact_post_menu_accessory_transition &&
        !exact_controller_pak_notice_transition) {
        return;
    }

    const uint32_t source_state = exact_post_selection_transition
        ? kRumblePromptRuntimeState
        : selected_state;
    const uint32_t target_state = exact_post_selection_transition
        ? kRumblePromptSuccessorState
        : (exact_post_menu_accessory_transition
            ? kRumblePromptState
            : kMainMenuState);
    const uint32_t target_descriptor = exact_post_selection_transition
        ? kRumblePromptSuccessorDescriptor
        : (exact_post_menu_accessory_transition
            ? kGameSelectionDescriptor
            : kMainMenuDescriptor);

    write_u32(rdram, kFrontendObject, target_state);
    if (exact_post_menu_accessory_transition) {
        write_u32(
            rdram,
            kFrontendObject + kFrontendPreviousStateOffset,
            kAccessoryChoiceState
        );
    }
    write_u32(
        rdram,
        kFrontendObject + kFrontendPreviousTargetOffset,
        target_state
    );
    context->r5 = static_cast<gpr>(target_state);
    context->r6 = guest_address(target_descriptor);
    g_rumble_prompt_bypass_latched.store(true, std::memory_order_release);
    const uint64_t count =
        g_rumble_prompt_bypass_count.fetch_add(
            1,
            std::memory_order_acq_rel
        ) + 1;
    std::fprintf(
        stderr,
        "BUMBLE_STARTUP_FLOW stage=pak_notice_preempted count=%" PRIu64
        " pc=0x800AB080 object=0x%08" PRIX32
        " source_state=0x%08" PRIX32 " source_descriptor=0x%08" PRIX32
        " selection_state=0x%08" PRIX32
        " target_state=0x%08" PRIX32 " target_descriptor=0x%08" PRIX32
        "\n",
        count,
        kFrontendObject,
        source_state,
        descriptor,
        exact_post_selection_transition
            ? previous_selection_target
            : selected_state,
        target_state,
        target_descriptor
    );
    std::fflush(stderr);
}
