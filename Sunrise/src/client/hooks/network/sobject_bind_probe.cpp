#include "sobject_bind_probe.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "../../../core/logging/log.h"
#include "../../targets/game.h"
#include "coordinator/network_call_coordinator.h"
#include "platform.h"

namespace sunrise::client::hooks::network::sobject_bind_probe {
namespace {

constexpr std::uint32_t kEntitySlotMask = 0x1FFF;
/** The armed decoder emits at most four records; spare entries keep this diagnostic bounded. */
constexpr std::size_t kWatchCapacity = 8;
/** Covers an initial dispatch, a queued callback, and later unbind/rebind activity per slot. */
constexpr std::uint32_t kDispatchReportLimit = 16;
/** Each glue entry owns at least its identity and native-object-index fields. */
constexpr std::uint32_t kMinimumGlueStride = 8;
/** Reject corrupt target storage before using it in pointer arithmetic. */
constexpr std::uint32_t kMaximumGlueStride = 0x1000;
constexpr std::size_t kNativeObjectIndexOffset = 4;

struct WatchEntry {
    std::atomic_uint64_t contextKey{};
    std::atomic_uint32_t entityId{};
    std::atomic_uint32_t dispatches{};
};

std::array<WatchEntry, kWatchCapacity> g_watches{};
/** Serialises the small HUD snapshot across gameplay hooks and the render thread. */
SRWLOCK g_debugLock{SRWLOCK_INIT};
EntityDebugSnapshot g_debug{};

using Binder = void(__fastcall*)(std::uint32_t, std::uint32_t);

struct BindingSnapshot {
    std::uint32_t nativeObjectIndex{0xFFFFFFFF};
    bool readable{};
};

/** Packs one namespace and zero-based entity slot into a nonzero atomic watch key. */
[[nodiscard]] constexpr std::uint64_t watch_key(std::int32_t namespaceId,
                                                std::uint32_t entityId) noexcept {
    if (namespaceId < 0) {
        return 0;
    }
    const std::uint64_t namespaceKey =
        static_cast<std::uint64_t>(static_cast<std::uint32_t>(namespaceId)) + 1;
    const std::uint64_t slotKey = static_cast<std::uint64_t>(entityId & kEntitySlotMask) + 1;
    return namespaceKey << 32U | slotKey;
}

/** @return Stable watch storage for an exact decoded namespace/slot pair. */
[[nodiscard]] WatchEntry* find_watch(std::int32_t namespaceId, std::uint32_t entityId) noexcept {
    const std::uint64_t key = watch_key(namespaceId, entityId);
    if (key == 0) {
        return nullptr;
    }
    for (WatchEntry& entry : g_watches) {
        if (entry.contextKey.load(std::memory_order_acquire) == key) {
            return &entry;
        }
    }
    return nullptr;
}

/** @return Stable watch storage for a generation-rewritten slot in any namespace. */
[[nodiscard]] WatchEntry* find_watch(std::uint32_t entityId) noexcept {
    const std::uint32_t slotKey = (entityId & kEntitySlotMask) + 1;
    for (WatchEntry& entry : g_watches) {
        if (static_cast<std::uint32_t>(entry.contextKey.load(std::memory_order_acquire))
            == slotKey) {
            return &entry;
        }
    }
    return nullptr;
}

/** @return Stable watch storage for the exact full entity decoded from the experiment. */
[[nodiscard]] WatchEntry* find_exact_watch(std::uint32_t entityId) noexcept {
    for (WatchEntry& entry : g_watches) {
        if (entry.contextKey.load(std::memory_order_acquire) != 0
            && entry.entityId.load(std::memory_order_acquire) == entityId) {
            return &entry;
        }
    }
    return nullptr;
}

/** Reads the exact glue-table postcondition used by the original dispatcher. */
[[nodiscard]] bool inspect_binding(std::uint32_t entityId, BindingSnapshot& output) noexcept {
    output = {};
    const targets::game::network::Targets& resolved = targets::game::network::get();
    if (resolved.sobjectGlueTableBaseSlot == nullptr || resolved.sobjectGlueStrideSlot == nullptr) {
        return false;
    }

    __try {
        std::byte* table = nullptr;
        std::uint32_t stride = 0;
        std::memcpy(&table, resolved.sobjectGlueTableBaseSlot, sizeof table);
        std::memcpy(&stride, resolved.sobjectGlueStrideSlot, sizeof stride);
        if (table == nullptr || stride < kMinimumGlueStride || stride > kMaximumGlueStride
            || stride % alignof(std::uint32_t) != 0) {
            return false;
        }

        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(table);
        const std::uintptr_t offset =
            static_cast<std::uintptr_t>(entityId & kEntitySlotMask) * stride
            + kNativeObjectIndexOffset;
        if (offset > (std::numeric_limits<std::uintptr_t>::max)() - base) {
            return false;
        }
        const auto* const address = reinterpret_cast<const void*>(base + offset);
        std::memcpy(&output.nativeObjectIndex, address, sizeof output.nativeObjectIndex);
        output.readable = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        return false;
    }
}

/** Preserves the glue dispatcher and reports bounded invocations for watched decoded slots. */
__declspec(noinline) void __fastcall binder_body(std::uint32_t entityId,
                                                 std::uint32_t nativeObjectIndex) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, HookSlot::sobjectBinder, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Binder>(lease.original);
    __try {
        WatchEntry* const watched = lease.accepting ? find_watch(entityId) : nullptr;
        BindingSnapshot before{};
        if (watched != nullptr) {
            (void)inspect_binding(entityId, before);
        }
        if (call != nullptr) {
            call(entityId, nativeObjectIndex);
        }
        BindingSnapshot after{};
        if (watched != nullptr) {
            (void)inspect_binding(entityId, after);
        }
        const std::uint32_t occurrence =
            watched != nullptr ? watched->dispatches.fetch_add(1, std::memory_order_relaxed) + 1
                               : 0;
        if (occurrence != 0 && occurrence <= kDispatchReportLimit) {
            const std::uint32_t slot = entityId & kEntitySlotMask;
            const bool bound = after.readable && after.nativeObjectIndex == nativeObjectIndex;
            record_binding(entityId, nativeObjectIndex, bound);
            std::array<char, 256> line{};
            const int written = std::snprintf(line.data(),
                                              line.size(),
                                              "ev=gameplay stage=sobject-bind-dispatch "
                                              "entity=0x%08X slot=%u native=0x%08X occurrence=%u "
                                              "status=%s before_readable=%u before=0x%08X "
                                              "after_readable=%u after=0x%08X",
                                              entityId,
                                              slot,
                                              nativeObjectIndex,
                                              occurrence,
                                              bound ? "bound" : "deferred-or-skipped",
                                              before.readable ? 1U : 0U,
                                              before.nativeObjectIndex,
                                              after.readable ? 1U : 0U,
                                              after.nativeObjectIndex);
            if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
    } __finally {
        coordinator::g_callEgress();
    }
}

} // namespace

void* binder_entry_point() noexcept {
    return reinterpret_cast<void*>(&binder_body);
}

void watch(std::int32_t namespaceId, std::uint32_t entityId) noexcept {
    const std::uint64_t key = watch_key(namespaceId, entityId);
    if (key == 0) {
        return;
    }
    for (WatchEntry& entry : g_watches) {
        std::uint64_t current = entry.contextKey.load(std::memory_order_acquire);
        if (current == key) {
            entry.entityId.store(entityId, std::memory_order_release);
            return;
        }
        if (current == 0
            && entry.contextKey.compare_exchange_strong(
                current, key, std::memory_order_release, std::memory_order_relaxed)) {
            entry.entityId.store(entityId, std::memory_order_release);
            return;
        }
        if (current == key) {
            entry.entityId.store(entityId, std::memory_order_release);
            return;
        }
    }
}

void record_plan(std::uint64_t token,
                 std::int32_t namespaceId,
                 std::uint8_t view,
                 std::uint16_t slot,
                 std::uint32_t rsat,
                 std::int32_t region,
                 std::uint8_t bubble,
                 std::uint8_t cell,
                 std::uint8_t attempts,
                 bool sent) noexcept {
    AcquireSRWLockExclusive(&g_debugLock);
    if (!g_debug.present || g_debug.token != token || g_debug.namespaceId != namespaceId
        || g_debug.slot != slot) {
        g_debug = {};
        g_debug.namespaceId = -1;
        g_debug.region = -1;
        g_debug.type2Result = -1;
        g_debug.nativeObjectIndex = 0xFFFFFFFF;
    }
    g_debug.token = token;
    g_debug.namespaceId = namespaceId;
    g_debug.view = view;
    g_debug.slot = slot;
    g_debug.rsat = rsat;
    g_debug.region = region;
    g_debug.bubble = bubble;
    g_debug.cell = cell;
    g_debug.attempts = attempts;
    g_debug.sent = g_debug.sent || sent;
    g_debug.present = true;
    ReleaseSRWLockExclusive(&g_debugLock);
}

void record_decoded(std::int32_t namespaceId,
                    std::uint32_t entityId,
                    std::uint16_t cell,
                    std::uint16_t wireFlags) noexcept {
    AcquireSRWLockExclusive(&g_debugLock);
    if (g_debug.present && g_debug.namespaceId == namespaceId
        && g_debug.slot == (entityId & kEntitySlotMask)) {
        g_debug.entityId = entityId;
        g_debug.cell = cell;
        g_debug.wireFlags = wireFlags;
        g_debug.decoded = true;
    }
    ReleaseSRWLockExclusive(&g_debugLock);
}

void record_promoted(std::int32_t namespaceId, std::uint32_t entityId, bool occupied) noexcept {
    AcquireSRWLockExclusive(&g_debugLock);
    if (g_debug.decoded && occupied && g_debug.namespaceId == namespaceId
        && g_debug.slot == (entityId & kEntitySlotMask)) {
        g_debug.runtimeEntityId = entityId;
        g_debug.promoted = true;
    }
    ReleaseSRWLockExclusive(&g_debugLock);
}

void record_dirty_service(std::int32_t namespaceId, std::uint32_t entityId) noexcept {
    AcquireSRWLockExclusive(&g_debugLock);
    if (g_debug.decoded && g_debug.namespaceId == namespaceId
        && g_debug.slot == (entityId & kEntitySlotMask)) {
        g_debug.dirtyServiced = true;
    }
    ReleaseSRWLockExclusive(&g_debugLock);
}

void record_type2(std::uint32_t entityId, int result, bool jobReturned) noexcept {
    AcquireSRWLockExclusive(&g_debugLock);
    if (g_debug.decoded && g_debug.slot == (entityId & kEntitySlotMask)) {
        g_debug.runtimeEntityId = entityId;
        g_debug.type2Result = result;
        g_debug.type2JobReturned = jobReturned;
        g_debug.type2Seen = true;
    }
    ReleaseSRWLockExclusive(&g_debugLock);
}

void record_apply(std::uint32_t entityId) noexcept {
    AcquireSRWLockExclusive(&g_debugLock);
    if (g_debug.decoded && g_debug.slot == (entityId & kEntitySlotMask)) {
        g_debug.runtimeEntityId = entityId;
        g_debug.applied = true;
    }
    ReleaseSRWLockExclusive(&g_debugLock);
}

void record_kind0(std::uint32_t entityId, bool result) noexcept {
    AcquireSRWLockExclusive(&g_debugLock);
    if (g_debug.decoded && g_debug.slot == (entityId & kEntitySlotMask)) {
        g_debug.runtimeEntityId = entityId;
        g_debug.kind0Seen = true;
        g_debug.kind0Result = result;
    }
    ReleaseSRWLockExclusive(&g_debugLock);
}

void record_native(std::uint32_t rsat, std::uint32_t objectId) noexcept {
    AcquireSRWLockExclusive(&g_debugLock);
    if (g_debug.decoded && g_debug.rsat == rsat) {
        g_debug.nativeObjectId = objectId;
        g_debug.nativeSeen = true;
    }
    ReleaseSRWLockExclusive(&g_debugLock);
}

void record_binding(std::uint32_t entityId, std::uint32_t nativeObjectIndex, bool bound) noexcept {
    AcquireSRWLockExclusive(&g_debugLock);
    if (g_debug.decoded && g_debug.slot == (entityId & kEntitySlotMask)) {
        g_debug.runtimeEntityId = entityId;
        g_debug.nativeObjectIndex = nativeObjectIndex;
        g_debug.bindSeen = true;
        g_debug.bound = g_debug.bound || bound;
    }
    ReleaseSRWLockExclusive(&g_debugLock);
}

bool debug_snapshot(EntityDebugSnapshot& output) noexcept {
    AcquireSRWLockShared(&g_debugLock);
    output = g_debug;
    ReleaseSRWLockShared(&g_debugLock);
    return output.present;
}

bool watched(std::int32_t namespaceId, std::uint32_t entityId) noexcept {
    return find_watch(namespaceId, entityId) != nullptr;
}

bool watched_exact(std::uint32_t entityId) noexcept {
    return find_exact_watch(entityId) != nullptr;
}

bool watched(std::uint32_t entityId) noexcept {
    return find_watch(entityId) != nullptr;
}

bool first_watched(std::int32_t namespaceId, std::uint32_t& entityId) noexcept {
    entityId = 0;
    if (namespaceId < 0) {
        return false;
    }
    const std::uint64_t namespaceKey =
        static_cast<std::uint64_t>(static_cast<std::uint32_t>(namespaceId)) + 1;
    for (WatchEntry& entry : g_watches) {
        const std::uint64_t contextKey = entry.contextKey.load(std::memory_order_acquire);
        if (contextKey >> 32U == namespaceKey) {
            entityId = entry.entityId.load(std::memory_order_acquire);
            return true;
        }
    }
    return false;
}

void reset() noexcept {
    for (WatchEntry& entry : g_watches) {
        entry.dispatches.store(0, std::memory_order_relaxed);
        entry.entityId.store(0, std::memory_order_relaxed);
        entry.contextKey.store(0, std::memory_order_relaxed);
    }
    AcquireSRWLockExclusive(&g_debugLock);
    g_debug = {};
    g_debug.namespaceId = -1;
    g_debug.region = -1;
    g_debug.type2Result = -1;
    g_debug.nativeObjectIndex = 0xFFFFFFFF;
    ReleaseSRWLockExclusive(&g_debugLock);
}

} // namespace sunrise::client::hooks::network::sobject_bind_probe
