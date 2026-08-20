#pragma once

namespace sunrise::client::hooks::network::sobject_apply_probe {

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

/** @return Type-2 replicated-object serialization/allocation replacement body. */
[[nodiscard]] void* type2_job_entry_point() noexcept;

/** Clears bounded report counters. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::sobject_apply_probe
