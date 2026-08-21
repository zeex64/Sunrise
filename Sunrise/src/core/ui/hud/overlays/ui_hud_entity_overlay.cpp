/** The synthetic entity overlay. It pairs the server's placement with observed client stages. */

#include "ui_hud_entity_overlay.h"

#include <cstdint>
#include <imgui.h>

#include "../../../../client/hooks/network/sobject_apply_probe.h"
#include "../../../../client/hooks/network/sobject_bind_probe.h"

namespace sunrise::core::ui::hud::overlays::entity {
namespace {

namespace probe = client::hooks::network::sobject_bind_probe;

constexpr int kColumnCount = 2;

/** @return The furthest client-side stage whose postcondition has been observed. */
[[nodiscard]] const char* lifecycle_name(const probe::EntityDebugSnapshot& entity) noexcept {
    if (entity.bound) {
        return "bound";
    }
    if (entity.bindSeen) {
        return "bind deferred";
    }
    if (entity.kind0Seen) {
        return entity.kind0Result ? "native constructed" : "constructor failed";
    }
    if (entity.nativeSeen) {
        return "native registered";
    }
    if (entity.applied) {
        return "apply dispatched";
    }
    if (entity.type2Seen) {
        return entity.type2JobReturned ? "type-2 queued" : "type-2 rejected";
    }
    if (entity.dirtyServiced) {
        return "dirty serviced";
    }
    if (entity.promoted) {
        return "promoted";
    }
    if (entity.decoded) {
        return "decoded";
    }
    return entity.sent ? "sent" : "send failed";
}

/** Begins a simple label/value row. */
void begin_row(const char* label) noexcept {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    ImGui::TableNextColumn();
}

} // namespace

/** Draws the synthetic replicated-entity lifecycle inside the active overlay window. */
void draw() noexcept {
    client::hooks::network::sobject_apply_probe::ActiveManagerDebugSnapshot manager{};
    const bool managerPresent =
        client::hooks::network::sobject_apply_probe::active_manager_debug_snapshot(manager);
    client::hooks::network::sobject_apply_probe::ZLegDebugSnapshot zLeg{};
    const bool zLegPresent =
        client::hooks::network::sobject_apply_probe::z_leg_debug_snapshot(zLeg);
    probe::EntityDebugSnapshot entity{};
    if (!probe::debug_snapshot(entity)) {
        ImGui::TextDisabled("no synthetic entity sent");
        if (managerPresent) {
            ImGui::Text("region %d  slice %d  active %d  wanted ns %d%s",
                        manager.region,
                        manager.nativeSlice,
                        manager.activeAfter,
                        manager.requestedNamespace,
                        manager.ready ? "  ready" : "  waiting");
        }
        if (zLegPresent) {
            ImGui::TextColored(ImVec4{1.0F, 0.55F, 0.25F, 1.0F},
                               "z-leg mode %d  state %d  %d -> %d  axis %d  pos %.1f/%.1f/%.1f",
                               zLeg.transitionMode,
                               zLeg.requestedState,
                               zLeg.anchorRegion,
                               zLeg.destinationRegion,
                               zLeg.axis,
                               static_cast<double>(zLeg.previousCoordinate),
                               static_cast<double>(zLeg.targetCoordinate),
                               static_cast<double>(zLeg.currentCoordinate));
        }
        return;
    }

    const bool ownerNativeCurrent =
        client::hooks::network::sobject_apply_probe::native_manager_active(entity.token);
    if (!ImGui::BeginTable("##sunrise_hud_entity_table", kColumnCount)) {
        return;
    }
    begin_row("State");
    ImGui::TextUnformatted(lifecycle_name(entity));

    begin_row("Identity");
    ImGui::Text("0x%08X  slot %u  RSAT 0x%08X",
                entity.runtimeEntityId != 0 ? entity.runtimeEntityId : entity.entityId,
                static_cast<unsigned>(entity.slot),
                entity.rsat);

    begin_row("Owner");
    ImGui::Text("ns %d  view %u  host %016llX",
                entity.namespaceId,
                static_cast<unsigned>(entity.view),
                static_cast<unsigned long long>(entity.token));

    begin_row("Spatial");
    ImGui::Text("region %d  bubble %u  cell %u",
                entity.region,
                static_cast<unsigned>(entity.bubble),
                static_cast<unsigned>(entity.cell));

    begin_row("Native current");
    if (!managerPresent || manager.activeAfter < 0) {
        ImGui::TextDisabled("unknown");
    } else if (!ownerNativeCurrent) {
        ImGui::TextColored(
            ImVec4{1.0F, 0.55F, 0.25F, 1.0F}, "native ns %d  OWNER MISMATCH", manager.activeAfter);
    } else {
        ImGui::Text("region %d  owner matches", entity.region);
    }

    begin_row("Wire");
    ImGui::Text("attempt %u  sent %u  decoded %u  flags 0x%04X",
                static_cast<unsigned>(entity.attempts),
                entity.sent ? 1U : 0U,
                entity.decoded ? 1U : 0U,
                static_cast<unsigned>(entity.wireFlags));

    begin_row("Runtime");
    ImGui::Text("promote %u  dirty %u  type2 %d/%u  apply %u",
                entity.promoted ? 1U : 0U,
                entity.dirtyServiced ? 1U : 0U,
                entity.type2Seen ? entity.type2Result : -1,
                entity.type2JobReturned ? 1U : 0U,
                entity.applied ? 1U : 0U);

    begin_row("Outbound");
    if (!entity.collectorSeen) {
        ImGui::TextDisabled("collector not observed");
    } else {
        ImGui::Text("internal %d  active %u  eligible %u  candidate %u",
                    entity.collectorInternal,
                    entity.collectorActive ? 1U : 0U,
                    entity.collectorEligible ? 1U : 0U,
                    entity.collectorCandidate ? 1U : 0U);
    }

    begin_row("Manager");
    if (!managerPresent) {
        ImGui::TextDisabled("unknown");
    } else if (!ownerNativeCurrent) {
        ImGui::TextColored(ImVec4{1.0F, 0.55F, 0.25F, 1.0F},
                           "native current ns %d  entity owner ns %d",
                           manager.activeAfter,
                           entity.namespaceId);
    } else {
        ImGui::Text("native current ns %d  owner region %d  slice %d",
                    manager.activeAfter,
                    entity.region,
                    manager.nativeSlice);
    }

    begin_row("Transition");
    if (!zLegPresent) {
        ImGui::TextDisabled("no active z-leg classification");
    } else {
        ImGui::TextColored(ImVec4{1.0F, 0.55F, 0.25F, 1.0F},
                           "mode %d state %d  %d -> %d  axis %d pos %.1f/%.1f/%.1f",
                           zLeg.transitionMode,
                           zLeg.requestedState,
                           zLeg.anchorRegion,
                           zLeg.destinationRegion,
                           zLeg.axis,
                           static_cast<double>(zLeg.previousCoordinate),
                           static_cast<double>(zLeg.targetCoordinate),
                           static_cast<double>(zLeg.currentCoordinate));
    }

    begin_row("Native");
    ImGui::Text("kind0 %u/%u  reg %u  bind %u/%u  index 0x%08X",
                entity.kind0Seen ? 1U : 0U,
                entity.kind0Result ? 1U : 0U,
                entity.nativeSeen ? 1U : 0U,
                entity.bindSeen ? 1U : 0U,
                entity.bound ? 1U : 0U,
                entity.nativeObjectIndex);
    ImGui::EndTable();
}

} // namespace sunrise::core::ui::hud::overlays::entity
