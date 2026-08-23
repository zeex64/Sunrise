#include "../runtime.h"
#include "../scenarios/scenario_catalog.h"

namespace sunrise::state::build_data {

/** Copies one roster group by the table index a destination row carries. */
bool find_roster_group(std::size_t index, scenarios::RosterGroup& group) noexcept {
    group = {};
    return scenario_layouts_ready() && scenarios::group(index, group);
}

/** Finds one state-local roster group by its package object tag. */
bool find_roster_group_by_object(std::uint32_t objectTag, scenarios::RosterGroup& group) noexcept {
    group = {};
    return scenario_layouts_ready() && scenarios::find_group(objectTag, group);
}

} // namespace sunrise::state::build_data
