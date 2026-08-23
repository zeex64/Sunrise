/**
 * Operator-facing controls for package-authored EDZ squads. The UI only publishes validated
 * intent into State; native activity and roster work stays on the server's normal update path.
 */

#include "squad_panel.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <string_view>

#include "../../../client/world/native_current_world.h"
#include "../../../core/ui/components/section/ui_section_component.h"
#include "../../../middleware/content/packages/tables/region_reader.h"
#include "../../../state/activity/membership/activity_membership_query.h"
#include "../../../state/activity/squads/activity_squad_control.h"
#include "../../../state/build_data/runtime.h"

namespace sunrise::server::ui::squads {
namespace {

namespace control = state::activity::squads;
namespace layouts = state::build_data::scenarios;
namespace native_current = client::world::native_current;
namespace tables = middleware::content::packages::tables;
namespace membership = state::activity::membership;

/** The installed EDZ free-roam scenario that owns the normalized authored catalog. */
constexpr std::uint32_t kEdzScenarioTag = 0x80B2F00AU;
/** Fixed presentation storage; no UI row retains a game or package string. */
constexpr std::size_t kValueCapacity = 192;
using Value = std::array<char, kValueCapacity>;

/** Exact selected source identity; an object alone is ambiguous in multi-source groups. */
struct SelectedSource final {
    std::uint32_t objectTag{};
    std::uint16_t sourceSlot{};
    bool present{};
};

SelectedSource g_selectedSource{};
std::array<char, 64> g_sourceFilter{};

/** Exact native-current route last observed, used to invalidate a stale operator selection. */
struct CurrentRouteIdentity final {
    std::uint64_t activitySessionId{};
    std::uint64_t currentGroupSessionId{};
    std::uint64_t currentHostSessionId{};
    std::uint64_t currentAdmissionGeneration{};
    std::uint32_t scenarioTag{};
    std::int32_t region{membership::kAbsentRegionIndex};
    std::uint16_t authorityToken{};
};

CurrentRouteIdentity g_observedCurrentRoute{};
bool g_observedCurrentRoutePresent{};

/** @return True when two frames still name the same exact native-current route generation. */
[[nodiscard]] bool same_current_route(const CurrentRouteIdentity& left,
                                      const CurrentRouteIdentity& right) noexcept {
    return left.activitySessionId == right.activitySessionId
           && left.currentGroupSessionId == right.currentGroupSessionId
           && left.currentHostSessionId == right.currentHostSessionId
           && left.currentAdmissionGeneration == right.currentAdmissionGeneration
           && left.scenarioTag == right.scenarioTag && left.region == right.region
           && left.authorityToken == right.authorityToken;
}

/** One coherent frame of native-current world, catalog, request, and readiness state. */
struct View final {
    native_current::Route route{};
    layouts::Definition layout{};
    control::CatalogSnapshot catalog{};
    control::DebugSnapshot debug{};
    std::array<std::size_t, control::kCatalogCapacity> currentRows{};
    std::size_t currentRowCount{};
    bool layoutPresent{};
    bool routeReady{};
};

/** @return The primary activity's bounded package name. */
[[nodiscard]] std::string_view destination_name(const View& view) noexcept {
    const auto& destination = view.route.current.world.destination;
    const std::size_t length = destination.packageNameLength < destination.packageName.size()
                                   ? destination.packageNameLength
                                   : destination.packageName.size();
    return {reinterpret_cast<const char*>(destination.packageName.data()), length};
}

/** @return A friendly fixed label for one catalog source row. */
[[nodiscard]] std::string_view source_name(const control::Source& source,
                                           Value& fallback) noexcept {
    fallback = {};
    const std::size_t length = source.displayNameLength < fallback.size() - 1
                                   ? source.displayNameLength
                                   : fallback.size() - 1;
    for (std::size_t index = 0; index < length; ++index) {
        const char value = source.displayName[index];
        fallback[index] = value == '_' ? ' ' : value;
    }
    return {fallback.data(), length};
}

/** @return The bounded archive-derived label for one proved authored location. */
[[nodiscard]] std::string_view
authored_location_name(const control::AuthoredLocation& location) noexcept {
    const std::size_t length = location.displayNameLength < location.displayName.size()
                                   ? location.displayNameLength
                                   : location.displayName.size();
    return {location.displayName.data(), length};
}

/** @return A source's complete normalized group, or null for a corrupt foreign key. */
[[nodiscard]] const control::Group* source_group(const View& view,
                                                 const control::Source& source) noexcept {
    return source.groupIndex < view.catalog.groupCount
                   && source.groupIndex < view.catalog.groups.size()
               ? &view.catalog.groups[source.groupIndex]
               : nullptr;
}

/** @return The selected row if it is still authored in the exact native-current region. */
[[nodiscard]] const control::Source* selected_source(const View& view) noexcept {
    for (std::size_t row = 0; row < view.currentRowCount; ++row) {
        const control::Source& source = view.catalog.sources[view.currentRows[row]];
        const control::Group* group = source_group(view, source);
        if (group != nullptr && g_selectedSource.present
            && group->objectTag == g_selectedSource.objectTag
            && source.sourceSlot == g_selectedSource.sourceSlot) {
            return &source;
        }
    }
    return nullptr;
}

/** Builds one frame without retaining any pointer owned by another subsystem. */
[[nodiscard]] View read_view() noexcept {
    View view{};
    view.routeReady = native_current::resolve_route(view.route);
    control::snapshot_catalog(view.catalog);
    control::snapshot_debug(view.debug);

    const std::string_view name = destination_name(view);
    view.layoutPresent =
        !name.empty() && state::build_data::find_scenario_layout(name, view.layout);

    const CurrentRouteIdentity currentRoute{
        .activitySessionId = view.route.current.world.sessionId,
        .currentGroupSessionId = view.route.current.groupSessionId,
        .currentHostSessionId = view.route.current.hostSessionId,
        .currentAdmissionGeneration = view.route.admissionGeneration,
        .scenarioTag = view.layout.tag,
        .region = view.route.current.world.region,
        .authorityToken = view.route.authorityToken,
    };
    if (!g_observedCurrentRoutePresent
        || !same_current_route(currentRoute, g_observedCurrentRoute)) {
        g_selectedSource = {};
        g_observedCurrentRoute = currentRoute;
        g_observedCurrentRoutePresent = true;
    }

    if (view.routeReady && view.route.current.activityPresent
        && view.route.current.selection == native_current::Selection::selected && view.layoutPresent
        && view.layout.tag == kEdzScenarioTag && view.route.current.world.region >= 0) {
        for (std::size_t index = 0;
             index < view.catalog.sourceCount && index < view.catalog.sources.size();
             ++index) {
            const control::Source& source = view.catalog.sources[index];
            const control::Group* group = source_group(view, source);
            if (group != nullptr && group->scenarioTag == view.layout.tag
                && group->region == view.route.current.world.region) {
                view.currentRows[view.currentRowCount] = index;
                ++view.currentRowCount;
            }
        }
    }

    // A selection is meaningful only while its authored row belongs to this exact current route.
    if (selected_source(view) == nullptr) {
        g_selectedSource = {};
    }
    return view;
}

/** Draws one two-column value row. */
void draw_value(const char* caption, std::string_view value) noexcept {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", caption);
    ImGui::TableNextColumn();
    if (value.empty()) {
        ImGui::TextDisabled("Not available");
        return;
    }
    ImGui::TextUnformatted(value.data(), value.data() + value.size());
}

/** Draws an explanatory line using the normal muted presentation. */
void draw_muted(std::string_view value) noexcept {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%.*s", static_cast<int>(value.size()), value.data());
    ImGui::PopStyleColor();
}

/** @return Lower-case ASCII for package labels and the operator's simple text filter. */
[[nodiscard]] char ascii_lower(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

/** @return True when one fixed label contains the current filter, case-insensitively. */
[[nodiscard]] bool label_matches_filter(const char* label,
                                        std::size_t labelLength,
                                        std::size_t filterLength) noexcept {
    if (filterLength == 0) {
        return true;
    }
    if (label == nullptr || filterLength > labelLength) {
        return false;
    }
    for (std::size_t start = 0; start + filterLength <= labelLength; ++start) {
        bool equal = true;
        for (std::size_t index = 0; index < filterLength; ++index) {
            char labelValue = label[start + index];
            if (labelValue == '_' || labelValue == '.') {
                labelValue = ' ';
            }
            char filterValue = g_sourceFilter[index];
            if (filterValue == '_' || filterValue == '.') {
                filterValue = ' ';
            }
            if (ascii_lower(labelValue) != ascii_lower(filterValue)) {
                equal = false;
                break;
            }
        }
        if (equal) {
            return true;
        }
    }
    return false;
}

/** @return True when a source or any location mapped to it contains the current filter. */
[[nodiscard]] bool source_matches_filter(const View& view, const control::Source& source) noexcept {
    std::size_t filterLength = 0;
    while (filterLength < g_sourceFilter.size() && g_sourceFilter[filterLength] != '\0') {
        ++filterLength;
    }
    const std::size_t sourceLength = source.displayNameLength < source.displayName.size()
                                         ? source.displayNameLength
                                         : source.displayName.size();
    if (label_matches_filter(source.displayName.data(), sourceLength, filterLength)) {
        return true;
    }
    const control::AuthoredLocation& authored = source.authoredLocation;
    const std::size_t authoredLength = authored.displayNameLength < authored.displayName.size()
                                           ? authored.displayNameLength
                                           : authored.displayName.size();
    if (label_matches_filter(authored.displayName.data(), authoredLength, filterLength)) {
        return true;
    }
    for (std::size_t index = 0;
         index < view.catalog.locationEvidenceCount && index < view.catalog.locationEvidence.size();
         ++index) {
        const control::LocationEvidence& evidence = view.catalog.locationEvidence[index];
        if (evidence.groupIndex != source.groupIndex || evidence.sourceSlot != source.sourceSlot) {
            continue;
        }
        const auto& label = evidence.location.displayName;
        const std::size_t length = evidence.location.displayNameLength < label.size()
                                       ? evidence.location.displayNameLength
                                       : label.size();
        if (label_matches_filter(label.data(), length, filterLength)) {
            return true;
        }
    }
    return false;
}

/** @return Conservative package-evidence class for one debug row. */
[[nodiscard]] const char* classification_name(control::SourceClassification value) noexcept {
    switch (value) {
    case control::SourceClassification::combat:
        return "combat source";
    case control::SourceClassification::vendorConflict:
        return "vendor conflict";
    case control::SourceClassification::scripted:
        return "scripted encounter";
    case control::SourceClassification::unknown:
        return "unknown behavior";
    }
    return "unknown behavior";
}

/** @return Conservative confidence that the source creates fightable enemy actors. */
[[nodiscard]] const char* confidence_name(control::EnemyConfidence value) noexcept {
    switch (value) {
    case control::EnemyConfidence::low:
        return "low enemy confidence";
    case control::EnemyConfidence::medium:
        return "medium enemy confidence";
    case control::EnemyConfidence::high:
        return "high enemy confidence";
    }
    return "low enemy confidence";
}

/** Builds the friendly bubble value from the exact native-current region. */
void build_bubble(const View& view, Value& output) noexcept {
    output = {};
    if (!view.route.current.activityPresent || !view.layoutPresent
        || view.route.current.world.region < 0) {
        return;
    }
    const std::size_t bubble =
        static_cast<std::uint32_t>(view.route.current.world.region) / tables::kSliceSetIndexFactor;
    if (bubble >= view.layout.bubbleCount || bubble >= view.layout.bubbleHashes.size()) {
        return;
    }

    state::build_data::hash_names::Name named{};
    const bool namePresent =
        state::build_data::find_hash_name(view.layout.bubbleHashes[bubble], named);
    if (!namePresent) {
        (void)std::snprintf(output.data(), output.size(), "%zu", bubble);
        return;
    }
    (void)std::snprintf(output.data(),
                        output.size(),
                        "%zu - %.*s",
                        bubble,
                        static_cast<int>(named.nameLength),
                        named.name.data());
}

/** @return True only when this frame can safely publish the selected authored request. */
[[nodiscard]] bool can_place(const View& view, const control::Source* source) noexcept {
    const native_current::Snapshot& current = view.route.current;
    const control::Group* group = source != nullptr ? source_group(view, *source) : nullptr;
    return source != nullptr && group != nullptr && source->authResolved
           && source->authSchema == control::kSquadAuthSchema && view.routeReady && current.inWorld
           && current.activityPresent && current.selection == native_current::Selection::selected
           && view.layoutPresent && view.layout.tag == kEdzScenarioTag && current.world.region >= 0
           && group->scenarioTag == view.layout.tag && group->region == current.world.region
           && source->memberSlotCount > 0 && source->defaultRequestedCount == 1
           && source->maximumRequestedCount >= 1
           && current.world.sessionId != control::kAbsentSessionId
           && current.groupSessionId != control::kAbsentSessionId
           && current.hostSessionId != control::kAbsentSessionId
           && current.hostSessionId != current.world.sessionId
           && view.route.admissionGeneration != 0 && view.route.authorityToken != 0
           && !view.debug.requestActive;
}

/** @return Plain-language reason placement is or is not ready. */
[[nodiscard]] const char* readiness_text(const View& view, const control::Source* source) noexcept {
    const native_current::Snapshot& current = view.route.current;
    if (!current.inWorld) {
        return "Enter the EDZ and wait for your character to finish loading.";
    }
    if (!current.activityPresent) {
        return "Waiting for the main activity session.";
    }
    if (!view.layoutPresent) {
        return "Waiting for the current activity details.";
    }
    if (view.layout.tag != kEdzScenarioTag) {
        return "Squad placement is currently available only in EDZ free roam.";
    }
    if (current.world.sessionId == control::kAbsentSessionId) {
        return "Waiting for the main activity session.";
    }
    if (view.debug.requestActive && view.debug.committedLatched) {
        return "This squad update is active. It retires safely when you leave the activity.";
    }
    if (view.debug.requestActive) {
        return "A squad placement is already pending. Clear it before placing another.";
    }
    if (current.selection == native_current::Selection::ambiguous) {
        return "Waiting for the game to finish choosing the current area.";
    }
    if (current.selection != native_current::Selection::selected || !view.routeReady) {
        return "Waiting for the current area's gameplay route to finish preparing.";
    }
    if (view.currentRowCount == 0) {
        return "No supported authored squad is available in this area yet.";
    }
    if (source == nullptr) {
        return "Choose a squad below.";
    }
    if (!source->authResolved) {
        return "This exact location is mapped, but its squad member layout is not proved yet. It "
               "is display-only to protect the game from an invalid Auth body.";
    }
    return "Ready to place this authored squad in the area currently on screen.";
}

/** Draws only the exact native-current area that can receive a squad. */
void draw_where(const View& view) noexcept {
    core::ui::components::section::header(
        "Current area", "Squads follow the area the game is currently simulating.");

    Value bubble{};
    Value currentRegion{};
    build_bubble(view, bubble);
    if (view.route.current.selection == native_current::Selection::selected
        && view.route.current.world.region >= 0) {
        (void)std::snprintf(
            currentRegion.data(), currentRegion.size(), "%d", view.route.current.world.region);
    }

    if (ImGui::BeginTable("##squad_where", 2, ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("##where_label",
                                ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("Current bubble").x
                                    + (ImGui::GetStyle().ItemSpacing.x * 2.0F));
        ImGui::TableSetupColumn("##where_value", ImGuiTableColumnFlags_WidthStretch);
        draw_value("Activity", destination_name(view));
        draw_value("Current region", std::string_view(currentRegion.data()));
        draw_value("Current bubble", std::string_view(bubble.data()));
        draw_value("Current route", view.routeReady ? "ready" : "waiting");
        ImGui::EndTable();
    }
}

/** Draws only catalog rows authored for the exact native-current region. */
void draw_candidates(View& view) noexcept {
    ImGui::Spacing();
    core::ui::components::section::header(
        "Spawn sources in this area",
        "Only package-authored sources for the exact current area can be selected.");
    if (view.currentRowCount == 0) {
        draw_muted("There are no supported authored spawn sources for the current area.");
        return;
    }

    ImGui::SetNextItemWidth(-1.0F);
    (void)ImGui::InputTextWithHint("##squad_source_filter",
                                   "Filter names or locations",
                                   g_sourceFilter.data(),
                                   g_sourceFilter.size());
    ImGui::Spacing();

    std::array<std::size_t, control::kCatalogCapacity> visibleRows{};
    std::size_t visibleCount = 0;
    for (std::size_t row = 0; row < view.currentRowCount; ++row) {
        const std::size_t catalogRow = view.currentRows[row];
        const control::Source& source = view.catalog.sources[catalogRow];
        if (source_group(view, source) != nullptr && source_matches_filter(view, source)) {
            visibleRows[visibleCount] = catalogRow;
            ++visibleCount;
        }
    }

    if (ImGui::BeginChild("##squad_source_rows", {0.0F, 280.0F})) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(visibleCount));
        while (clipper.Step()) {
            for (int visible = clipper.DisplayStart; visible < clipper.DisplayEnd; ++visible) {
                const control::Source& source =
                    view.catalog.sources[visibleRows[static_cast<std::size_t>(visible)]];
                const control::Group* group = source_group(view, source);
                if (group == nullptr) {
                    continue;
                }
                Value fallback{};
                const std::string_view name = source_name(source, fallback);
                const bool selected = g_selectedSource.present
                                      && g_selectedSource.objectTag == group->objectTag
                                      && g_selectedSource.sourceSlot == source.sourceSlot;
                ImGui::PushID(static_cast<int>(group->objectTag & 0x7FFFFFFFU));
                ImGui::PushID(static_cast<int>(source.sourceSlot));
                ImGui::BeginDisabled(view.debug.requestActive || !source.authResolved);
                if (ImGui::Selectable(name.data(), selected)) {
                    g_selectedSource = {group->objectTag, source.sourceSlot, true};
                }
                ImGui::Indent();
                ImGui::TextDisabled("%s / %s / %s",
                                    classification_name(source.classification),
                                    confidence_name(source.enemyConfidence),
                                    source.authResolved ? "spawn enabled" : "display only");
                const std::string_view location = authored_location_name(source.authoredLocation);
                ImGui::TextDisabled("%.*s / anchor 0x%08X:%u",
                                    static_cast<int>(location.size()),
                                    location.data(),
                                    source.authoredLocation.anchor.objectTag,
                                    static_cast<unsigned>(source.authoredLocation.anchor.ordinal));
                ImGui::TextDisabled("Position %.2f, %.2f, %.2f",
                                    static_cast<double>(source.authoredLocation.anchor.x),
                                    static_cast<double>(source.authoredLocation.anchor.y),
                                    static_cast<double>(source.authoredLocation.anchor.z));
                ImGui::Unindent();
                ImGui::EndDisabled();
                ImGui::PopID();
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
    if (visibleCount == 0) {
        draw_muted("No authored source in the current region matches this filter.");
    }
}

/** Draws proved rule-to-anchor locations without offering a control message 5 cannot honor. */
void draw_authored_locations(const View& view, const control::Source* source) noexcept {
    ImGui::Spacing();
    core::ui::components::section::header(
        "Authored spawn locations",
        "These are exact package locations; Sunrise does not inject coordinates.");
    if (source == nullptr) {
        draw_muted("Choose an authored source to see its proved spawn locations.");
        return;
    }

    const control::AuthoredLocation& location = source->authoredLocation;
    {
        const std::string_view name = authored_location_name(location);
        ImGui::TextUnformatted(name.data(), name.data() + name.size());
        ImGui::Indent();
        ImGui::TextDisabled("Source-specific rule; selected by this exact type-1 slot");
        Value coordinates{};
        (void)std::snprintf(coordinates.data(),
                            coordinates.size(),
                            "Approx. authored position: %.3f, %.3f, %.3f",
                            static_cast<double>(location.anchor.x),
                            static_cast<double>(location.anchor.y),
                            static_cast<double>(location.anchor.z));
        ImGui::TextDisabled("%s", coordinates.data());
        ImGui::Unindent();
    }

    for (std::size_t index = 0;
         index < view.catalog.locationEvidenceCount && index < view.catalog.locationEvidence.size();
         ++index) {
        const control::LocationEvidence& evidence = view.catalog.locationEvidence[index];
        if (evidence.groupIndex != source->groupIndex
            || evidence.sourceSlot != source->sourceSlot) {
            continue;
        }
        ImGui::Spacing();
        const std::string_view name = authored_location_name(evidence.location);
        ImGui::TextUnformatted(name.data(), name.data() + name.size());
        ImGui::Indent();
        ImGui::TextDisabled("Package evidence only - the type-1 ClientRef cannot select it");
        ImGui::TextDisabled("Position %.3f, %.3f, %.3f",
                            static_cast<double>(evidence.location.anchor.x),
                            static_cast<double>(evidence.location.anchor.y),
                            static_cast<double>(evidence.location.anchor.z));
        ImGui::Unindent();
    }
}

/** @return A plain description of the current or last request lifecycle. */
[[nodiscard]] const char* lifecycle_text(const control::DebugSnapshot& debug) noexcept {
    switch (debug.lifecycle) {
    case control::Lifecycle::idle:
        return "No squad placement has been requested.";
    case control::Lifecycle::requested:
        return "Placement requested. Waiting for the next world update.";
    case control::Lifecycle::prepared:
        return "The squad update is ready to send.";
    case control::Lifecycle::committed:
        return "The squad update was sent to the game.";
    case control::Lifecycle::discarded:
        return "The world changed before an update was sent. The active request can retry.";
    case control::Lifecycle::cleared:
        return "The active squad request was cleared.";
    case control::Lifecycle::refused:
        break;
    }

    switch (debug.refusalReason) {
    case control::RefusalReason::worldNotReady:
        return "Placement paused because the world was not ready.";
    case control::RefusalReason::activitySessionChanged:
        return "Placement stopped because the activity changed.";
    case control::RefusalReason::currentGroupChanged:
        return "Placement stopped because the current area's session changed.";
    case control::RefusalReason::currentHostChanged:
        return "Placement stopped because the current area's host changed.";
    case control::RefusalReason::scenarioChanged:
        return "Placement stopped because the current activity changed.";
    case control::RefusalReason::regionChanged:
        return "Placement stopped because you moved to another region.";
    case control::RefusalReason::candidateUnavailable:
        return "That authored squad is no longer available in this region.";
    case control::RefusalReason::rosterCapacity:
        return "The activity roster has no safe room for this squad.";
    case control::RefusalReason::invalidAuth:
        return "The game rejected the authored squad state.";
    case control::RefusalReason::none:
        return "The squad request could not be prepared.";
    }
    return "The squad request could not be prepared.";
}

/** @return Stable technical name for native-current selection. */
[[nodiscard]] const char* selection_name(native_current::Selection selection) noexcept {
    switch (selection) {
    case native_current::Selection::selected:
        return "selected";
    case native_current::Selection::ambiguous:
        return "ambiguous";
    case native_current::Selection::unavailable:
        return "unavailable";
    }
    return "unavailable";
}

/** @return Stable technical name for the request lifecycle. */
[[nodiscard]] const char* lifecycle_name(control::Lifecycle lifecycle) noexcept {
    switch (lifecycle) {
    case control::Lifecycle::idle:
        return "idle";
    case control::Lifecycle::requested:
        return "requested";
    case control::Lifecycle::prepared:
        return "prepared";
    case control::Lifecycle::committed:
        return "committed";
    case control::Lifecycle::discarded:
        return "discarded";
    case control::Lifecycle::refused:
        return "refused";
    case control::Lifecycle::cleared:
        return "cleared";
    }
    return "idle";
}

/** Draws the controls, publishing only through the activity squad State API. */
void draw_actions(View& view, const control::Source* source) noexcept {
    ImGui::Spacing();
    core::ui::components::section::header(
        "Placement", "One authored member is used while the first placement path is verified.");

    Value members{};
    const std::int32_t requestedCount = source != nullptr ? source->defaultRequestedCount : 0;
    if (source != nullptr && source->authResolved) {
        (void)std::snprintf(members.data(), members.size(), "%d (fixed for now)", requestedCount);
    } else {
        (void)std::snprintf(members.data(), members.size(), "Unresolved - display only");
    }
    if (ImGui::BeginTable("##squad_members", 2, ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("##members_label",
                                ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("Members").x
                                    + (ImGui::GetStyle().ItemSpacing.x * 2.0F));
        ImGui::TableSetupColumn("##members_value", ImGuiTableColumnFlags_WidthStretch);
        draw_value("Members", std::string_view(members.data()));
        ImGui::EndTable();
    }

    const control::Group* group = source != nullptr ? source_group(view, *source) : nullptr;
    const bool ready = can_place(view, source);
    bool publishFailed = false;
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("Place squad") && source != nullptr && group != nullptr) {
        const control::PlacementRequestInput input{
            .activitySessionId = view.route.current.world.sessionId,
            .currentGroupSessionId = view.route.current.groupSessionId,
            .currentHostSessionId = view.route.current.hostSessionId,
            .currentAdmissionGeneration = view.route.admissionGeneration,
            .scenarioTag = view.layout.tag,
            .region = view.route.current.world.region,
            .candidateObjectTag = group->objectTag,
            .candidateSourceSlot = source->sourceSlot,
            .requestedCount = requestedCount,
            .authorityToken = view.route.authorityToken,
        };
        control::PlacementRequest request{};
        publishFailed = !control::request_placement(input, request);
        control::snapshot_debug(view.debug);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!view.debug.requestActive || view.debug.committedLatched
                         || view.debug.lifecycle == control::Lifecycle::prepared);
    if (ImGui::Button("Clear active request")) {
        control::clear();
        control::snapshot_debug(view.debug);
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    if (publishFailed) {
        ImGui::TextWrapped("The world changed before the request was accepted. Check the status "
                           "and try again.");
    } else {
        draw_muted(readiness_text(view, source));
    }
    ImGui::Spacing();
    ImGui::TextWrapped("%s", lifecycle_text(view.debug));
}

/** Draws exact identities and counters only when the operator asks for them. */
void draw_technical(const View& view, const control::Source* source) noexcept {
    ImGui::Spacing();
    if (!ImGui::CollapsingHeader("Technical details")) {
        return;
    }
    if (!ImGui::BeginTable("##squad_technical", 2, ImGuiTableFlags_NoSavedSettings)) {
        return;
    }
    ImGui::TableSetupColumn("##technical_label",
                            ImGuiTableColumnFlags_WidthFixed,
                            ImGui::CalcTextSize("Current admission generation").x
                                + (ImGui::GetStyle().ItemSpacing.x * 2.0F));
    ImGui::TableSetupColumn("##technical_value", ImGuiTableColumnFlags_WidthStretch);

    draw_value("Native current", selection_name(view.route.current.selection));

    Value value{};
    (void)std::snprintf(value.data(), value.size(), "0x%08X", view.layout.tag);
    draw_value("Scenario", std::string_view(value.data()));
    (void)std::snprintf(value.data(),
                        value.size(),
                        "%d / 0x%08X",
                        view.route.current.world.region,
                        view.route.current.world.regionHash);
    draw_value("Current region / hash", std::string_view(value.data()));
    (void)std::snprintf(value.data(),
                        value.size(),
                        "0x%016llX",
                        static_cast<unsigned long long>(view.route.current.world.sessionId));
    draw_value("Root activity session", std::string_view(value.data()));
    (void)std::snprintf(value.data(),
                        value.size(),
                        "0x%016llX",
                        static_cast<unsigned long long>(view.route.current.groupSessionId));
    draw_value("Current group session", std::string_view(value.data()));
    (void)std::snprintf(value.data(),
                        value.size(),
                        "0x%016llX",
                        static_cast<unsigned long long>(view.route.current.hostSessionId));
    draw_value("Current host session", std::string_view(value.data()));
    (void)std::snprintf(value.data(),
                        value.size(),
                        "admission %llu / authority %u",
                        static_cast<unsigned long long>(view.route.admissionGeneration),
                        static_cast<unsigned>(view.route.authorityToken));
    draw_value("Current generations", std::string_view(value.data()));
    draw_value("Current route", view.routeReady ? "ready" : "waiting");

    const control::Group* group = source != nullptr ? source_group(view, *source) : nullptr;
    if (source != nullptr && group != nullptr) {
        (void)std::snprintf(value.data(),
                            value.size(),
                            "object 0x%08X / registry 0x%08X / slot %u",
                            group->objectTag,
                            group->registryKey,
                            static_cast<unsigned>(source->sourceSlot));
        draw_value("Selected source", std::string_view(value.data()));
        const control::AuthoredLocation& location = source->authoredLocation;
        (void)std::snprintf(value.data(),
                            value.size(),
                            "rule 0x%08X / slot %u / anchor 0x%08X:%u / %.3f, %.3f, %.3f",
                            location.spawnRuleTag,
                            static_cast<unsigned>(location.spawnRuleSlot),
                            location.anchor.objectTag,
                            static_cast<unsigned>(location.anchor.ordinal),
                            static_cast<double>(location.anchor.x),
                            static_cast<double>(location.anchor.y),
                            static_cast<double>(location.anchor.z));
        draw_value("Source-specific location", std::string_view(value.data()));
        (void)std::snprintf(value.data(),
                            value.size(),
                            "schema 0x%08X / resolved %s / member slots %u",
                            source->authSchema,
                            source->authResolved ? "yes" : "no",
                            static_cast<unsigned>(source->memberSlotCount));
        draw_value("Squad Auth", std::string_view(value.data()));
    } else {
        draw_value("Selected source", {});
    }

    (void)std::snprintf(value.data(),
                        value.size(),
                        "%s / revision %llu / active %s",
                        lifecycle_name(view.debug.lifecycle),
                        static_cast<unsigned long long>(view.debug.lifecycleRevision),
                        view.debug.requestActive ? "yes" : "no");
    draw_value("Request lifecycle", std::string_view(value.data()));
    draw_value("Committed identity", view.debug.committedLatched ? "latched" : "not latched");
    (void)std::snprintf(value.data(),
                        value.size(),
                        "%llu / generation %u / count %d",
                        static_cast<unsigned long long>(view.debug.request.requestId),
                        view.debug.request.spawnGeneration,
                        view.debug.request.requestedCount);
    draw_value("Request", std::string_view(value.data()));
    (void)std::snprintf(value.data(),
                        value.size(),
                        "root %016llX / group %016llX / host %016llX",
                        static_cast<unsigned long long>(view.debug.request.activitySessionId),
                        static_cast<unsigned long long>(view.debug.request.currentGroupSessionId),
                        static_cast<unsigned long long>(view.debug.request.currentHostSessionId));
    draw_value("Request sessions", std::string_view(value.data()));
    (void)std::snprintf(value.data(),
                        value.size(),
                        "scenario 0x%08X / region %d / object 0x%08X / slot %u",
                        view.debug.request.scenarioTag,
                        view.debug.request.region,
                        view.debug.request.candidateObjectTag,
                        static_cast<unsigned>(view.debug.request.candidateSourceSlot));
    draw_value("Request world", std::string_view(value.data()));
    (void)std::snprintf(
        value.data(),
        value.size(),
        "admission %llu / authority %u",
        static_cast<unsigned long long>(view.debug.request.currentAdmissionGeneration),
        static_cast<unsigned>(view.debug.request.authorityToken));
    draw_value("Request generations", std::string_view(value.data()));
    (void)std::snprintf(value.data(),
                        value.size(),
                        "prepared %u / sent %u / discarded %u / refused %u",
                        view.debug.preparedCount,
                        view.debug.committedCount,
                        view.debug.discardedCount,
                        view.debug.refusedCount);
    draw_value("Counters", std::string_view(value.data()));
    ImGui::EndTable();
}

} // namespace

/** Draws the authored squad placement page inside the active Core UI frame. */
void draw() noexcept {
    View view = read_view();
    draw_where(view);
    draw_candidates(view);
    const control::Source* source = selected_source(view);
    draw_authored_locations(view, source);
    draw_actions(view, source);
    draw_technical(view, source);
}

} // namespace sunrise::server::ui::squads
