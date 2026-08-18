#include "view_slot_probe.h"

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
/** FUN_1416BB2B0 embeds the native replication scheduler in the manager. */
constexpr std::size_t kSchedulerOffset = 0x5498;
constexpr std::size_t kSchedulerLocalSignatureOffset = 0x10;
constexpr std::size_t kSchedulerLocalSignatureViewCountOffset = 0x20;
constexpr std::size_t kSchedulerLocalSignatureViewsOffset = 0x28;
constexpr std::size_t kSchedulerRemoteSignatureOffset = 0x58;
constexpr std::size_t kSchedulerRemoteSignatureViewCountOffset = 0x68;
constexpr std::size_t kSchedulerRemoteSignatureViewsOffset = 0x70;
constexpr std::size_t kSchedulerSignatureViewCapacity = 3;
constexpr std::size_t kSchedulerSignatureViewStride = 0x10;
constexpr std::size_t kSchedulerSignatureViewTagOffset = 8;
constexpr std::size_t kSchedulerRegisteredViewCountOffset = 0xC0;
constexpr std::size_t kSchedulerSignatureFlagsOffset = 0x1DC;
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

/** One entry appended to the scheduler's 128-bit signature header for a registered view. */
struct SignatureViewSnapshot {
    std::uint64_t key{};
    std::uint8_t tag{};
};

struct Snapshot {
    const void* manager{};
    std::int32_t state{-1};
    std::int32_t schedulerRegisteredViewCount{-1};
    std::array<std::uint32_t, 4> schedulerLocalSignature{};
    std::int32_t schedulerLocalSignatureViewCount{-1};
    std::array<SignatureViewSnapshot, kSchedulerSignatureViewCapacity>
        schedulerLocalSignatureViews{};
    std::array<std::uint32_t, 4> schedulerRemoteSignature{};
    std::int32_t schedulerRemoteSignatureViewCount{-1};
    std::array<SignatureViewSnapshot, kSchedulerSignatureViewCapacity>
        schedulerRemoteSignatureViews{};
    std::uint16_t schedulerSignatureFlags{};
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
        const auto* const scheduler = manager + kSchedulerOffset;
        output.schedulerRegisteredViewCount =
            *reinterpret_cast<const std::int32_t*>(scheduler + kSchedulerRegisteredViewCountOffset);
        std::memcpy(output.schedulerLocalSignature.data(),
                    scheduler + kSchedulerLocalSignatureOffset,
                    sizeof output.schedulerLocalSignature);
        output.schedulerLocalSignatureViewCount = *reinterpret_cast<const std::int32_t*>(
            scheduler + kSchedulerLocalSignatureViewCountOffset);
        for (std::size_t index = 0; index < kSchedulerSignatureViewCapacity; ++index) {
            const auto* const entry = scheduler + kSchedulerLocalSignatureViewsOffset
                                      + index * kSchedulerSignatureViewStride;
            output.schedulerLocalSignatureViews[index].key =
                *reinterpret_cast<const std::uint64_t*>(entry);
            output.schedulerLocalSignatureViews[index].tag =
                *reinterpret_cast<const std::uint8_t*>(entry + kSchedulerSignatureViewTagOffset);
        }
        std::memcpy(output.schedulerRemoteSignature.data(),
                    scheduler + kSchedulerRemoteSignatureOffset,
                    sizeof output.schedulerRemoteSignature);
        output.schedulerRemoteSignatureViewCount = *reinterpret_cast<const std::int32_t*>(
            scheduler + kSchedulerRemoteSignatureViewCountOffset);
        for (std::size_t index = 0; index < kSchedulerSignatureViewCapacity; ++index) {
            const auto* const entry = scheduler + kSchedulerRemoteSignatureViewsOffset
                                      + index * kSchedulerSignatureViewStride;
            output.schedulerRemoteSignatureViews[index].key =
                *reinterpret_cast<const std::uint64_t*>(entry);
            output.schedulerRemoteSignatureViews[index].tag =
                *reinterpret_cast<const std::uint8_t*>(entry + kSchedulerSignatureViewTagOffset);
        }
        output.schedulerSignatureFlags =
            *reinterpret_cast<const std::uint16_t*>(scheduler + kSchedulerSignatureFlagsOffset);
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
    mix(hash, static_cast<std::uint32_t>(snapshot.schedulerRegisteredViewCount));
    for (const std::uint32_t word : snapshot.schedulerLocalSignature) {
        mix(hash, word);
    }
    mix(hash, static_cast<std::uint32_t>(snapshot.schedulerLocalSignatureViewCount));
    for (const SignatureViewSnapshot& view : snapshot.schedulerLocalSignatureViews) {
        mix(hash, view.key);
        mix(hash, view.tag);
    }
    for (const std::uint32_t word : snapshot.schedulerRemoteSignature) {
        mix(hash, word);
    }
    mix(hash, static_cast<std::uint32_t>(snapshot.schedulerRemoteSignatureViewCount));
    for (const SignatureViewSnapshot& view : snapshot.schedulerRemoteSignatureViews) {
        mix(hash, view.key);
        mix(hash, view.tag);
    }
    mix(hash, snapshot.schedulerSignatureFlags);
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
            std::array<char, 1536> line{};
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
                "views=%u first=%p key=0x%llX] "
                "scheduler[views=%d local=%08X%08X%08X%08X lcount=%d "
                "l0=0x%llX/%u l1=0x%llX/%u l2=0x%llX/%u "
                "remote=%08X%08X%08X%08X rcount=%d "
                "r0=0x%llX/%u r1=0x%llX/%u r2=0x%llX/%u flags=0x%04X]",
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
                static_cast<unsigned long long>(slot2.firstViewKey),
                snapshot.schedulerRegisteredViewCount,
                snapshot.schedulerLocalSignature[0],
                snapshot.schedulerLocalSignature[1],
                snapshot.schedulerLocalSignature[2],
                snapshot.schedulerLocalSignature[3],
                snapshot.schedulerLocalSignatureViewCount,
                static_cast<unsigned long long>(snapshot.schedulerLocalSignatureViews[0].key),
                static_cast<unsigned>(snapshot.schedulerLocalSignatureViews[0].tag),
                static_cast<unsigned long long>(snapshot.schedulerLocalSignatureViews[1].key),
                static_cast<unsigned>(snapshot.schedulerLocalSignatureViews[1].tag),
                static_cast<unsigned long long>(snapshot.schedulerLocalSignatureViews[2].key),
                static_cast<unsigned>(snapshot.schedulerLocalSignatureViews[2].tag),
                snapshot.schedulerRemoteSignature[0],
                snapshot.schedulerRemoteSignature[1],
                snapshot.schedulerRemoteSignature[2],
                snapshot.schedulerRemoteSignature[3],
                snapshot.schedulerRemoteSignatureViewCount,
                static_cast<unsigned long long>(snapshot.schedulerRemoteSignatureViews[0].key),
                static_cast<unsigned>(snapshot.schedulerRemoteSignatureViews[0].tag),
                static_cast<unsigned long long>(snapshot.schedulerRemoteSignatureViews[1].key),
                static_cast<unsigned>(snapshot.schedulerRemoteSignatureViews[1].tag),
                static_cast<unsigned long long>(snapshot.schedulerRemoteSignatureViews[2].key),
                static_cast<unsigned>(snapshot.schedulerRemoteSignatureViews[2].tag),
                static_cast<unsigned>(snapshot.schedulerSignatureFlags));
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
