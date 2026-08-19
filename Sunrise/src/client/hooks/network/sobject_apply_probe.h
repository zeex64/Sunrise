#pragma once

namespace sunrise::client::hooks::network::sobject_apply_probe {

/** @return Asynchronous replicated-object apply-job replacement body. */
[[nodiscard]] void* apply_entry_point() noexcept;

/** @return Kind-0 replicated-object constructor replacement body. */
[[nodiscard]] void* kind0_entry_point() noexcept;

/** Clears bounded report counters. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::sobject_apply_probe
