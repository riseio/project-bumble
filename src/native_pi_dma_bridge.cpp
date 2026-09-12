#include "native_pi_dma_bridge.hpp"

#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include "native_checkpoint_bridge.hpp"
#include "xxHash/xxh3.h"

namespace {

constexpr uint32_t kRdramSize = 0x00800000u;

constexpr uint32_t kBlockingCallSite = 0x80011580u;
constexpr uint32_t kChunkedCallSite = 0x80011620u;
constexpr uint32_t kAudioPageCallSite = 0x80051C38u;

constexpr uint32_t kBlockingReceiptPc = 0x80011598u;
constexpr uint32_t kChunkedReceiptPc = 0x80011638u;
constexpr uint32_t kAudioDrainReceiptPc = 0x80051CE8u;

std::atomic_uint64_t g_observer_tick{0};
std::atomic_bool g_capture_enabled{false};
std::atomic_bool g_capture_draining{false};
std::mutex g_state_mutex;
uint64_t g_transfer_sequence = 0;
uint64_t g_receipt_sequence = 0;
bumble::native_pi_dma::CaptureStats g_stats{};

struct PendingTransfer {
    uint64_t sequence;
    uint32_t call_site;
    bool detail_logged;
};

std::unordered_map<uint32_t, std::deque<PendingTransfer>> g_pending_by_queue;

struct CallSiteIdentity {
    const char* kind;
    bool known;
};

CallSiteIdentity identify_call_site(uint32_t call_site) {
    switch (call_site) {
    case kBlockingCallSite:
        return {"blocking_wrapper", true};
    case kChunkedCallSite:
        return {"chunked_blocking_wrapper", true};
    case kAudioPageCallSite:
        return {"direct_audio_page_cache", true};
    default:
        return {"unknown", false};
    }
}

uint32_t expected_call_site_for_receipt(uint32_t receipt_pc) {
    switch (receipt_pc) {
    case kBlockingReceiptPc:
        return kBlockingCallSite;
    case kChunkedReceiptPc:
        return kChunkedCallSite;
    case kAudioDrainReceiptPc:
        return kAudioPageCallSite;
    default:
        return 0;
    }
}

bool is_rdram_virtual_address(uint32_t address) {
    const uint32_t segment = address & 0xE0000000u;
    return segment == 0x80000000u || segment == 0xA0000000u;
}

bool range_within(uint32_t start, uint32_t size, size_t limit) {
    return size != 0u && static_cast<size_t>(start) < limit &&
        static_cast<size_t>(size) <= limit - static_cast<size_t>(start);
}

uint32_t stack_u32(uint8_t* rdram, const recomp_context* context, int32_t offset) {
    return static_cast<uint32_t>(MEM_W(offset, context->r29));
}

uint64_t pending_transfer_count_locked() {
    uint64_t total = 0;
    for (const auto& [queue, transfers] : g_pending_by_queue) {
        static_cast<void>(queue);
        total += static_cast<uint64_t>(transfers.size());
    }
    return total;
}

void increment_transfer_kind_locked(uint32_t call_site) {
    switch (call_site) {
    case kBlockingCallSite:
        ++g_stats.blocking_transfers;
        break;
    case kChunkedCallSite:
        ++g_stats.chunked_transfers;
        break;
    case kAudioPageCallSite:
        ++g_stats.audio_page_transfers;
        break;
    default:
        ++g_stats.unknown_transfers;
        break;
    }
}

void increment_receipt_kind_locked(uint32_t expected_call_site) {
    switch (expected_call_site) {
    case kBlockingCallSite:
        ++g_stats.blocking_receipts;
        break;
    case kChunkedCallSite:
        ++g_stats.chunked_receipts;
        break;
    case kAudioPageCallSite:
        ++g_stats.audio_page_receipts;
        break;
    default:
        ++g_stats.unknown_receipts;
        break;
    }
}

bool is_power_of_two(uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

bool should_log_transfer_detail_locked(
    uint32_t call_site,
    bool anomalous
) {
    if (anomalous) {
        return true;
    }

    uint64_t kind_ordinal = 0;
    switch (call_site) {
    case kBlockingCallSite:
        kind_ordinal = g_stats.blocking_transfers;
        break;
    case kAudioPageCallSite:
        kind_ordinal = g_stats.audio_page_transfers;
        break;
    default:
        return true;
    }

    return kind_ordinal <= 8 || is_power_of_two(kind_ordinal);
}

void log_summary_locked(const char* stage) {
    g_stats.enabled = g_capture_enabled.load(std::memory_order_acquire);
    g_stats.pending_transfers = pending_transfer_count_locked();

    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=%s capture_enabled=%d"
        " transfers=%" PRIu64 " receipts=%" PRIu64
        " successful_starts=%" PRIu64 " start_failures=%" PRIu64
        " successful_receives=%" PRIu64 " receive_failures=%" PRIu64
        " associated_receipts=%" PRIu64 " unassociated_receipts=%" PRIu64
        " excluded_unadmitted_receipts=%" PRIu64
        " excluded_pre_admission_receipts=%" PRIu64
        " excluded_post_boundary_receipts=%" PRIu64
        " receipt_call_site_mismatches=%" PRIu64
        " verified_copies=%" PRIu64 " copy_mismatches=%" PRIu64
        " source_bounds_failures=%" PRIu64
        " destination_bounds_failures=%" PRIu64
        " bounds_failures=%" PRIu64 " alignment_failures=%" PRIu64
        " unknown_call_sites=%" PRIu64 " unknown_receipt_pcs=%" PRIu64
        " blocking_transfers=%" PRIu64 " chunked_transfers=%" PRIu64
        " audio_page_transfers=%" PRIu64 " unknown_transfers=%" PRIu64
        " blocking_receipts=%" PRIu64 " chunked_receipts=%" PRIu64
        " audio_page_receipts=%" PRIu64 " unknown_receipts=%" PRIu64
        " detailed_transfer_logs=%" PRIu64
        " detailed_receipt_logs=%" PRIu64
        " pending_transfers=%" PRIu64 " tick=%" PRIu64 "\n",
        stage,
        g_stats.enabled ? 1 : 0,
        g_stats.transfers,
        g_stats.receipts,
        g_stats.successful_starts,
        g_stats.start_failures,
        g_stats.successful_receives,
        g_stats.receive_failures,
        g_stats.associated_receipts,
        g_stats.unassociated_receipts,
        g_stats.excluded_unadmitted_receipts,
        g_stats.excluded_pre_admission_receipts,
        g_stats.excluded_post_boundary_receipts,
        g_stats.receipt_call_site_mismatches,
        g_stats.verified_copies,
        g_stats.copy_mismatches,
        g_stats.source_bounds_failures,
        g_stats.destination_bounds_failures,
        g_stats.bounds_failures,
        g_stats.alignment_failures,
        g_stats.unknown_call_sites,
        g_stats.unknown_receipt_pcs,
        g_stats.blocking_transfers,
        g_stats.chunked_transfers,
        g_stats.audio_page_transfers,
        g_stats.unknown_transfers,
        g_stats.blocking_receipts,
        g_stats.chunked_receipts,
        g_stats.audio_page_receipts,
        g_stats.unknown_receipts,
        g_stats.detailed_transfer_logs,
        g_stats.detailed_receipt_logs,
        g_stats.pending_transfers,
        bumble::native_pi_dma::replay_observer_tick()
    );
    std::fflush(stderr);
}

} // namespace

void bumble::native_pi_dma::set_replay_observer_tick(uint64_t tick) {
    g_observer_tick.store(tick, std::memory_order_release);
}

uint64_t bumble::native_pi_dma::replay_observer_tick() {
    return g_observer_tick.load(std::memory_order_acquire);
}

void bumble::native_pi_dma::set_capture_enabled(bool enabled) {
    const bool previous = g_capture_enabled.exchange(enabled, std::memory_order_acq_rel);
    const bool was_draining =
        g_capture_draining.exchange(false, std::memory_order_acq_rel);
    if (previous == enabled && !was_draining) {
        return;
    }

    std::lock_guard lock(g_state_mutex);
    if (enabled) {
        g_transfer_sequence = 0;
        g_receipt_sequence = 0;
        g_pending_by_queue.clear();
        g_stats = {};
    }
    log_summary_locked(enabled
        ? "native_pi_dma_capture_started"
        : "native_pi_dma_capture_stopped");
}

bool bumble::native_pi_dma::capture_enabled() {
    return g_capture_enabled.load(std::memory_order_acquire);
}

void bumble::native_pi_dma::begin_capture_drain() {
    if (!g_capture_enabled.load(std::memory_order_acquire)) {
        return;
    }

    g_capture_draining.store(true, std::memory_order_release);
    if (!g_capture_enabled.exchange(false, std::memory_order_acq_rel)) {
        g_capture_draining.store(false, std::memory_order_release);
        return;
    }

    std::lock_guard lock(g_state_mutex);
    log_summary_locked("native_pi_dma_capture_drain_started");
}

bool bumble::native_pi_dma::capture_draining() {
    return g_capture_draining.load(std::memory_order_acquire);
}

bumble::native_pi_dma::CaptureStats bumble::native_pi_dma::capture_stats() {
    std::lock_guard lock(g_state_mutex);
    CaptureStats snapshot = g_stats;
    snapshot.enabled = g_capture_enabled.load(std::memory_order_acquire);
    snapshot.pending_transfers = pending_transfer_count_locked();
    return snapshot;
}

uint64_t bumble::native_pi_dma::pending_transfer_count() {
    std::lock_guard lock(g_state_mutex);
    return pending_transfer_count_locked();
}

bool bumble::native_pi_dma::pending_transfers_drained() {
    return pending_transfer_count() == 0;
}

extern "C" void buck_native_pi_dma_post_probe(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t call_site
) {
    if (!bumble::native_pi_dma::capture_enabled() ||
        rdram == nullptr || context == nullptr) {
        return;
    }

    const uint32_t message = static_cast<uint32_t>(context->r4);
    const uint32_t priority = static_cast<uint32_t>(context->r5);
    const uint32_t direction = static_cast<uint32_t>(context->r6);
    const uint32_t source_argument = static_cast<uint32_t>(context->r7);
    const uint32_t destination = stack_u32(rdram, context, 0x10);
    const gpr destination_guest =
        static_cast<gpr>(static_cast<int32_t>(destination));
    const uint32_t requested_size = stack_u32(rdram, context, 0x14);
    const uint32_t queue = stack_u32(rdram, context, 0x18);
    const int32_t start_result = static_cast<int32_t>(context->r2);

    const uint32_t source_physical =
        (source_argument | recomp::rom_base) & 0x1FFFFFFFu;
    const bool source_is_rom = source_physical >= recomp::rom_base;
    const uint32_t source_rom = source_is_rom
        ? source_physical - recomp::rom_base
        : 0u;
    const uint32_t destination_physical = destination & 0x1FFFFFFFu;
    const std::span<const uint8_t> rom = recomp::get_rom();

    const bool source_bounds_valid = source_is_rom &&
        range_within(source_rom, requested_size, rom.size());
    const bool destination_bounds_valid = is_rdram_virtual_address(destination) &&
        range_within(destination_physical, requested_size, kRdramSize);
    const bool alignment_valid = (source_physical & 1u) == 0u &&
        (destination & 7u) == 0u;
    const bool bounds_valid = source_bounds_valid && destination_bounds_valid;

    const CallSiteIdentity identity = identify_call_site(call_site);
    const uint32_t wrapper_advance_size = call_site == kChunkedCallSite
        ? static_cast<uint32_t>(context->r16)
        : requested_size;
    const uint64_t tick = bumble::native_pi_dma::replay_observer_tick();
    const uint32_t frontend_phase =
        bumble::native_checkpoint::last_frontend_phase();

    // Lock against receipt hooks so queued completion cannot precede transfer registration.
    std::lock_guard lock(g_state_mutex);
    if (!g_capture_enabled.load(std::memory_order_acquire)) {
        return;
    }
    const uint64_t sequence = ++g_transfer_sequence;
    ++g_stats.transfers;
    increment_transfer_kind_locked(call_site);

    uint64_t source_hash = 0;
    uint64_t copied_hash = 0;
    bool copy_matches_source = false;
    std::vector<uint8_t> copied_bytes;
    if (bounds_valid) {
        copied_bytes.resize(requested_size);
        copy_matches_source = true;
        for (uint32_t offset = 0; offset < requested_size; ++offset) {
            const uint8_t copied = MEM_BU(offset, destination_guest);
            copied_bytes[offset] = copied;
            if (copied != rom[static_cast<size_t>(source_rom) + offset]) {
                copy_matches_source = false;
            }
        }
        source_hash = XXH3_64bits(
            rom.data() + source_rom,
            static_cast<size_t>(requested_size)
        );
        copied_hash = XXH3_64bits(copied_bytes.data(), copied_bytes.size());
        if (copy_matches_source) {
            ++g_stats.verified_copies;
        } else {
            ++g_stats.copy_mismatches;
        }
    }

    if (!identity.known) {
        ++g_stats.unknown_call_sites;
    }
    if (!source_bounds_valid) {
        ++g_stats.source_bounds_failures;
    }
    if (!destination_bounds_valid) {
        ++g_stats.destination_bounds_failures;
    }
    if (!bounds_valid) {
        ++g_stats.bounds_failures;
    }
    if (!alignment_valid) {
        ++g_stats.alignment_failures;
    }
    if (start_result == 0) {
        ++g_stats.successful_starts;
    } else {
        ++g_stats.start_failures;
    }

    const bool anomalous = !identity.known || !bounds_valid ||
        !alignment_valid || start_result != 0 ||
        (bounds_valid && !copy_matches_source);
    const bool detail_logged =
        should_log_transfer_detail_locked(call_site, anomalous);
    if (start_result == 0) {
        g_pending_by_queue[queue].push_back(
            {sequence, call_site, detail_logged}
        );
    }
    const auto pending_it = g_pending_by_queue.find(queue);
    const size_t pending_for_queue = pending_it == g_pending_by_queue.end()
        ? 0u
        : pending_it->second.size();

    if (detail_logged) {
        ++g_stats.detailed_transfer_logs;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=native_pi_dma_copy"
            " sequence=%" PRIu64 " call_site=0x%08" PRIX32 " kind=%s known_call_site=%d"
            " message=0x%08" PRIX32 " priority=%" PRIu32 " direction=%" PRIu32
            " source_argument=0x%08" PRIX32 " source_rom=0x%08" PRIX32
            " destination=0x%08" PRIX32 " destination_physical=0x%08" PRIX32
            " requested_size=0x%08" PRIX32 " programmed_size=0x%08" PRIX32
            " effective_size=0x%08" PRIX32 " wrapper_advance_size=0x%08" PRIX32
            " size_model=native_exact queue=0x%08" PRIX32 " start_result=%" PRId32
            " source_bounds_valid=%d destination_bounds_valid=%d alignment_valid=%d"
            " bounds_valid=%d source_xxh3_64=0x%016" PRIX64
            " copy_xxh3_64=0x%016" PRIX64 " copy_matches_source=%d"
            " pending_for_queue=%zu tick=%" PRIu64 " frontend_phase=0x%08" PRIX32 "\n",
            sequence,
            call_site,
            identity.kind,
            identity.known ? 1 : 0,
            message,
            priority,
            direction,
            source_argument,
            source_rom,
            destination,
            destination_physical,
            requested_size,
            requested_size,
            requested_size,
            wrapper_advance_size,
            queue,
            start_result,
            source_bounds_valid ? 1 : 0,
            destination_bounds_valid ? 1 : 0,
            alignment_valid ? 1 : 0,
            bounds_valid ? 1 : 0,
            source_hash,
            copied_hash,
            copy_matches_source ? 1 : 0,
            pending_for_queue,
            tick,
            frontend_phase
        );
        std::fflush(stderr);
    }
}

extern "C" void buck_native_pi_dma_receipt_probe(
    uint8_t*,
    recomp_context* context,
    uint32_t receipt_pc
) {
    if ((!bumble::native_pi_dma::capture_enabled() &&
         !bumble::native_pi_dma::capture_draining()) ||
        context == nullptr) {
        return;
    }

    // osRecvMesg_recomp preserves a0 and returns its status in v0.
    const uint32_t queue = static_cast<uint32_t>(context->r4);
    const int32_t receive_result = static_cast<int32_t>(context->r2);
    const uint32_t expected_call_site = expected_call_site_for_receipt(receipt_pc);
    const CallSiteIdentity receipt_identity = identify_call_site(expected_call_site);
    const uint64_t tick = bumble::native_pi_dma::replay_observer_tick();
    const uint32_t frontend_phase =
        bumble::native_checkpoint::last_frontend_phase();

    std::lock_guard lock(g_state_mutex);
    if (!g_capture_enabled.load(std::memory_order_acquire) &&
        !g_capture_draining.load(std::memory_order_acquire)) {
        return;
    }
    auto queue_it = g_pending_by_queue.find(queue);
    if (queue_it == g_pending_by_queue.end() || queue_it->second.empty()) {
        const bool after_closed_boundary =
            g_capture_draining.load(std::memory_order_acquire);
        ++g_stats.excluded_unadmitted_receipts;
        if (after_closed_boundary) {
            ++g_stats.excluded_post_boundary_receipts;
        } else {
            ++g_stats.excluded_pre_admission_receipts;
        }
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=native_pi_dma_receipt_excluded"
            " receipt_pc=0x%08" PRIX32 " receipt_kind=%s"
            " queue=0x%08" PRIX32 " receive_result=%" PRId32
            " reason=no_admitted_transfer capture_draining=%d"
            " tick=%" PRIu64 " frontend_phase=0x%08" PRIX32 "\n",
            receipt_pc,
            receipt_identity.kind,
            queue,
            receive_result,
            after_closed_boundary ? 1 : 0,
            tick,
            frontend_phase
        );
        std::fflush(stderr);
        return;
    }
    const uint64_t receipt_sequence = ++g_receipt_sequence;
    ++g_stats.receipts;
    increment_receipt_kind_locked(expected_call_site);
    if (expected_call_site == 0) {
        ++g_stats.unknown_receipt_pcs;
    }
    if (receive_result == 0) {
        ++g_stats.successful_receives;
    } else {
        ++g_stats.receive_failures;
    }
    PendingTransfer pending{};
    bool pending_found = false;
    if (receive_result == 0 && queue_it != g_pending_by_queue.end() &&
        !queue_it->second.empty()) {
        pending = queue_it->second.front();
        queue_it->second.pop_front();
        pending_found = true;
    }
    const size_t pending_after = queue_it == g_pending_by_queue.end()
        ? 0u
        : queue_it->second.size();
    if (queue_it != g_pending_by_queue.end() && queue_it->second.empty()) {
        g_pending_by_queue.erase(queue_it);
    }
    const CallSiteIdentity pending_identity = identify_call_site(pending.call_site);
    if (pending_found) {
        ++g_stats.associated_receipts;
    } else {
        ++g_stats.unassociated_receipts;
    }
    const bool call_site_matches = pending_found &&
        pending.call_site == expected_call_site;
    if (pending_found && !call_site_matches) {
        ++g_stats.receipt_call_site_mismatches;
    }
    const bool detail_logged = pending.detail_logged || receive_result != 0 ||
        !pending_found || !call_site_matches;

    if (detail_logged) {
        ++g_stats.detailed_receipt_logs;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=native_pi_dma_guest_receipt"
            " receipt_sequence=%" PRIu64 " sequence=%" PRIu64
            " receipt_pc=0x%08" PRIX32 " receipt_kind=%s"
            " call_site=0x%08" PRIX32 " kind=%s queue=0x%08" PRIX32
            " receive_result=%" PRId32 " pending_found=%d call_site_matches_receipt=%d"
            " association=queue_fifo pending_after=%zu tick=%" PRIu64
            " frontend_phase=0x%08" PRIX32 "\n",
            receipt_sequence,
            pending.sequence,
            receipt_pc,
            receipt_identity.kind,
            pending.call_site,
            pending_identity.kind,
            queue,
            receive_result,
            pending_found ? 1 : 0,
            call_site_matches ? 1 : 0,
            pending_after,
            tick,
            frontend_phase
        );
        std::fflush(stderr);
    }

    const uint64_t receipt_kind_ordinal =
        expected_call_site == kBlockingCallSite
            ? g_stats.blocking_receipts
            : expected_call_site == kAudioPageCallSite
                ? g_stats.audio_page_receipts
                : 0;
    if (receipt_kind_ordinal > 8 &&
        is_power_of_two(receipt_kind_ordinal)) {
        log_summary_locked("native_pi_dma_progress");
    }
}
