#include "activity_mode_probe.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "../../../core/logging/log.h"
#include "coordinator/network_call_coordinator.h"
#include "platform.h"

namespace sunrise::client::hooks::network::activity_mode_probe {
namespace {

struct Observation {
    std::uint16_t primaryActivity{};
    std::int32_t modeIndex{};
    std::uint16_t fallbackActivity{};
    bool occupied{};
};

struct DefinitionObservation {
    std::int32_t definition{};
    bool selected{};
    bool occupied{};
};

SRWLOCK g_observationLock{SRWLOCK_INIT};
Observation g_observation{};
DefinitionObservation g_definitionObservation{};

using Selector = void(__fastcall*)(std::uint16_t, std::int32_t, std::uint16_t);
using Setter = bool(__fastcall*)(const std::int32_t*);

/** Records the first selector tuple and every later change without flooding the client log. */
[[nodiscard]] bool observe(const Observation& value) noexcept {
    bool report = false;
    AcquireSRWLockExclusive(&g_observationLock);
    if (!g_observation.occupied || g_observation.primaryActivity != value.primaryActivity
        || g_observation.modeIndex != value.modeIndex
        || g_observation.fallbackActivity != value.fallbackActivity) {
        g_observation = value;
        g_observation.occupied = true;
        report = true;
    }
    ReleaseSRWLockExclusive(&g_observationLock);
    return report;
}

/** Records each resolved mode definition and its native selection result once. */
[[nodiscard]] bool observe_definition(const DefinitionObservation& value) noexcept {
    bool report = false;
    AcquireSRWLockExclusive(&g_observationLock);
    if (!g_definitionObservation.occupied || g_definitionObservation.definition != value.definition
        || g_definitionObservation.selected != value.selected) {
        g_definitionObservation = value;
        g_definitionObservation.occupied = true;
        report = true;
    }
    ReleaseSRWLockExclusive(&g_observationLock);
    return report;
}

/** Preserves native mode selection and reports only its content-derived selector inputs. */
__declspec(noinline) void __fastcall selector_body(std::uint16_t primaryActivity,
                                                   std::int32_t modeIndex,
                                                   std::uint16_t fallbackActivity) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::activityModeSelector, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Selector>(lease.original);
    __try {
        const Observation observation{
            primaryActivity,
            modeIndex,
            fallbackActivity,
            true,
        };
        if (lease.accepting && observe(observation)) {
            std::array<char, 192> line{};
            const int written = std::snprintf(
                line.data(),
                line.size(),
                "ev=gameplay stage=activity-mode result=called primary=%u index=%d fallback=%u",
                static_cast<unsigned>(primaryActivity),
                modeIndex,
                static_cast<unsigned>(fallbackActivity));
            if (written > 0) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
        if (call != nullptr) {
            call(primaryActivity, modeIndex, fallbackActivity);
        }
    } __finally {
        coordinator::g_callEgress();
    }
}

/** Preserves native definition resolution and reports its exact 32-bit content identifier. */
__declspec(noinline) bool __fastcall setter_body(const std::int32_t* definition) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::activityModeSetter, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Setter>(lease.original);
    std::int32_t snapshot{};
    bool readable = false;
    bool selected = false;
    __try {
        if (definition != nullptr) {
            snapshot = *definition;
            readable = true;
        }
        if (call != nullptr) {
            selected = call(definition);
        }
        const DefinitionObservation observation{snapshot, selected, true};
        if (lease.accepting && readable && observe_definition(observation)) {
            std::array<char, 160> line{};
            const int written = std::snprintf(
                line.data(),
                line.size(),
                "ev=gameplay stage=activity-mode-definition result=%s definition=0x%08X",
                selected ? "ok" : "fail",
                static_cast<std::uint32_t>(snapshot));
            if (written > 0) {
                core::log::write(core::log::Channel::client,
                                 selected ? core::log::Level::info : core::log::Level::warn,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return selected;
}

} // namespace

void* selector_entry_point() noexcept {
    return reinterpret_cast<void*>(&selector_body);
}

void* setter_entry_point() noexcept {
    return reinterpret_cast<void*>(&setter_body);
}

void reset() noexcept {
    AcquireSRWLockExclusive(&g_observationLock);
    g_observation = {};
    g_definitionObservation = {};
    ReleaseSRWLockExclusive(&g_observationLock);
}

} // namespace sunrise::client::hooks::network::activity_mode_probe
