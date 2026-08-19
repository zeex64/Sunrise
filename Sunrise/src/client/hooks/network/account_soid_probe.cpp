#include "account_soid_probe.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../../core/logging/log.h"
#include "../../targets/game.h"
#include "coordinator/network_call_coordinator.h"
#include "platform.h"

namespace sunrise::client::hooks::network::account_soid_probe {
namespace {

constexpr std::size_t kAccountCapacity = 32;
constexpr std::size_t kTargetTableOffset = 0x210;
constexpr std::size_t kTargetStride = 0x20;
constexpr std::size_t kSourceTableOffset = 0x10;
constexpr std::size_t kSourceStride = 0x10;
constexpr std::uint8_t kValidAccountState = 3;
constexpr std::size_t kReportEntryCapacity = 4;
constexpr std::size_t kObservationCapacity = 4;
constexpr ULONGLONG kReportIntervalMilliseconds = 5000;

using Validator = bool(__fastcall*)(const void*);
using SourceAccessor = const void*(__fastcall*)();

struct AccountEntry {
    std::uint64_t soid{};
    std::uint64_t stateAndFlags{};
    std::uint64_t timer{};
    std::uint64_t auxiliary{};
};

struct Snapshot {
    const void* manager{};
    const void* source{};
    std::array<AccountEntry, kAccountCapacity> targets{};
    std::array<std::uint64_t, kAccountCapacity> desired{};
    std::size_t targetNonzero{};
    std::size_t targetValid{};
    std::size_t desiredNonzero{};
    bool targetReadable{};
    bool sourceReadable{};
};

struct Observation {
    const void* manager{};
    std::uint64_t hash{};
    ULONGLONG lastReport{};
    std::uint64_t calls{};
    bool occupied{};
};

SRWLOCK g_observationLock{SRWLOCK_INIT};
std::array<Observation, kObservationCapacity> g_observations{};
std::size_t g_replacementCursor{};

/** FNV-1a keeps the complete target and source images comparable without retaining pointers. */
[[nodiscard]] std::uint64_t
hash_bytes(const void* address, std::size_t size, std::uint64_t hash) noexcept {
    const auto* const bytes = static_cast<const std::byte*>(address);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= std::to_integer<std::uint8_t>(bytes[index]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

/** Copies the exact table consumed by FUN_140BE2070 and its upstream desired-SOID list. */
[[nodiscard]] bool inspect(const void* managerAddress, Snapshot& output) noexcept {
    output = {};
    output.manager = managerAddress;
    if (managerAddress == nullptr) {
        return false;
    }
    __try {
        const auto* const manager = static_cast<const std::byte*>(managerAddress);
        for (std::size_t index = 0; index < output.targets.size(); ++index) {
            const auto* const entry = manager + kTargetTableOffset + index * kTargetStride;
            std::memcpy(&output.targets[index], entry, sizeof output.targets[index]);
            if (output.targets[index].soid != 0) {
                ++output.targetNonzero;
                if (static_cast<std::uint8_t>(output.targets[index].stateAndFlags)
                    == kValidAccountState) {
                    ++output.targetValid;
                }
            }
        }
        output.targetReadable = true;

        const targets::game::network::Targets& resolved = targets::game::network::get();
        const auto sourceAccessor = reinterpret_cast<SourceAccessor>(resolved.accountSoidSource);
        if (sourceAccessor != nullptr) {
            output.source = sourceAccessor();
        }
        if (output.source != nullptr) {
            const auto* const source = static_cast<const std::byte*>(output.source);
            for (std::size_t index = 0; index < output.desired.size(); ++index) {
                std::memcpy(&output.desired[index],
                            source + kSourceTableOffset + index * kSourceStride,
                            sizeof output.desired[index]);
                if (output.desired[index] != 0) {
                    ++output.desiredNonzero;
                }
            }
            output.sourceReadable = true;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output.targetReadable = false;
        output.sourceReadable = false;
        return false;
    }
}

/** Reports every table transition and periodically samples a still-invalid account table. */
[[nodiscard]] bool record(const Snapshot& snapshot, ULONGLONG now, std::uint64_t& calls) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    hash = hash_bytes(snapshot.targets.data(), sizeof snapshot.targets, hash);
    hash = hash_bytes(snapshot.desired.data(), sizeof snapshot.desired, hash);
    hash = hash_bytes(&snapshot.targetReadable, sizeof snapshot.targetReadable, hash);
    hash = hash_bytes(&snapshot.sourceReadable, sizeof snapshot.sourceReadable, hash);

    bool report = false;
    AcquireSRWLockExclusive(&g_observationLock);
    Observation* destination = nullptr;
    for (Observation& entry : g_observations) {
        if (entry.occupied && entry.manager == snapshot.manager) {
            destination = &entry;
            break;
        }
        if (destination == nullptr && !entry.occupied) {
            destination = &entry;
        }
    }
    if (destination == nullptr) {
        destination = &g_observations[g_replacementCursor % g_observations.size()];
        ++g_replacementCursor;
    }
    if (!destination->occupied || destination->manager != snapshot.manager) {
        *destination = {};
        destination->manager = snapshot.manager;
        destination->occupied = true;
        report = true;
    } else if (destination->hash != hash) {
        report = true;
    } else if (snapshot.targetValid == 0
               && now - destination->lastReport >= kReportIntervalMilliseconds) {
        report = true;
    }
    destination->hash = hash;
    ++destination->calls;
    calls = destination->calls;
    if (report) {
        destination->lastReport = now;
    }
    ReleaseSRWLockExclusive(&g_observationLock);
    return report;
}

/** Adds the first nonempty target records to the bounded diagnostic line. */
void append_targets(const Snapshot& snapshot,
                    std::array<char, 1280>& line,
                    std::size_t& used) noexcept {
    std::size_t reported = 0;
    for (std::size_t index = 0; index < snapshot.targets.size() && reported < kReportEntryCapacity;
         ++index) {
        const AccountEntry& entry = snapshot.targets[index];
        if (entry.soid == 0) {
            continue;
        }
        const int written = std::snprintf(
            line.data() + used,
            line.size() - used,
            " t%zu[slot=%zu soid=0x%llX state=%u flags=0x%llX timer=0x%llX aux=0x%llX]",
            reported,
            index,
            static_cast<unsigned long long>(entry.soid),
            static_cast<unsigned>(static_cast<std::uint8_t>(entry.stateAndFlags)),
            static_cast<unsigned long long>(entry.stateAndFlags),
            static_cast<unsigned long long>(entry.timer),
            static_cast<unsigned long long>(entry.auxiliary));
        if (written <= 0 || static_cast<std::size_t>(written) >= line.size() - used) {
            return;
        }
        used += static_cast<std::size_t>(written);
        ++reported;
    }
}

/** Adds the first desired source SOIDs to the bounded diagnostic line. */
void append_desired(const Snapshot& snapshot,
                    std::array<char, 1280>& line,
                    std::size_t& used) noexcept {
    std::size_t reported = 0;
    for (std::size_t index = 0; index < snapshot.desired.size() && reported < kReportEntryCapacity;
         ++index) {
        if (snapshot.desired[index] == 0) {
            continue;
        }
        const int written = std::snprintf(line.data() + used,
                                          line.size() - used,
                                          " s%zu[slot=%zu soid=0x%llX]",
                                          reported,
                                          index,
                                          static_cast<unsigned long long>(snapshot.desired[index]));
        if (written <= 0 || static_cast<std::size_t>(written) >= line.size() - used) {
            return;
        }
        used += static_cast<std::size_t>(written);
        ++reported;
    }
}

/** Emits one compact comparison of the prerequisite table and the source feeding it. */
void report(const Snapshot& snapshot, bool predicate, std::uint64_t calls) noexcept {
    std::array<char, 1280> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=gameplay stage=account-soids predicate=%s manager=%p target_readable=%u "
        "target_nonzero=%zu target_valid=%zu source=%p source_readable=%u source_nonzero=%zu "
        "calls=%llu",
        predicate ? "allow" : "block",
        snapshot.manager,
        snapshot.targetReadable ? 1U : 0U,
        snapshot.targetNonzero,
        snapshot.targetValid,
        snapshot.source,
        snapshot.sourceReadable ? 1U : 0U,
        snapshot.desiredNonzero,
        static_cast<unsigned long long>(calls));
    if (written <= 0 || static_cast<std::size_t>(written) >= line.size()) {
        return;
    }
    std::size_t used = static_cast<std::size_t>(written);
    append_targets(snapshot, line, used);
    append_desired(snapshot, line, used);
    core::log::write(core::log::Channel::client, core::log::Level::info, {line.data(), used});
}

/** Preserves the prerequisite predicate while exposing the account identity state it consumes. */
__declspec(noinline) bool __fastcall validator_body(const void* manager) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::accountSoidValidator, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Validator>(lease.original);
    bool result = true;
    __try {
        if (call != nullptr) {
            result = call(manager);
        }
        if (lease.accepting) {
            Snapshot snapshot{};
            (void)inspect(manager, snapshot);
            std::uint64_t calls = 0;
            if (record(snapshot, GetTickCount64(), calls)) {
                report(snapshot, result, calls);
            }
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

} // namespace

void* validator_entry_point() noexcept {
    return reinterpret_cast<void*>(&validator_body);
}

void reset() noexcept {
    AcquireSRWLockExclusive(&g_observationLock);
    g_observations = {};
    g_replacementCursor = 0;
    ReleaseSRWLockExclusive(&g_observationLock);
}

} // namespace sunrise::client::hooks::network::account_soid_probe
