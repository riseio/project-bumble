#pragma once

#include <cstdint>

#include "recomp.h"

namespace bumble::native_pi_dma {

struct CaptureStats {
    bool enabled = false;

    uint64_t transfers = 0;
    uint64_t receipts = 0;
    uint64_t successful_starts = 0;
    uint64_t start_failures = 0;
    uint64_t successful_receives = 0;
    uint64_t receive_failures = 0;
    uint64_t associated_receipts = 0;
    uint64_t unassociated_receipts = 0;
    uint64_t excluded_unadmitted_receipts = 0;
    uint64_t excluded_pre_admission_receipts = 0;
    uint64_t excluded_post_boundary_receipts = 0;
    uint64_t receipt_call_site_mismatches = 0;

    uint64_t verified_copies = 0;
    uint64_t copy_mismatches = 0;
    uint64_t source_bounds_failures = 0;
    uint64_t destination_bounds_failures = 0;
    uint64_t bounds_failures = 0;
    uint64_t alignment_failures = 0;
    uint64_t unknown_call_sites = 0;
    uint64_t unknown_receipt_pcs = 0;

    uint64_t blocking_transfers = 0;
    uint64_t chunked_transfers = 0;
    uint64_t audio_page_transfers = 0;
    uint64_t unknown_transfers = 0;
    uint64_t blocking_receipts = 0;
    uint64_t chunked_receipts = 0;
    uint64_t audio_page_receipts = 0;
    uint64_t unknown_receipts = 0;

    uint64_t detailed_transfer_logs = 0;
    uint64_t detailed_receipt_logs = 0;
    uint64_t pending_transfers = 0;
};

void set_replay_observer_tick(uint64_t tick);
uint64_t replay_observer_tick();
void set_capture_enabled(bool enabled);
bool capture_enabled();

void begin_capture_drain();
bool capture_draining();

CaptureStats capture_stats();
uint64_t pending_transfer_count();
bool pending_transfers_drained();

} // namespace bumble::native_pi_dma

extern "C" void buck_native_pi_dma_post_probe(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t call_site
);

extern "C" void buck_native_pi_dma_receipt_probe(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t receipt_pc
);
