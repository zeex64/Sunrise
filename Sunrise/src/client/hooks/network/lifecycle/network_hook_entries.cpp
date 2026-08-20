#include "network_hook_entries.h"

#include "../../../targets/game.h"
#include "../../../targets/steam_targets.h"
#include "../account_soid_probe.h"
#include "../activity_host_probe.h"
#include "../activity_mode_probe.h"
#include "../activity_route_probe.h"
#include "../bubble_authority/bubble_authority_replacements.h"
#include "../coordinator/network_call_coordinator.h"
#include "../entity_create_probe.h"
#include "../entity_slot_probe.h"
#include "../membership_update_probe.h"
#include "../scheduler_handler_probe.h"
#include "../scheduler_signature_probe.h"
#include "../sobject_apply_probe.h"
#include "../sobject_bind_probe.h"
#include "../sobject_create_probe.h"
#include "../sobject_native_probe.h"
#include "../sobject_update_probe.h"
#include "../view_creation_probe.h"
#include "../view_membership_probe.h"
#include "../view_message_probe.h"
#include "../view_readiness_probe.h"
#include "../view_signature_capture.h"
#include "../view_slot_probe.h"

namespace sunrise::client::hooks::network::lifecycle {
namespace {

/** @return The game replacement bodies, in hook-slot order. */
[[nodiscard]] std::array<void*, kGameSlots.size()> game_replacements() noexcept {
    return {
        platform::transport_kind_entry_point(),
        http::execute_request_entry_point(),
        bubble_authority::decoder_entry_point(),
        bubble_authority::content_untracked_entry_point(),
        view_signature::refresh_entry_point(),
        view_message_probe::lookup_entry_point(),
        view_readiness_probe::scan_entry_point(),
        view_slot_probe::pump_entry_point(),
        scheduler_signature_probe::encoder_entry_point(),
        sobject_create_probe::encoder_entry_point(),
        sobject_update_probe::encoder_entry_point(),
        sobject_native_probe::registration_entry_point(),
        sobject_bind_probe::binder_entry_point(),
        entity_create_probe::encoder_entry_point(),
        entity_slot_probe::decoder_entry_point(),
        scheduler_handler_probe::event_entry_point(),
        scheduler_handler_probe::mask_entry_point(),
        scheduler_handler_probe::entity_prelude_entry_point(),
        scheduler_handler_probe::fixed_entry_point(),
        view_membership_probe::sync_entry_point(),
        view_membership_probe::wire_entry_point(),
        view_membership_probe::decoded_entry_point(),
        membership_update_probe::encoder_entry_point(),
        account_soid_probe::validator_entry_point(),
        account_soid_probe::publisher_entry_point(),
        view_creation_probe::creator_entry_point(),
        activity_host_probe::decoder_entry_point(),
        activity_host_probe::connection_state_entry_point(),
        activity_route_probe::record_entry_point(),
        activity_route_probe::local_entry_point(),
        activity_route_probe::authored_entry_point(),
        activity_mode_probe::selector_entry_point(),
        activity_mode_probe::setter_entry_point(),
        activity_mode_probe::type_resolver_entry_point(),
        signon::readiness_entry_point(),
        signon::ready_entry_point(),
        sobject_apply_probe::apply_entry_point(),
        sobject_apply_probe::kind0_entry_point(),
        sobject_apply_probe::promotion_entry_point(),
        sobject_apply_probe::dirty_service_entry_point(),
        sobject_apply_probe::backend_busy_entry_point(),
        sobject_apply_probe::type2_job_entry_point(),
    };
}

/** @return The platform replacement bodies, in hook-slot order. */
[[nodiscard]] std::array<void*, kPlatformSlots.size()> platform_replacements() noexcept {
    return {
        platform::authentication_status_entry_point(),
        platform::set_certificate_entry_point(),
    };
}

} // namespace

/** @return Game target and body pairs, in slot order. */
GameSpecs game_specs() noexcept {
    const auto replacements = game_replacements();
    const targets::game::network::Targets& resolved = targets::game::network::get();
    return {
        hooking::detour::Spec{resolved.transportKind, replacements[0]},
        hooking::detour::Spec{resolved.httpExecuteRequest, replacements[1]},
        hooking::detour::Spec{resolved.bubbleAuthorityDecoder, replacements[2]},
        hooking::detour::Spec{resolved.contentUntrackedGetter, replacements[3]},
        hooking::detour::Spec{resolved.viewSignatureRefresh, replacements[4]},
        hooking::detour::Spec{resolved.viewMessageLookup, replacements[5]},
        hooking::detour::Spec{resolved.viewReadinessScan, replacements[6]},
        hooking::detour::Spec{resolved.viewSlotPump, replacements[7]},
        hooking::detour::Spec{resolved.schedulerSignatureEncoder, replacements[8]},
        hooking::detour::Spec{resolved.sobjectCreateEncoder, replacements[9]},
        hooking::detour::Spec{resolved.sobjectUpdateEncoder, replacements[10]},
        hooking::detour::Spec{resolved.sobjectNativeRegistration, replacements[11]},
        hooking::detour::Spec{resolved.sobjectBinder, replacements[12]},
        hooking::detour::Spec{resolved.entityCreateEncoder, replacements[13]},
        hooking::detour::Spec{resolved.entitySlotDecoder, replacements[14]},
        hooking::detour::Spec{resolved.schedulerEventDecoder, replacements[15]},
        hooking::detour::Spec{resolved.schedulerMaskDecoder, replacements[16]},
        hooking::detour::Spec{resolved.schedulerEntityPreludeDecoder, replacements[17]},
        hooking::detour::Spec{resolved.schedulerFixedDecoder, replacements[18]},
        hooking::detour::Spec{resolved.viewMembershipSync, replacements[19]},
        hooking::detour::Spec{resolved.activityMembershipDecoder, replacements[20]},
        hooking::detour::Spec{resolved.activityMembershipQueue, replacements[21]},
        hooking::detour::Spec{resolved.membershipUpdateEncoder, replacements[22]},
        hooking::detour::Spec{resolved.accountSoidValidator, replacements[23]},
        hooking::detour::Spec{resolved.accountSoidPublisher, replacements[24]},
        hooking::detour::Spec{resolved.viewCreator, replacements[25]},
        hooking::detour::Spec{resolved.activityHostDecoder, replacements[26]},
        hooking::detour::Spec{resolved.activityHostConnectionState, replacements[27]},
        hooking::detour::Spec{resolved.activityRouteRecord, replacements[28]},
        hooking::detour::Spec{resolved.activityRouteLocal, replacements[29]},
        hooking::detour::Spec{resolved.activityRouteAuthored, replacements[30]},
        hooking::detour::Spec{resolved.activityModeSelector, replacements[31]},
        hooking::detour::Spec{resolved.activityModeSetter, replacements[32]},
        hooking::detour::Spec{resolved.activityTypeResolver, replacements[33]},
        hooking::detour::Spec{resolved.signOnReadinessFailure, replacements[34]},
        hooking::detour::Spec{resolved.signOnReadinessReady, replacements[35]},
        hooking::detour::Spec{resolved.sobjectApplyJob, replacements[36]},
        hooking::detour::Spec{resolved.sobjectKind0Constructor, replacements[37]},
        hooking::detour::Spec{resolved.sobjectRecordPromotion, replacements[38]},
        hooking::detour::Spec{resolved.sobjectDirtyService, replacements[39]},
        hooking::detour::Spec{resolved.sobjectBackendBusy, replacements[40]},
        hooking::detour::Spec{resolved.sobjectType2Job, replacements[41]},
    };
}

/** @return Platform target and body pairs, in slot order. */
PlatformSpecs platform_specs() noexcept {
    const auto replacements = platform_replacements();
    const targets::steam::Targets& resolved = targets::steam::get();
    return {
        hooking::detour::Spec{resolved.authenticationStatus, replacements[0]},
        hooking::detour::Spec{resolved.setCertificate, replacements[1]},
    };
}

/** @return The game replacement, ingress and egress bodies kept safe at detach. */
GameProtectedEntries game_protected_entries() noexcept {
    const auto replacements = game_replacements();
    GameProtectedEntries entries{};
    for (std::size_t index = 0; index < replacements.size(); ++index) {
        entries[index].address = replacements[index];
    }
    entries[replacements.size()].address = coordinator::ingress_entry_point();
    entries[replacements.size() + 1].address = coordinator::egress_entry_point();
    return entries;
}

/** @return The platform replacement, ingress and egress bodies kept safe at detach. */
PlatformProtectedEntries platform_protected_entries() noexcept {
    const auto replacements = platform_replacements();
    PlatformProtectedEntries entries{};
    for (std::size_t index = 0; index < replacements.size(); ++index) {
        entries[index].address = replacements[index];
    }
    entries[replacements.size()].address = coordinator::ingress_entry_point();
    entries[replacements.size() + 1].address = coordinator::egress_entry_point();
    return entries;
}

} // namespace sunrise::client::hooks::network::lifecycle
