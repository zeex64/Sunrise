#include "native_current_world.h"

#include <array>
#include <cstddef>

#include "../../server/gameplay/group/group_host.h"
#include "../../server/gameplay/group/group_host_sessions.h"
#include "../../server/gameplay/peer/peer_transport.h"
#include "../../state/gameplay/definition.h"
#include "../hooks/bootflow/bootflow_hook_lifecycle.h"
#include "../hooks/network/sobject_apply_probe.h"

namespace sunrise::client::world::native_current {
namespace {

namespace group = server::gameplay::group;
namespace peer = server::gameplay::peer;

/** Public host rows one native-current lookup can inspect. */
constexpr std::size_t kHostRowCapacity = 8;
/** Admitted public groups one native-current lookup can inspect. */
constexpr std::size_t kAdmittedCapacity = 8;

/** @return True while a live join record still names this gameplay group. */
[[nodiscard]] bool is_admitted(const std::array<group::AdmittedRow, kAdmittedCapacity>& admitted,
                               std::size_t count,
                               std::uint64_t groupSessionId) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (admitted[index].sessionId == groupSessionId) {
            return true;
        }
    }
    return false;
}

} // namespace

/** Reads the primary activity and replaces only its location with native PUBLIC CURRENT. */
Snapshot query() noexcept {
    Snapshot output{};
    output.activityPresent = state::activity::membership::primary_world(output.world);
    output.inWorld = hooks::bootflow::in_world() && output.activityPresent;

    // The primary region is semantic during a public handoff. Clear it before every early return,
    // so the value object never labels a target as the current simulation.
    output.world.region = state::activity::membership::kAbsentRegionIndex;
    output.world.regionHash = 0;
    if (!output.inWorld) {
        return output;
    }

    std::array<group::AdmittedRow, kAdmittedCapacity> admitted{};
    std::size_t admittedCount = 0;
    group::snapshot_admitted(admitted, admittedCount);

    std::array<group::HostSessionRow, kHostRowCapacity> rows{};
    std::size_t rowCount = 0;
    group::snapshot_host_sessions(rows, rowCount);
    group::HostSessionRow selected{};
    bool found = false;
    for (std::size_t index = 0; index < rowCount; ++index) {
        const group::HostSessionRow& row = rows[index];
        state::gameplay::PeerStage stage = state::gameplay::PeerStage::absent;
        const bool channelPresent = peer::link_stage(row.groupSessionId, stage);
        if (!channelPresent && !is_admitted(admitted, admittedCount, row.groupSessionId)) {
            continue;
        }
        if (!hooks::network::sobject_apply_probe::native_manager_active(row.hostSessionId)) {
            continue;
        }
        if (found) {
            output.selection = Selection::ambiguous;
            return output;
        }
        selected = row;
        found = true;
    }
    if (!found) {
        return output;
    }

    output.selection = Selection::selected;
    output.groupSessionId = selected.groupSessionId;
    output.hostSessionId = selected.hostSessionId;
    output.world.region = selected.regionIndex;

    state::activity::membership::WorldSnapshot current{};
    if (state::activity::membership::session_world(selected.hostSessionId, current)
        && current.region == selected.regionIndex) {
        output.world.regionHash = current.regionHash;
    }
    return output;
}

/** Resolves one coherent, ready foreign route for native PUBLIC CURRENT. */
bool resolve_route(Route& output) noexcept {
    output = {};
    output.current = query();
    const Snapshot& current = output.current;
    if (!current.inWorld || current.selection != Selection::selected
        || current.world.sessionId == state::activity::kAbsentSessionId
        || current.world.region <= state::activity::membership::kAbsentRegionIndex
        || current.groupSessionId == 0 || current.hostSessionId == state::activity::kAbsentSessionId
        || current.hostSessionId == current.world.sessionId) {
        return false;
    }

    output.admissionGeneration = group::session_admission_generation(current.groupSessionId);
    output.authorityToken = group::authority_manager_token(current.groupSessionId);
    std::int32_t heldRegion = group::kUnknownRegion;
    state::activity::membership::WorldSnapshot host{};
    if (output.admissionGeneration == 0 || output.authorityToken == 0
        || group::advertised_group_session(current.world.region) != current.groupSessionId
        || group::held_host_session(current.groupSessionId) != current.hostSessionId
        || group::holding_group_session(current.hostSessionId) != current.groupSessionId
        || !group::holding_region_index(current.hostSessionId, heldRegion)
        || heldRegion != current.world.region || !group::session_admitted(current.groupSessionId)
        || !peer::view_bound(current.groupSessionId)
        || !group::activity_host_published(current.groupSessionId)
        || !hooks::network::sobject_apply_probe::native_manager_active(current.hostSessionId)
        || !state::activity::membership::session_world(current.hostSessionId, host)
        || host.sessionId != current.hostSessionId || host.region != current.world.region) {
        return false;
    }

    // The route spans independent State, group, peer and passive-native locks. Re-read its complete
    // identity and generations so a transition between those snapshots cannot authorize a send.
    const Snapshot verified = query();
    std::int32_t verifiedHeldRegion = group::kUnknownRegion;
    state::activity::membership::WorldSnapshot verifiedHost{};
    return verified.inWorld && verified.selection == Selection::selected
           && verified.world.sessionId == current.world.sessionId
           && verified.world.region == current.world.region
           && verified.groupSessionId == current.groupSessionId
           && verified.hostSessionId == current.hostSessionId
           && group::advertised_group_session(verified.world.region) == current.groupSessionId
           && group::held_host_session(current.groupSessionId) == current.hostSessionId
           && group::holding_group_session(current.hostSessionId) == current.groupSessionId
           && group::holding_region_index(current.hostSessionId, verifiedHeldRegion)
           && verifiedHeldRegion == current.world.region
           && group::session_admission_generation(current.groupSessionId)
                  == output.admissionGeneration
           && group::authority_manager_token(current.groupSessionId) == output.authorityToken
           && group::session_admitted(current.groupSessionId)
           && peer::view_bound(current.groupSessionId)
           && group::activity_host_published(current.groupSessionId)
           && hooks::network::sobject_apply_probe::native_manager_active(current.hostSessionId)
           && state::activity::membership::session_world(current.hostSessionId, verifiedHost)
           && verifiedHost.sessionId == current.hostSessionId
           && verifiedHost.region == current.world.region;
}

} // namespace sunrise::client::world::native_current
