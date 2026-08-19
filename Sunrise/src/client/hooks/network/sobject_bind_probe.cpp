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
    std::atomic_uint32_t slotKey{};
    std::atomic_uint32_t dispatches{};
};

std::array<WatchEntry, kWatchCapacity> g_watches{};

using Binder = void(__fastcall*)(std::uint32_t, std::uint32_t);

struct BindingSnapshot {
    std::uint32_t nativeObjectIndex{0xFFFFFFFF};
    bool readable{};
};

/** Converts a zero-based entity slot into a nonzero atomic watch key. */
[[nodiscard]] constexpr std::uint32_t watch_key(std::uint32_t entityId) noexcept {
    return (entityId & kEntitySlotMask) + 1;
}

/** @return Stable watch storage for a decoded slot, or null when it was not armed. */
[[nodiscard]] WatchEntry* find_watch(std::uint32_t entityId) noexcept {
    const std::uint32_t key = watch_key(entityId);
    for (WatchEntry& entry : g_watches) {
        if (entry.slotKey.load(std::memory_order_acquire) == key) {
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

void watch(std::uint32_t entityId) noexcept {
    const std::uint32_t key = watch_key(entityId);
    for (WatchEntry& entry : g_watches) {
        std::uint32_t current = entry.slotKey.load(std::memory_order_acquire);
        if (current == key) {
            return;
        }
        if (current == 0
            && entry.slotKey.compare_exchange_strong(
                current, key, std::memory_order_release, std::memory_order_relaxed)) {
            return;
        }
        if (current == key) {
            return;
        }
    }
}

void reset() noexcept {
    for (WatchEntry& entry : g_watches) {
        entry.dispatches.store(0, std::memory_order_relaxed);
        entry.slotKey.store(0, std::memory_order_relaxed);
    }
}

} // namespace sunrise::client::hooks::network::sobject_bind_probe
