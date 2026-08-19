#include "account_soid_probe.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>

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
constexpr std::size_t kGlobalTimerOffset = 0x610;
constexpr std::size_t kConnectionClassOffset = 0xDECC0;
constexpr std::size_t kConnectionRecordOffset = 0x60;
constexpr std::size_t kConnectionRecordStride = 0x58;
constexpr std::size_t kConnectionSoidOffset = 0x20;
constexpr std::size_t kConnectionStateOffset = 0x28;
constexpr std::size_t kConnectionRecordSize = 0x58;
constexpr std::uint8_t kExpiredAccountState = 3;
constexpr std::size_t kReportEntryCapacity = 4;
constexpr std::size_t kObservationCapacity = 4;
constexpr ULONGLONG kReportIntervalMilliseconds = 5000;

using Validator = bool(__fastcall*)(const void*);
using Publisher = void(__fastcall*)(void*, const void*);
using SourceAccessor = const void*(__fastcall*)();
using ConnectionAccessor = const void*(__fastcall*)();

struct AccountEntry {
    std::uint64_t soid{};
    std::uint64_t stateAndFlags{};
    std::uint64_t timer{};
    std::uint64_t auxiliary{};
};

struct Snapshot {
    const void* manager{};
    const void* source{};
    const void* connectionManager{};
    std::array<AccountEntry, kAccountCapacity> targets{};
    std::array<std::uint64_t, kAccountCapacity> desired{};
    std::array<std::byte, kConnectionRecordSize> connectionRecord{};
    std::uint64_t managerHeader0{};
    std::uint64_t managerHeader1{};
    std::uint64_t sourceHeader0{};
    std::uint64_t sourceHeader1{};
    std::uint64_t globalTimer0{};
    std::uint64_t globalTimer1{};
    std::int32_t connectionStart{-1};
    std::int32_t connectionCount{-1};
    std::int32_t connectionIndex{-1};
    std::uint8_t connectionState{};
    std::size_t targetNonzero{};
    std::size_t targetExpired{};
    std::size_t desiredNonzero{};
    bool targetReadable{};
    bool sourceReadable{};
    bool connectionReadable{};
    bool connectionMatched{};
};

struct Observation {
    const void* manager{};
    std::uint64_t hash{};
    ULONGLONG lastReport{};
    std::uint64_t calls{};
    bool occupied{};
};

struct PublisherSnapshot {
    const void* input{};
    std::array<std::uint64_t, kAccountCapacity> desired{};
    std::array<std::uint16_t, kAccountCapacity> masks{};
    std::uint64_t header0{};
    std::uint64_t header1{};
    std::size_t desiredNonzero{};
    bool readable{};
};

struct PublisherObservation {
    std::uint64_t hash{};
    std::uintptr_t callerRva{};
    std::uint64_t calls{};
    bool occupied{};
};

SRWLOCK g_observationLock{SRWLOCK_INIT};
std::array<Observation, kObservationCapacity> g_observations{};
std::size_t g_replacementCursor{};
SRWLOCK g_publisherLock{SRWLOCK_INIT};
PublisherObservation g_publisherObservation{};

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

/** Copies the producer input before the native publisher replaces its singleton. */
[[nodiscard]] bool inspect_publisher(const void* inputAddress, PublisherSnapshot& output) noexcept {
    output = {};
    output.input = inputAddress;
    if (inputAddress == nullptr) {
        return false;
    }
    __try {
        const auto* const input = static_cast<const std::byte*>(inputAddress);
        std::memcpy(&output.header0, input, sizeof output.header0);
        std::memcpy(&output.header1, input + 8, sizeof output.header1);
        for (std::size_t index = 0; index < output.desired.size(); ++index) {
            const auto* const entry = input + kSourceTableOffset + index * kSourceStride;
            std::memcpy(&output.desired[index], entry, sizeof output.desired[index]);
            std::memcpy(&output.masks[index], entry + 8, sizeof output.masks[index]);
            if (output.desired[index] != 0) {
                ++output.desiredNonzero;
            }
        }
        output.readable = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output.readable = false;
        return false;
    }
}

/** Retains publisher changes so a per-frame virtual callback cannot flood the log. */
[[nodiscard]] bool record_publisher(const PublisherSnapshot& snapshot,
                                    std::uintptr_t callerRva,
                                    std::uint64_t& calls) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    hash = hash_bytes(&snapshot.header0, sizeof snapshot.header0, hash);
    hash = hash_bytes(&snapshot.header1, sizeof snapshot.header1, hash);
    hash = hash_bytes(snapshot.desired.data(), sizeof snapshot.desired, hash);
    hash = hash_bytes(snapshot.masks.data(), sizeof snapshot.masks, hash);
    hash = hash_bytes(&snapshot.readable, sizeof snapshot.readable, hash);
    AcquireSRWLockExclusive(&g_publisherLock);
    ++g_publisherObservation.calls;
    calls = g_publisherObservation.calls;
    const bool report = !g_publisherObservation.occupied || g_publisherObservation.hash != hash
                        || g_publisherObservation.callerRva != callerRva;
    g_publisherObservation.hash = hash;
    g_publisherObservation.callerRva = callerRva;
    g_publisherObservation.occupied = true;
    ReleaseSRWLockExclusive(&g_publisherLock);
    return report;
}

/** Emits the writer callsite and first desired identities in its complete input snapshot. */
void report_publisher(const PublisherSnapshot& snapshot,
                      std::uintptr_t callerRva,
                      std::uint64_t calls) noexcept {
    std::array<char, 768> line{};
    int written = std::snprintf(line.data(),
                                line.size(),
                                "ev=gameplay stage=account-soid-publish caller=+0x%llX input=%p "
                                "readable=%u h0=0x%llX h1=0x%llX nonzero=%zu calls=%llu",
                                static_cast<unsigned long long>(callerRva),
                                snapshot.input,
                                snapshot.readable ? 1U : 0U,
                                static_cast<unsigned long long>(snapshot.header0),
                                static_cast<unsigned long long>(snapshot.header1),
                                snapshot.desiredNonzero,
                                static_cast<unsigned long long>(calls));
    if (written <= 0 || static_cast<std::size_t>(written) >= line.size()) {
        return;
    }
    std::size_t used = static_cast<std::size_t>(written);
    std::size_t reported = 0;
    for (std::size_t index = 0; index < snapshot.desired.size() && reported < kReportEntryCapacity;
         ++index) {
        if (snapshot.desired[index] == 0) {
            continue;
        }
        written = std::snprintf(line.data() + used,
                                line.size() - used,
                                " s%zu[slot=%zu soid=0x%llX mask=0x%X]",
                                reported,
                                index,
                                static_cast<unsigned long long>(snapshot.desired[index]),
                                static_cast<unsigned>(snapshot.masks[index]));
        if (written <= 0 || static_cast<std::size_t>(written) >= line.size() - used) {
            break;
        }
        used += static_cast<std::size_t>(written);
        ++reported;
    }
    core::log::write(core::log::Channel::client, core::log::Level::info, {line.data(), used});
}

/** Finds the connection record that keeps an account entry in state 2. */
void inspect_connection(std::uint64_t soid, Snapshot& output) noexcept {
    const targets::game::network::Targets& resolved = targets::game::network::get();
    const auto connectionAccessor =
        reinterpret_cast<ConnectionAccessor>(resolved.accountConnectionSource);
    if (connectionAccessor == nullptr) {
        return;
    }
    output.connectionManager = connectionAccessor();
    if (output.connectionManager == nullptr) {
        return;
    }
    const auto* const manager = static_cast<const std::byte*>(output.connectionManager);
    std::memcpy(
        &output.connectionStart, manager + kConnectionClassOffset, sizeof output.connectionStart);
    std::memcpy(&output.connectionCount,
                manager + kConnectionClassOffset + sizeof output.connectionStart,
                sizeof output.connectionCount);
    if (output.connectionStart < 0 || output.connectionStart > 0x4000 || output.connectionCount < 0
        || output.connectionCount > 0x4000
        || output.connectionStart + output.connectionCount > 0x4000) {
        return;
    }
    output.connectionReadable = true;
    for (std::int32_t ordinal = 0; ordinal < output.connectionCount; ++ordinal) {
        const std::int32_t index = output.connectionStart + ordinal;
        if (index < 0) {
            continue;
        }
        const auto* const record = manager + kConnectionRecordOffset
                                   + static_cast<std::size_t>(index) * kConnectionRecordStride;
        std::uint64_t candidate = 0;
        std::memcpy(&candidate, record + kConnectionSoidOffset, sizeof candidate);
        if (candidate != soid) {
            continue;
        }
        output.connectionIndex = index;
        std::memcpy(&output.connectionState,
                    record + kConnectionStateOffset,
                    sizeof output.connectionState);
        std::memcpy(output.connectionRecord.data(), record, output.connectionRecord.size());
        output.connectionMatched = true;
        break;
    }
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
        std::memcpy(&output.managerHeader0, manager, sizeof output.managerHeader0);
        std::memcpy(&output.managerHeader1, manager + 8, sizeof output.managerHeader1);
        std::memcpy(&output.globalTimer0, manager + kGlobalTimerOffset, sizeof output.globalTimer0);
        std::memcpy(
            &output.globalTimer1, manager + kGlobalTimerOffset + 8, sizeof output.globalTimer1);
        std::uint64_t firstSoid = 0;
        for (std::size_t index = 0; index < output.targets.size(); ++index) {
            const auto* const entry = manager + kTargetTableOffset + index * kTargetStride;
            std::memcpy(&output.targets[index], entry, sizeof output.targets[index]);
            if (output.targets[index].soid != 0) {
                if (firstSoid == 0) {
                    firstSoid = output.targets[index].soid;
                }
                ++output.targetNonzero;
                if (static_cast<std::uint8_t>(output.targets[index].stateAndFlags)
                    == kExpiredAccountState) {
                    ++output.targetExpired;
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
            std::memcpy(&output.sourceHeader0, source, sizeof output.sourceHeader0);
            std::memcpy(&output.sourceHeader1, source + 8, sizeof output.sourceHeader1);
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
        inspect_connection(firstSoid, output);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output.targetReadable = false;
        output.sourceReadable = false;
        output.connectionReadable = false;
        return false;
    }
}

/** Reports every table transition and periodically samples a still-invalid account table. */
[[nodiscard]] bool record(const Snapshot& snapshot, ULONGLONG now, std::uint64_t& calls) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    hash = hash_bytes(snapshot.targets.data(), sizeof snapshot.targets, hash);
    hash = hash_bytes(snapshot.desired.data(), sizeof snapshot.desired, hash);
    hash = hash_bytes(&snapshot.managerHeader0, sizeof snapshot.managerHeader0, hash);
    hash = hash_bytes(&snapshot.managerHeader1, sizeof snapshot.managerHeader1, hash);
    hash = hash_bytes(&snapshot.sourceHeader0, sizeof snapshot.sourceHeader0, hash);
    hash = hash_bytes(&snapshot.sourceHeader1, sizeof snapshot.sourceHeader1, hash);
    hash = hash_bytes(&snapshot.connectionState, sizeof snapshot.connectionState, hash);
    hash = hash_bytes(&snapshot.connectionMatched, sizeof snapshot.connectionMatched, hash);
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
    } else if (snapshot.targetExpired == 0
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

/** Converts the matched 0x58-byte connection record to a byte-exact exemplar. */
void connection_hex(const Snapshot& snapshot,
                    std::array<char, kConnectionRecordSize * 2 + 1>& out) {
    constexpr char kDigits[] = "0123456789ABCDEF";
    for (std::size_t index = 0; index < snapshot.connectionRecord.size(); ++index) {
        const std::uint8_t value = std::to_integer<std::uint8_t>(snapshot.connectionRecord[index]);
        out[index * 2] = kDigits[value >> 4];
        out[index * 2 + 1] = kDigits[value & 0x0F];
    }
    out[snapshot.connectionRecord.size() * 2] = '\0';
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
    std::array<char, kConnectionRecordSize * 2 + 1> connectionHex{};
    if (snapshot.connectionMatched) {
        connection_hex(snapshot, connectionHex);
    }
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=gameplay stage=account-soids predicate=%s manager=%p target_readable=%u "
                      "target_nonzero=%zu target_expired=%zu mh0=0x%llX mh1=0x%llX "
                      "timer=0x%llX/0x%llX source=%p source_readable=%u source_nonzero=%zu "
                      "sh0=0x%llX sh1=0x%llX conn=%p conn_readable=%u conn_start=%d conn_count=%d "
                      "conn_match=%u conn_index=%d conn_state=%u conn_bytes=%s calls=%llu",
                      predicate ? "allow" : "block",
                      snapshot.manager,
                      snapshot.targetReadable ? 1U : 0U,
                      snapshot.targetNonzero,
                      snapshot.targetExpired,
                      static_cast<unsigned long long>(snapshot.managerHeader0),
                      static_cast<unsigned long long>(snapshot.managerHeader1),
                      static_cast<unsigned long long>(snapshot.globalTimer0),
                      static_cast<unsigned long long>(snapshot.globalTimer1),
                      snapshot.source,
                      snapshot.sourceReadable ? 1U : 0U,
                      snapshot.desiredNonzero,
                      static_cast<unsigned long long>(snapshot.sourceHeader0),
                      static_cast<unsigned long long>(snapshot.sourceHeader1),
                      snapshot.connectionManager,
                      snapshot.connectionReadable ? 1U : 0U,
                      snapshot.connectionStart,
                      snapshot.connectionCount,
                      snapshot.connectionMatched ? 1U : 0U,
                      snapshot.connectionIndex,
                      static_cast<unsigned>(snapshot.connectionState),
                      connectionHex.data(),
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

/** Preserves the native snapshot copy while exposing its indirect caller and complete input. */
__declspec(noinline) void __fastcall publisher_body(void* publisher, const void* input) noexcept {
    void* const caller = _ReturnAddress();
    const auto* const image = reinterpret_cast<const std::byte*>(GetModuleHandleW(nullptr));
    const std::uintptr_t callerRva =
        image != nullptr && caller != nullptr ? static_cast<const std::byte*>(caller) - image : 0;
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::accountSoidPublisher, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Publisher>(lease.original);
    __try {
        PublisherSnapshot snapshot{};
        const bool inspected = lease.accepting && inspect_publisher(input, snapshot);
        if (call != nullptr) {
            call(publisher, input);
        }
        if (lease.accepting && inspected) {
            std::uint64_t calls = 0;
            if (record_publisher(snapshot, callerRva, calls)) {
                report_publisher(snapshot, callerRva, calls);
            }
        }
    } __finally {
        coordinator::g_callEgress();
    }
}

} // namespace

void* validator_entry_point() noexcept {
    return reinterpret_cast<void*>(&validator_body);
}

void* publisher_entry_point() noexcept {
    return reinterpret_cast<void*>(&publisher_body);
}

void reset() noexcept {
    AcquireSRWLockExclusive(&g_observationLock);
    g_observations = {};
    g_replacementCursor = 0;
    ReleaseSRWLockExclusive(&g_observationLock);
    AcquireSRWLockExclusive(&g_publisherLock);
    g_publisherObservation = {};
    ReleaseSRWLockExclusive(&g_publisherLock);
}

} // namespace sunrise::client::hooks::network::account_soid_probe
