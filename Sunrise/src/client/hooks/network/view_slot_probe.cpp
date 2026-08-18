#include "view_slot_probe.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "../../../core/logging/log.h"
#include "coordinator/network_call_coordinator.h"
#include "platform.h"

namespace sunrise::client::hooks::network::view_slot_probe {
namespace {

/** The native replication manager owns exactly three fixed-size session-family slots. */
constexpr std::size_t kSlotCount = 3;
constexpr std::size_t kFirstSlotOffset = 0x206C8;
constexpr std::size_t kSlotStride = 0x11E08;
/** Each slot owns 31 possible channel-view pointers. */
constexpr std::size_t kViewCapacity = 31;
constexpr std::size_t kViewTableOffset = 0x178;
constexpr std::size_t kViewActiveOffset = 0x44;
constexpr std::size_t kViewKeyOffset = 0x68;
/** Distinct snapshots suppress the per-frame manager pump without hiding state changes. */
constexpr std::size_t kSeenCapacity = 96;

std::array<std::atomic_uint64_t, kSeenCapacity> g_seen{};

using Pump = void(__fastcall*)(void*);

struct SlotSnapshot {
    std::int32_t family{-1};
    std::uint64_t id10{};
    std::uint64_t id18{};
    std::uint64_t token{};
    const void* syncContext{};
    std::uint32_t viewCount{};
    const void* firstView{};
    std::uint64_t firstViewKey{};
};

struct Snapshot {
    const void* manager{};
    std::int32_t state{-1};
    std::array<SlotSnapshot, kSlotCount> slots{};
};

/** Reads the exact fields used by slot pumping and message-40 view lookup. */
[[nodiscard]] bool inspect(void* managerAddress, Snapshot& output) noexcept {
    if (managerAddress == nullptr) {
        return false;
    }

    __try {
        const auto* const manager = static_cast<const std::byte*>(managerAddress);
        output.manager = managerAddress;
        output.state = *reinterpret_cast<const std::int32_t*>(manager + 8);
        for (std::size_t index = 0; index < output.slots.size(); ++index) {
            const auto* const slot = manager + kFirstSlotOffset + index * kSlotStride;
            SlotSnapshot& snapshot = output.slots[index];
            snapshot.family = *reinterpret_cast<const std::int32_t*>(slot + 8);
            snapshot.id10 = *reinterpret_cast<const std::uint64_t*>(slot + 0x10);
            snapshot.id18 = *reinterpret_cast<const std::uint64_t*>(slot + 0x18);
            snapshot.token = *reinterpret_cast<const std::uint64_t*>(slot + 0x20);
            snapshot.syncContext = *reinterpret_cast<const void* const*>(slot + 0x30);

            for (std::size_t viewIndex = 0; viewIndex < kViewCapacity; ++viewIndex) {
                const auto* const view = *reinterpret_cast<const std::byte* const*>(
                    slot + kViewTableOffset + viewIndex * sizeof(void*));
                if (view == nullptr
                    || *reinterpret_cast<const std::uint8_t*>(view + kViewActiveOffset) == 0) {
                    continue;
                }
                ++snapshot.viewCount;
                if (snapshot.firstView == nullptr) {
                    snapshot.firstView = view;
                    snapshot.firstViewKey =
                        *reinterpret_cast<const std::uint64_t*>(view + kViewKeyOffset);
                }
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        output.state = -1;
        return false;
    }
}

/** FNV-1a is sufficient for suppressing identical diagnostic snapshots. */
void mix(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        hash ^= static_cast<std::uint8_t>(value >> (index * 8));
        hash *= 1099511628211ULL;
    }
}

/** @return True only for the first observation of this exact manager state. */
[[nodiscard]] bool record_once(const Snapshot& snapshot) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    mix(hash, reinterpret_cast<std::uintptr_t>(snapshot.manager));
    mix(hash, static_cast<std::uint32_t>(snapshot.state));
    for (const SlotSnapshot& slot : snapshot.slots) {
        mix(hash, static_cast<std::uint32_t>(slot.family));
        mix(hash, slot.id10);
        mix(hash, slot.id18);
        mix(hash, slot.token);
        mix(hash, reinterpret_cast<std::uintptr_t>(slot.syncContext));
        mix(hash, slot.viewCount);
        mix(hash, reinterpret_cast<std::uintptr_t>(slot.firstView));
        mix(hash, slot.firstViewKey);
    }
    if (hash == 0) {
        hash = 1;
    }

    for (std::atomic_uint64_t& seen : g_seen) {
        std::uint64_t current = seen.load(std::memory_order_relaxed);
        if (current == hash) {
            return false;
        }
        if (current == 0
            && seen.compare_exchange_strong(
                current, hash, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return true;
        }
        if (current == hash) {
            return false;
        }
    }
    return false;
}

/** Preserves the native pump and snapshots the resulting session-family slots. */
__declspec(noinline) void __fastcall pump_body(void* manager) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, HookSlot::viewSlotPump, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Pump>(lease.original);
    Snapshot snapshot{};
    bool readable = false;
    __try {
        if (call != nullptr) {
            call(manager);
        }
        if (lease.accepting) {
            readable = inspect(manager, snapshot);
        }
        if (readable && record_once(snapshot)) {
            std::array<char, 1024> line{};
            const SlotSnapshot& slot0 = snapshot.slots[0];
            const SlotSnapshot& slot1 = snapshot.slots[1];
            const SlotSnapshot& slot2 = snapshot.slots[2];
            const int written = std::snprintf(
                line.data(),
                line.size(),
                "ev=gameplay stage=view-slots manager=%p state=%d "
                "s0[family=%d id10=0x%llX id18=0x%llX token=0x%llX sync=%p "
                "views=%u first=%p key=0x%llX] "
                "s1[family=%d id10=0x%llX id18=0x%llX token=0x%llX sync=%p "
                "views=%u first=%p key=0x%llX] "
                "s2[family=%d id10=0x%llX id18=0x%llX token=0x%llX sync=%p "
                "views=%u first=%p key=0x%llX]",
                snapshot.manager,
                snapshot.state,
                slot0.family,
                static_cast<unsigned long long>(slot0.id10),
                static_cast<unsigned long long>(slot0.id18),
                static_cast<unsigned long long>(slot0.token),
                slot0.syncContext,
                slot0.viewCount,
                slot0.firstView,
                static_cast<unsigned long long>(slot0.firstViewKey),
                slot1.family,
                static_cast<unsigned long long>(slot1.id10),
                static_cast<unsigned long long>(slot1.id18),
                static_cast<unsigned long long>(slot1.token),
                slot1.syncContext,
                slot1.viewCount,
                slot1.firstView,
                static_cast<unsigned long long>(slot1.firstViewKey),
                slot2.family,
                static_cast<unsigned long long>(slot2.id10),
                static_cast<unsigned long long>(slot2.id18),
                static_cast<unsigned long long>(slot2.token),
                slot2.syncContext,
                slot2.viewCount,
                slot2.firstView,
                static_cast<unsigned long long>(slot2.firstViewKey));
            if (written > 0) {
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

void* pump_entry_point() noexcept {
    return reinterpret_cast<void*>(&pump_body);
}

void reset() noexcept {
    for (std::atomic_uint64_t& seen : g_seen) {
        seen.store(0, std::memory_order_relaxed);
    }
}

} // namespace sunrise::client::hooks::network::view_slot_probe
