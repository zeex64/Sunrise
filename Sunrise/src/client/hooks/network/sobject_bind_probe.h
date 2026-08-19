#pragma once

#include <cstdint>

namespace sunrise::client::hooks::network::sobject_bind_probe {

/** @return Replicated-handle to native-object binding replacement body. */
[[nodiscard]] void* binder_entry_point() noexcept;

/** Watches one successfully decoded server slot through later glue dispatches. */
void watch(std::uint32_t entityId) noexcept;

/** @return True when a successfully decoded experiment armed this entity slot. */
[[nodiscard]] bool watched(std::uint32_t entityId) noexcept;

/** Clears watched slots and their bounded dispatch counts. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::sobject_bind_probe
