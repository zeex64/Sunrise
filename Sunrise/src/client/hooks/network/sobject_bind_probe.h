#pragma once

#include <cstdint>

namespace sunrise::client::hooks::network::sobject_bind_probe {

/** One coherent snapshot of the synthetic entity's server plan and observed client lifecycle. */
struct EntityDebugSnapshot {
    std::uint64_t token{};
    std::uint32_t entityId{};
    std::uint32_t runtimeEntityId{};
    std::uint32_t rsat{};
    std::uint32_t nativeObjectId{};
    std::uint32_t nativeObjectIndex{0xFFFFFFFF};
    std::int32_t namespaceId{-1};
    std::int32_t region{-1};
    std::int32_t type2Result{-1};
    std::uint16_t slot{};
    std::uint16_t cell{};
    std::uint16_t wireFlags{};
    std::uint8_t view{};
    std::uint8_t bubble{};
    std::uint8_t attempts{};
    bool present{};
    bool sent{};
    bool decoded{};
    bool promoted{};
    bool dirtyServiced{};
    bool type2Seen{};
    bool type2JobReturned{};
    bool applied{};
    bool kind0Seen{};
    bool kind0Result{};
    bool nativeSeen{};
    bool bindSeen{};
    bool bound{};
};

/** @return Replicated-handle to native-object binding replacement body. */
[[nodiscard]] void* binder_entry_point() noexcept;

/** Watches one successfully decoded server namespace/slot through later glue dispatches. */
void watch(std::int32_t namespaceId, std::uint32_t entityId) noexcept;

/** Records the exact server-authored placement that was handed to transport. */
void record_plan(std::uint64_t token,
                 std::int32_t namespaceId,
                 std::uint8_t view,
                 std::uint16_t slot,
                 std::uint32_t rsat,
                 std::int32_t region,
                 std::uint8_t bubble,
                 std::uint8_t cell,
                 std::uint8_t attempts,
                 bool sent) noexcept;

/** Records a successfully decoded synthetic entity record. */
void record_decoded(std::int32_t namespaceId,
                    std::uint32_t entityId,
                    std::uint16_t cell,
                    std::uint16_t wireFlags) noexcept;

/** Records immediate decoded-record promotion into the replicated object table. */
void record_promoted(std::int32_t namespaceId, std::uint32_t entityId, bool occupied) noexcept;

/** Records that the active manager serviced the watched dirty row. */
void record_dirty_service(std::int32_t namespaceId, std::uint32_t entityId) noexcept;

/** Records the type-2 serializer/job-builder result. */
void record_type2(std::uint32_t entityId, int result, bool jobReturned) noexcept;

/** Records dispatch of the asynchronous replicated-object apply job. */
void record_apply(std::uint32_t entityId) noexcept;

/** Records the kind-0 native constructor's definitive result. */
void record_kind0(std::uint32_t entityId, bool result) noexcept;

/** Records native dependency registration for the synthetic RSAT. */
void record_native(std::uint32_t rsat, std::uint32_t objectId) noexcept;

/** Records one glue-table dispatch and whether its postcondition became true. */
void record_binding(std::uint32_t entityId, std::uint32_t nativeObjectIndex, bool bound) noexcept;

/** Copies the latest synthetic entity snapshot for the HUD. */
[[nodiscard]] bool debug_snapshot(EntityDebugSnapshot& output) noexcept;

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
