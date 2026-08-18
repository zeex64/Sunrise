#include "view_message_probe.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../../core/logging/log.h"
#include "coordinator/network_call_coordinator.h"
#include "platform.h"

namespace sunrise::client::hooks::network::view_message_probe {
namespace {

/** Native networking retains at most three session families, with overlap during transitions. */
constexpr std::size_t kObservationCapacity = 8;

/** Native c_network_channel_view handshake fields read after lookup returns the borrowed view. */
struct ViewState {
    std::int32_t error{};
    std::int32_t localStage{};
    std::int32_t localIndex{};
    std::int32_t previousIndex{};
    std::int32_t localPeerIndex{};
    std::int32_t remoteStage{};
    std::int32_t remoteIndex{};
    std::uint32_t signatureCount{};
    bool signatureValid{};
    bool fullyOpen{};
    bool compatible{};

    [[nodiscard]] bool operator==(const ViewState&) const noexcept = default;
};

struct Observation {
    std::uint64_t token{};
    ViewState state{};
    bool found{};
    bool stateValid{};
    bool occupied{};
};

SRWLOCK g_observationLock{SRWLOCK_INIT};
std::array<Observation, kObservationCapacity> g_observations{};
std::size_t g_replacementCursor{};

using Lookup = void*(__fastcall*)(void*, std::uint64_t);

/** Copies the compact native handshake state while the lookup result remains valid. */
[[nodiscard]] bool inspect(void* view, ViewState& output) noexcept {
    if (view == nullptr) {
        return false;
    }
    ViewState candidate{};
    const auto* const bytes = static_cast<const std::byte*>(view);
    __try {
        std::memcpy(&candidate.error, bytes + 0x70, sizeof candidate.error);
        std::memcpy(&candidate.localStage, bytes + 0x74, sizeof candidate.localStage);
        std::memcpy(&candidate.localIndex, bytes + 0x78, sizeof candidate.localIndex);
        std::memcpy(&candidate.previousIndex, bytes + 0x7C, sizeof candidate.previousIndex);
        std::memcpy(&candidate.localPeerIndex, bytes + 0x80, sizeof candidate.localPeerIndex);
        std::memcpy(&candidate.remoteStage, bytes + 0x84, sizeof candidate.remoteStage);
        std::memcpy(&candidate.remoteIndex, bytes + 0x88, sizeof candidate.remoteIndex);
        candidate.signatureValid = std::to_integer<unsigned>(bytes[0x8C]) != 0;
        std::memcpy(
            &candidate.signatureCount, bytes + 0xA0, sizeof candidate.signatureCount);
        candidate.fullyOpen = std::to_integer<unsigned>(bytes[0xA4]) != 0;
        candidate.compatible = std::to_integer<unsigned>(bytes[0xA5]) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    output = candidate;
    return true;
}

/** Logs only a token's first lookup and a later missing/found transition. */
void observe(std::uint64_t token, void* view) noexcept {
    const bool found = view != nullptr;
    ViewState state{};
    const bool stateValid = inspect(view, state);
    bool reportLookup = false;
    bool reportState = false;
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
        reportLookup = true;
        destination->token = token;
        destination->found = found;
        destination->occupied = true;
    }
    if (stateValid
        && (!destination->stateValid || !(destination->state == state))) {
        reportState = true;
        destination->state = state;
    }
    destination->stateValid = stateValid;
    ReleaseSRWLockExclusive(&g_observationLock);

    if (reportLookup) {
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
    if (reportState) {
        std::array<char, 320> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=gameplay stage=view-state token=0x%llX view=%p error=%d "
            "local=%d local_index=%d previous=%d local_peer=%d remote=%d remote_index=%d "
            "signature=%u bytes=%u compatible=%u open=%u",
            static_cast<unsigned long long>(token),
            view,
            state.error,
            state.localStage,
            state.localIndex,
            state.previousIndex,
            state.localPeerIndex,
            state.remoteStage,
            state.remoteIndex,
            state.signatureValid ? 1U : 0U,
            static_cast<unsigned>(state.signatureCount),
            state.compatible ? 1U : 0U,
            state.fullyOpen ? 1U : 0U);
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
