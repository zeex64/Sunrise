#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::client::hooks::network::entity_slot_probe {

/** Safe-slot and scheduler identity captured from one native view. */
struct ViewCapture {
    static constexpr std::size_t kSchedulerViewCapacity = 3;

    const void* manager{};
    std::uint64_t token{};
    std::uint64_t schedulerKey{};
    std::int32_t namespaceId{-1};
    std::uint32_t freeCount{};
    std::uint32_t occupiedCount{};
    std::uint32_t availableCount{};
    std::uint16_t slot{};
    std::uint8_t schedulerTag{};
    std::uint8_t handleGeneration{};
    std::uint8_t reservedGeneration{};
    std::uint8_t objectGeneration{};
    std::uint8_t schedulerViewCount{};
    std::uint8_t schedulerRemoteViewCount{};
    std::array<std::byte, 16> schedulerSignature{};
    std::array<std::byte, 16> schedulerRemoteSignature{};
    std::array<std::uint64_t, kSchedulerViewCapacity> schedulerViewKeys{};
    std::array<std::uint8_t, kSchedulerViewCapacity> schedulerViewTags{};
    std::array<std::uint64_t, kSchedulerViewCapacity> schedulerRemoteViewKeys{};
    std::array<std::uint8_t, kSchedulerViewCapacity> schedulerRemoteViewTags{};
    bool candidatePresent{};
    bool schedulerSignatureValid{};
    bool schedulerRemoteSignatureValid{};

    [[nodiscard]] bool operator==(const ViewCapture&) const noexcept = default;
};

/** @return Native inbound entity-list decoder replacement body. */
[[nodiscard]] void* decoder_entry_point() noexcept;

/**
 * Snapshots one entity manager reached through an already-created native view.
 * @param manager Native per-view entity manager.
 * @param result Result value associated with the observation, or zero for a view lookup.
 */
void observe_manager(const void* manager, int result = 0) noexcept;

/** Arms a bounded trace for the next namespace-2 entity-list decode after server emission. */
void arm_decoder_trace() noexcept;

/** Captures one view's scheduler key, logical signature entries, and entity-manager state. */
void observe_view(std::uint64_t token, const void* view) noexcept;

/** @return True when a current safe-slot capture exists for the requested view token. */
[[nodiscard]] bool find(std::uint64_t token, ViewCapture& output) noexcept;

/** Clears the bounded set of already-reported entity managers. */
void reset() noexcept;

} // namespace sunrise::client::hooks::network::entity_slot_probe
