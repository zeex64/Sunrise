#pragma once

#include <cstddef>
#include <cstdint>

namespace sunrise::client::hooks::network::scheduler_handler_probe {

/** Native scheduler handlers, in their per-view inbound call order. */
enum class Lane : std::uint8_t {
    event,
    mask,
    entityPrelude,
    entityList,
    fixed,
};

/** Passive reader fields captured at one handler boundary. */
struct ReaderState {
    const std::byte* begin{};
    const std::byte* end{};
    std::int32_t loadedBits{};
    std::int32_t totalBits{};
    std::uint64_t accumulator{};
    std::uint32_t pendingBits{};
    const std::byte* cursor{};
    bool readable{};
};

/** Opaque correlation token carried across one original handler call. */
struct Call {
    ReaderState before{};
    const void* reader{};
    std::uint64_t epoch{};
    std::uint32_t threadId{};
    std::uint8_t ordinal{};
    std::uint8_t view{};
    Lane lane{Lane::event};
    Lane expectedLane{Lane::event};
    std::uint8_t status{};
    bool report{};
};

/** @return Native event-list decoder replacement body. */
[[nodiscard]] void* event_entry_point() noexcept;

/** @return Native mask-list decoder replacement body. */
[[nodiscard]] void* mask_entry_point() noexcept;

/** @return Native direct-entity prelude decoder replacement body. */
[[nodiscard]] void* entity_prelude_entry_point() noexcept;

/** @return Native fixed-list decoder replacement body. */
[[nodiscard]] void* fixed_entry_point() noexcept;

/**
 * Arms one strict, passive trace for the next complete scheduler body.
 * @param viewCount Expected scheduler views, from one through three.
 */
void arm(std::uint8_t viewCount) noexcept;

/** Cancels the current trace without changing any native reader state. */
void cancel() noexcept;

/** @return True while a non-expired trace still accepts handler calls. */
[[nodiscard]] bool active() noexcept;

/** Captures the entry state for one handler while an epoch is armed. */
[[nodiscard]] Call begin(Lane lane, const void* reader) noexcept;

/** Captures the exit state and advances or rejects the armed epoch. */
void finish(const Call& call, int result) noexcept;

/** Clears the bounded diagnostic state during hook teardown. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::scheduler_handler_probe
