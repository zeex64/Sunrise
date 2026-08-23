#include "authored_spawn_probe.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <span>

#include "../../../core/logging/log.h"
#include "../../player/player_position.h"
#include "coordinator/network_call_coordinator.h"
#include "platform.h"

namespace sunrise::client::hooks::network::authored_spawn_probe {
namespace {

constexpr std::size_t kDescriptorSize = 0x50;
constexpr std::size_t kSeenCapacity = 64;
constexpr std::size_t kStateSeenCapacity = 64;
constexpr std::size_t kInitializeSeenCapacity = 16;
constexpr std::size_t kQueueSeenCapacity = 32;
constexpr std::size_t kResetSeenCapacity = 32;
constexpr std::size_t kSquadSeenCapacity = 32;
constexpr std::size_t kAuthorityStateSeenCapacity = 24;
constexpr std::size_t kAuthorityApplySeenCapacity = 48;
constexpr std::size_t kCallbackSeenCapacity = 32;
constexpr std::size_t kCallbackResourceSeenCapacity = 8;
constexpr std::size_t kMaximumCallbackResourceSize = 0x88;
constexpr std::uint64_t kAuthoredAuthorityBootstrapDurationMs = 8'000;
/** A roster authority sample older than this cannot authorize a later activity. */
constexpr std::uint64_t kRosterAuthoritySampleFreshMs = 2'500;
constexpr std::uint32_t kInvalidHandle = 0xFFFFFFFF;
constexpr std::uint32_t kAuthoredControllerClass = 0x8080670A;
constexpr std::uint32_t kAuthoredControllerBootstrapResource = 0x80C0E7F2;
constexpr std::uint32_t kEdzSpawnRule = 0x80B2E997;
constexpr std::uint32_t kSquadSpawnMode = 3;
constexpr std::uint32_t kAuthoredControllerFamilyFirst = 0x80806702;
constexpr std::uint32_t kAuthoredControllerFamilyLast = 0x8080670A;
constexpr std::size_t kAuthorityGrantedOffset = 0x10EB8;
constexpr std::size_t kAuthorityPendingOffset = 0x10EC4;
constexpr std::size_t kAuthorityTokenOffset = 0x10EDC;
constexpr std::size_t kAuthorityRowStride = 0x0C;
constexpr std::uint8_t kMaximumAuthorityIndex = 0x40;

using Materialize = std::uint8_t(__fastcall*)(void*, const void*, std::uint32_t*);
using ServiceState = bool(__fastcall*)(void*);
using Initialize = void(__fastcall*)(void*, const void*, const std::int32_t*);
using QueueBuilder = void(__fastcall*)(void*);
using ResetController = void(__fastcall*)(void*);
using SquadTransform = std::uintptr_t(__fastcall*)(std::uint32_t, const void*, std::int32_t*);
using SquadObject = std::uintptr_t(__fastcall*)(std::uint32_t, std::int32_t, std::int32_t*);
using AuthorityPredicate = bool(__fastcall*)();
using AuthorityStateTest = bool(__fastcall*)(const void*, const std::uint8_t*);
using AuthorityApply =
    void(__fastcall*)(void*, const std::uint8_t*, std::uint8_t, const void*, const void*);
using CallbackResolver = std::uintptr_t(__fastcall*)(const std::uint32_t*);
using CallbackLink = void*(__fastcall*)(void*, std::int32_t, std::uint32_t);
using CallbackTableLink = void(__fastcall*)(const void*);

enum class AuthoritySampleKind : std::uint8_t {
    none,
    currentBubble,
    domain,
};

struct AuthoritySampleContext {
    AuthoritySampleKind kind{AuthoritySampleKind::none};
    bool roster{};
};

struct CallbackLinkContext {
    std::uint32_t ownerTag{};
    const void* resource{};
    std::uint32_t targetCount{};
    bool active{};
};

struct AuthorityStateSnapshot {
    const void* manager{};
    std::uint8_t index{};
    bool granted{};
    bool pending{};
    bool readable{};
};

struct AuthorityApplySnapshot {
    const void* manager{};
    std::uint16_t incomingToken{};
    std::uint16_t storedToken{};
    std::uint8_t index{};
    bool release{};
    bool granted{};
    bool pending{};
    bool readable{};
};

struct Snapshot {
    std::array<std::byte, kDescriptorSize> descriptor{};
    std::uint32_t output{kInvalidHandle};
    bool descriptorReadable{};
    bool outputReadable{};
};

struct StateSnapshot {
    std::uintptr_t context{};
    std::uint64_t deadline{};
    std::uint64_t firstSourceOffset{};
    std::uint64_t firstDue{};
    std::uint32_t objectTag{};
    std::uint32_t sourceCount{};
    std::uint32_t firstSourceTag{};
    std::uint32_t scheduledCount{};
    std::uint32_t firstDescriptorId{};
    std::uint32_t firstDefinitionTag{};
    std::uint32_t firstClass{};
    std::uint8_t state{};
    std::uint8_t firstState{};
    bool readable{};
};

struct SquadSourceSnapshot {
    std::array<std::byte, 0x20> transform{};
    std::uint64_t lastSourceOffset{};
    std::uint32_t lastSourceTag{};
    std::int32_t count{-1};
    bool readable{};
    bool transformReadable{};
};

std::array<std::atomic_uint64_t, kSeenCapacity> g_seen{};
std::array<std::atomic_uint64_t, kStateSeenCapacity> g_stateSeen{};
std::array<std::atomic_uint64_t, kInitializeSeenCapacity> g_initializeSeen{};
std::array<std::atomic_uint64_t, kQueueSeenCapacity> g_queueSeen{};
std::array<std::atomic_uint64_t, kResetSeenCapacity> g_resetSeen{};
std::array<std::atomic_uint64_t, kSquadSeenCapacity> g_squadSeen{};
std::array<std::atomic_uint64_t, kAuthorityStateSeenCapacity> g_authorityStateSeen{};
std::array<std::atomic_uint64_t, kAuthorityApplySeenCapacity> g_authorityApplySeen{};
std::array<std::atomic_uint64_t, kCallbackSeenCapacity> g_callbackSeen{};
std::array<std::atomic_uint64_t, kCallbackResourceSeenCapacity> g_callbackResourceSeen{};
std::atomic_uint8_t g_authoritySeen{};
std::atomic_uint8_t g_rosterAuthoritySeen{};
std::atomic_uint8_t g_authorityBootstrapSeen{};
std::atomic_uint64_t g_authorityBootstrapOpenedAt{};
std::atomic_bool g_rosterAuthorityReady{};
std::atomic_uint64_t g_rosterAuthoritySampledAt{};
std::atomic_bool g_squadSpawnAttempted{};
thread_local AuthoritySampleContext g_authoritySample{};
thread_local CallbackLinkContext g_callbackLink{};

struct DirectSquadRequest {
    std::int32_t count{1};
    std::uint32_t reserved{};
    std::uint32_t resource{kEdzSpawnRule};
    std::uint32_t flags{};
    std::uint64_t offset{};
};

DirectSquadRequest g_directSquadRequest{};
std::array<float, 8> g_directSquadTransform{};

[[nodiscard]] bool reserve_transition(std::span<std::atomic_uint64_t> registry,
                                      std::uint64_t key) noexcept;

/** @return Exact installed size of one observed authored callback resource, or zero. */
[[nodiscard]] std::size_t callback_resource_size(std::uint32_t ownerTag) noexcept {
    if (ownerTag == 0x80C0E7F2U) {
        return 0x88;
    }
    if (ownerTag >= 0x80C0E7F3U && ownerTag <= 0x80C0E7F5U) {
        return 0x58;
    }
    return 0;
}

/** Reports one generic lookup for the authored-controller family before any callback executes. */
void report_callback_resolver(std::uint32_t classId,
                              std::uint32_t ordinal,
                              std::uintptr_t result,
                              const void* caller) noexcept {
    if (classId < kAuthoredControllerFamilyFirst || classId > kAuthoredControllerFamilyLast) {
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto returnAddress = reinterpret_cast<std::uintptr_t>(caller);
    const auto callerOffset = returnAddress >= base ? returnAddress - base : returnAddress;
    const CallbackLinkContext link = g_callbackLink;
    std::uint64_t key = static_cast<std::uint64_t>(classId) << 32U;
    key ^= static_cast<std::uint64_t>(ordinal) << 16U;
    key ^= static_cast<std::uint64_t>(link.ownerTag);
    key ^= callerOffset & 0xFFFFU;
    if (!reserve_transition(g_callbackSeen, key == 0 ? 1 : key)) {
        return;
    }
    std::uintptr_t callback{};
    std::uint64_t metadata{};
    bool readable{};
    if (result != 0) {
        __try {
            std::memcpy(&callback, reinterpret_cast<const void*>(result), sizeof(callback));
            std::memcpy(&metadata,
                        reinterpret_cast<const std::byte*>(result) + sizeof(callback),
                        sizeof(metadata));
            readable = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            callback = 0;
            metadata = 0;
        }
    }
    std::array<char, 256> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=gameplay stage=authored-callback-resolve class=0x%08X target=%u ordinal=%u "
        "result=%u owner=0x%08X linked=%u callback=0x%016llX metadata=0x%016llX "
        "caller=+0x%llX",
        classId,
        classId == kAuthoredControllerClass ? 1U : 0U,
        ordinal,
        readable ? 1U : 0U,
        link.ownerTag,
        link.active ? 1U : 0U,
        static_cast<unsigned long long>(callback),
        static_cast<unsigned long long>(metadata),
        static_cast<unsigned long long>(callerOffset));
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reports the first false and first true result for each native authority predicate. */
void report_authority(const char* kind,
                      bool result,
                      std::uint8_t falseBit,
                      const void* caller) noexcept {
    const std::uint8_t bit = static_cast<std::uint8_t>(falseBit + (result ? 1 : 0));
    const std::uint8_t mask = static_cast<std::uint8_t>(1U << bit);
    if ((g_authoritySeen.fetch_or(mask, std::memory_order_relaxed) & mask) != 0) {
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto returnAddress = reinterpret_cast<std::uintptr_t>(caller);
    const auto callerOffset = returnAddress >= base ? returnAddress - base : returnAddress;
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=authored-spawn stage=authority source=native "
                                      "kind=%s result=%u caller=+0x%llX",
                                      kind,
                                      result ? 1U : 0U,
                                      static_cast<unsigned long long>(callerOffset));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reports each false/true state observed immediately after a successful roster decode. */
void report_roster_authority(const char* kind, bool result, std::uint8_t falseBit) noexcept {
    const std::uint8_t bit = static_cast<std::uint8_t>(falseBit + (result ? 1 : 0));
    const std::uint8_t mask = static_cast<std::uint8_t>(1U << bit);
    if ((g_rosterAuthoritySeen.fetch_or(mask, std::memory_order_relaxed) & mask) != 0) {
        return;
    }
    std::array<char, 144> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=authored-spawn stage=authority source=roster "
                                      "kind=%s result=%u",
                                      kind,
                                      result ? 1U : 0U);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Calls one predicate's trampoline without re-entering its observation detour. */
[[nodiscard]] bool
sample_authority(HookSlot slot, AuthoritySampleKind kind, bool& result) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, slot, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<AuthorityPredicate>(lease.original);
    bool sampled{};
    result = false;
    const AuthoritySampleContext previous = g_authoritySample;
    g_authoritySample = {kind, true};
    __try {
        if (lease.accepting && call != nullptr) {
            result = call();
            sampled = true;
        }
    } __finally {
        g_authoritySample = previous;
        coordinator::g_callEgress();
    }
    return sampled;
}

/** Reads the exact two bitsets consumed by the native authority-state leaf. */
[[nodiscard]] bool inspect_authority_state(const void* manager,
                                           const std::uint8_t* index,
                                           AuthorityStateSnapshot& output) noexcept {
    output = {};
    if (manager == nullptr || index == nullptr) {
        return false;
    }
    __try {
        output.manager = manager;
        output.index = *index;
        if (output.index > kMaximumAuthorityIndex) {
            return false;
        }
        const std::size_t word = output.index >> 5U;
        const std::uint32_t mask = 1U << (output.index & 31U);
        std::uint32_t granted{};
        std::uint32_t pending{};
        const auto* bytes = static_cast<const std::byte*>(manager);
        std::memcpy(
            &granted, bytes + kAuthorityGrantedOffset + word * sizeof(granted), sizeof(granted));
        std::memcpy(
            &pending, bytes + kAuthorityPendingOffset + word * sizeof(pending), sizeof(pending));
        output.granted = (granted & mask) != 0;
        output.pending = (pending & mask) != 0;
        output.readable = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        return false;
    }
}

/** Reserves a bounded unique authority-state transition. */
[[nodiscard]] bool reserve_authority_state(std::uint64_t key) noexcept {
    key = key == 0 ? 1 : key;
    for (std::atomic_uint64_t& seen : g_authorityStateSeen) {
        std::uint64_t value = seen.load(std::memory_order_relaxed);
        if (value == key) {
            return false;
        }
        if (value == 0 && seen.compare_exchange_strong(value, key, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

/** Reports raw granted/pending bits only for the two observed authority predicates. */
void report_authority_state(const AuthoritySampleContext& sample,
                            const AuthorityStateSnapshot& snapshot,
                            bool result) noexcept {
    if (!snapshot.readable || sample.kind == AuthoritySampleKind::none) {
        return;
    }
    const std::uint64_t key = 1ULL | (static_cast<std::uint64_t>(snapshot.index) << 1U)
                              | (static_cast<std::uint64_t>(snapshot.granted) << 8U)
                              | (static_cast<std::uint64_t>(snapshot.pending) << 9U)
                              | (static_cast<std::uint64_t>(result) << 10U)
                              | (static_cast<std::uint64_t>(sample.kind) << 11U)
                              | (static_cast<std::uint64_t>(sample.roster) << 13U);
    if (!reserve_authority_state(key)) {
        return;
    }
    const char* const kind =
        sample.kind == AuthoritySampleKind::currentBubble ? "current-bubble" : "domain";
    std::array<char, 224> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=authored-spawn stage=authority-bits source=%s kind=%s "
                                      "index=%u granted=%u pending=%u result=%u manager=%p",
                                      sample.roster ? "roster" : "native",
                                      kind,
                                      static_cast<unsigned>(snapshot.index),
                                      snapshot.granted ? 1U : 0U,
                                      snapshot.pending ? 1U : 0U,
                                      result ? 1U : 0U,
                                      snapshot.manager);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Copies the scheduled descriptor before native code consumes it. */
[[nodiscard]] bool inspect_descriptor(const void* descriptor, Snapshot& output) noexcept {
    output = {};
    output.output = kInvalidHandle;
    if (descriptor == nullptr) {
        return false;
    }
    __try {
        std::memcpy(output.descriptor.data(), descriptor, output.descriptor.size());
        output.descriptorReadable = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        output.output = kInvalidHandle;
        return false;
    }
}

/** Reads the game-owned result only while the caller's stack output is live. */
void inspect_output(const std::uint32_t* handle, Snapshot& output) noexcept {
    if (handle == nullptr) {
        return;
    }
    __try {
        output.output = *handle;
        output.outputReadable = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output.output = kInvalidHandle;
        output.outputReadable = false;
    }
}

/** Hashes the exact descriptor so repeated state-machine polls do not consume the log budget. */
[[nodiscard]] std::uint64_t descriptor_key(const Snapshot& snapshot) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::byte value : snapshot.descriptor) {
        hash ^= std::to_integer<std::uint8_t>(value);
        hash *= 1099511628211ULL;
    }
    return hash == 0 ? 1 : hash;
}

/** Reserves one of 64 unique descriptor shapes without blocking the materializer thread. */
[[nodiscard]] bool reserve(std::uint64_t key) noexcept {
    for (std::atomic_uint64_t& seen : g_seen) {
        if (seen.load(std::memory_order_relaxed) == key) {
            return false;
        }
    }
    for (std::atomic_uint64_t& seen : g_seen) {
        std::uint64_t empty = 0;
        if (seen.compare_exchange_strong(
                empty, key, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return true;
        }
        if (empty == key) {
            return false;
        }
    }
    return false;
}

/** Reserves one bounded state transition without allocating on the activity thread. */
[[nodiscard]] bool reserve_state(std::uint64_t key) noexcept {
    for (std::atomic_uint64_t& seen : g_stateSeen) {
        if (seen.load(std::memory_order_relaxed) == key) {
            return false;
        }
    }
    for (std::atomic_uint64_t& seen : g_stateSeen) {
        std::uint64_t empty = 0;
        if (seen.compare_exchange_strong(
                empty, key, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return true;
        }
        if (empty == key) {
            return false;
        }
    }
    return false;
}

/** Reserves one unique transition in a caller-selected bounded registry. */
[[nodiscard]] bool reserve_transition(std::span<std::atomic_uint64_t> registry,
                                      std::uint64_t key) noexcept {
    for (std::atomic_uint64_t& seen : registry) {
        if (seen.load(std::memory_order_relaxed) == key) {
            return false;
        }
    }
    for (std::atomic_uint64_t& seen : registry) {
        std::uint64_t empty = 0;
        if (seen.compare_exchange_strong(
                empty, key, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return true;
        }
        if (empty == key) {
            return false;
        }
    }
    return false;
}

/** Captures one per-lane authority transaction while every decoder argument is still live. */
[[nodiscard]] bool inspect_authority_apply(const void* manager,
                                           const std::uint8_t* index,
                                           const void* decoded,
                                           AuthorityApplySnapshot& output) noexcept {
    output = {};
    if (manager == nullptr || index == nullptr || decoded == nullptr) {
        return false;
    }
    __try {
        output.manager = manager;
        output.index = *index;
        if (output.index > kMaximumAuthorityIndex) {
            return false;
        }
        const auto* managerBytes = static_cast<const std::byte*>(manager);
        const auto* decodedBytes = static_cast<const std::byte*>(decoded);
        std::memcpy(&output.incomingToken, decodedBytes + 8, sizeof output.incomingToken);
        std::memcpy(&output.storedToken,
                    managerBytes + kAuthorityTokenOffset
                        + static_cast<std::size_t>(output.index) * kAuthorityRowStride,
                    sizeof output.storedToken);
        output.release = std::to_integer<std::uint8_t>(decodedBytes[10]) != 0;

        const std::size_t word = output.index >> 5U;
        const std::uint32_t bit = 1U << (output.index & 31U);
        std::uint32_t granted{};
        std::uint32_t pending{};
        std::memcpy(&granted,
                    managerBytes + kAuthorityGrantedOffset + word * sizeof granted,
                    sizeof granted);
        std::memcpy(&pending,
                    managerBytes + kAuthorityPendingOffset + word * sizeof pending,
                    sizeof pending);
        output.granted = (granted & bit) != 0;
        output.pending = (pending & bit) != 0;
        output.readable = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        return false;
    }
}

/** Hashes the complete before/after transaction so periodic roster repeats stay silent. */
[[nodiscard]] std::uint64_t authority_apply_key(const AuthorityApplySnapshot& before,
                                                const AuthorityApplySnapshot& after,
                                                std::uint8_t mask) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) noexcept {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    mix(reinterpret_cast<std::uintptr_t>(before.manager));
    mix(before.index);
    mix(mask);
    mix(before.incomingToken);
    mix(before.storedToken);
    mix(before.release);
    mix(before.granted);
    mix(before.pending);
    mix(after.storedToken);
    mix(after.granted);
    mix(after.pending);
    return hash == 0 ? 1 : hash;
}

/** Reports only present authority lanes, including the exact native manager receiving them. */
void report_authority_apply(const AuthorityApplySnapshot& before,
                            const AuthorityApplySnapshot& after,
                            std::uint8_t mask) noexcept {
    if (mask == 0 || !before.readable || !after.readable
        || !reserve_transition(g_authorityApplySeen, authority_apply_key(before, after, mask))) {
        return;
    }
    std::array<char, 320> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=authored-spawn stage=authority-apply manager=%p index=%u mask=%u "
                      "incoming=%u stored=%u/%u release=%u granted=%u/%u pending=%u/%u",
                      before.manager,
                      static_cast<unsigned>(before.index),
                      static_cast<unsigned>(mask),
                      static_cast<unsigned>(before.incomingToken),
                      static_cast<unsigned>(before.storedToken),
                      static_cast<unsigned>(after.storedToken),
                      before.release ? 1U : 0U,
                      before.granted ? 1U : 0U,
                      after.granted ? 1U : 0U,
                      before.pending ? 1U : 0U,
                      after.pending ? 1U : 0U);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Hex-encodes one fixed descriptor into caller-owned storage. */
void hex(std::span<const std::byte> input, std::span<char> output) noexcept {
    constexpr char kDigits[] = "0123456789ABCDEF";
    const std::size_t count =
        input.size() < (output.size() - 1) / 2 ? input.size() : (output.size() - 1) / 2;
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint8_t value = std::to_integer<std::uint8_t>(input[index]);
        output[index * 2] = kDigits[value >> 4];
        output[index * 2 + 1] = kDigits[value & 0x0F];
    }
    output[count * 2] = '\0';
}

/** Reports one already-decoded resource that actually linked the target callback family. */
void report_callback_resource(std::uint32_t ownerTag,
                              const void* resource,
                              std::span<const std::byte> bytes,
                              std::uint32_t targetCount) noexcept {
    if (ownerTag == kAuthoredControllerBootstrapResource && targetCount != 0) {
        std::uint64_t expected = 0;
        const std::uint64_t now = GetTickCount64();
        (void)g_authorityBootstrapOpenedAt.compare_exchange_strong(
            expected, now, std::memory_order_release, std::memory_order_relaxed);
    }
    if (bytes.empty()
        || !reserve_transition(g_callbackResourceSeen, static_cast<std::uint64_t>(ownerTag))) {
        return;
    }
    std::uint64_t callbackCount{};
    std::int64_t relative{};
    if (bytes.size() >= 0x20) {
        std::memcpy(&callbackCount, bytes.data() + 0x10, sizeof(callbackCount));
        std::memcpy(&relative, bytes.data() + 0x18, sizeof(relative));
    }
    std::array<char, kMaximumCallbackResourceSize * 2 + 1> raw{};
    hex(bytes, raw);
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=gameplay stage=authored-callback-resource owner=0x%08X resource=%p size=%zu "
        "callbacks=%llu target_callbacks=%u relative=%lld raw=%s",
        ownerTag,
        resource,
        bytes.size(),
        static_cast<unsigned long long>(callbackCount),
        targetCount,
        static_cast<long long>(relative),
        raw.data());
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Returns true only during the one bounded controller-construction window. */
[[nodiscard]] bool authored_authority_bootstrap_active(std::uint64_t& age) noexcept {
    age = 0;
    const std::uint64_t openedAt = g_authorityBootstrapOpenedAt.load(std::memory_order_acquire);
    if (openedAt == 0) {
        return false;
    }
    const std::uint64_t now = GetTickCount64();
    if (now < openedAt) {
        return false;
    }
    age = now - openedAt;
    return age <= kAuthoredAuthorityBootstrapDurationMs;
}

/** Reports each forced predicate once; the original result remains separately observable. */
void report_authority_bootstrap(const char* kind,
                                std::uint8_t bit,
                                std::uint64_t age,
                                const void* caller) noexcept {
    const std::uint8_t mask = static_cast<std::uint8_t>(1U << bit);
    if ((g_authorityBootstrapSeen.fetch_or(mask, std::memory_order_relaxed) & mask) != 0) {
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto returnAddress = reinterpret_cast<std::uintptr_t>(caller);
    const auto callerOffset = returnAddress >= base ? returnAddress - base : returnAddress;
    std::array<char, 192> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=authored-spawn stage=authority-bootstrap kind=%s result=forced age=%llu "
                      "window=%llu caller=+0x%llX",
                      kind,
                      static_cast<unsigned long long>(age),
                      static_cast<unsigned long long>(kAuthoredAuthorityBootstrapDurationMs),
                      static_cast<unsigned long long>(callerOffset));
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reads one little-endian dword from the already-copied descriptor. */
[[nodiscard]] std::uint32_t field32(const Snapshot& snapshot, std::size_t offset) noexcept {
    std::uint32_t value{};
    std::memcpy(&value, snapshot.descriptor.data() + offset, sizeof value);
    return value;
}

/** Reads one little-endian qword from the already-copied descriptor. */
[[nodiscard]] std::uint64_t field64(const Snapshot& snapshot, std::size_t offset) noexcept {
    std::uint64_t value{};
    std::memcpy(&value, snapshot.descriptor.data() + offset, sizeof value);
    return value;
}

/** Emits one passive descriptor/result sample from the activity-authored materializer. */
void report(const Snapshot& snapshot, std::uint8_t result) noexcept {
    if (!snapshot.descriptorReadable || !reserve(descriptor_key(snapshot))) {
        return;
    }
    std::array<char, kDescriptorSize * 2 + 1> raw{};
    hex(snapshot.descriptor, raw);
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=gameplay stage=authored-object-materialize result=%u output_readable=%u "
        "output=0x%08X tag=0x%08X d18=0x%08X d1c=0x%08X d20=0x%016llX "
        "d28=0x%08X d30=0x%08X d34=0x%08X due=0x%016llX d40=0x%016llX "
        "state=%u raw=%s",
        static_cast<unsigned>(result),
        snapshot.outputReadable ? 1U : 0U,
        snapshot.output,
        field32(snapshot, 0x00),
        field32(snapshot, 0x18),
        field32(snapshot, 0x1C),
        static_cast<unsigned long long>(field64(snapshot, 0x20)),
        field32(snapshot, 0x28),
        field32(snapshot, 0x30),
        field32(snapshot, 0x34),
        static_cast<unsigned long long>(field64(snapshot, 0x38)),
        static_cast<unsigned long long>(field64(snapshot, 0x40)),
        static_cast<unsigned>(std::to_integer<std::uint8_t>(snapshot.descriptor[0x48])),
        raw.data());
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reads only the state-machine fields proven by its native service and queue routines. */
[[nodiscard]] bool inspect_state(const void* context, StateSnapshot& output) noexcept {
    output = {};
    if (context == nullptr) {
        return false;
    }
    __try {
        const auto* bytes = static_cast<const std::byte*>(context);
        output.context = reinterpret_cast<std::uintptr_t>(context);
        std::memcpy(&output.objectTag, bytes, sizeof output.objectTag);
        std::memcpy(&output.state, bytes + 0x1E0, sizeof output.state);
        std::memcpy(&output.deadline, bytes + 0x1E8, sizeof output.deadline);
        std::memcpy(&output.sourceCount, bytes + 0x1F0, sizeof output.sourceCount);
        if (output.sourceCount != 0 && output.sourceCount <= 0x400) {
            std::memcpy(&output.firstSourceTag, bytes + 0x1F8, sizeof output.firstSourceTag);
            std::memcpy(&output.firstSourceOffset, bytes + 0x200, sizeof output.firstSourceOffset);
        }
        std::memcpy(&output.scheduledCount, bytes + 0x278, sizeof output.scheduledCount);
        if (output.scheduledCount != 0 && output.scheduledCount <= 0x20) {
            std::memcpy(&output.firstDescriptorId, bytes + 0x280, sizeof output.firstDescriptorId);
            std::memcpy(
                &output.firstDefinitionTag, bytes + 0x29C, sizeof output.firstDefinitionTag);
            std::memcpy(&output.firstClass, bytes + 0x2A8, sizeof output.firstClass);
            std::memcpy(&output.firstDue, bytes + 0x2B8, sizeof output.firstDue);
            std::memcpy(&output.firstState, bytes + 0x2C8, sizeof output.firstState);
        }
        output.readable = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        return false;
    }
}

/** Hashes stable state/count fields while deliberately excluding the changing deadline. */
[[nodiscard]] std::uint64_t
state_key(const StateSnapshot& before, const StateSnapshot& after, bool result) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) noexcept {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    mix(before.context);
    mix(before.state);
    mix(before.objectTag);
    mix(before.sourceCount);
    mix(before.firstSourceTag);
    mix(before.firstSourceOffset);
    mix(before.scheduledCount);
    mix(before.firstDescriptorId);
    mix(before.firstDefinitionTag);
    mix(before.firstClass);
    mix(before.firstState);
    mix(after.state);
    mix(after.objectTag);
    mix(after.sourceCount);
    mix(after.firstSourceTag);
    mix(after.firstSourceOffset);
    mix(after.scheduledCount);
    mix(after.firstDescriptorId);
    mix(after.firstDefinitionTag);
    mix(after.firstClass);
    mix(after.firstState);
    mix(result ? 1U : 0U);
    return hash == 0 ? 1 : hash;
}

/** Emits the first occurrence of each activity spawn state/count transition. */
void report_state(const StateSnapshot& before, const StateSnapshot& after, bool result) noexcept {
    if (!before.readable || !after.readable || !reserve_state(state_key(before, after, result))) {
        return;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=gameplay stage=authored-spawn-state result=%u context=0x%016llX "
                      "state=%u/%u sources=%u/%u scheduled=%u/%u deadline=0x%016llX/0x%016llX "
                      "object=0x%08X first_source=0x%08X/0x%08X source_offset=0x%016llX/0x%016llX "
                      "first_id=0x%08X/0x%08X definition=0x%08X/0x%08X class=0x%08X/0x%08X "
                      "first_due=0x%016llX/0x%016llX first_state=%u/%u",
                      result ? 1U : 0U,
                      static_cast<unsigned long long>(before.context),
                      static_cast<unsigned>(before.state),
                      static_cast<unsigned>(after.state),
                      before.sourceCount,
                      after.sourceCount,
                      before.scheduledCount,
                      after.scheduledCount,
                      static_cast<unsigned long long>(before.deadline),
                      static_cast<unsigned long long>(after.deadline),
                      after.objectTag,
                      before.firstSourceTag,
                      after.firstSourceTag,
                      static_cast<unsigned long long>(before.firstSourceOffset),
                      static_cast<unsigned long long>(after.firstSourceOffset),
                      before.firstDescriptorId,
                      after.firstDescriptorId,
                      before.firstDefinitionTag,
                      after.firstDefinitionTag,
                      before.firstClass,
                      after.firstClass,
                      static_cast<unsigned long long>(before.firstDue),
                      static_cast<unsigned long long>(after.firstDue),
                      static_cast<unsigned>(before.firstState),
                      static_cast<unsigned>(after.firstState));
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reads the initializer's source-row count while its game-owned list is live. */
[[nodiscard]] bool inspect_input_count(const std::int32_t* sourceList,
                                       std::int32_t& count) noexcept {
    count = -1;
    if (sourceList == nullptr) {
        return false;
    }
    __try {
        count = *sourceList;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        count = -1;
        return false;
    }
}

/** Emits one bounded initialization sample, including whether authored sources reached the client.
 */
void report_initialize(const StateSnapshot& before,
                       const StateSnapshot& after,
                       std::int32_t inputCount,
                       bool inputReadable) noexcept {
    const std::uint64_t key = state_key(before, after, inputReadable)
                              ^ (static_cast<std::uint64_t>(inputCount) << 32)
                              ^ 0x494E495449414C31ULL;
    if (!before.readable || !after.readable || !reserve_transition(g_initializeSeen, key)) {
        return;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=gameplay stage=authored-spawn-initialize context=0x%016llX input_readable=%u "
        "input_count=%d state=%u/%u object=0x%08X sources=%u/%u first_source=0x%08X "
        "source_offset=0x%016llX scheduled=%u/%u",
        static_cast<unsigned long long>(after.context),
        inputReadable ? 1U : 0U,
        inputCount,
        static_cast<unsigned>(before.state),
        static_cast<unsigned>(after.state),
        after.objectTag,
        before.sourceCount,
        after.sourceCount,
        after.firstSourceTag,
        static_cast<unsigned long long>(after.firstSourceOffset),
        before.scheduledCount,
        after.scheduledCount);
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Emits the exact first descriptor produced by a bounded authored queue build. */
void report_queue(const StateSnapshot& before, const StateSnapshot& after) noexcept {
    const std::uint64_t key = state_key(before, after, true) ^ 0x5155455545425549ULL;
    if (!before.readable || !after.readable || !reserve_transition(g_queueSeen, key)) {
        return;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=gameplay stage=authored-spawn-queue context=0x%016llX state=%u/%u "
                      "object=0x%08X sources=%u/%u scheduled=%u/%u first_source=0x%08X "
                      "first_id=0x%08X definition=0x%08X class=0x%08X due=0x%016llX first_state=%u",
                      static_cast<unsigned long long>(after.context),
                      static_cast<unsigned>(before.state),
                      static_cast<unsigned>(after.state),
                      after.objectTag,
                      before.sourceCount,
                      after.sourceCount,
                      before.scheduledCount,
                      after.scheduledCount,
                      after.firstSourceTag,
                      after.firstDescriptorId,
                      after.firstDefinitionTag,
                      after.firstClass,
                      static_cast<unsigned long long>(after.firstDue),
                      static_cast<unsigned>(after.firstState));
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reports whether a controller existed, and what authored state its native reset discarded. */
void report_reset(const StateSnapshot& before, const StateSnapshot& after) noexcept {
    const std::uint64_t key = state_key(before, after, true) ^ 0x5245534554435452ULL;
    if (!before.readable || !after.readable || !reserve_transition(g_resetSeen, key)) {
        return;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=gameplay stage=authored-spawn-reset context=0x%016llX state=%u/%u "
                      "object=0x%08X sources=%u/%u first_source=0x%08X/0x%08X "
                      "source_offset=0x%016llX/0x%016llX scheduled=%u/%u "
                      "first_id=0x%08X/0x%08X definition=0x%08X/0x%08X class=0x%08X/0x%08X",
                      static_cast<unsigned long long>(after.context),
                      static_cast<unsigned>(before.state),
                      static_cast<unsigned>(after.state),
                      before.objectTag,
                      before.sourceCount,
                      after.sourceCount,
                      before.firstSourceTag,
                      after.firstSourceTag,
                      static_cast<unsigned long long>(before.firstSourceOffset),
                      static_cast<unsigned long long>(after.firstSourceOffset),
                      before.scheduledCount,
                      after.scheduledCount,
                      before.firstDescriptorId,
                      after.firstDescriptorId,
                      before.firstDefinitionTag,
                      after.firstDefinitionTag,
                      before.firstClass,
                      after.firstClass);
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Snapshots the source row the high-level squad helper will consume from the end of its list. */
[[nodiscard]] bool inspect_squad_source(const std::int32_t* sourceList,
                                        SquadSourceSnapshot& output) noexcept {
    output = {};
    output.count = -1;
    if (sourceList == nullptr) {
        return false;
    }
    __try {
        const auto* bytes = reinterpret_cast<const std::byte*>(sourceList);
        std::memcpy(&output.count, bytes, sizeof output.count);
        if (output.count > 0 && output.count <= 0x400) {
            const std::size_t offset = 8 + static_cast<std::size_t>(output.count - 1) * 0x10;
            std::memcpy(&output.lastSourceTag, bytes + offset, sizeof output.lastSourceTag);
            std::memcpy(
                &output.lastSourceOffset, bytes + offset + 8, sizeof output.lastSourceOffset);
        }
        output.readable = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        output.count = -1;
        return false;
    }
}

/** Snapshots the native 0x20-byte placement row before the helper uses it. */
void inspect_transform(const void* transform, SquadSourceSnapshot& output) noexcept {
    if (transform == nullptr) {
        return;
    }
    __try {
        std::memcpy(output.transform.data(), transform, output.transform.size());
        output.transformReadable = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output.transform = {};
        output.transformReadable = false;
    }
}

/** Hashes one high-level squad request without retaining any game-owned address. */
[[nodiscard]] std::uint64_t squad_key(const SquadSourceSnapshot& before,
                                      const SquadSourceSnapshot& after,
                                      std::uint32_t mode,
                                      std::int32_t object,
                                      std::uintptr_t result) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) noexcept {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    mix(mode);
    mix(static_cast<std::uint32_t>(object));
    mix(static_cast<std::uint32_t>(before.count));
    mix(before.lastSourceTag);
    mix(before.lastSourceOffset);
    mix(static_cast<std::uint32_t>(after.count));
    mix(result & 0xFFU);
    for (const std::byte value : before.transform) {
        mix(std::to_integer<std::uint8_t>(value));
    }
    return hash == 0 ? 1 : hash;
}

/** Emits a bounded request/result sample from either game-owned client squad helper. */
void report_squad(const char* path,
                  const SquadSourceSnapshot& before,
                  const SquadSourceSnapshot& after,
                  std::uint32_t mode,
                  std::int32_t object,
                  std::uintptr_t result) noexcept {
    if (!before.readable || !after.readable
        || !reserve_transition(g_squadSeen, squad_key(before, after, mode, object, result))) {
        return;
    }
    std::array<char, 0x20 * 2 + 1> transform{};
    hex(before.transform, transform);
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=gameplay stage=client-squad-spawn path=%s result=%u mode=%u object=0x%08X "
        "sources=%d/%d last_source=0x%08X source_offset=0x%016llX "
        "transform_readable=%u transform=%s",
        path,
        static_cast<unsigned>(result & 0xFFU),
        mode,
        static_cast<std::uint32_t>(object),
        before.count,
        after.count,
        before.lastSourceTag,
        static_cast<unsigned long long>(before.lastSourceOffset),
        before.transformReadable ? 1U : 0U,
        transform.data());
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Preserves materialization while observing the exact game-owned descriptor and output handle. */
__declspec(noinline) std::uint8_t __fastcall
materialize_body(void* context, const void* descriptor, std::uint32_t* output) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::authoredObjectMaterialize, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Materialize>(lease.original);
    Snapshot snapshot{};
    if (lease.accepting) {
        (void)inspect_descriptor(descriptor, snapshot);
    }
    std::uint8_t result{};
    __try {
        if (call != nullptr) {
            result = call(context, descriptor, output);
        }
        if (lease.accepting && snapshot.descriptorReadable) {
            inspect_output(output, snapshot);
            report(snapshot, result);
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

/** Preserves the activity spawn state machine while observing its queue boundary. */
__declspec(noinline) bool __fastcall state_body(void* context) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::authoredSpawnState, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<ServiceState>(lease.original);
    StateSnapshot before{};
    StateSnapshot after{};
    if (lease.accepting) {
        (void)inspect_state(context, before);
    }
    bool result{};
    __try {
        if (call != nullptr) {
            result = call(context);
        }
        if (lease.accepting && before.readable && inspect_state(context, after)) {
            report_state(before, after, result);
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

/** Preserves spawn-controller initialization while observing its authored source list. */
__declspec(noinline) void __fastcall
initialize_body(void* context, const void* transform, const std::int32_t* sourceList) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::authoredSpawnInitialize, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Initialize>(lease.original);
    StateSnapshot before{};
    StateSnapshot after{};
    std::int32_t inputCount{-1};
    bool inputReadable{};
    if (lease.accepting) {
        (void)inspect_state(context, before);
        inputReadable = inspect_input_count(sourceList, inputCount);
    }
    __try {
        if (call != nullptr) {
            call(context, transform, sourceList);
        }
        if (lease.accepting && before.readable && inspect_state(context, after)) {
            report_initialize(before, after, inputCount, inputReadable);
        }
    } __finally {
        coordinator::g_callEgress();
    }
}

/** Preserves descriptor generation while recording the first emitted squad/object request. */
__declspec(noinline) void __fastcall queue_builder_body(void* context) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::authoredSpawnQueueBuilder, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<QueueBuilder>(lease.original);
    StateSnapshot before{};
    StateSnapshot after{};
    if (lease.accepting) {
        (void)inspect_state(context, before);
    }
    __try {
        if (call != nullptr) {
            call(context);
        }
        if (lease.accepting && before.readable && inspect_state(context, after)) {
            report_queue(before, after);
        }
    } __finally {
        coordinator::g_callEgress();
    }
}

/** Preserves controller teardown while recording whether it ever held authored sources. */
__declspec(noinline) void __fastcall reset_body(void* context) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::authoredSpawnReset, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<ResetController>(lease.original);
    StateSnapshot before{};
    StateSnapshot after{};
    if (lease.accepting) {
        (void)inspect_state(context, before);
    }
    __try {
        if (call != nullptr) {
            call(context);
        }
        if (lease.accepting && before.readable && inspect_state(context, after)) {
            report_reset(before, after);
        }
    } __finally {
        coordinator::g_callEgress();
    }
}

/** Preserves generic callback resolution and reports only the authored-controller class family. */
__declspec(noinline) std::uintptr_t __fastcall
callback_resolver_body(const std::uint32_t* request) noexcept {
    const void* caller = _ReturnAddress();
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::authoredCallbackResolver, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<CallbackResolver>(lease.original);
    std::uint32_t classId{};
    std::uint32_t ordinal{};
    bool readable{};
    if (lease.accepting && request != nullptr) {
        __try {
            classId = request[0];
            ordinal = request[1];
            readable = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            classId = 0;
            ordinal = 0;
        }
    }
    std::uintptr_t result{};
    __try {
        if (call != nullptr) {
            result = call(request);
        }
        if (lease.accepting && readable) {
            if (classId >= kAuthoredControllerFamilyFirst
                && classId <= kAuthoredControllerFamilyLast && g_callbackLink.active) {
                ++g_callbackLink.targetCount;
            }
            report_callback_resolver(classId, ordinal, result, caller);
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

/** Preserves resource callback linking while exposing the tag that owns nested resolutions. */
__declspec(noinline) void* __fastcall
callback_link_body(void* output, std::int32_t kind, std::uint32_t ownerTag) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::authoredCallbackLink, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<CallbackLink>(lease.original);
    const CallbackLinkContext previous = g_callbackLink;
    if (lease.accepting && kind == 4) {
        g_callbackLink = {ownerTag, nullptr, 0, true};
    }
    void* result = output;
    __try {
        if (call != nullptr) {
            result = call(output, kind, ownerTag);
        }
    } __finally {
        g_callbackLink = previous;
        coordinator::g_callEgress();
    }
    return result;
}

/** Preserves decoded callback-table linking while copying only four known tiny resources. */
__declspec(noinline) void __fastcall callback_table_link_body(const void* resource) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::authoredCallbackTableLink, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<CallbackTableLink>(lease.original);
    const CallbackLinkContext previous = g_callbackLink;
    const std::size_t size =
        lease.accepting && previous.active ? callback_resource_size(previous.ownerTag) : 0;
    std::array<std::byte, kMaximumCallbackResourceSize> bytes{};
    bool readable{};
    if (size != 0 && resource != nullptr) {
        __try {
            std::memcpy(bytes.data(), resource, size);
            readable = true;
            g_callbackLink.resource = resource;
            g_callbackLink.targetCount = 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            readable = false;
        }
    }
    __try {
        if (call != nullptr) {
            call(resource);
        }
        if (lease.accepting && readable && g_callbackLink.targetCount != 0) {
            report_callback_resource(previous.ownerTag,
                                     resource,
                                     std::span(bytes).first(size),
                                     g_callbackLink.targetCount);
        }
    } __finally {
        g_callbackLink = previous;
        coordinator::g_callEgress();
    }
}

/** Preserves the game-owned spawn-at-transform helper while observing its source and result. */
__declspec(noinline) std::uintptr_t __fastcall
squad_transform_body(std::uint32_t mode, const void* transform, std::int32_t* sourceList) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::authoredSquadSpawnTransform, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<SquadTransform>(lease.original);
    SquadSourceSnapshot before{};
    SquadSourceSnapshot after{};
    if (lease.accepting) {
        (void)inspect_squad_source(sourceList, before);
        inspect_transform(transform, before);
    }
    std::uintptr_t result{};
    __try {
        if (call != nullptr) {
            result = call(mode, transform, sourceList);
        }
        if (lease.accepting && before.readable && inspect_squad_source(sourceList, after)) {
            report_squad("transform", before, after, mode, -1, result);
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

/** Preserves the game-owned spawn-at-object helper while observing its source and result. */
__declspec(noinline) std::uintptr_t __fastcall
squad_object_body(std::uint32_t mode, std::int32_t object, std::int32_t* sourceList) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::authoredSquadSpawnObject, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<SquadObject>(lease.original);
    SquadSourceSnapshot before{};
    SquadSourceSnapshot after{};
    if (lease.accepting) {
        (void)inspect_squad_source(sourceList, before);
    }
    std::uintptr_t result{};
    __try {
        if (call != nullptr) {
            result = call(mode, object, sourceList);
        }
        if (lease.accepting && before.readable && inspect_squad_source(sourceList, after)) {
            report_squad("object", before, after, mode, object, result);
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

/** Preserves the current-bubble authority decision while recording its observed states. */
__declspec(noinline) bool __fastcall current_bubble_authority_body() noexcept {
    const void* caller = _ReturnAddress();
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::currentBubbleAuthority, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<AuthorityPredicate>(lease.original);
    bool result{};
    const AuthoritySampleContext previous = g_authoritySample;
    g_authoritySample = {AuthoritySampleKind::currentBubble, false};
    __try {
        if (call != nullptr) {
            result = call();
        }
        if (lease.accepting) {
            report_authority("current-bubble", result, 0, caller);
        }
    } __finally {
        g_authoritySample = previous;
        coordinator::g_callEgress();
    }
    return result;
}

/** Preserves the domain-authority decision while recording its observed states. */
__declspec(noinline) bool __fastcall domain_authority_body() noexcept {
    const void* caller = _ReturnAddress();
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, HookSlot::domainAuthority, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<AuthorityPredicate>(lease.original);
    bool result{};
    const AuthoritySampleContext previous = g_authoritySample;
    g_authoritySample = {AuthoritySampleKind::domain, false};
    __try {
        if (call != nullptr) {
            result = call();
        }
        if (lease.accepting) {
            report_authority("domain", result, 2, caller);
        }
    } __finally {
        g_authoritySample = previous;
        coordinator::g_callEgress();
    }
    return result;
}

/** Preserves the leaf authority decision while exposing its granted and pending inputs. */
__declspec(noinline) bool __fastcall authority_state_test_body(const void* manager,
                                                               const std::uint8_t* index) noexcept {
    const AuthoritySampleContext sample = g_authoritySample;
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::authorityStateTest, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<AuthorityStateTest>(lease.original);
    AuthorityStateSnapshot snapshot{};
    if (lease.accepting && sample.kind != AuthoritySampleKind::none) {
        (void)inspect_authority_state(manager, index, snapshot);
    }
    bool result{};
    __try {
        if (call != nullptr) {
            result = call(manager, index);
        }
        if (lease.accepting) {
            report_authority_state(sample, snapshot, result);
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

/** Preserves one decoded authority lane while exposing its token and state transition. */
__declspec(noinline) void __fastcall authority_apply_body(void* manager,
                                                          const std::uint8_t* index,
                                                          std::uint8_t mask,
                                                          const void* decoded,
                                                          const void* context) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::bubbleAuthorityApply, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<AuthorityApply>(lease.original);
    AuthorityApplySnapshot before{};
    AuthorityApplySnapshot after{};
    if (lease.accepting && mask != 0) {
        (void)inspect_authority_apply(manager, index, decoded, before);
    }
    __try {
        if (call != nullptr) {
            call(manager, index, mask, decoded, context);
        }
        if (lease.accepting && before.readable
            && inspect_authority_apply(manager, index, decoded, after)) {
            report_authority_apply(before, after, mask);
        }
    } __finally {
        coordinator::g_callEgress();
    }
}

} // namespace

void* materialize_entry_point() noexcept {
    return reinterpret_cast<void*>(&materialize_body);
}

void* state_entry_point() noexcept {
    return reinterpret_cast<void*>(&state_body);
}

void* initialize_entry_point() noexcept {
    return reinterpret_cast<void*>(&initialize_body);
}

void* queue_builder_entry_point() noexcept {
    return reinterpret_cast<void*>(&queue_builder_body);
}

void* reset_entry_point() noexcept {
    return reinterpret_cast<void*>(&reset_body);
}

void* callback_resolver_entry_point() noexcept {
    return reinterpret_cast<void*>(&callback_resolver_body);
}

void* callback_link_entry_point() noexcept {
    return reinterpret_cast<void*>(&callback_link_body);
}

void* callback_table_link_entry_point() noexcept {
    return reinterpret_cast<void*>(&callback_table_link_body);
}

void* squad_transform_entry_point() noexcept {
    return reinterpret_cast<void*>(&squad_transform_body);
}

void* squad_object_entry_point() noexcept {
    return reinterpret_cast<void*>(&squad_object_body);
}

void* current_bubble_authority_entry_point() noexcept {
    return reinterpret_cast<void*>(&current_bubble_authority_body);
}

void* domain_authority_entry_point() noexcept {
    return reinterpret_cast<void*>(&domain_authority_body);
}

void* authority_state_test_entry_point() noexcept {
    return reinterpret_cast<void*>(&authority_state_test_body);
}

void* bubble_authority_apply_entry_point() noexcept {
    return reinterpret_cast<void*>(&authority_apply_body);
}

void sample_authority_after_roster() noexcept {
    bool current{};
    const bool currentSampled = sample_authority(
        HookSlot::currentBubbleAuthority, AuthoritySampleKind::currentBubble, current);
    if (currentSampled) {
        report_roster_authority("current-bubble", current, 0);
    }
    bool domain{};
    const bool domainSampled =
        sample_authority(HookSlot::domainAuthority, AuthoritySampleKind::domain, domain);
    if (domainSampled) {
        report_roster_authority("domain", domain, 2);
    }
    g_rosterAuthorityReady.store(currentSampled && domainSampled && current && domain,
                                 std::memory_order_relaxed);
    g_rosterAuthoritySampledAt.store(GetTickCount64(), std::memory_order_release);
}

bool roster_authority_ready() noexcept {
    const std::uint64_t sampledAt = g_rosterAuthoritySampledAt.load(std::memory_order_acquire);
    const std::uint64_t now = GetTickCount64();
    return sampledAt != 0 && now >= sampledAt && now - sampledAt < kRosterAuthoritySampleFreshMs
           && g_rosterAuthorityReady.load(std::memory_order_relaxed);
}

void poll_direct_squad_spawn() noexcept {
    if (g_squadSpawnAttempted.load(std::memory_order_acquire)
        || g_authorityBootstrapOpenedAt.load(std::memory_order_acquire) == 0) {
        return;
    }

    bool currentAuthority{};
    bool domainAuthority{};
    if (!sample_authority(
            HookSlot::currentBubbleAuthority, AuthoritySampleKind::currentBubble, currentAuthority)
        || !sample_authority(
            HookSlot::domainAuthority, AuthoritySampleKind::domain, domainAuthority)
        || !currentAuthority || !domainAuthority) {
        return;
    }
    const player::position::Snapshot player = player::position::snapshot();
    if (!player.present) {
        return;
    }

    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::authoredSquadSpawnTransform, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<SquadTransform>(lease.original);
    if (!lease.accepting || call == nullptr) {
        coordinator::g_callEgress();
        return;
    }
    bool expected = false;
    if (!g_squadSpawnAttempted.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        coordinator::g_callEgress();
        return;
    }

    g_directSquadRequest = {};
    g_directSquadTransform = {0.0F,
                              0.0F,
                              0.0F,
                              1.0F,
                              player.position[0] + 6.0F,
                              player.position[1],
                              player.position[2],
                              0.0F};
    SquadSourceSnapshot before{};
    SquadSourceSnapshot after{};
    (void)inspect_squad_source(reinterpret_cast<const std::int32_t*>(&g_directSquadRequest),
                               before);
    inspect_transform(g_directSquadTransform.data(), before);

    std::uintptr_t result{};
    bool faulted = false;
    __try {
        result = call(kSquadSpawnMode,
                      g_directSquadTransform.data(),
                      reinterpret_cast<std::int32_t*>(&g_directSquadRequest));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
    }
    coordinator::g_callEgress();

    if (inspect_squad_source(reinterpret_cast<const std::int32_t*>(&g_directSquadRequest), after)) {
        report_squad("direct-transform", before, after, kSquadSpawnMode, -1, result);
    }
    if (faulted) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=gameplay stage=client-squad-spawn path=direct-transform result=fault");
    }
}

void reset() noexcept {
    g_authoritySeen.store(0, std::memory_order_relaxed);
    g_rosterAuthoritySeen.store(0, std::memory_order_relaxed);
    g_authorityBootstrapSeen.store(0, std::memory_order_relaxed);
    g_authorityBootstrapOpenedAt.store(0, std::memory_order_relaxed);
    g_rosterAuthorityReady.store(false, std::memory_order_relaxed);
    g_rosterAuthoritySampledAt.store(0, std::memory_order_release);
    g_squadSpawnAttempted.store(false, std::memory_order_relaxed);
    g_directSquadRequest = {};
    g_directSquadTransform = {};
    for (std::atomic_uint64_t& seen : g_seen) {
        seen.store(0, std::memory_order_relaxed);
    }
    for (std::atomic_uint64_t& seen : g_stateSeen) {
        seen.store(0, std::memory_order_relaxed);
    }
    for (std::atomic_uint64_t& seen : g_initializeSeen) {
        seen.store(0, std::memory_order_relaxed);
    }
    for (std::atomic_uint64_t& seen : g_queueSeen) {
        seen.store(0, std::memory_order_relaxed);
    }
    for (std::atomic_uint64_t& seen : g_resetSeen) {
        seen.store(0, std::memory_order_relaxed);
    }
    for (std::atomic_uint64_t& seen : g_squadSeen) {
        seen.store(0, std::memory_order_relaxed);
    }
    for (std::atomic_uint64_t& seen : g_authorityStateSeen) {
        seen.store(0, std::memory_order_relaxed);
    }
    for (std::atomic_uint64_t& seen : g_authorityApplySeen) {
        seen.store(0, std::memory_order_relaxed);
    }
    for (std::atomic_uint64_t& seen : g_callbackSeen) {
        seen.store(0, std::memory_order_relaxed);
    }
    for (std::atomic_uint64_t& seen : g_callbackResourceSeen) {
        seen.store(0, std::memory_order_relaxed);
    }
    g_authoritySample = {};
    g_callbackLink = {};
}

} // namespace sunrise::client::hooks::network::authored_spawn_probe
