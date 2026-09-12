#pragma once

#include <cstdint>

#include "librecomp/rsp.hpp"
#include "recomp.h"

namespace bumble::native_rsp_task {

struct CaptureStats {
    bool capture_started = false;
    bool contract_passed = false;
    uint64_t audio_producers = 0;
    uint64_t audio_scheduler_loads = 0;
    uint64_t audio_scheduler_starts = 0;
    uint64_t audio_execution_begins = 0;
    uint64_t audio_execution_completes = 0;
    uint64_t graphics_producers = 0;
    uint64_t graphics_scheduler_loads = 0;
    uint64_t graphics_scheduler_starts = 0;
    uint64_t graphics_execution_begins = 0;
    uint64_t graphics_execution_completes = 0;
    uint64_t fully_correlated_audio = 0;
    uint64_t fully_correlated_graphics = 0;
    uint64_t sampled_audio = 0;
    uint64_t sampled_graphics = 0;
    uint64_t snapshot_mismatches = 0;
    uint64_t unexpected_exits = 0;
    uint64_t pending_sampled_lifecycles = 0;
};

RspUcodeFunc* select_audio_microcode(const OSTask* task);
void graphics_execution_begin(const OSTask* task);
void graphics_execution_complete(bool processed);

CaptureStats capture_stats();
void begin_capture_drain();
CaptureStats finalize_capture();

} // namespace bumble::native_rsp_task

extern "C" void buck_native_rsp_task_probe(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t anchor_pc
);
