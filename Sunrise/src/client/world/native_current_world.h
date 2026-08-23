#pragma once

#include <cstdint>

#include "../../state/activity/membership/activity_membership_query.h"

namespace sunrise::client::world::native_current {

/** Outcome of selecting native PUBLIC CURRENT from the live public host rows. */
enum class Selection : std::uint8_t {
    /** No live host row names the active native manager. */
    unavailable,
    /** Exactly one live host row names the active native manager. */
    selected,
    /** More than one live host row names it, so none is safe to present as current. */
    ambiguous,
};

/**
 * One coherent activity and native-current world view.
 * Destination and teleport state always come from the player's primary activity. Region and hash
 * are present only when exactly one live public host owns native CURRENT.
 */
struct Snapshot final {
    state::activity::membership::WorldSnapshot world{};
    std::uint64_t groupSessionId{};
    std::uint64_t hostSessionId{};
    Selection selection{Selection::unavailable};
    bool activityPresent{};
    bool inWorld{};
};

/**
 * Exact transport and gameplay route that owns native PUBLIC CURRENT.
 * The primary activity session remains the world/activity identity, while the group and host name
 * the foreign ActivityClient stream whose native manager is current on screen.
 */
struct Route final {
    Snapshot current{};
    /** Durable accepted-join generation for current.groupSessionId. */
    std::uint64_t admissionGeneration{};
    /** Nonzero replication-view authority token for current.groupSessionId. */
    std::uint16_t authorityToken{};
};

/**
 * Reads the primary activity and replaces only its location with native PUBLIC CURRENT.
 * A missing or ambiguous native selection leaves `world.region` absent and `world.regionHash`
 * zero, so a semantic transition target can never be mistaken for the current simulation.
 * @return Value-only snapshot assembled from published State and passive native observations.
 */
[[nodiscard]] Snapshot query() noexcept;

/**
 * Resolves one coherent, ready foreign route for native PUBLIC CURRENT.
 * The selected group must be advertised for the selected region, admitted with a bound view and
 * published activity host, and that host must still own the active native manager and report the
 * same region. Independent State/group reads are repeated before success so a handoff fails
 * closed.
 * @param output Cleared first, then receives the exact root/group/host/generation identity.
 * @return True only while the complete native-current route is coherent and ready.
 */
[[nodiscard]] bool resolve_route(Route& output) noexcept;

} // namespace sunrise::client::world::native_current
