#include "view_message_probe.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../../core/logging/log.h"
#include "coordinator/network_call_coordinator.h"
#include "entity_slot_probe.h"
#include "platform.h"

namespace sunrise::client::hooks::network::view_message_probe {
namespace {

/** Native networking retains at most three session families, with overlap during transitions. */
constexpr std::size_t kObservationCapacity = 8;
/** The wire reserves two bits for a replicated-object kind. */
constexpr std::size_t kCodecCapacity = 4;

/** Native replicated-object codec table reached through the view's entity handler. */
struct CodecState {
    std::uintptr_t manager{};
    std::uintptr_t registry{};
    std::int32_t count{};
    std::array<std::uintptr_t, kCodecCapacity> codecs{};
    std::array<std::uintptr_t, kCodecCapacity> vtables{};
    std::array<std::uintptr_t, kCodecCapacity> createEncoders{};
    std::array<std::uintptr_t, kCodecCapacity> createDecoders{};
    std::array<std::uintptr_t, kCodecCapacity> updateEncoders{};
    std::array<std::uintptr_t, kCodecCapacity> updateDecoders{};

    [[nodiscard]] bool operator==(const CodecState&) const noexcept = default;
};

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
    CodecState codecs{};
    bool found{};
    bool stateValid{};
    bool codecsValid{};
    bool occupied{};
};

SRWLOCK g_observationLock{SRWLOCK_INIT};
std::array<Observation, kObservationCapacity> g_observations{};
std::size_t g_replacementCursor{};

using Lookup = void*(__fastcall*)(void*, std::uint64_t);

/** Copies the compact native handshake state while the lookup result remains valid. */
[[nodiscard]] bool inspect(void* view, ViewState& output, CodecState& codecs) noexcept {
    if (view == nullptr) {
        return false;
    }
    ViewState candidate{};
    CodecState codecCandidate{};
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
        std::memcpy(&candidate.signatureCount, bytes + 0xA0, sizeof candidate.signatureCount);
        candidate.fullyOpen = std::to_integer<unsigned>(bytes[0xA4]) != 0;
        candidate.compatible = std::to_integer<unsigned>(bytes[0xA5]) != 0;
        // FUN_1417135A0 stores the per-family entity manager at entity-handler +0x10. The entity
        // handler begins at view +0xA8, so this is view +0xB8. The entity manager points to the
        // global codec registry at +0x10; that registry begins with a count and four pointers.
        std::memcpy(&codecCandidate.manager, bytes + 0xB8, sizeof codecCandidate.manager);
        if (codecCandidate.manager != 0) {
            const auto* const manager = reinterpret_cast<const std::byte*>(codecCandidate.manager);
            std::memcpy(&codecCandidate.registry, manager + 0x10, sizeof codecCandidate.registry);
        }
        if (codecCandidate.registry != 0) {
            const auto* const registry =
                reinterpret_cast<const std::byte*>(codecCandidate.registry);
            std::memcpy(&codecCandidate.count, registry, sizeof codecCandidate.count);
            std::size_t codecCount =
                codecCandidate.count > 0 ? static_cast<std::size_t>(codecCandidate.count) : 0;
            if (codecCount > kCodecCapacity) {
                codecCount = kCodecCapacity;
            }
            for (std::size_t kind = 0; kind < codecCount; ++kind) {
                std::memcpy(&codecCandidate.codecs[kind],
                            registry + sizeof(std::uintptr_t) * (kind + 1),
                            sizeof codecCandidate.codecs[kind]);
                if (codecCandidate.codecs[kind] == 0) {
                    continue;
                }
                const auto* const codec =
                    reinterpret_cast<const std::byte*>(codecCandidate.codecs[kind]);
                std::memcpy(
                    &codecCandidate.vtables[kind], codec, sizeof codecCandidate.vtables[kind]);
                if (codecCandidate.vtables[kind] == 0) {
                    continue;
                }
                const auto* const vtable =
                    reinterpret_cast<const std::byte*>(codecCandidate.vtables[kind]);
                std::memcpy(&codecCandidate.createEncoders[kind],
                            vtable + 0x58,
                            sizeof codecCandidate.createEncoders[kind]);
                std::memcpy(&codecCandidate.createDecoders[kind],
                            vtable + 0x60,
                            sizeof codecCandidate.createDecoders[kind]);
                std::memcpy(&codecCandidate.updateEncoders[kind],
                            vtable + 0x68,
                            sizeof codecCandidate.updateEncoders[kind]);
                std::memcpy(&codecCandidate.updateDecoders[kind],
                            vtable + 0x70,
                            sizeof codecCandidate.updateDecoders[kind]);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    output = candidate;
    codecs = codecCandidate;
    return true;
}

/** Converts a main-image address to the RVA used by the Ghidra dump. */
[[nodiscard]] std::uint64_t image_rva(std::uintptr_t address) noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    return address == 0 || address < base ? 0 : static_cast<std::uint64_t>(address - base);
}

/** Logs only a token's first lookup and a later missing/found transition. */
void observe(std::uint64_t token, void* view) noexcept {
    const bool found = view != nullptr;
    ViewState state{};
    CodecState codecs{};
    const bool stateValid = inspect(view, state, codecs);
    const bool codecsValid = stateValid && codecs.registry != 0;
    if (stateValid && codecs.manager != 0) {
        entity_slot_probe::observe_view(token, view);
    }
    bool reportLookup = false;
    bool reportState = false;
    bool reportCodecs = false;
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
    if (stateValid && (!destination->stateValid || !(destination->state == state))) {
        reportState = true;
        destination->state = state;
    }
    if (codecsValid && (!destination->codecsValid || !(destination->codecs == codecs))) {
        reportCodecs = true;
        destination->codecs = codecs;
    }
    destination->stateValid = stateValid;
    destination->codecsValid = codecsValid;
    ReleaseSRWLockExclusive(&g_observationLock);

    if (reportLookup) {
        std::array<char, 160> line{};
        const int written =
            std::snprintf(line.data(),
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
    if (reportCodecs) {
        std::array<char, 192> summary{};
        const int summaryWritten =
            std::snprintf(summary.data(),
                          summary.size(),
                          "ev=gameplay stage=view-codecs token=0x%llX manager=%p registry=%p "
                          "count=%d",
                          static_cast<unsigned long long>(token),
                          reinterpret_cast<void*>(codecs.manager),
                          reinterpret_cast<void*>(codecs.registry),
                          codecs.count);
        if (summaryWritten > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {summary.data(), static_cast<std::size_t>(summaryWritten)});
        }
        std::size_t codecCount = codecs.count > 0 ? static_cast<std::size_t>(codecs.count) : 0;
        if (codecCount > kCodecCapacity) {
            codecCount = kCodecCapacity;
        }
        for (std::size_t kind = 0; kind < codecCount; ++kind) {
            std::array<char, 320> line{};
            const int written = std::snprintf(
                line.data(),
                line.size(),
                "ev=gameplay stage=view-codec token=0x%llX kind=%zu registry=%p codec=%p "
                "vtable_rva=0x%llX create_out_rva=0x%llX create_in_rva=0x%llX "
                "update_out_rva=0x%llX update_in_rva=0x%llX",
                static_cast<unsigned long long>(token),
                kind,
                reinterpret_cast<void*>(codecs.registry),
                reinterpret_cast<void*>(codecs.codecs[kind]),
                static_cast<unsigned long long>(image_rva(codecs.vtables[kind])),
                static_cast<unsigned long long>(image_rva(codecs.createEncoders[kind])),
                static_cast<unsigned long long>(image_rva(codecs.createDecoders[kind])),
                static_cast<unsigned long long>(image_rva(codecs.updateEncoders[kind])),
                static_cast<unsigned long long>(image_rva(codecs.updateDecoders[kind])));
            if (written > 0) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
    }
}

/** Preserves native lookup behavior while exposing whether message 40 reached its handler. */
__declspec(noinline) void* __fastcall lookup_body(void* owner, std::uint64_t token) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, HookSlot::viewMessageLookup, coordinator::ConsumerKind::none);
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
