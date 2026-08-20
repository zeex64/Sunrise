/**
 * The session overlay. One row per instance the player is in.
 * A row names the region, its group session, its activity host, its channel and its join.
 */

#include "ui_hud_session_overlay.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <imgui.h>

#include "../../../../client/hooks/network/sobject_apply_probe.h"
#include "../../../../middleware/content/packages/tables/region_reader.h"
#include "../../../../server/gameplay/group/group_host.h"
#include "../../../../server/gameplay/group/group_host_sessions.h"
#include "../../../../server/gameplay/peer/peer_transport.h"
#include "../../../../state/activity/membership/activity_membership_query.h"
#include "../../../../state/gameplay/definition.h"

namespace sunrise::core::ui::hud::overlays::session {
namespace {

namespace group = server::gameplay::group;
namespace peer = server::gameplay::peer;
namespace tables = middleware::content::packages::tables;

/** Instances one snapshot reads. The host table holds no more than this. */
constexpr std::size_t kRowCapacity = 8;
/** Admitted peers one snapshot reads. */
constexpr std::size_t kAdmittedCapacity = 8;
/** Columns of the instance table, in draw order. */
constexpr int kColumnCount = 7;
/** Shown while this host serves no live or current instance. */
constexpr char kNoInstance[] = "no active session instances";
/** Shown for a row no advertisement gave a region. */
constexpr char kNoBubble[] = "-";

/** Channel stage names, in PeerStage order. */
constexpr std::array<const char*, 6> kChannelStages{
    "absent", "allocated", "teardown", "connecting", "establishing", "connected"};

/** One host row after durable cache-only entries have been removed. */
struct DisplayRow {
    group::HostSessionRow host{};
    state::gameplay::PeerStage stage{state::gameplay::PeerStage::absent};
    bool reportedCurrent{};
    bool nativeCurrent{};
};

/** Names the semantic/native role without presenting a pending target as current. */
[[nodiscard]] const char* role_name(const DisplayRow& row) noexcept {
    if (row.nativeCurrent) {
        return "current";
    }
    return row.reportedCurrent ? "target" : "overlap";
}

/** @param stage Link stage. @return Its name, or the absent one for a value out of range. */
[[nodiscard]] const char* channel_name(state::gameplay::PeerStage stage) noexcept {
    const auto index = static_cast<std::size_t>(stage);
    return index < kChannelStages.size() ? kChannelStages[index] : kChannelStages[0];
}

/** Names how far one join has got. @param count Admitted rows in use. @return The join word. */
[[nodiscard]] const char*
join_name(const std::array<group::AdmittedRow, kAdmittedCapacity>& admitted,
          std::size_t count,
          std::uint64_t sessionId) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        const group::AdmittedRow& row = admitted[index];
        if (row.sessionId != sessionId) {
            continue;
        }
        if (!row.joinComplete) {
            return "joining";
        }
        return row.activityHostPublished ? "ready" : "joined";
    }
    return "unjoined";
}

/** @return True while a live join record still names this group session. */
[[nodiscard]] bool is_admitted(const std::array<group::AdmittedRow, kAdmittedCapacity>& admitted,
                               std::size_t count,
                               std::uint64_t sessionId) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (admitted[index].sessionId == sessionId) {
            return true;
        }
    }
    return false;
}

/** Draws one cell of hexadecimal identity. @param value Session id, or zero when there is none. */
void draw_session_cell(std::uint64_t value) noexcept {
    if (value == 0) {
        ImGui::TextDisabled("pending");
        return;
    }
    ImGui::Text("%016llX", static_cast<unsigned long long>(value));
}

/** Draws the region and bubble cells. @param region Region the advertisement named, or -1. */
void draw_region_cells(std::int32_t region) noexcept {
    if (region < 0) {
        ImGui::TextDisabled("%s", kNoBubble);
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", kNoBubble);
        return;
    }
    ImGui::Text("%d", static_cast<int>(region));
    ImGui::TableNextColumn();
    ImGui::Text("%u", static_cast<unsigned>(region) / tables::kSliceSetIndexFactor);
}

} // namespace

/** Draws the session instance table inside the overlay window the stack has already started. */
void draw() noexcept {
    std::array<group::AdmittedRow, kAdmittedCapacity> admitted{};
    std::size_t admittedCount = 0;
    group::snapshot_admitted(admitted, admittedCount);

    std::array<group::HostSessionRow, kRowCapacity> cached{};
    std::size_t cachedCount = 0;
    group::snapshot_host_sessions(cached, cachedCount);

    state::activity::membership::WorldSnapshot world{};
    const std::uint64_t currentGroup = state::activity::membership::primary_world(world)
                                           ? group::advertised_group_session(world.region)
                                           : 0;

    std::array<DisplayRow, kRowCapacity> rows{};
    std::size_t rowCount = 0;
    for (std::size_t index = 0; index < cachedCount; ++index) {
        state::gameplay::PeerStage stage = state::gameplay::PeerStage::absent;
        const bool channelPresent = peer::link_stage(cached[index].groupSessionId, stage);
        const bool admittedPresent =
            is_admitted(admitted, admittedCount, cached[index].groupSessionId);
        const bool current = currentGroup != 0 && cached[index].groupSessionId == currentGroup;
        // Host-session rows are a durable reconnect cache. Once a non-current row has neither a
        // channel nor a join record, it is no longer a session instance and must not linger here.
        if (!current && !channelPresent && !admittedPresent) {
            continue;
        }
        const bool nativeCurrent =
            client::hooks::network::sobject_apply_probe::native_manager_active(
                cached[index].hostSessionId);
        rows[rowCount] = {cached[index], stage, current, nativeCurrent};
        ++rowCount;
    }
    if (rowCount == 0) {
        ImGui::TextDisabled("%s", kNoInstance);
        return;
    }

    if (!ImGui::BeginTable("##sunrise_hud_session_table", kColumnCount)) {
        return;
    }
    ImGui::TableSetupColumn("role");
    ImGui::TableSetupColumn("region");
    ImGui::TableSetupColumn("bubble");
    ImGui::TableSetupColumn("group session");
    ImGui::TableSetupColumn("activity host");
    ImGui::TableSetupColumn("channel");
    ImGui::TableSetupColumn("join");
    ImGui::TableHeadersRow();
    for (std::size_t index = 0; index < rowCount; ++index) {
        const DisplayRow& row = rows[index];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(role_name(row));
        ImGui::TableNextColumn();
        draw_region_cells(row.host.regionIndex);
        ImGui::TableNextColumn();
        draw_session_cell(row.host.groupSessionId);
        ImGui::TableNextColumn();
        draw_session_cell(row.host.hostSessionId);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(channel_name(row.stage));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(join_name(admitted, admittedCount, row.host.groupSessionId));
    }
    ImGui::EndTable();
}

} // namespace sunrise::core::ui::hud::overlays::session
