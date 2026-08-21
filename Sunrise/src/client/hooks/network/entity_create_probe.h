#pragma once

#include <cstdint>

namespace sunrise::client::hooks::network::entity_create_probe {

/** @return Core replicated-entity object-body encoder replacement. */
[[nodiscard]] void* encoder_entry_point() noexcept;

/** Associates a nested kind-0 create call with the entity record currently being encoded. */
void observe_sobject_rsat(std::uint32_t rsat) noexcept;

/** Clears the bounded set of already-reported entity creates. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::entity_create_probe
