#pragma once

#include <cstdint>

namespace sunrise::client::hooks::network::sobject_apply_probe {

/** Latest guarded relationship between the player's current region and native active manager. */
struct ActiveManagerDebugSnapshot {
    std::uint64_t token{};
    std::uint64_t observedAt{};
    std::int32_t region{-1};
    std::int32_t nativeSlice{-1};
    std::int32_t requestedNamespace{-1};
    std::int32_t activeBefore{-1};
    std::int32_t activeAfter{-1};
    bool managerMatched{};
    bool promoted{};
    bool ready{};
};

/** Latest native normal-z-leg classification that is still being evaluated. */
struct ZLegDebugSnapshot {
    std::uint64_t observedAt{};
    std::uintptr_t controller{};
    std::int32_t requestedState{-1};
    std::int32_t storedBefore{-1};
    std::int32_t storedAfter{-1};
    std::int32_t transitionMode{-1};
    std::uint32_t transitionFlags{};
    std::int32_t targetRegion{-1};
    std::int32_t regionA{-1};
    std::int32_t regionB{-1};
    std::int32_t authoredRegion{-1};
    std::int32_t entryIndex{-1};
    std::int32_t axis{-1};
    float previousCoordinate{};
    float targetCoordinate{};
    float currentCoordinate{};
    std::uint32_t positionReference{};
    bool positionValid{};
    bool readable{};
};

/** @return Asynchronous replicated-object apply-job replacement body. */
[[nodiscard]] void* apply_entry_point() noexcept;

/** @return Kind-0 replicated-object constructor replacement body. */
[[nodiscard]] void* kind0_entry_point() noexcept;

/** @return Immediate decoded-record promotion replacement body. */
[[nodiscard]] void* promotion_entry_point() noexcept;

/** @return Per-tick replicated-object dirty-service replacement body. */
[[nodiscard]] void* dirty_service_entry_point() noexcept;

/** @return Dirty-service backend-busy predicate replacement body. */
[[nodiscard]] void* backend_busy_entry_point() noexcept;

/** @return Per-row replicated-object dirty processor replacement body. */
[[nodiscard]] void* dirty_row_entry_point() noexcept;

/** @return Type-2 replicated-object serialization/allocation replacement body. */
[[nodiscard]] void* type2_job_entry_point() noexcept;

/** @return Native public-session active-manager refresh replacement body. */
[[nodiscard]] void* active_manager_refresh_entry_point() noexcept;

/** @return Public-session initialization predicate replacement body. */
[[nodiscard]] void* citizen_session_ready_entry_point() noexcept;

/** @return Asynchronous world-controller citizen-join status replacement body. */
[[nodiscard]] void* citizen_join_status_entry_point() noexcept;

/** @return Native normal-z-leg state publisher replacement body. */
[[nodiscard]] void* z_leg_state_entry_point() noexcept;

/** Refreshes the passive current-region/native-manager observation from gameplay service. */
void service_current_region_manager() noexcept;

/** @return True while the exact current token/namespace is the freshly active native manager. */
[[nodiscard]] bool current_region_manager_active(std::uint64_t token,
                                                 std::int32_t namespaceId) noexcept;

/** @return True while native PUBLIC CURRENT freshly selects the exact current-region token. */
[[nodiscard]] bool current_region_manager_active(std::uint64_t token) noexcept;

/** @return True while a live token's captured namespace is the native active manager. */
[[nodiscard]] bool native_manager_active(std::uint64_t token) noexcept;

/** Copies the last guarded manager-selection observation for the debug overlay. */
[[nodiscard]] bool active_manager_debug_snapshot(ActiveManagerDebugSnapshot& output) noexcept;

/** Copies the freshly evaluated native normal-z-leg state for the debug overlay. */
[[nodiscard]] bool z_leg_debug_snapshot(ZLegDebugSnapshot& output) noexcept;

/** Clears bounded report counters. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::sobject_apply_probe
