#pragma once

#include <cstdint>

namespace sunrise::client::hooks::network::sobject_bind_probe {

/** @return Replicated-handle to native-object binding replacement body. */
[[nodiscard]] void* binder_entry_point() noexcept;

/** Watches one successfully decoded server namespace/slot through later glue dispatches. */
void watch(std::int32_t namespaceId, std::uint32_t entityId) noexcept;

/** @return True when this exact namespace/slot pair belongs to the armed experiment. */
[[nodiscard]] bool watched(std::int32_t namespaceId, std::uint32_t entityId) noexcept;

/** @return True when this exact decoded entity belongs to the armed experiment. */
[[nodiscard]] bool watched_exact(std::uint32_t entityId) noexcept;

/** @return True when an armed experiment owns this slot in any namespace. */
[[nodiscard]] bool watched(std::uint32_t entityId) noexcept;

/** Finds one armed experiment entity in the requested manager namespace. */
[[nodiscard]] bool first_watched(std::int32_t namespaceId, std::uint32_t& entityId) noexcept;

/** Clears watched slots and their bounded dispatch counts. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::sobject_bind_probe
