#include "view_message_probe.h"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstdio>

#include "../../../core/logging/log.h"
#include "coordinator/network_call_coordinator.h"
#include "platform.h"

namespace sunrise::client::hooks::network::view_message_probe {
namespace {

/** Native networking retains at most three session families, with overlap during transitions. */
constexpr std::size_t kObservationCapacity = 8;

struct Observation {
    std::uint64_t token{};
    bool found{};
    bool occupied{};
};

SRWLOCK g_observationLock{SRWLOCK_INIT};
std::array<Observation, kObservationCapacity> g_observations{};
std::size_t g_replacementCursor{};

using Lookup = void*(__fastcall*)(void*, std::uint64_t);

/** Logs only a token's first lookup and a later missing/found transition. */
void observe(std::uint64_t token, void* view) noexcept {
    const bool found = view != nullptr;
    bool report = false;
    AcquireSRWLockExclusive(&g_observationLock);
    Observation* destination = nullptr;
    for (Observation& entry : g_observations) {
        if (entry.occupied && entry.token == token) {
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
    if (!destination->occupied || destination->token != token || destination->found != found) {
        report = true;
        destination->token = token;
        destination->found = found;
        destination->occupied = true;
    }
    ReleaseSRWLockExclusive(&g_observationLock);

    if (report) {
        std::array<char, 160> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=gameplay stage=view-lookup result=%s token=0x%llX view=%p",
            found ? "found" : "missing",
            static_cast<unsigned long long>(token),
            view);
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
}

/** Preserves native lookup behavior while exposing whether message 40 reached its handler. */
__declspec(noinline) void* __fastcall lookup_body(void* owner, std::uint64_t token) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::viewMessageLookup, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Lookup>(lease.original);
    void* view = nullptr;
    __try {
        if (call != nullptr) {
            view = call(owner, token);
        }
        if (lease.accepting) {
            observe(token, view);
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return view;
}

} // namespace

void* lookup_entry_point() noexcept {
    return reinterpret_cast<void*>(&lookup_body);
}

void reset() noexcept {
    AcquireSRWLockExclusive(&g_observationLock);
    g_observations = {};
    g_replacementCursor = 0;
    ReleaseSRWLockExclusive(&g_observationLock);
}

} // namespace sunrise::client::hooks::network::view_message_probe
