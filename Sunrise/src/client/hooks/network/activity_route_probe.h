#pragma once

namespace sunrise::client::hooks::network::activity_route_probe {

/** @return Native activity-start record lookup replacement body. */
[[nodiscard]] void* record_entry_point() noexcept;

/** @return Native local activity-manager initializer replacement body. */
[[nodiscard]] void* local_entry_point() noexcept;

/** @return Native authored activity-manager initializer replacement body. */
[[nodiscard]] void* authored_entry_point() noexcept;

/** Clears the per-route identity observations after the detours are removed. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::activity_route_probe
