#include "scheduler_handler_probe.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../../core/logging/log.h"
#include "coordinator/network_call_coordinator.h"
#include "platform.h"

namespace sunrise::client::hooks::network::scheduler_handler_probe {
namespace {

constexpr std::uint8_t kLaneCount = 5;
constexpr std::uint8_t kMaximumViews = 3;
constexpr std::uint8_t kMaximumCalls = kLaneCount * kMaximumViews;
constexpr ULONGLONG kTraceLifetimeMilliseconds = 1000;

enum class Status : std::uint8_t {
    none,
    matched,
    ok,
    complete,
    timeout,
    laneMismatch,
    readerMismatch,
    threadMismatch,
    readerUnreadable,
    callLimit,
    stale,
};

struct EpochState {
    const void* reader{};
    std::uint64_t epoch{};
    ULONGLONG deadline{};
    DWORD threadId{};
    std::uint8_t viewCount{};
    std::uint8_t expectedCalls{};
    std::uint8_t ordinal{};
    bool armed{};
};

SRWLOCK g_traceLock{SRWLOCK_INIT};
std::atomic_bool g_active{};
EpochState g_epoch{};
std::uint64_t g_nextEpoch{};

using MainDecoder = int(__fastcall*)(void*, void*, void*, void*, int, void*, int*);
using PreludeDecoder = int(__fastcall*)(void*, void*, void*);

/** @return Expected handler lane for a zero-based epoch call ordinal. */
[[nodiscard]] constexpr Lane lane_for(std::uint8_t ordinal) noexcept {
    switch (ordinal % kLaneCount) {
    case 0:
        return Lane::event;
    case 1:
        return Lane::mask;
    case 2:
        return Lane::entityPrelude;
    case 3:
        return Lane::entityList;
    default:
        return Lane::fixed;
    }
}

/** @return Stable log spelling for one handler lane. */
[[nodiscard]] const char* lane_name(Lane lane) noexcept {
    switch (lane) {
    case Lane::event:
        return "event";
    case Lane::mask:
        return "mask";
    case Lane::entityPrelude:
        return "entity-prelude";
    case Lane::entityList:
        return "entity-list";
    case Lane::fixed:
        return "fixed";
    }
    return "unknown";
}

/** @return Stable log spelling for one trace disposition. */
[[nodiscard]] const char* status_name(Status status) noexcept {
    switch (status) {
    case Status::matched:
        return "matched";
    case Status::ok:
        return "ok";
    case Status::complete:
        return "complete";
    case Status::timeout:
        return "timeout";
    case Status::laneMismatch:
        return "lane-mismatch";
    case Status::readerMismatch:
        return "reader-mismatch";
    case Status::threadMismatch:
        return "thread-mismatch";
    case Status::readerUnreadable:
        return "reader-unreadable";
    case Status::callLimit:
        return "call-limit";
    case Status::stale:
        return "stale";
    case Status::none:
    default:
        return "none";
    }
}

/** Reads the native bit reader without changing its cursor or accumulator. */
[[nodiscard]] bool inspect_reader(const void* readerAddress, ReaderState& output) noexcept {
    output = {};
    if (readerAddress == nullptr) {
        return false;
    }
    __try {
        const auto* const bytes = static_cast<const std::byte*>(readerAddress);
        std::memcpy(&output.begin, bytes, sizeof output.begin);
        std::memcpy(&output.end, bytes + 0x08, sizeof output.end);
        std::memcpy(&output.loadedBits, bytes + 0x20, sizeof output.loadedBits);
        std::memcpy(&output.totalBits, bytes + 0x24, sizeof output.totalBits);
        std::memcpy(&output.accumulator, bytes + 0x28, sizeof output.accumulator);
        std::memcpy(&output.pendingBits, bytes + 0x30, sizeof output.pendingBits);
        std::memcpy(&output.cursor, bytes + 0x38, sizeof output.cursor);
        output.readable = output.pendingBits <= 64 && output.begin != nullptr
                          && reinterpret_cast<std::uintptr_t>(output.end)
                                 >= reinterpret_cast<std::uintptr_t>(output.begin);
        return output.readable;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        return false;
    }
}

/** Disarms the current epoch while the caller owns the trace lock. */
void disarm_locked() noexcept {
    g_epoch.armed = false;
    g_active.store(false, std::memory_order_release);
}

/** Reports one complete native handler call after all trace locks are released. */
void report_call(const Call& call, const ReaderState& after, Status status, int result) noexcept {
    const long long consumed = call.before.readable && after.readable
                                   ? static_cast<long long>(after.totalBits)
                                         - static_cast<long long>(call.before.totalBits)
                                   : -1;
    std::array<char, 768> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=gameplay stage=scheduler-handler-trace epoch=%llu ordinal=%u view=%u "
        "lane=%s expected=%s status=%s result=%d reader=%p thread=%lu readable=%u "
        "consumed=%lld before[total=%d loaded=%d pending=%u accum=0x%016llX cursor=%p] "
        "after[total=%d loaded=%d pending=%u accum=0x%016llX cursor=%p]",
        static_cast<unsigned long long>(call.epoch),
        static_cast<unsigned>(call.ordinal),
        static_cast<unsigned>(call.view),
        lane_name(call.lane),
        lane_name(call.expectedLane),
        status_name(status),
        result,
        call.reader,
        static_cast<unsigned long>(call.threadId),
        after.readable ? 1U : 0U,
        consumed,
        call.before.totalBits,
        call.before.loadedBits,
        static_cast<unsigned>(call.before.pendingBits),
        static_cast<unsigned long long>(call.before.accumulator),
        static_cast<const void*>(call.before.cursor),
        after.totalBits,
        after.loadedBits,
        static_cast<unsigned>(after.pendingBits),
        static_cast<unsigned long long>(after.accumulator),
        static_cast<const void*>(after.cursor));
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Preserves one of the three seven-argument scheduler handlers. */
template <HookSlot Slot, Lane HandlerLane>
__declspec(noinline) int __fastcall decode_main(void* context,
                                                void* view,
                                                void* control,
                                                void* reader,
                                                int capacity,
                                                void* records,
                                                int* count) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, Slot, coordinator::ConsumerKind::none);
    const auto original = reinterpret_cast<MainDecoder>(lease.original);
    Call trace{};
    int result = 0;
    __try {
        if (lease.accepting) {
            trace = begin(HandlerLane, reader);
        }
        if (original != nullptr) {
            result = original(context, view, control, reader, capacity, records, count);
        }
        finish(trace, result);
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

/** Preserves the direct-entity three-argument prelude handler. */
__declspec(noinline) int __fastcall
decode_entity_prelude(void* context, void* reader, void* prelude) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::schedulerEntityPreludeDecoder, coordinator::ConsumerKind::none);
    const auto original = reinterpret_cast<PreludeDecoder>(lease.original);
    Call trace{};
    int result = 0;
    __try {
        if (lease.accepting) {
            trace = begin(Lane::entityPrelude, reader);
        }
        if (original != nullptr) {
            result = original(context, reader, prelude);
        }
        finish(trace, result);
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

} // namespace

void* event_entry_point() noexcept {
    return reinterpret_cast<void*>(&decode_main<HookSlot::schedulerEventDecoder, Lane::event>);
}

void* mask_entry_point() noexcept {
    return reinterpret_cast<void*>(&decode_main<HookSlot::schedulerMaskDecoder, Lane::mask>);
}

void* entity_prelude_entry_point() noexcept {
    return reinterpret_cast<void*>(&decode_entity_prelude);
}

void* fixed_entry_point() noexcept {
    return reinterpret_cast<void*>(&decode_main<HookSlot::schedulerFixedDecoder, Lane::fixed>);
}

void arm(std::uint8_t viewCount) noexcept {
    if (viewCount == 0 || viewCount > kMaximumViews) {
        cancel();
        return;
    }

    AcquireSRWLockExclusive(&g_traceLock);
    ++g_nextEpoch;
    if (g_nextEpoch == 0) {
        ++g_nextEpoch;
    }
    g_epoch = {};
    g_epoch.epoch = g_nextEpoch;
    g_epoch.deadline = GetTickCount64() + kTraceLifetimeMilliseconds;
    g_epoch.viewCount = viewCount;
    g_epoch.expectedCalls = static_cast<std::uint8_t>(viewCount * kLaneCount);
    g_epoch.armed = true;
    g_active.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_traceLock);
}

void cancel() noexcept {
    AcquireSRWLockExclusive(&g_traceLock);
    disarm_locked();
    ReleaseSRWLockExclusive(&g_traceLock);
}

bool active() noexcept {
    if (!g_active.load(std::memory_order_acquire)) {
        return false;
    }

    AcquireSRWLockExclusive(&g_traceLock);
    if (g_epoch.armed && GetTickCount64() >= g_epoch.deadline) {
        disarm_locked();
    }
    const bool armed = g_epoch.armed;
    ReleaseSRWLockExclusive(&g_traceLock);
    return armed;
}

Call begin(Lane lane, const void* reader) noexcept {
    Call call{};
    if (!g_active.load(std::memory_order_acquire)) {
        return call;
    }

    call.reader = reader;
    call.threadId = GetCurrentThreadId();
    call.lane = lane;
    (void)inspect_reader(reader, call.before);

    AcquireSRWLockExclusive(&g_traceLock);
    if (!g_epoch.armed) {
        ReleaseSRWLockExclusive(&g_traceLock);
        return {};
    }

    call.epoch = g_epoch.epoch;
    call.ordinal = g_epoch.ordinal;
    call.view = static_cast<std::uint8_t>(g_epoch.ordinal / kLaneCount);
    call.expectedLane = lane_for(g_epoch.ordinal);
    call.report = true;

    Status status = Status::matched;
    if (GetTickCount64() >= g_epoch.deadline) {
        status = Status::timeout;
    } else if (g_epoch.ordinal >= g_epoch.expectedCalls || g_epoch.ordinal >= kMaximumCalls) {
        status = Status::callLimit;
    } else if (lane != call.expectedLane) {
        status = Status::laneMismatch;
    } else if (!call.before.readable) {
        status = Status::readerUnreadable;
    } else {
        if (g_epoch.ordinal == 0) {
            g_epoch.reader = reader;
            g_epoch.threadId = call.threadId;
        }
        if (g_epoch.reader != reader) {
            status = Status::readerMismatch;
        } else if (g_epoch.threadId != call.threadId) {
            status = Status::threadMismatch;
        }
    }
    call.status = static_cast<std::uint8_t>(status);
    if (status != Status::matched) {
        disarm_locked();
    }
    ReleaseSRWLockExclusive(&g_traceLock);
    return call;
}

void finish(const Call& call, int result) noexcept {
    if (!call.report) {
        return;
    }

    ReaderState after{};
    (void)inspect_reader(call.reader, after);
    Status status = static_cast<Status>(call.status);
    if (status == Status::matched) {
        AcquireSRWLockExclusive(&g_traceLock);
        if (!g_epoch.armed || g_epoch.epoch != call.epoch) {
            status = Status::stale;
        } else if (GetTickCount64() >= g_epoch.deadline) {
            status = Status::timeout;
            disarm_locked();
        } else if (!after.readable) {
            status = Status::readerUnreadable;
            disarm_locked();
        } else if (g_epoch.reader != call.reader) {
            status = Status::readerMismatch;
            disarm_locked();
        } else if (g_epoch.threadId != GetCurrentThreadId() || g_epoch.threadId != call.threadId) {
            status = Status::threadMismatch;
            disarm_locked();
        } else if (g_epoch.ordinal != call.ordinal || g_epoch.ordinal >= g_epoch.expectedCalls
                   || g_epoch.ordinal >= kMaximumCalls) {
            status = Status::callLimit;
            disarm_locked();
        } else {
            ++g_epoch.ordinal;
            if (g_epoch.ordinal == g_epoch.expectedCalls) {
                status = Status::complete;
                disarm_locked();
            } else {
                status = Status::ok;
            }
        }
        ReleaseSRWLockExclusive(&g_traceLock);
    }
    report_call(call, after, status, result);
}

void reset() noexcept {
    cancel();
}

} // namespace sunrise::client::hooks::network::scheduler_handler_probe
