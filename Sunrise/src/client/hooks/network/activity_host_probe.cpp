#include "activity_host_probe.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../../core/logging/log.h"
#include "coordinator/network_call_coordinator.h"
#include "platform.h"

namespace sunrise::client::hooks::network::activity_host_probe {
namespace {

/** Exact 32-byte native value decoded for registry parameter 3. */
struct NativeActivityHost {
    std::uint64_t selectionId{};
    std::uint64_t hostId{};
    std::uint32_t memberMask{};
    std::uint32_t address{};
    std::uint16_t port{};
    std::array<std::byte, 6> padding{};
};
static_assert(sizeof(NativeActivityHost) == 0x20);

struct HostObservation {
    NativeActivityHost value{};
    bool occupied{};
};

struct ConnectionObservation {
    std::int32_t family{};
    std::int32_t role{};
    std::uint32_t state{};
    std::uint64_t sessionLow{};
    std::uint64_t sessionHigh{};
    bool occupied{};
};

/** A transition uses at most fireteam, current-group, and target-group activity hosts. */
constexpr std::size_t kObservationCapacity = 8;
SRWLOCK g_observationLock{SRWLOCK_INIT};
std::array<HostObservation, kObservationCapacity> g_hosts{};
std::array<ConnectionObservation, kObservationCapacity> g_connections{};
std::size_t g_hostCursor{};
std::size_t g_connectionCursor{};

using Decoder = bool(__fastcall*)(void*, NativeActivityHost*);
using ConnectionState = void(__fastcall*)(void*, const char*, std::uint32_t);

/** Records each distinct decoded host once. */
[[nodiscard]] bool observe_host(const NativeActivityHost& value) noexcept {
    bool report = false;
    AcquireSRWLockExclusive(&g_observationLock);
    HostObservation* destination = nullptr;
    for (HostObservation& entry : g_hosts) {
        if (entry.occupied && std::memcmp(&entry.value, &value, sizeof(value)) == 0) {
            destination = &entry;
            break;
        }
        if (destination == nullptr && !entry.occupied) {
            destination = &entry;
        }
    }
    if (destination == nullptr) {
        destination = &g_hosts[g_hostCursor % g_hosts.size()];
        ++g_hostCursor;
    }
    if (!destination->occupied || std::memcmp(&destination->value, &value, sizeof(value)) != 0) {
        destination->value = value;
        destination->occupied = true;
        report = true;
    }
    ReleaseSRWLockExclusive(&g_observationLock);
    return report;
}

/** Copies the native 128-bit session value without trusting its lifetime after the callback. */
void copy_session(const char* source, ConnectionObservation& output) noexcept {
    if (source == nullptr) {
        return;
    }
    __try {
        const auto* const bytes = reinterpret_cast<const std::byte*>(source);
        output.sessionLow = *reinterpret_cast<const std::uint64_t*>(bytes);
        output.sessionHigh = *reinterpret_cast<const std::uint64_t*>(bytes + sizeof(std::uint64_t));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output.sessionLow = 0;
        output.sessionHigh = 0;
    }
}

/** Records only native connection-state changes. */
[[nodiscard]] bool observe_connection(const ConnectionObservation& value) noexcept {
    bool report = false;
    AcquireSRWLockExclusive(&g_observationLock);
    ConnectionObservation* destination = nullptr;
    for (ConnectionObservation& entry : g_connections) {
        if (entry.occupied && entry.family == value.family && entry.role == value.role) {
            destination = &entry;
            break;
        }
        if (destination == nullptr && !entry.occupied) {
            destination = &entry;
        }
    }
    if (destination == nullptr) {
        destination = &g_connections[g_connectionCursor % g_connections.size()];
        ++g_connectionCursor;
    }
    if (!destination->occupied || destination->state != value.state
        || destination->sessionLow != value.sessionLow
        || destination->sessionHigh != value.sessionHigh) {
        *destination = value;
        destination->occupied = true;
        report = true;
    }
    ReleaseSRWLockExclusive(&g_observationLock);
    return report;
}

/** Preserves the decoder and reports the value that actually reached native storage. */
__declspec(noinline) bool __fastcall decoder_body(void* reader, NativeActivityHost* output) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::activityHostDecoder, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Decoder>(lease.original);
    bool decoded = false;
    NativeActivityHost snapshot{};
    bool readable = false;
    __try {
        if (call != nullptr) {
            decoded = call(reader, output);
        }
        if (decoded && lease.accepting && output != nullptr) {
            snapshot = *output;
            readable = true;
        }
        if (readable && observe_host(snapshot)) {
            std::array<char, 256> line{};
            const int written = std::snprintf(
                line.data(),
                line.size(),
                "ev=gameplay stage=activity-host-decode result=ok selection=0x%016llX "
                "host=0x%016llX members=0x%08X address=0x%08X port=%u",
                static_cast<unsigned long long>(snapshot.selectionId),
                static_cast<unsigned long long>(snapshot.hostId),
                snapshot.memberMask,
                snapshot.address,
                static_cast<unsigned>(snapshot.port));
            if (written > 0) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return decoded;
}

/** Preserves the publisher and reports native fireteam/current/target connection state. */
__declspec(noinline) void __fastcall
connection_state_body(void* connection, const char* session, std::uint32_t state) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::activityHostConnectionState, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<ConnectionState>(lease.original);
    ConnectionObservation observation{};
    bool readable = false;
    __try {
        if (call != nullptr) {
            call(connection, session, state);
        }
        if (lease.accepting && connection != nullptr) {
            const auto* const bytes = static_cast<const std::byte*>(connection);
            observation.family = *reinterpret_cast<const std::int32_t*>(bytes + 0x18);
            observation.role = *reinterpret_cast<const std::int32_t*>(bytes + 0x1C);
            observation.state = state;
            copy_session(session, observation);
            readable = true;
        }
        if (readable && observe_connection(observation)) {
            std::array<char, 256> line{};
            const int written = std::snprintf(
                line.data(),
                line.size(),
                "ev=gameplay stage=activity-host-link family=%d role=%d state=%u "
                "session=0x%016llX:0x%016llX connected=%u",
                observation.family,
                observation.role,
                observation.state,
                static_cast<unsigned long long>(observation.sessionHigh),
                static_cast<unsigned long long>(observation.sessionLow),
                observation.sessionLow == 0 && observation.sessionHigh == 0 ? 0U : 1U);
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

void* decoder_entry_point() noexcept {
    return reinterpret_cast<void*>(&decoder_body);
}

void* connection_state_entry_point() noexcept {
    return reinterpret_cast<void*>(&connection_state_body);
}

void reset() noexcept {
    AcquireSRWLockExclusive(&g_observationLock);
    g_hosts = {};
    g_connections = {};
    g_hostCursor = 0;
    g_connectionCursor = 0;
    ReleaseSRWLockExclusive(&g_observationLock);
}

} // namespace sunrise::client::hooks::network::activity_host_probe
