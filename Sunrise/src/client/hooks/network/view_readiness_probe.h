#pragma once

namespace sunrise::client::hooks::network::view_readiness_probe {

/** @return Replacement body for the stage-four entity-handler readiness scan. */
[[nodiscard]] void* scan_entry_point() noexcept;

/** Clears the bounded per-handler readiness observations. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::view_readiness_probe
