#pragma once

namespace sunrise::client::hooks::network::entity_create_probe {

/** @return Core replicated-entity object-body encoder replacement. */
[[nodiscard]] void* encoder_entry_point() noexcept;

/** Clears the bounded set of already-reported entity creates. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::entity_create_probe
