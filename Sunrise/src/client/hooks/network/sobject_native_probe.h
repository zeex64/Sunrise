#pragma once

namespace sunrise::client::hooks::network::sobject_native_probe {

/** @return Native sobject RSAT-registration replacement body. */
[[nodiscard]] void* registration_entry_point() noexcept;

/** Clears the bounded set of already-reported native RSAT tags. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::sobject_native_probe
