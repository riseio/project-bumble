#include "native_rsp_task_bridge.hpp"

#include <atomic>
#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <stdexcept>

#include "common/rt64_performance_profiler.h"
#include "native_campaign_level_observer.hpp"
#include "native_checkpoint_bridge.hpp"
#include "native_pi_dma_bridge.hpp"
#include "xxHash/xxh3.h"
#include "librecomp/addresses.hpp"
#include "ultramodern/ultramodern.hpp"

extern RspUcodeFunc n_aspMain;

namespace {

constexpr uint32_t kAudioProducerPc = 0x80051A58u;
constexpr uint32_t kGraphicsProducerPc = 0x80054E84u;
constexpr uint32_t kSchedulerLoadPc = 0x80028F2Cu;
constexpr uint32_t kSchedulerStartPc = 0x800290BCu;
constexpr uint32_t kAudioProducerHookPc = 0x80051A58u;
constexpr uint32_t kGraphicsProducerHookPc = 0x80054E84u;
constexpr uint32_t kSchedulerLoadCallerPc = 0x80027944u;
constexpr uint32_t kSchedulerStartCallerPc = 0x8002794Cu;
constexpr uint64_t kPeriodicSampleInterval = 120u;
constexpr uint64_t kMaximumSamplesPerType = 64u;

struct ParseWaiter {
    int32_t queue = 0;
    bool waiting = false;
};
std::mutex g_parse_mutex;
std::array<ParseWaiter, 2> g_parse_waiters{};
uint8_t* g_parse_rdram = nullptr;
uint64_t g_parse_produced = 0;
uint64_t g_parse_completed = 0;
std::deque<std::pair<uint32_t, uint64_t>> g_pending_parses;
thread_local uint64_t g_active_parse = 0;

void initialize_parse_waiters(uint8_t* rdram) {
    if (g_parse_rdram == rdram) return;
    if (g_parse_rdram != nullptr) throw std::runtime_error("Graphics input runtime changed");
    constexpr size_t Stride = sizeof(OSMesgQueue) + sizeof(OSMesg);
    auto* memory = static_cast<uint8_t*>(recomp::alloc(rdram, Stride * g_parse_waiters.size()));
    if (!memory) throw std::runtime_error("Graphics input queue allocation failed");
    const ptrdiff_t offset = memory - rdram;
    if (offset < 0 || static_cast<size_t>(offset) + Stride * g_parse_waiters.size() > recomp::mem_size) {
        recomp::free(rdram, memory);
        throw std::runtime_error("Graphics input queue outside RDRAM");
    }
    for (size_t i = 0; i < g_parse_waiters.size(); ++i) {
        const int32_t queue = static_cast<int32_t>(0x80000000u + offset + i * Stride);
        osCreateMesgQueue(rdram, queue, queue + sizeof(OSMesgQueue), 1);
        g_parse_waiters[i].queue = queue;
    }
    // Queue storage belongs to RDRAM until all guest and renderer threads have joined.
    g_parse_rdram = rdram;
}

void register_graphics_input(uint8_t* rdram, uint32_t task) {
    std::lock_guard lock(g_parse_mutex);
    initialize_parse_waiters(rdram);
    const uint32_t displayList = static_cast<uint32_t>(MEM_W(0x30, static_cast<int32_t>(task)));
    g_pending_parses.emplace_back(displayList & 0x03FFFFFFu, ++g_parse_produced);
}

struct TaskSnapshot {
    std::array<uint32_t, 16> words{};
    uint64_t xxh3_64 = 0;
};

struct ProducerObservation {
    uint64_t producer_sequence = 0;
    uint64_t tick = 0;
    uint32_t frontend_phase = 0;
    uint32_t producer_pc = 0;
    uint32_t hook_pc = 0;
    uint32_t wrapper = 0;
    uint32_t task_pointer = 0;
    TaskSnapshot snapshot{};
};

struct Lifecycle {
    uint64_t sequence = 0;
    uint64_t type_ordinal = 0;
    uint32_t wrapper = 0;
    uint32_t task_pointer = 0;
    TaskSnapshot snapshot{};
    bool sampled = false;
    bool scheduler_started = false;
    bool producer_observed = false;
    bool scheduler_load_observed = false;
    bool scheduler_start_observed = false;
    bool execution_begin_observed = false;
    bool execution_complete_observed = false;
    bool snapshot_mismatch = false;
    bool unexpected_exit = false;
    const char* sample_reason = "not_sampled";
    std::shared_ptr<ProducerObservation> producer;
};

struct GraphicsProfileStamp {
    uint64_t sequence = 0;
    uint64_t snapshot_hash = 0;
    uint64_t producer_ns = 0;
    uint64_t scheduler_start_ns = 0;
    uint32_t frontend_phase = 0;
};

std::mutex g_state_mutex;
bool g_capture_was_enabled = false;
bool g_capture_started = false;
bool g_capture_finalized = false;
bool g_capture_draining = false;
uint64_t g_lifecycle_sequence = 0;
uint64_t g_producer_sequence = 0;
std::array<uint64_t, 3> g_type_ordinals{};
std::array<uint64_t, 3> g_sample_counts{};
std::array<uint32_t, 3> g_observed_checkpoint_masks{};
std::array<uint32_t, 3> g_last_observed_phases{
    UINT32_MAX,
    UINT32_MAX,
    UINT32_MAX,
};
std::array<uint64_t, 3> g_producer_counts{};
std::array<uint64_t, 3> g_scheduler_load_counts{};
std::array<uint64_t, 3> g_scheduler_start_counts{};
std::array<uint64_t, 3> g_execution_begin_counts{};
std::array<uint64_t, 3> g_execution_complete_counts{};
uint64_t g_snapshot_mismatches = 0;
uint64_t g_unexpected_exits = 0;

std::unordered_map<uint32_t, std::deque<std::shared_ptr<ProducerObservation>>>
    g_producers_by_task;
std::unordered_map<uint32_t, std::deque<std::shared_ptr<Lifecycle>>>
    g_loaded_by_task;
std::unordered_map<uint32_t, std::deque<std::shared_ptr<Lifecycle>>>
    g_waiting_for_producer_by_task;
std::array<std::deque<std::shared_ptr<Lifecycle>>, 3> g_pending_execution;
std::unordered_map<uint64_t, std::shared_ptr<Lifecycle>> g_all_lifecycles;

thread_local std::shared_ptr<Lifecycle> g_audio_execution;
thread_local std::shared_ptr<Lifecycle> g_graphics_execution;

std::mutex g_graphics_profile_mutex;
std::deque<GraphicsProfileStamp> g_graphics_profile_pending;
std::atomic_uint64_t g_graphics_profile_sequence{0};
std::atomic_uint64_t g_graphics_profile_previous_producer_ns{0};
std::atomic_uint64_t g_graphics_profile_previous_completion_ns{0};

const char* task_type_name(uint32_t type) {
    switch (type) {
    case M_GFXTASK:
        return "graphics";
    case M_AUDTASK:
        return "audio";
    default:
        return "unsupported";
    }
}

uint32_t checkpoint_mask() {
    uint32_t mask = 0;
    mask |= bumble::native_checkpoint::level_select_cheat_complete() ? 1u << 0 : 0u;
    mask |= bumble::native_checkpoint::mission2_selector_initialized() ? 1u << 1 : 0u;
    mask |= bumble::native_checkpoint::mission2_selected() ? 1u << 2 : 0u;
    mask |= bumble::native_checkpoint::mission2_selection_committed() ? 1u << 3 : 0u;
    mask |= bumble::native_checkpoint::mission2_player_observed() ? 1u << 4 : 0u;
    return mask;
}

uint64_t snapshot_hash(const std::array<uint32_t, 16>& words) {
    std::array<uint8_t, 64> canonical_bytes{};
    for (size_t index = 0; index < words.size(); ++index) {
        const uint32_t word = words[index];
        canonical_bytes[index * 4 + 0] = static_cast<uint8_t>(word >> 24);
        canonical_bytes[index * 4 + 1] = static_cast<uint8_t>(word >> 16);
        canonical_bytes[index * 4 + 2] = static_cast<uint8_t>(word >> 8);
        canonical_bytes[index * 4 + 3] = static_cast<uint8_t>(word);
    }
    return XXH3_64bits(canonical_bytes.data(), canonical_bytes.size());
}

TaskSnapshot guest_snapshot(uint8_t* rdram, uint32_t task_pointer) {
    TaskSnapshot snapshot{};
    const gpr guest = static_cast<gpr>(static_cast<int32_t>(task_pointer));
    for (size_t index = 0; index < snapshot.words.size(); ++index) {
        snapshot.words[index] = static_cast<uint32_t>(
            MEM_W(static_cast<int32_t>(index * 4), guest)
        );
    }
    snapshot.xxh3_64 = snapshot_hash(snapshot.words);
    return snapshot;
}

TaskSnapshot host_snapshot(const OSTask* task) {
    TaskSnapshot snapshot{};
    if (task == nullptr) {
        return snapshot;
    }
    snapshot.words = {
        task->t.type,
        task->t.flags,
        static_cast<uint32_t>(task->t.ucode_boot),
        task->t.ucode_boot_size,
        static_cast<uint32_t>(task->t.ucode),
        task->t.ucode_size,
        static_cast<uint32_t>(task->t.ucode_data),
        task->t.ucode_data_size,
        static_cast<uint32_t>(task->t.dram_stack),
        task->t.dram_stack_size,
        static_cast<uint32_t>(task->t.output_buff),
        static_cast<uint32_t>(task->t.output_buff_size),
        static_cast<uint32_t>(task->t.data_ptr),
        task->t.data_size,
        static_cast<uint32_t>(task->t.yield_data_ptr),
        task->t.yield_data_size,
    };
    snapshot.xxh3_64 = snapshot_hash(snapshot.words);
    return snapshot;
}

void profile_graphics_producer(const TaskSnapshot& snapshot) {
    if (!RT64::PerformanceProfiler::captureEnabled() ||
        snapshot.words[0] != M_GFXTASK) {
        return;
    }

    const uint64_t now = RT64::PerformanceProfiler::nowNanoseconds();
    const uint64_t sequence =
        g_graphics_profile_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint32_t phase =
        bumble::native_checkpoint::last_frontend_phase();
    const uint64_t previous_producer =
        g_graphics_profile_previous_producer_ns.exchange(
            now,
            std::memory_order_acq_rel
        );
    if (previous_producer != 0 && previous_producer < now) {
        RT64::PerformanceProfiler::recordDuration(
            RT64::PerformanceCategory::Wait,
            "FrameProduction.GuestGraphicsTaskInterval",
            previous_producer,
            now,
            sequence,
            phase,
            snapshot.words[12]
        );
    }

    const uint64_t previous_completion =
        g_graphics_profile_previous_completion_ns.exchange(
            0,
            std::memory_order_acq_rel
        );
    if (previous_completion != 0 && previous_completion < now) {
        RT64::PerformanceProfiler::recordDuration(
            RT64::PerformanceCategory::Wait,
            "FrameProduction.GuestGraphicsWaitAfterCompletion",
            previous_completion,
            now,
            sequence,
            phase,
            snapshot.words[12]
        );
    }

    const std::scoped_lock lock(g_graphics_profile_mutex);
    g_graphics_profile_pending.push_back({
        sequence,
        snapshot.xxh3_64,
        now,
        0,
        phase,
    });
    while (g_graphics_profile_pending.size() > 128) {
        g_graphics_profile_pending.pop_front();
    }
}

void profile_graphics_scheduler_start(const TaskSnapshot& snapshot) {
    if (!RT64::PerformanceProfiler::captureEnabled() ||
        snapshot.words[0] != M_GFXTASK) {
        return;
    }

    const uint64_t now = RT64::PerformanceProfiler::nowNanoseconds();
    const std::scoped_lock lock(g_graphics_profile_mutex);
    for (GraphicsProfileStamp& stamp : g_graphics_profile_pending) {
        if (stamp.scheduler_start_ns == 0 &&
            stamp.snapshot_hash == snapshot.xxh3_64) {
            stamp.scheduler_start_ns = now;
            RT64::PerformanceProfiler::recordDuration(
                RT64::PerformanceCategory::Wait,
                "FrameProduction.GuestGraphicsSchedulerDelay",
                stamp.producer_ns,
                now,
                stamp.sequence,
                stamp.frontend_phase,
                snapshot.words[12]
            );
            return;
        }
    }
}

void profile_graphics_execution_begin(const TaskSnapshot& snapshot) {
    if (!RT64::PerformanceProfiler::captureEnabled() ||
        snapshot.words[0] != M_GFXTASK) {
        return;
    }

    const uint64_t now = RT64::PerformanceProfiler::nowNanoseconds();
    GraphicsProfileStamp stamp{};
    bool found = false;
    {
        const std::scoped_lock lock(g_graphics_profile_mutex);
        for (auto it = g_graphics_profile_pending.begin();
             it != g_graphics_profile_pending.end(); ++it) {
            if (it->snapshot_hash == snapshot.xxh3_64) {
                stamp = *it;
                g_graphics_profile_pending.erase(it);
                found = true;
                break;
            }
        }
    }
    if (!found || stamp.producer_ns >= now) {
        return;
    }

    RT64::PerformanceProfiler::recordDuration(
        RT64::PerformanceCategory::Wait,
        "FrameProduction.GraphicsTaskDispatchDelay",
        stamp.producer_ns,
        now,
        stamp.sequence,
        stamp.frontend_phase,
        snapshot.words[12]
    );
    if (stamp.scheduler_start_ns != 0 &&
        stamp.scheduler_start_ns < now) {
        RT64::PerformanceProfiler::recordDuration(
            RT64::PerformanceCategory::Wait,
            "FrameProduction.GraphicsActionQueueDelay",
            stamp.scheduler_start_ns,
            now,
            stamp.sequence,
            stamp.frontend_phase,
            snapshot.words[12]
        );
    }
}

void profile_graphics_execution_complete() {
    if (RT64::PerformanceProfiler::captureEnabled()) {
        g_graphics_profile_previous_completion_ns.store(
            RT64::PerformanceProfiler::nowNanoseconds(),
            std::memory_order_release
        );
    }
}

void clear_capture_state_locked() {
    g_capture_started = true;
    g_capture_finalized = false;
    g_capture_draining = false;
    g_lifecycle_sequence = 0;
    g_producer_sequence = 0;
    g_type_ordinals.fill(0);
    g_sample_counts.fill(0);
    g_observed_checkpoint_masks.fill(0);
    g_last_observed_phases.fill(UINT32_MAX);
    g_producer_counts.fill(0);
    g_scheduler_load_counts.fill(0);
    g_scheduler_start_counts.fill(0);
    g_execution_begin_counts.fill(0);
    g_execution_complete_counts.fill(0);
    g_snapshot_mismatches = 0;
    g_unexpected_exits = 0;
    g_producers_by_task.clear();
    g_loaded_by_task.clear();
    g_waiting_for_producer_by_task.clear();
    for (auto& queue : g_pending_execution) {
        queue.clear();
    }
    g_all_lifecycles.clear();
}

bool synchronize_capture_locked() {
    const bool enabled = bumble::native_pi_dma::capture_enabled();
    if (enabled && !g_capture_was_enabled) {
        clear_capture_state_locked();
        g_capture_was_enabled = true;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=native_rsp_task_capture_started"
            " policy=first4_checkpoint_phase_periodic120 cap_per_type=%" PRIu64
            " tick=%" PRIu64 "\n",
            kMaximumSamplesPerType,
            bumble::native_pi_dma::replay_observer_tick()
        );
        std::fflush(stderr);
    } else if (!enabled && !g_capture_draining) {
        g_capture_was_enabled = false;
    }
    return enabled || g_capture_draining;
}

const char* choose_sample_reason_locked(
    uint32_t type,
    uint64_t ordinal,
    uint32_t phase,
    uint32_t checkpoints
) {
    if (type > M_AUDTASK || g_sample_counts[type] >= kMaximumSamplesPerType) {
        return nullptr;
    }

    const uint32_t new_checkpoints =
        checkpoints & ~g_observed_checkpoint_masks[type];
    const bool phase_changed = phase != g_last_observed_phases[type];
    g_observed_checkpoint_masks[type] |= checkpoints;
    g_last_observed_phases[type] = phase;

    const char* reason = nullptr;
    if (ordinal <= 4u) {
        reason = "initial";
    } else if (new_checkpoints != 0u) {
        reason = "checkpoint_transition";
    } else if (phase_changed) {
        reason = "frontend_phase_transition";
    } else if ((ordinal % kPeriodicSampleInterval) == 0u) {
        reason = "periodic120";
    }

    if (reason != nullptr) {
        ++g_sample_counts[type];
    }
    return reason;
}

void emit_lifecycle(
    const Lifecycle& lifecycle,
    const char* lifecycle_stage,
    uint32_t anchor_pc,
    uint32_t hook_or_caller_pc,
    const char* native_semantics,
    const char* execution_owner,
    const char* outcome,
    uint64_t observed_tick,
    uint32_t observed_phase,
    bool snapshot_matches_scheduler
) {
    const auto& w = lifecycle.snapshot.words;
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=native_rsp_task_lifecycle"
        " sequence=%" PRIu64 " type_ordinal=%" PRIu64
        " lifecycle_stage=%s sample_reason=%s"
        " task_type=%" PRIu32 " task_type_name=%s"
        " anchor_pc=0x%08" PRIX32 " hook_or_caller_pc=0x%08" PRIX32
        " wrapper=0x%08" PRIX32 " task_pointer=0x%08" PRIX32
        " ostask_xxh3_64=0x%016" PRIX64 " snapshot_matches_scheduler=%d"
        " type=0x%08" PRIX32 " flags=0x%08" PRIX32
        " ucode_boot=0x%08" PRIX32 " ucode_boot_size=0x%08" PRIX32
        " ucode=0x%08" PRIX32 " ucode_size=0x%08" PRIX32
        " ucode_data=0x%08" PRIX32 " ucode_data_size=0x%08" PRIX32
        " dram_stack=0x%08" PRIX32 " dram_stack_size=0x%08" PRIX32
        " output_buff=0x%08" PRIX32 " output_buff_size=0x%08" PRIX32
        " data_ptr=0x%08" PRIX32 " data_size=0x%08" PRIX32
        " yield_data_ptr=0x%08" PRIX32 " yield_data_size=0x%08" PRIX32
        " native_semantics=%s execution_owner=%s outcome=%s"
        " tick=%" PRIu64 " frontend_phase=0x%08" PRIX32
        " checkpoint_mask=0x%02" PRIX32 "\n",
        lifecycle.sequence,
        lifecycle.type_ordinal,
        lifecycle_stage,
        lifecycle.sample_reason,
        w[0],
        task_type_name(w[0]),
        anchor_pc,
        hook_or_caller_pc,
        lifecycle.wrapper,
        lifecycle.task_pointer,
        lifecycle.snapshot.xxh3_64,
        snapshot_matches_scheduler ? 1 : 0,
        w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7],
        w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15],
        native_semantics,
        execution_owner,
        outcome,
        observed_tick,
        observed_phase,
        checkpoint_mask()
    );
    std::fflush(stderr);
}

std::shared_ptr<ProducerObservation> take_matching_producer_locked(
    uint32_t task_pointer,
    uint64_t snapshot_hash_value
) {
    auto map_it = g_producers_by_task.find(task_pointer);
    if (map_it == g_producers_by_task.end()) {
        return {};
    }
    auto& queue = map_it->second;
    for (auto it = queue.begin(); it != queue.end(); ++it) {
        if ((*it)->snapshot.xxh3_64 == snapshot_hash_value) {
            auto producer = *it;
            queue.erase(it);
            if (queue.empty()) {
                g_producers_by_task.erase(map_it);
            }
            return producer;
        }
    }
    return {};
}

std::shared_ptr<Lifecycle> take_waiting_lifecycle_locked(
    uint32_t task_pointer,
    uint64_t snapshot_hash_value
) {
    auto map_it = g_waiting_for_producer_by_task.find(task_pointer);
    if (map_it == g_waiting_for_producer_by_task.end()) {
        return {};
    }
    auto& queue = map_it->second;
    for (auto it = queue.begin(); it != queue.end(); ++it) {
        if ((*it)->snapshot.xxh3_64 == snapshot_hash_value) {
            auto lifecycle = *it;
            queue.erase(it);
            if (queue.empty()) {
                g_waiting_for_producer_by_task.erase(map_it);
            }
            return lifecycle;
        }
    }
    return {};
}

void emit_producer_locked(
    const Lifecycle& lifecycle,
    const ProducerObservation& producer
) {
    if (!lifecycle.sampled) {
        return;
    }
    const bool matches =
        producer.snapshot.xxh3_64 == lifecycle.snapshot.xxh3_64;
    emit_lifecycle(
        lifecycle,
        "producer_submit",
        producer.producer_pc,
        producer.hook_pc,
        "guest_osSendMesg_pre_call",
        "guest_scheduler_queue",
        "about_to_submit",
        producer.tick,
        producer.frontend_phase,
        matches
    );
}

void observe_producer_locked(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t producer_pc
) {
    const uint32_t message_pointer = static_cast<uint32_t>(context->r5);
    const uint32_t wrapper = message_pointer;
    const uint32_t task_pointer = message_pointer + 0x10u;
    auto producer = std::make_shared<ProducerObservation>();
    producer->producer_sequence = ++g_producer_sequence;
    producer->tick = bumble::native_pi_dma::replay_observer_tick();
    producer->frontend_phase = bumble::native_checkpoint::last_frontend_phase();
    producer->producer_pc = producer_pc;
    producer->hook_pc = producer_pc == kAudioProducerPc
        ? kAudioProducerHookPc
        : kGraphicsProducerHookPc;
    producer->wrapper = wrapper;
    producer->task_pointer = task_pointer;
    producer->snapshot = guest_snapshot(rdram, task_pointer);
    const uint32_t type = producer->snapshot.words[0];
    if (type == M_GFXTASK || type == M_AUDTASK) {
        ++g_producer_counts[type];
    }

    auto waiting = take_waiting_lifecycle_locked(
        task_pointer,
        producer->snapshot.xxh3_64
    );
    if (waiting != nullptr) {
        waiting->producer = producer;
        waiting->producer_observed = true;
        waiting->wrapper = wrapper;
        emit_producer_locked(*waiting, *producer);
        return;
    }
    g_producers_by_task[task_pointer].push_back(std::move(producer));
}

void observe_scheduler_load_locked(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t task_pointer = static_cast<uint32_t>(context->r18);
    TaskSnapshot snapshot = guest_snapshot(rdram, task_pointer);
    const uint32_t type = snapshot.words[0];
    if (type != M_GFXTASK && type != M_AUDTASK) {
        return;
    }

    auto producer = take_matching_producer_locked(
        task_pointer,
        snapshot.xxh3_64
    );
    if (producer == nullptr) {
        return;
    }

    auto lifecycle = std::make_shared<Lifecycle>();
    lifecycle->sequence = ++g_lifecycle_sequence;
    lifecycle->type_ordinal = ++g_type_ordinals[type];
    lifecycle->task_pointer = task_pointer;
    lifecycle->wrapper = task_pointer - 0x10u;
    lifecycle->snapshot = snapshot;
    lifecycle->scheduler_load_observed = true;
    lifecycle->producer = std::move(producer);
    const uint32_t phase = bumble::native_checkpoint::last_frontend_phase();
    const char* reason = choose_sample_reason_locked(
        type,
        lifecycle->type_ordinal,
        phase,
        checkpoint_mask()
    );
    lifecycle->sampled = reason != nullptr;
    lifecycle->sample_reason = reason == nullptr ? "not_sampled" : reason;
    lifecycle->producer_observed = true;
    lifecycle->wrapper = lifecycle->producer->wrapper;
    emit_producer_locked(*lifecycle, *lifecycle->producer);
    ++g_scheduler_load_counts[type];
    g_all_lifecycles[lifecycle->sequence] = lifecycle;
    if (lifecycle->sampled) {
        emit_lifecycle(
            *lifecycle,
            "scheduler_load",
            kSchedulerLoadPc,
            kSchedulerLoadCallerPc,
            "native_osSpTaskLoad_noop_call",
            "pinned_librecomp",
            "about_to_call_noop",
            bumble::native_pi_dma::replay_observer_tick(),
            phase,
            true
        );
    }
    g_loaded_by_task[task_pointer].push_back(std::move(lifecycle));
}

std::shared_ptr<Lifecycle> take_loaded_lifecycle_locked(
    uint32_t task_pointer,
    uint64_t snapshot_hash_value
) {
    auto map_it = g_loaded_by_task.find(task_pointer);
    if (map_it == g_loaded_by_task.end()) {
        return {};
    }
    auto& queue = map_it->second;
    for (auto it = queue.begin(); it != queue.end(); ++it) {
        if (!(*it)->scheduler_started &&
            (*it)->snapshot.xxh3_64 == snapshot_hash_value) {
            auto lifecycle = *it;
            lifecycle->scheduler_started = true;
            queue.erase(it);
            if (queue.empty()) {
                g_loaded_by_task.erase(map_it);
            }
            return lifecycle;
        }
    }
    return {};
}

void observe_scheduler_start_locked(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t task_pointer = static_cast<uint32_t>(context->r18);
    const TaskSnapshot snapshot = guest_snapshot(rdram, task_pointer);
    const uint32_t type = snapshot.words[0];
    if (type != M_GFXTASK && type != M_AUDTASK) {
        return;
    }

    auto lifecycle = take_loaded_lifecycle_locked(
        task_pointer,
        snapshot.xxh3_64
    );
    if (lifecycle == nullptr) {
        return;
    }

    lifecycle->scheduler_start_observed = true;
    ++g_scheduler_start_counts[type];

    if (lifecycle->sampled) {
        emit_lifecycle(
            *lifecycle,
            "scheduler_start",
            kSchedulerStartPc,
            kSchedulerStartCallerPc,
            "native_osSpTaskStartGo_submit_call",
            type == M_GFXTASK ? "ultramodern_graphics_action_queue" : "ultramodern_sp_task_queue",
            "about_to_enqueue",
            bumble::native_pi_dma::replay_observer_tick(),
            bumble::native_checkpoint::last_frontend_phase(),
            snapshot.xxh3_64 == lifecycle->snapshot.xxh3_64
        );
    }
    g_pending_execution[type].push_back(std::move(lifecycle));
}

std::shared_ptr<Lifecycle> take_pending_execution_locked(
    const TaskSnapshot& snapshot
) {
    const uint32_t type = snapshot.words[0];
    if (type > M_AUDTASK) {
        return {};
    }
    auto& queue = g_pending_execution[type];
    for (auto it = queue.begin(); it != queue.end(); ++it) {
        if ((*it)->snapshot.xxh3_64 == snapshot.xxh3_64) {
            auto lifecycle = *it;
            queue.erase(it);
            return lifecycle;
        }
    }
    return {};
}

std::shared_ptr<Lifecycle> execution_begin_locked(
    const OSTask* task,
    const char* owner
) {
    const TaskSnapshot snapshot = host_snapshot(task);
    const uint32_t type = snapshot.words[0];
    auto lifecycle = take_pending_execution_locked(snapshot);
    if (lifecycle != nullptr) {
        lifecycle->execution_begin_observed = true;
        if (type == M_GFXTASK || type == M_AUDTASK) {
            ++g_execution_begin_counts[type];
        }
    }
    if (lifecycle != nullptr && lifecycle->sampled) {
        emit_lifecycle(
            *lifecycle,
            "execution_begin",
            0u,
            0u,
            type == M_GFXTASK ? "rt64_processDisplayLists" : "generated_n_aspMain_dispatch",
            owner,
            "entered",
            bumble::native_pi_dma::replay_observer_tick(),
            bumble::native_checkpoint::last_frontend_phase(),
            snapshot.xxh3_64 == lifecycle->snapshot.xxh3_64
        );
    }
    return lifecycle;
}

void execution_complete_locked(
    const std::shared_ptr<Lifecycle>& lifecycle,
    const char* owner,
    const char* outcome
) {
    if (lifecycle == nullptr || !lifecycle->sampled) {
        if (lifecycle != nullptr) {
            lifecycle->execution_complete_observed = true;
            const uint32_t type = lifecycle->snapshot.words[0];
            if (type == M_GFXTASK || type == M_AUDTASK) {
                ++g_execution_complete_counts[type];
            }
            if (outcome[0] == 'u') {
                lifecycle->unexpected_exit = true;
                ++g_unexpected_exits;
            }
        }
        return;
    }
    lifecycle->execution_complete_observed = true;
    const uint32_t type = lifecycle->snapshot.words[0];
    if (type == M_GFXTASK || type == M_AUDTASK) {
        ++g_execution_complete_counts[type];
    }
    if (outcome[0] == 'u') {
        lifecycle->unexpected_exit = true;
        ++g_unexpected_exits;
    }
    emit_lifecycle(
        *lifecycle,
        "execution_complete",
        0u,
        0u,
        lifecycle->snapshot.words[0] == M_GFXTASK
            ? "rt64_processDisplayLists"
            : "generated_n_aspMain_return",
        owner,
        outcome,
        bumble::native_pi_dma::replay_observer_tick(),
        bumble::native_checkpoint::last_frontend_phase(),
        true
    );
}

bumble::native_rsp_task::CaptureStats capture_stats_locked() {
    bumble::native_rsp_task::CaptureStats stats{};
    stats.capture_started = g_capture_started;
    stats.graphics_producers = g_producer_counts[M_GFXTASK];
    stats.graphics_scheduler_loads = g_scheduler_load_counts[M_GFXTASK];
    stats.graphics_scheduler_starts = g_scheduler_start_counts[M_GFXTASK];
    stats.graphics_execution_begins = g_execution_begin_counts[M_GFXTASK];
    stats.graphics_execution_completes = g_execution_complete_counts[M_GFXTASK];
    stats.audio_producers = g_producer_counts[M_AUDTASK];
    stats.audio_scheduler_loads = g_scheduler_load_counts[M_AUDTASK];
    stats.audio_scheduler_starts = g_scheduler_start_counts[M_AUDTASK];
    stats.audio_execution_begins = g_execution_begin_counts[M_AUDTASK];
    stats.audio_execution_completes = g_execution_complete_counts[M_AUDTASK];
    stats.snapshot_mismatches = g_snapshot_mismatches;
    stats.unexpected_exits = g_unexpected_exits;

    for (const auto& [sequence, lifecycle] : g_all_lifecycles) {
        (void)sequence;
        if (lifecycle == nullptr) {
            continue;
        }
        const uint32_t type = lifecycle->snapshot.words[0];
        if (lifecycle->sampled) {
            if (type == M_AUDTASK) {
                ++stats.sampled_audio;
            } else if (type == M_GFXTASK) {
                ++stats.sampled_graphics;
            }
        }
        const bool fully_correlated =
            lifecycle->producer_observed &&
            lifecycle->scheduler_load_observed &&
            lifecycle->scheduler_start_observed &&
            lifecycle->execution_begin_observed &&
            lifecycle->execution_complete_observed &&
            !lifecycle->snapshot_mismatch &&
            !lifecycle->unexpected_exit;
        if (fully_correlated) {
            if (type == M_AUDTASK) {
                ++stats.fully_correlated_audio;
            } else if (type == M_GFXTASK) {
                ++stats.fully_correlated_graphics;
            }
        } else if (lifecycle->sampled) {
            ++stats.pending_sampled_lifecycles;
        }
    }

    stats.contract_passed =
        stats.capture_started &&
        stats.fully_correlated_audio != 0u &&
        stats.fully_correlated_graphics != 0u &&
        stats.snapshot_mismatches == 0u &&
        stats.unexpected_exits == 0u &&
        stats.pending_sampled_lifecycles == 0u;
    return stats;
}

void emit_capture_stopped_locked(
    const bumble::native_rsp_task::CaptureStats& stats
) {
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=native_rsp_task_capture_stopped"
        " capture_started=%d"
        " audio_producers=%" PRIu64
        " audio_scheduler_loads=%" PRIu64
        " audio_scheduler_starts=%" PRIu64
        " audio_execution_begins=%" PRIu64
        " audio_execution_completes=%" PRIu64
        " graphics_producers=%" PRIu64
        " graphics_scheduler_loads=%" PRIu64
        " graphics_scheduler_starts=%" PRIu64
        " graphics_execution_begins=%" PRIu64
        " graphics_execution_completes=%" PRIu64
        " fully_correlated_audio=%" PRIu64
        " fully_correlated_graphics=%" PRIu64
        " sampled_audio=%" PRIu64
        " sampled_graphics=%" PRIu64
        " snapshot_mismatches=%" PRIu64
        " unexpected_exits=%" PRIu64
        " pending_sampled_lifecycles=%" PRIu64
        " contract_passed=%d tick=%" PRIu64 "\n",
        stats.capture_started ? 1 : 0,
        stats.audio_producers,
        stats.audio_scheduler_loads,
        stats.audio_scheduler_starts,
        stats.audio_execution_begins,
        stats.audio_execution_completes,
        stats.graphics_producers,
        stats.graphics_scheduler_loads,
        stats.graphics_scheduler_starts,
        stats.graphics_execution_begins,
        stats.graphics_execution_completes,
        stats.fully_correlated_audio,
        stats.fully_correlated_graphics,
        stats.sampled_audio,
        stats.sampled_graphics,
        stats.snapshot_mismatches,
        stats.unexpected_exits,
        stats.pending_sampled_lifecycles,
        stats.contract_passed ? 1 : 0,
        bumble::native_pi_dma::replay_observer_tick()
    );
    std::fflush(stderr);
}

RspExitReason instrumented_audio_microcode(uint8_t* rdram, uint32_t ucode_addr) {
    const RspExitReason reason = n_aspMain(rdram, ucode_addr);
    {
        std::lock_guard lock(g_state_mutex);
        execution_complete_locked(
            g_audio_execution,
            "generated_rsp_recomp",
            reason == RspExitReason::Broke ? "broke" : "unexpected_exit"
        );
    }
    g_audio_execution.reset();
    return reason;
}

} // namespace

extern "C" void bumble_wait_graphics_input(uint8_t* rdram, uint32_t waiter_index) {
    std::unique_lock lock(g_parse_mutex);
    initialize_parse_waiters(rdram);
    auto& waiter = g_parse_waiters.at(waiter_index);
    while (g_parse_completed < g_parse_produced ||
           (waiter_index == 0 && g_parse_waiters[1].waiting)) {
        waiter.waiting = true;
        const int32_t queue = waiter.queue;
        lock.unlock();
        osRecvMesg(rdram, queue, 0, OS_MESG_BLOCK);
        lock.lock();
        waiter.waiting = false;
    }
    if (waiter_index == 1 && g_parse_waiters[0].waiting) {
        ultramodern::enqueue_external_message(g_parse_waiters[0].queue, 0, false, false);
    }
}

extern "C" void buck_native_rsp_task_probe(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t anchor_pc
) {
    if (rdram == nullptr || context == nullptr) {
        return;
    }
    if (anchor_pc == kGraphicsProducerPc) {
        bumble::native_campaign_level::observe(
            rdram,
            bumble::native_campaign_level::ObservationSite::GraphicsTaskSubmit
        );
        const uint32_t task_pointer =
            static_cast<uint32_t>(context->r5) + 0x10u;
        register_graphics_input(rdram, task_pointer);
        profile_graphics_producer(guest_snapshot(rdram, task_pointer));
    } else if (anchor_pc == kSchedulerStartPc) {
        const uint32_t task_pointer = static_cast<uint32_t>(context->r18);
        profile_graphics_scheduler_start(
            guest_snapshot(rdram, task_pointer)
        );
    }
    std::lock_guard lock(g_state_mutex);
    if (!synchronize_capture_locked()) {
        return;
    }
    switch (anchor_pc) {
    case kAudioProducerPc:
    case kGraphicsProducerPc:
        if (!g_capture_draining) {
            observe_producer_locked(rdram, context, anchor_pc);
        }
        break;
    case kSchedulerLoadPc:
        observe_scheduler_load_locked(rdram, context);
        break;
    case kSchedulerStartPc:
        observe_scheduler_start_locked(rdram, context);
        break;
    default:
        break;
    }
}

RspUcodeFunc* bumble::native_rsp_task::select_audio_microcode(
    const OSTask* task
) {
    std::lock_guard lock(g_state_mutex);
    synchronize_capture_locked();
    g_audio_execution = execution_begin_locked(task, "generated_rsp_recomp");
    return instrumented_audio_microcode;
}

void bumble::native_rsp_task::graphics_execution_begin(const OSTask* task) {
    {
        std::lock_guard lock(g_parse_mutex);
        const uint32_t address = static_cast<uint32_t>(task->t.data_ptr) & 0x03FFFFFFu;
        if (g_pending_parses.empty() || g_pending_parses.front().first != address) {
            throw std::runtime_error("Graphics input submission ownership mismatch");
        }
        g_active_parse = g_pending_parses.front().second;
        g_pending_parses.pop_front();
    }
    profile_graphics_execution_begin(host_snapshot(task));
    std::lock_guard lock(g_state_mutex);
    synchronize_capture_locked();
    g_graphics_execution = execution_begin_locked(task, "pinned_rt64");
}

void bumble::native_rsp_task::graphics_execution_complete(bool processed) {
    if (processed) {
        std::lock_guard lock(g_parse_mutex);
        g_parse_completed = g_active_parse;
        g_active_parse = 0;
        for (const auto& waiter : g_parse_waiters) {
            if (waiter.waiting) ultramodern::enqueue_external_message(waiter.queue, 0, false, false);
        }
    }
    {
        std::lock_guard lock(g_state_mutex);
        execution_complete_locked(
            g_graphics_execution,
            "pinned_rt64",
            processed ? "processed" : "not_processed"
        );
        g_graphics_execution.reset();
    }
    profile_graphics_execution_complete();
}

bumble::native_rsp_task::CaptureStats
bumble::native_rsp_task::capture_stats() {
    std::lock_guard lock(g_state_mutex);
    return capture_stats_locked();
}

void bumble::native_rsp_task::begin_capture_drain() {
    std::lock_guard lock(g_state_mutex);
    if (!g_capture_started || g_capture_finalized || g_capture_draining) {
        return;
    }
    g_capture_draining = true;
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=native_rsp_task_capture_drain_started"
        " tick=%" PRIu64 "\n",
        bumble::native_pi_dma::replay_observer_tick()
    );
    std::fflush(stderr);
}

bumble::native_rsp_task::CaptureStats
bumble::native_rsp_task::finalize_capture() {
    std::lock_guard lock(g_state_mutex);
    const CaptureStats stats = capture_stats_locked();
    if (!g_capture_finalized) {
        emit_capture_stopped_locked(stats);
        g_capture_finalized = true;
    }
    g_capture_was_enabled = false;
    g_capture_draining = false;
    return stats;
}
