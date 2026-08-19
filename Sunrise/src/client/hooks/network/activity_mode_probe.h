#pragma once

namespace sunrise::client::hooks::network::activity_mode_probe {

/** @return Activity-mode definition selector replacement body. */
[[nodiscard]] void* selector_entry_point() noexcept;

/** @return Resolved activity-mode definition setter replacement body. */
[[nodiscard]] void* setter_entry_point() noexcept;

/** @return Activity-definition type resolver replacement body. */
[[nodiscard]] void* type_resolver_entry_point() noexcept;

/** Clears the last reported selector and resolved-definition observations during shutdown. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::activity_mode_probe
