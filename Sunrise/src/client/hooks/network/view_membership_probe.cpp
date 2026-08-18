#include "view_membership_probe.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "../../../core/logging/log.h"
#include "coordinator/network_call_coordinator.h"
#include "platform.h"

namespace sunrise::client::hooks::network::view_membership_probe {
namespace {

/** Native session-membership storage begins after the local peer identity. */
constexpr std::size_t kMembershipOffset = 0x6C38;
constexpr std::size_t kLocalIdentityOffset = 0x6C30;
/** The queue callback receives a wrapper header immediately before the membership image. */
constexpr std::size_t kDecodedMembershipOffset = sizeof(std::uint64_t);
/** Native peer records are fixed-size and occupy 32 membership slots. */
constexpr std::size_t kPeerStride = 0x2AC0;
constexpr std::size_t kPeerCount = 32;
/** Masks consumed by the membership-to-view synchronization loop. */
constexpr std::size_t kEligibleMaskOffset = 0x59248;
constexpr std::size_t kOccupiedMaskOffset = 0x5924C;
constexpr std::size_t kRetainMaskOffset = 0x59250;
/** Fields read by the native view-creation predicates. */
constexpr std::size_t kPeerIdentityOffset = 8;
constexpr std::size_t kPeerKindOffset = 0x10;
constexpr std::size_t kPeerGateByteOffset = 0x38;
constexpr std::uint8_t kPeerGateBit = 0x10;
/** Distinct snapshots are retained so a per-tick sync cannot flood the log. */
constexpr std::size_t kSeenCapacity = 64;

std::array<std::atomic_uint64_t, kSeenCapacity> g_seen{};
std::array<std::atomic_uint64_t, kSeenCapacity> g_decodedSeen{};

using Synchronizer = void(__fastcall*)(void*);
using ActivityMembershipDecoder =
    bool(__fastcall*)(void*, void*, std::int32_t*, std::uint32_t, std::uint32_t);
using DecodedMembershipQueue = void(__fastcall*)(void*, const void*);

/** Static schema key for the complete activity-membership object. */
constexpr std::uint32_t kActivityMembershipType = 0x808086A8;

struct PeerSnapshot {
    std::uint64_t identity{};
    std::uint32_t kind{};
    std::uint8_t gateByte{};
    std::uint8_t terminalByte{};
    bool occupied{};
    bool eligible{};
};

struct Snapshot {
    std::uint64_t session{};
    std::uint64_t localIdentity{};
    std::int32_t family{-1};
    std::int32_t localPeer{-1};
    std::uint32_t occupiedMask{};
    std::uint32_t eligibleMask{};
    std::uint32_t retainMask{};
    std::array<PeerSnapshot, 2> peers{};
};

struct DecodedSnapshot {
    std::uint32_t revision{};
    std::uint32_t epoch{};
    std::uint32_t eligibleMask{};
    std::uint32_t occupiedMask{};
    std::uint32_t tail2{};
    std::uint32_t tail3{};
    std::uint32_t tail4{};
    std::uint64_t peer0{};
    std::uint64_t peer1{};
    std::uint8_t peer1Gate{};
    std::array<std::uint8_t, 16> peer1GateWindow{};
    std::uint32_t peer1BlockNonzeroCount{};
    std::array<std::uint8_t, 8> peer1BlockOffsets{};
    std::array<std::uint8_t, 8> peer1BlockValues{};
};

/** Reads the activity-membership image immediately after the type-12 wire decoder. */
[[nodiscard]] bool inspect_decoded(const void* membership, DecodedSnapshot& output) noexcept {
    if (membership == nullptr) {
        return false;
    }

    __try {
        const auto* const bytes =
            static_cast<const std::byte*>(membership) + kDecodedMembershipOffset;
        output.revision = *reinterpret_cast<const std::uint32_t*>(bytes);
        output.epoch = *reinterpret_cast<const std::uint32_t*>(bytes + 4);
        output.eligibleMask =
            *reinterpret_cast<const std::uint32_t*>(bytes + kEligibleMaskOffset);
        output.occupiedMask =
            *reinterpret_cast<const std::uint32_t*>(bytes + kOccupiedMaskOffset);
        output.tail2 = *reinterpret_cast<const std::uint32_t*>(bytes + kRetainMaskOffset);
        output.tail3 = *reinterpret_cast<const std::uint32_t*>(bytes + 0x59254);
        output.tail4 = *reinterpret_cast<const std::uint32_t*>(bytes + 0x59258);
        output.peer0 = *reinterpret_cast<const std::uint64_t*>(bytes + kPeerIdentityOffset);
        output.peer1 = *reinterpret_cast<const std::uint64_t*>(
            bytes + kPeerIdentityOffset + kPeerStride);
        output.peer1Gate = *reinterpret_cast<const std::uint8_t*>(
            bytes + kPeerGateByteOffset + kPeerStride);
        for (std::size_t index = 0; index < output.peer1GateWindow.size(); ++index) {
            output.peer1GateWindow[index] = *reinterpret_cast<const std::uint8_t*>(
                bytes + 0x30 + kPeerStride + index);
        }
        std::size_t retained = 0;
        for (std::size_t index = 0; index < 128; ++index) {
            const std::uint8_t value = *reinterpret_cast<const std::uint8_t*>(
                bytes + 0x31 + kPeerStride + index);
            if (value == 0) {
                continue;
            }
            ++output.peer1BlockNonzeroCount;
            if (retained < output.peer1BlockOffsets.size()) {
                output.peer1BlockOffsets[retained] = static_cast<std::uint8_t>(index);
                output.peer1BlockValues[retained] = value;
                ++retained;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        return false;
    }
}

/** Reads only the fields used by FUN_141702580 before it invokes the view creator. */
[[nodiscard]] bool inspect(void* context, Snapshot& output) noexcept {
    if (context == nullptr) {
        return false;
    }

    __try {
        const auto* const wrapper = static_cast<const std::byte*>(context);
        const auto* const session =
            *reinterpret_cast<const std::byte* const*>(wrapper);
        const auto* const familyState =
            *reinterpret_cast<const std::byte* const*>(wrapper + sizeof(void*));
        if (session == nullptr || familyState == nullptr) {
            return false;
        }

        const auto* const membership = session + kMembershipOffset;
        output.session = *reinterpret_cast<const std::uint64_t*>(familyState + 0x20);
        output.family = *reinterpret_cast<const std::int32_t*>(familyState + 8);
        output.localIdentity =
            *reinterpret_cast<const std::uint64_t*>(session + kLocalIdentityOffset);
        output.eligibleMask =
            *reinterpret_cast<const std::uint32_t*>(membership + kEligibleMaskOffset);
        output.occupiedMask =
            *reinterpret_cast<const std::uint32_t*>(membership + kOccupiedMaskOffset);
        output.retainMask =
            *reinterpret_cast<const std::uint32_t*>(membership + kRetainMaskOffset);

        for (std::size_t peer = 0; peer < kPeerCount; ++peer) {
            const std::uint32_t bit = std::uint32_t{1} << peer;
            if ((output.occupiedMask & bit) != 0
                && *reinterpret_cast<const std::uint64_t*>(
                       membership + kPeerIdentityOffset + peer * kPeerStride)
                       == output.localIdentity) {
                output.localPeer = static_cast<std::int32_t>(peer);
                break;
            }
        }

        for (std::size_t peer = 0; peer < output.peers.size(); ++peer) {
            const std::uint32_t bit = std::uint32_t{1} << peer;
            PeerSnapshot& snapshot = output.peers[peer];
            snapshot.identity = *reinterpret_cast<const std::uint64_t*>(
                membership + kPeerIdentityOffset + peer * kPeerStride);
            snapshot.kind = *reinterpret_cast<const std::uint32_t*>(
                membership + kPeerKindOffset + peer * kPeerStride);
            snapshot.gateByte = *reinterpret_cast<const std::uint8_t*>(
                membership + kPeerGateByteOffset + peer * kPeerStride);
            snapshot.terminalByte = *reinterpret_cast<const std::uint8_t*>(
                membership + (peer + 1) * kPeerStride);
            snapshot.occupied = (output.occupiedMask & bit) != 0;
            snapshot.eligible = (output.eligibleMask & bit) != 0;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        output.family = -1;
        output.localPeer = -1;
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

/** @return True only for the first observation of this exact membership state. */
[[nodiscard]] bool record_once(const Snapshot& snapshot) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    mix(hash, snapshot.session);
    mix(hash, snapshot.localIdentity);
    mix(hash, static_cast<std::uint32_t>(snapshot.family));
    mix(hash, static_cast<std::uint32_t>(snapshot.localPeer));
    mix(hash, snapshot.occupiedMask);
    mix(hash, snapshot.eligibleMask);
    mix(hash, snapshot.retainMask);
    for (const PeerSnapshot& peer : snapshot.peers) {
        mix(hash, peer.identity);
        mix(hash, peer.kind);
        mix(hash, peer.gateByte);
        mix(hash, peer.terminalByte);
        mix(hash, peer.occupied ? 1U : 0U);
        mix(hash, peer.eligible ? 1U : 0U);
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

/** @return True only for the first observation of this exact decoded tail. */
[[nodiscard]] bool record_decoded_once(const DecodedSnapshot& snapshot) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    mix(hash, snapshot.revision);
    mix(hash, snapshot.epoch);
    mix(hash, snapshot.eligibleMask);
    mix(hash, snapshot.occupiedMask);
    mix(hash, snapshot.tail2);
    mix(hash, snapshot.tail3);
    mix(hash, snapshot.tail4);
    mix(hash, snapshot.peer0);
    mix(hash, snapshot.peer1);
    mix(hash, snapshot.peer1Gate);
    for (const std::uint8_t value : snapshot.peer1GateWindow) {
        mix(hash, value);
    }
    mix(hash, snapshot.peer1BlockNonzeroCount);
    for (std::size_t index = 0; index < snapshot.peer1BlockOffsets.size(); ++index) {
        mix(hash, snapshot.peer1BlockOffsets[index]);
        mix(hash, snapshot.peer1BlockValues[index]);
    }
    if (hash == 0) {
        hash = 1;
    }

    for (std::atomic_uint64_t& seen : g_decodedSeen) {
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

/** Mirrors the complete native gate for the peer, excluding channel checks in the creator. */
[[nodiscard]] bool would_create(const Snapshot& snapshot, std::size_t peer) noexcept {
    const PeerSnapshot& candidate = snapshot.peers[peer];
    return candidate.occupied && candidate.eligible
           && static_cast<std::int32_t>(peer) != snapshot.localPeer
           && candidate.terminalByte == 0 && (candidate.gateByte & kPeerGateBit) != 0;
}

/** Preserves synchronization and records the membership predicates before they are consumed. */
__declspec(noinline) void __fastcall sync_body(void* context) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::viewMembershipSync, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Synchronizer>(lease.original);
    Snapshot snapshot{};
    bool readable = false;
    __try {
        if (lease.accepting) {
            readable = inspect(context, snapshot);
        }
        if (call != nullptr) {
            call(context);
        }
        if (lease.accepting && readable && snapshot.occupiedMask != 0
            && record_once(snapshot)) {
            std::array<char, 768> line{};
            const int written = std::snprintf(
                line.data(),
                line.size(),
                "ev=gameplay stage=view-membership session=0x%llX family=%d local=%d "
                "local_id=0x%llX occupied=0x%08X eligible=0x%08X retain=0x%08X "
                "p0[id=0x%llX gate=0x%02X terminal=%u kind=%u create=%u] "
                "p1[id=0x%llX gate=0x%02X terminal=%u kind=%u create=%u]",
                static_cast<unsigned long long>(snapshot.session),
                snapshot.family,
                snapshot.localPeer,
                static_cast<unsigned long long>(snapshot.localIdentity),
                snapshot.occupiedMask,
                snapshot.eligibleMask,
                snapshot.retainMask,
                static_cast<unsigned long long>(snapshot.peers[0].identity),
                static_cast<unsigned>(snapshot.peers[0].gateByte),
                static_cast<unsigned>(snapshot.peers[0].terminalByte),
                snapshot.peers[0].kind,
                would_create(snapshot, 0) ? 1U : 0U,
                static_cast<unsigned long long>(snapshot.peers[1].identity),
                static_cast<unsigned>(snapshot.peers[1].gateByte),
                static_cast<unsigned>(snapshot.peers[1].terminalByte),
                snapshot.peers[1].kind,
                would_create(snapshot, 1) ? 1U : 0U);
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

/** Records the exact bit count consumed by the native type-12 membership decoder. */
__declspec(noinline) bool __fastcall wire_body(void* reader,
                                               void* membership,
                                               std::int32_t* version,
                                               std::uint32_t type,
                                               std::uint32_t flags) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::activityMembershipDecoder, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<ActivityMembershipDecoder>(lease.original);
    std::uint32_t before = 0;
    std::uint32_t after = 0;
    DecodedSnapshot snapshot{};
    bool readable = false;
    bool result = false;
    __try {
        if (lease.accepting && type == kActivityMembershipType && reader != nullptr) {
            __try {
                before = *reinterpret_cast<const std::uint32_t*>(
                    static_cast<const std::byte*>(reader) + 0x24);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                before = 0;
            }
        }
        if (call != nullptr) {
            result = call(reader, membership, version, type, flags);
        }
        if (lease.accepting && type == kActivityMembershipType && reader != nullptr) {
            __try {
                after = *reinterpret_cast<const std::uint32_t*>(
                    static_cast<const std::byte*>(reader) + 0x24);
                readable = inspect_decoded(
                    static_cast<const std::byte*>(membership) - kDecodedMembershipOffset,
                    snapshot);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                readable = false;
            }
            std::array<char, 512> line{};
            const int written = std::snprintf(
                line.data(),
                line.size(),
                "ev=gameplay stage=membership-wire result=%s before=%u after=%u consumed=%u "
                "revision=%u eligible=0x%08X occupied=0x%08X p1=0x%llX",
                result ? "ok" : "fail",
                before,
                after,
                after >= before ? after - before : 0,
                readable ? snapshot.revision : 0,
                readable ? snapshot.eligibleMask : 0,
                readable ? snapshot.occupiedMask : 0,
                static_cast<unsigned long long>(readable ? snapshot.peer1 : 0));
            if (written > 0) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

/** Preserves queue insertion and records the exact snapshot produced by the type-12 decoder. */
__declspec(noinline) void __fastcall decoded_body(void* owner, const void* membership) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::activityMembershipQueue, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<DecodedMembershipQueue>(lease.original);
    DecodedSnapshot snapshot{};
    bool readable = false;
    __try {
        if (lease.accepting) {
            readable = inspect_decoded(membership, snapshot);
        }
        if (call != nullptr) {
            call(owner, membership);
        }
        if (lease.accepting && readable && snapshot.occupiedMask != 0
            && record_decoded_once(snapshot)) {
            std::array<char, 384> line{};
            const int written = std::snprintf(
                line.data(),
                line.size(),
                "ev=gameplay stage=membership-decode revision=%u epoch=%u "
                "eligible=0x%08X occupied=0x%08X tail2=0x%08X tail3=0x%08X "
                "tail4=0x%08X p0=0x%llX p1=0x%llX p1_gate=0x%02X "
                "p1_30=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X "
                "block_nz=%u first=%u:%02X,%u:%02X,%u:%02X,%u:%02X,%u:%02X,%u:%02X,%u:%02X,%u:%02X",
                snapshot.revision,
                snapshot.epoch,
                snapshot.eligibleMask,
                snapshot.occupiedMask,
                snapshot.tail2,
                snapshot.tail3,
                snapshot.tail4,
                static_cast<unsigned long long>(snapshot.peer0),
                static_cast<unsigned long long>(snapshot.peer1),
                static_cast<unsigned>(snapshot.peer1Gate),
                static_cast<unsigned>(snapshot.peer1GateWindow[0]),
                static_cast<unsigned>(snapshot.peer1GateWindow[1]),
                static_cast<unsigned>(snapshot.peer1GateWindow[2]),
                static_cast<unsigned>(snapshot.peer1GateWindow[3]),
                static_cast<unsigned>(snapshot.peer1GateWindow[4]),
                static_cast<unsigned>(snapshot.peer1GateWindow[5]),
                static_cast<unsigned>(snapshot.peer1GateWindow[6]),
                static_cast<unsigned>(snapshot.peer1GateWindow[7]),
                static_cast<unsigned>(snapshot.peer1GateWindow[8]),
                static_cast<unsigned>(snapshot.peer1GateWindow[9]),
                static_cast<unsigned>(snapshot.peer1GateWindow[10]),
                static_cast<unsigned>(snapshot.peer1GateWindow[11]),
                static_cast<unsigned>(snapshot.peer1GateWindow[12]),
                static_cast<unsigned>(snapshot.peer1GateWindow[13]),
                static_cast<unsigned>(snapshot.peer1GateWindow[14]),
                static_cast<unsigned>(snapshot.peer1GateWindow[15]),
                snapshot.peer1BlockNonzeroCount,
                static_cast<unsigned>(snapshot.peer1BlockOffsets[0]),
                static_cast<unsigned>(snapshot.peer1BlockValues[0]),
                static_cast<unsigned>(snapshot.peer1BlockOffsets[1]),
                static_cast<unsigned>(snapshot.peer1BlockValues[1]),
                static_cast<unsigned>(snapshot.peer1BlockOffsets[2]),
                static_cast<unsigned>(snapshot.peer1BlockValues[2]),
                static_cast<unsigned>(snapshot.peer1BlockOffsets[3]),
                static_cast<unsigned>(snapshot.peer1BlockValues[3]),
                static_cast<unsigned>(snapshot.peer1BlockOffsets[4]),
                static_cast<unsigned>(snapshot.peer1BlockValues[4]),
                static_cast<unsigned>(snapshot.peer1BlockOffsets[5]),
                static_cast<unsigned>(snapshot.peer1BlockValues[5]),
                static_cast<unsigned>(snapshot.peer1BlockOffsets[6]),
                static_cast<unsigned>(snapshot.peer1BlockValues[6]),
                static_cast<unsigned>(snapshot.peer1BlockOffsets[7]),
                static_cast<unsigned>(snapshot.peer1BlockValues[7]));
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

void* sync_entry_point() noexcept {
    return reinterpret_cast<void*>(&sync_body);
}

void* wire_entry_point() noexcept {
    return reinterpret_cast<void*>(&wire_body);
}

void* decoded_entry_point() noexcept {
    return reinterpret_cast<void*>(&decoded_body);
}

void reset() noexcept {
    for (std::atomic_uint64_t& seen : g_seen) {
        seen.store(0, std::memory_order_relaxed);
    }
    for (std::atomic_uint64_t& seen : g_decodedSeen) {
        seen.store(0, std::memory_order_relaxed);
    }
}

} // namespace sunrise::client::hooks::network::view_membership_probe
