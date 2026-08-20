#include <array>
#include <string_view>

#include "game.h"
#include "game/retail_log/retail_log_signature_bytes.h"
#include "game/signon/signon_readiness_signature_bytes.h"
#include "signature_text.h"

namespace sunrise::client::patterns::game {
namespace {

// Matches the selector that distinguishes socket transport from SDR transport.
// Every displacement is wildcarded so the match carries no position-dependent bytes.
constexpr std::string_view kTransportKindText =
    "40 53 48 83 EC ? 80 3D ? ? ? ? 00 BB 01 00 00 00 74 ? 48 8B 0D ? ? ? ? 48 85 C9 74 ? 48 8B "
    "01 FF 50 60 84 C0 B9 02 00 00 00 0F 44 D9 8B C3 48 83 C4 ? 5B C3";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kTransportKind = signature<signature_length(kTransportKindText)>(kTransportKindText);

// Matches the executor that receives every queued HTTP request descriptor.
constexpr std::string_view kHttpExecuteRequestText =
    "48 8B C4 55 57 48 8D 68 ? 48 81 EC ? ? ? ? 48 89 70 ? 33 FF 48 8B F2 4C 89 70 ? 4C "
    "8B F1 40 38 7A 0D";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kHttpExecuteRequest =
    signature<signature_length(kHttpExecuteRequestText)>(kHttpExecuteRequestText);

// Matches the config fetch wrapper whose guarded call reaches the allocated-buffer GET path.
constexpr std::string_view kContentConfigFetchText =
    "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 41 0F B6 F8 48 8B DA";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kContentConfigFetch =
    signature<signature_length(kContentConfigFetchText)>(kContentConfigFetchText);

// Matches the content boot tick that owns the phase field at byte offset 48.
// The prologue alone is not unique once the frame values are wildcarded, so the match runs
// on through the stack-cookie store and the first two register saves.
constexpr std::string_view kContentConfigTickText =
    "4C 8B DC 55 56 57 49 8D AB ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 "
    "? ? ? ? 48 8B F9 49 89 5B";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kContentConfigTick =
    signature<signature_length(kContentConfigTickText)>(kContentConfigTickText);

// Matches the guarded read of the signature-enable byte: the store, its preceding call,
// the rip-relative compare against zero, and the branch that skips verification.
// Every displacement is wildcarded so the match carries no position-dependent bytes.
constexpr std::string_view kContentManifestGateText =
    "4C 89 B4 24 ? ? ? ? E8 ? ? ? ? 80 3D ? ? ? ? 00 74";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kContentManifestGate =
    signature<signature_length(kContentManifestGateText)>(kContentManifestGateText);

// Matches the roster-prefix decoder that applies all 65 bubble authority lanes.
// The match runs on past the stack-cookie store because the prologue alone is not unique.
constexpr std::string_view kBubbleAuthorityDecoderText =
    "40 55 53 56 41 55 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 "
    "48 89 85 ? ? ? ? 4C 8B E9 48 89 BC 24";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kBubbleAuthorityDecoder =
    signature<signature_length(kBubbleAuthorityDecoderText)>(kBubbleAuthorityDecoderText);

// Matches the process build-state getter used by each bubble authority lane.
// Every displacement is wildcarded so the match carries no position-dependent bytes.
constexpr std::string_view kContentUntrackedGetterText =
    "48 83 EC ? C7 44 24 ? 00 00 00 00 0F B6 05 ? ? ? ? 85 C0 74 ? 0F B6 05 ? ? ? ?";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kContentUntrackedGetter =
    signature<signature_length(kContentUntrackedGetterText)>(kContentUntrackedGetterText);

// Matches c_network_channel_view's signature refresh. The fixed tail clears the signature storage
// immediately after the prologue, which separates it from the other small view helpers.
constexpr std::string_view kViewSignatureRefreshText =
    "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8B 41 48 40 32 ED 40 32 F6 "
    "48 8B D9 48 8B 78 10 33 C0 C7 81 A0 00 00 00 00 00 00 00";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kViewSignatureRefresh =
    signature<signature_length(kViewSignatureRefreshText)>(kViewSignatureRefreshText);

// Matches the message-40 view lookup. The complete fixed prologue is unique in the current image;
// its saved second argument is the group-session token that the message body carries.
constexpr std::string_view kViewMessageLookupText =
    "48 89 54 24 10 53 57 48 83 EC 28 33 FF 48 8B D9";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kViewMessageLookup =
    signature<signature_length(kViewMessageLookupText)>(kViewMessageLookupText);

// Matches the entity-handler readiness scan used by message 40 while a compatible view waits at
// stage four. The enabled-byte test and manager load distinguish it from adjacent view helpers.
constexpr std::string_view kViewReadinessScanText =
    "41 57 48 83 EC 60 80 79 09 00 4C 8B F9 0F 84 ? ? ? ? 48 8B 51 10";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kViewReadinessScan =
    signature<signature_length(kViewReadinessScanText)>(kViewReadinessScanText);

// Matches the manager pump that walks all three fixed replication slots. The fixed prologue
// includes its leading REX prefix so the detour begins at the function entry, not byte +1.
constexpr std::string_view kViewSlotPumpText = "40 55 56 48 83 EC 38 83 79 08 01 48 8B E9";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kViewSlotPump = signature<signature_length(kViewSlotPumpText)>(kViewSlotPumpText);

// Matches the generic schema encoder wrapper used for scheduler signature schema 0x80806AEA.
// Its own tiny body is common, so the following function's fixed 0x160-byte prologue makes this
// entry unique without retaining the wrapper's position-dependent call displacement.
constexpr std::string_view kSchedulerSignatureEncoderText =
    "48 83 EC 28 E8 ? ? ? ? B0 01 48 83 C4 28 C3 48 89 5C 24 08 55 56 57 41 56 41 57 "
    "48 81 EC 60 01 00 00";
/** Compiled pattern bytes of the signature-encoder signature above. */
constexpr auto kSchedulerSignatureEncoder =
    signature<signature_length(kSchedulerSignatureEncoderText)>(kSchedulerSignatureEncoderText);

// Matches kind 0's sobject creation encoder. The fixed tail is the trailing create boolean's
// bit-writer fast path; together with the schema call and saved arguments it is unique.
constexpr std::string_view kSobjectCreateEncoderText =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B 05 ? ? ? ? 41 B9 01 00 00 00 49 8B F8 48 8B DA "
    "8B 08 E8 ? ? ? ? 8B 47 30 0F B6 4B 04 83 F8 40 73 27 FF 47 24 FF C0";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kSobjectCreateEncoder =
    signature<signature_length(kSobjectCreateEncoderText)>(kSobjectCreateEncoderText);

// Matches kind 0's sobject update encoder. The saved update context, its create-buffer load, and
// the two zeroed component cursors distinguish this entry from the adjacent codec functions.
constexpr std::string_view kSobjectUpdateEncoderText =
    "48 89 5C 24 08 48 89 6C 24 20 56 57 41 56 48 83 EC 40 48 8B 72 20 33 FF 48 8B DA "
    "4C 8B 72 30 8B EF 89 7C 24 68 89 7C 24 70 40 38 7A 54";
/** Compiled pattern bytes of the update-encoder signature above. */
constexpr auto kSobjectUpdateEncoder =
    signature<signature_length(kSobjectUpdateEncoderText)>(kSobjectUpdateEncoderText);

// Matches the sole native sobject RSAT dependency-registration entry. Its fixed tag-table
// normalization prefix is unique and receives the resolved RSAT directly in EDX.
constexpr std::string_view kSobjectNativeRegistrationText =
    "40 53 41 56 41 57 48 83 EC 40 8B C2 81 E2 FF 1F 00 00 C1 F8 0D 44 8B C8 "
    "49 81 C9 00 00 FC 0F 0F B7 C0 49 C1 E9 12 4C 23 C8";
/** Compiled pattern bytes of the native RSAT-registration signature above. */
constexpr auto kSobjectNativeRegistration =
    signature<signature_length(kSobjectNativeRegistrationText)>(kSobjectNativeRegistrationText);

// Matches simulation_object_glue_set_object_index. The direct-path tail is included because its
// two rip-relative globals are derived as the glue stride and table-base storage.
constexpr std::string_view kSobjectBinderText =
    "48 89 5C 24 08 57 48 83 EC 30 8B FA 8B D9 E8 ? ? ? ? F6 00 07 74 ? "
    "E8 ? ? ? ? F6 00 02 74 ? E8 ? ? ? ? 48 8D 0D ? ? ? ? 89 5C 24 50 "
    "48 89 4C 24 20 4C 8D 44 24 50 48 8B C8 89 7C 24 54 41 B9 08 00 00 00 "
    "48 8D 15 ? ? ? ? E8 ? ? ? ? 48 8B 5C 24 40 48 83 C4 30 5F C3 "
    "81 E3 FF 1F 00 00 0F AF 1D ? ? ? ? 8B C3 48 03 05 ? ? ? ?";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kSobjectBinder = signature<signature_length(kSobjectBinderText)>(kSobjectBinderText);

// Matches the core replicated-object body encoder. Its complete nonvolatile-save prologue and
// 0x1A8-byte stack frame are unique in the current image.
constexpr std::string_view kEntityCreateEncoderText =
    "40 53 55 56 57 41 56 41 57 48 81 EC A8 01 00 00";
/** Compiled pattern bytes of the entity body-encoder signature above. */
constexpr auto kEntityCreateEncoder =
    signature<signature_length(kEntityCreateEncoderText)>(kEntityCreateEncoderText);

// Matches the inbound direct-entity list decoder. The three position-dependent cookie bytes are
// wildcarded; the full save sequence, 0x4F0-byte frame, and first context load make it unique.
constexpr std::string_view kEntitySlotDecoderText =
    "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 10 FC FF FF "
    "48 81 EC F0 04 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 E0 03 00 00 "
    "48 8B 41 10 48 8B F1";
/** Compiled pattern bytes of the entity-list decoder signature above. */
constexpr auto kEntitySlotDecoder =
    signature<signature_length(kEntitySlotDecoderText)>(kEntitySlotDecoderText);

// Matches the scheduler event-list main decoder. The full nonvolatile-save prologue and first
// handler loads distinguish it from the other seven-argument scheduler lanes.
constexpr std::string_view kSchedulerEventDecoderText =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 "
    "48 83 EC 50 48 8B 41 20 4C 8B E9 48 8D 4C 24 24 49 8B E9";
/** Compiled pattern bytes of the event-list decoder signature above. */
constexpr auto kSchedulerEventDecoder =
    signature<signature_length(kSchedulerEventDecoderText)>(kSchedulerEventDecoderText);

// Matches the scheduler mask-list main decoder. The context offset and complete register setup
// make this fixed prologue unique in the current image.
constexpr std::string_view kSchedulerMaskDecoderText =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 "
    "48 83 EC 20 48 8B 81 30 15 00 00 4C 8B F9 48 8D 4C 24 50 49 8B F1 4D 8B E0";
/** Compiled pattern bytes of the mask-list decoder signature above. */
constexpr auto kSchedulerMaskDecoder =
    signature<signature_length(kSchedulerMaskDecoderText)>(kSchedulerMaskDecoderText);

// Matches the direct-entity predecoder invoked immediately before the entity-list main decoder.
// Its three-argument setup is fixed and unique in the current image.
constexpr std::string_view kSchedulerEntityPreludeDecoderText =
    "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 41 10 48 8B F2 49 8B F8 "
    "48 8B 48 08 8B 51 08 48 8D 4C 24 30";
/** Compiled pattern bytes of the direct-entity predecoder signature above. */
constexpr auto kSchedulerEntityPreludeDecoder =
    signature<signature_length(kSchedulerEntityPreludeDecoderText)>(
        kSchedulerEntityPreludeDecoderText);

// Matches the scheduler fixed-list main decoder. Its short save sequence, stack argument load,
// and fixed output initialization distinguish it from the other scheduler lanes.
constexpr std::string_view kSchedulerFixedDecoderText =
    "48 89 5C 24 10 48 89 74 24 18 41 56 48 83 EC 30 48 8B 5C 24 70 4C 8B F1 "
    "49 8B C9 49 8B F1 C7 03 00 00 00 00";
/** Compiled pattern bytes of the fixed-list decoder signature above. */
constexpr auto kSchedulerFixedDecoder =
    signature<signature_length(kSchedulerFixedDecoderText)>(kSchedulerFixedDecoderText);

// Matches the membership-to-view synchronization pass. The fixed prologue is unique in the
// current image and ends before the first position-dependent branch displacement.
constexpr std::string_view kViewMembershipSyncText =
    "41 57 48 83 EC 50 4C 8B F9 48 8B 09 48 85 C9 0F";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kViewMembershipSync =
    signature<signature_length(kViewMembershipSyncText)>(kViewMembershipSyncText);

// Matches the generic root-object decoder used for activity membership type 0x808086A8. The
// complete nonvolatile-save prologue and 0x160-byte frame distinguish the decoder entry.
constexpr std::string_view kActivityMembershipDecoderText =
    "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 60 01 00 00";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kActivityMembershipDecoder =
    signature<signature_length(kActivityMembershipDecoderText)>(kActivityMembershipDecoderText);

// Matches the simulation-queue insertion that receives a fully decoded activity membership.
// The allocation size and payload label load distinguish it from the adjacent queue insertions.
constexpr std::string_view kActivityMembershipQueueText =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B FA 4C 8D 0D ? ? ? ? "
    "48 8B D9 BA 70 92 05 00";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kActivityMembershipQueue =
    signature<signature_length(kActivityMembershipQueueText)>(kActivityMembershipQueueText);

// Matches message 30's native encoder. A remote fireteam peer is required before a solo client
// exercises this path, so the probe remains useful for a future multi-peer exemplar capture.
constexpr std::string_view kMembershipUpdateEncoderText =
    "48 8B C4 53 48 81 EC 80 00 00 00 48 89 68 08 45 33 C9 48 89 70 10 49 8B E8 "
    "4C 89 70 F0 BE 40 00 00 00";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kMembershipUpdateEncoder =
    signature<signature_length(kMembershipUpdateEncoderText)>(kMembershipUpdateEncoderText);

// Matches the prerequisite predicate that scans 32 account-SOID records at manager +0x210.
// Its complete save sequence and 0x470-byte frame are unique in the current runtime image.
constexpr std::string_view kAccountSoidValidatorText =
    "40 55 53 56 41 55 41 57 48 8D AC 24 90 FC FF FF 48 81 EC 70 04 00 00 "
    "48 8B 05 ? ? ? ? 48 33 C4 48 89 85 60 03 00 00";
/** Compiled pattern bytes of the account-SOID validator signature above. */
constexpr auto kAccountSoidValidator =
    signature<signature_length(kAccountSoidValidatorText)>(kAccountSoidValidatorText);

// Matches the virtual publisher that copies a complete 0x210-byte desired-account snapshot into
// the singleton consumed by the account reconciler. The fixed singleton lea, four-block copy
// count, and saved input register make the entry unique in the current runtime image.
constexpr std::string_view kAccountSoidPublisherText =
    "40 53 48 83 EC 20 48 8D 0D ? ? ? ? 48 8B DA E8 ? ? ? ? B9 04 00 00 00";
/** Compiled pattern bytes of the desired account-SOID publisher signature above. */
constexpr auto kAccountSoidPublisher =
    signature<signature_length(kAccountSoidPublisherText)>(kAccountSoidPublisherText);

// Matches the tiny accessor for the desired account-SOID singleton. The adjacent function prefix
// makes the two-instruction tail-call wrapper unique without retaining either displacement.
constexpr std::string_view kAccountSoidSourceText =
    "48 8D 0D ? ? ? ? E9 ? ? ? ? CC 48 89 5C 48 8B D1 48 8D 0D ? ? ? ? E9";
/** Compiled pattern bytes of the desired account-SOID source accessor signature above. */
constexpr auto kAccountSoidSource =
    signature<signature_length(kAccountSoidSourceText)>(kAccountSoidSourceText);

// Matches the connection-manager singleton accessor used by the account-SOID reconciler. The
// adjacent indexed-dispatch prefix keeps the two-instruction tail-call wrapper unique.
constexpr std::string_view kAccountConnectionSourceText =
    "48 8D 0D ? ? ? ? E9 ? ? ? ? CC ? ? ? 33 D2 83 F9 07 77 56 48 63 C1 4C 8D 05";
/** Compiled pattern bytes of the account connection-manager accessor signature above. */
constexpr auto kAccountConnectionSource =
    signature<signature_length(kAccountConnectionSourceText)>(kAccountConnectionSourceText);

// Matches the view creator driven by session-membership synchronization. It is the only path that
// allocates and binds a per-peer native view before message 40 can find it.
constexpr std::string_view kViewCreatorText =
    "40 55 53 56 41 54 41 55 41 56 41 57 48 8D AC 24 C0 FD FF FF 48 81 EC 40 03 00 00";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kViewCreator = signature<signature_length(kViewCreatorText)>(kViewCreatorText);

// Matches the active-channel address resolver used at the head of view creation.
constexpr std::string_view kViewAddressResolverText =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 49 8B E8 8B F2";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kViewAddressResolver =
    signature<signature_length(kViewAddressResolverText)>(kViewAddressResolverText);

// Matches the resolved-channel validity check. The fixed comparison distinguishes it from the
// adjacent accessor, which has the same prologue.
constexpr std::string_view kViewChannelValidatorText =
    "40 53 48 83 EC ? 49 63 D8 E8 ? ? ? ? 48 69 CB F0 41 00 00 83 BC 08 E8 30 00 00 04 0F 94 C0";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kViewChannelValidator =
    signature<signature_length(kViewChannelValidatorText)>(kViewChannelValidatorText);

// Matches the accessor whose returned channel owns the establishment state at byte 0x1d18.
constexpr std::string_view kViewChannelAccessorText =
    "40 53 48 83 EC ? 49 63 D8 E8 ? ? ? ? 48 69 CB F0 41 00 00 48 81 C1 A8 00 00 00 48 03 C1";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kViewChannelAccessor =
    signature<signature_length(kViewChannelAccessorText)>(kViewChannelAccessorText);

// Matches registry parameter 3's decoder. Its fixed prefix includes the raw selection-id read and
// the saved output pointer, which separates it from the adjacent activity-host encoder.
constexpr std::string_view kActivityHostDecoderText =
    "48 89 5C 24 08 57 48 83 EC 20 41 B8 40 00 00 00 48 8B DA 48 8B F9 "
    "E8 ? ? ? ? BA 40 00 00 00 48 8B CF E8 ? ? ? ? 48 8D 53 10";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kActivityHostDecoder =
    signature<signature_length(kActivityHostDecoderText)>(kActivityHostDecoderText);

// Matches the native activity-host connection-state publisher. The large fixed frame and complete
// nonvolatile-register save sequence make its entry unique in the current image.
constexpr std::string_view kActivityHostConnectionStateText =
    "40 55 53 56 57 41 54 41 56 41 57 48 8D AC 24 D0 FD FF FF 48 81 EC 30 03 00 00 "
    "48 8B 05 ? ? ? ? 48 33 C4 48 89 85 20 02 00 00 48 8B F2 48 8B D9 BA 04 00 00 00 "
    "41 8B F8";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kActivityHostConnectionState =
    signature<signature_length(kActivityHostConnectionStateText)>(kActivityHostConnectionStateText);

// Matches the native activity-start record lookup used by the managed-session pump. The complete
// nonvolatile save plus its obfuscated-global load, identity extension and null branch are unique.
constexpr std::string_view kActivityRouteRecordText =
    "48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 20 "
    "48 8B 1D ? ? ? ? 33 ED 4C 63 F1 4C 8B FA 48 85 DB 0F 84 ? ? ? ?";
/** Compiled pattern bytes of the activity-start record lookup signature above. */
constexpr auto kActivityRouteRecord =
    signature<signature_length(kActivityRouteRecordText)>(kActivityRouteRecordText);

// Matches the native local activity-manager initializer. The complete nonvolatile-register save
// and 0x460-byte frame are unique in the pinned image and end before position-dependent data.
constexpr std::string_view kActivityRouteLocalText =
    "48 89 5C 24 10 4C 89 4C 24 20 55 56 57 41 54 41 55 41 56 41 57 "
    "48 8D AC 24 A0 FC FF FF 48 81 EC 60 04 00 00";
/** Compiled pattern bytes of the local activity-route initializer signature above. */
constexpr auto kActivityRouteLocal =
    signature<signature_length(kActivityRouteLocalText)>(kActivityRouteLocalText);

// Matches the native authored activity-manager initializer. The distinct 0x3c0-byte frame and
// complete save sequence make the position-independent prefix unique in the pinned image.
constexpr std::string_view kActivityRouteAuthoredText =
    "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 "
    "48 8D AC 24 40 FD FF FF 48 81 EC C0 03 00 00";
/** Compiled pattern bytes of the authored activity-route initializer signature above. */
constexpr auto kActivityRouteAuthored =
    signature<signature_length(kActivityRouteAuthoredText)>(kActivityRouteAuthoredText);

// Matches the activity-mode definition selector. The fixed prefix preserves all three selector
// inputs before the first position-dependent call and is unique in the current runtime image.
constexpr std::string_view kActivityModeSelectorText =
    "48 89 5C 24 10 55 56 57 48 83 EC 20 41 0F B7 F8 48 63 EA 0F B7 F1 BB C5 9D 1C 81";
/** Compiled pattern bytes of the activity-mode selector signature above. */
constexpr auto kActivityModeSelector =
    signature<signature_length(kActivityModeSelectorText)>(kActivityModeSelectorText);

// Matches the activity-mode definition setter called by the selector above. Its fixed prefix
// saves the definition pointer and prepares the installed-resource lookup before the first call.
constexpr std::string_view kActivityModeSetterText = "40 53 57 48 83 EC 28 48 8B D9 48 8D 4C 24 40";
/** Compiled pattern bytes of the activity-mode setter signature above. */
constexpr auto kActivityModeSetter =
    signature<signature_length(kActivityModeSetterText)>(kActivityModeSetterText);

// Matches the activity-definition type resolver. The tail following its first wildcarded call is
// unique and covers the disabled sentinel checks plus the activity-definition lookup argument.
constexpr std::string_view kActivityTypeResolverText =
    "40 53 48 83 EC 30 0F B7 D9 E8 ? ? ? ? 84 C0 74 41 66 83 FB FF 74 3B 0F B7 D3 48 8D 4C 24 48";
/** Compiled pattern bytes of the activity-type resolver signature above. */
constexpr auto kActivityTypeResolver =
    signature<signature_length(kActivityTypeResolverText)>(kActivityTypeResolverText);

// Matches the asynchronous type-2 replicated-object apply job. The RIP-relative security-cookie
// load is wildcarded; the fixed frame size and saved job pointer make the entry unique.
constexpr std::string_view kSobjectApplyJobText =
    "40 53 48 81 EC 50 01 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 24 40 01 00 00 48 8B D9 "
    "33 D2 48 8D 4C 24 30 41 B8 10 01 00 00";
/** Compiled pattern bytes of the sobject apply-job signature above. */
constexpr auto kSobjectApplyJob =
    signature<signature_length(kSobjectApplyJobText)>(kSobjectApplyJobText);

// Matches the kind-0 replicated-object constructor reached through the entity codec's +0xB0
// virtual. Its large fixed frame and saved argument registers form a unique current-image entry.
constexpr std::string_view kSobjectKind0ConstructorText =
    "48 89 5C 24 08 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D AC 24 00 F5 FF FF 48 81 EC "
    "00 0C 00 00";
/** Compiled pattern bytes of the kind-0 constructor signature above. */
constexpr auto kSobjectKind0Constructor =
    signature<signature_length(kSobjectKind0ConstructorText)>(kSobjectKind0ConstructorText);

// Matches the immediate decoded-record promotion pass. The fixed prefix reads the full entity,
// derives its 13-bit slot, and tests the validated create bit before any position-dependent call.
constexpr std::string_view kSobjectRecordPromotionText =
    "40 53 56 57 48 83 EC 20 8B 42 08 48 8B F2 25 FF 1F 00 00 "
    "48 8B F9 F6 42 42 01 48 8D 1C 40";
/** Compiled pattern bytes of the decoded-record promotion signature above. */
constexpr auto kSobjectRecordPromotion =
    signature<signature_length(kSobjectRecordPromotionText)>(kSobjectRecordPromotionText);

// Matches the per-tick replicated-object dirty service. The fixed save set and 0x6090-byte frame
// uniquely identify the routine before its position-dependent stack-probe call.
constexpr std::string_view kSobjectDirtyServiceText =
    "48 89 5C 24 18 48 89 6C 24 20 56 57 41 54 41 55 41 57 B8 90 60 00 00";
/** Compiled pattern bytes of the replicated-object dirty-service signature above. */
constexpr auto kSobjectDirtyService =
    signature<signature_length(kSobjectDirtyServiceText)>(kSobjectDirtyServiceText);

// Matches the exact replicated-object backend-busy predicate. The current-image 11-byte leaf is
// unique and reads only the signed work counter at context +0x560E4.
constexpr std::string_view kSobjectBackendBusyText = "83 B9 E4 60 05 00 00 0F 9F C0 C3";
/** Compiled pattern bytes of the backend-busy predicate signature above. */
constexpr auto kSobjectBackendBusy =
    signature<signature_length(kSobjectBackendBusyText)>(kSobjectBackendBusyText);

// Matches the per-row dirty processor called only by the non-busy dirty-service branch. Its fixed
// save set and 0x170-byte frame are unique before any position-dependent object-table load.
constexpr std::string_view kSobjectDirtyRowText =
    "4C 8B DC 55 41 56 41 57 48 8D 6C 24 90 48 81 EC 70 01 00 00";
/** Compiled pattern bytes of the per-row dirty-processor signature above. */
constexpr auto kSobjectDirtyRow =
    signature<signature_length(kSobjectDirtyRowText)>(kSobjectDirtyRowText);

// Matches the type-2 replicated-object job builder. The distinct 0x4050-byte frame makes this
// position-independent entry prefix unique in the current image.
constexpr std::string_view kSobjectType2JobText = "48 89 5C 24 20 57 B8 50 40 00 00";
/** Compiled pattern bytes of the type-2 job-builder signature above. */
constexpr auto kSobjectType2Job =
    signature<signature_length(kSobjectType2JobText)>(kSobjectType2JobText);

// Matches the public-session refresh that selects which of the three simulation managers is
// serviced. The complete nonvolatile save set and fixed 0x38-byte frame are unique before the
// first position-dependent call.
constexpr std::string_view kActiveManagerRefreshText =
    "48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 38 4C 8B F1";
/** Compiled pattern bytes of the active simulation-manager refresh signature above. */
constexpr auto kActiveManagerRefresh =
    signature<signature_length(kActiveManagerRefreshText)>(kActiveManagerRefreshText);

// Matches the public-session initialized/count predicate used immediately before the world
// controller checks whether its selected citizen peer has reached state 10. The whole leaf is
// position independent and unique in the current image.
constexpr std::string_view kCitizenSessionReadyText = "83 B9 6C 08 00 00 00 0F 95 C0 C3";
/** Compiled pattern bytes of the citizen-session readiness predicate above. */
constexpr auto kCitizenSessionReady =
    signature<signature_length(kCitizenSessionReadyText)>(kCitizenSessionReadyText);

// Matches the sole asynchronous citizen-join status query used by the world controller. The
// nonvolatile save set followed by the encrypted-table load is unique in the current image.
constexpr std::string_view kCitizenJoinStatusText =
    "48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B 3D ? ? ? ?";
/** Compiled pattern bytes of the citizen-join status query signature above. */
constexpr auto kCitizenJoinStatus =
    signature<signature_length(kCitizenJoinStatusText)>(kCitizenJoinStatusText);

// Matches the native normal-z-leg state publisher called by the positional transition tick.
// This position-independent prologue is unique in the current image; the function stores the
// requested classifier band at controller+0x352 after performing its native side effects.
constexpr std::string_view kZLegStateText =
    "48 89 5C 24 18 48 89 74 24 20 55 57 41 54 41 55 41 56 48 8D AC 24 90 FD FF FF 48 81 EC "
    "70 03 00 00";
/** Compiled pattern bytes of the normal-z-leg state publisher signature above. */
constexpr auto kZLegState = signature<signature_length(kZLegStateText)>(kZLegStateText);

// Matches the shared scheduler finalizer used by event, mask and fixed lanes. It appends their
// literal zero terminal bit to the writer received in R8.
constexpr std::string_view kSchedulerZeroFinalizerText =
    "41 8B 40 30 4D 8B C8 83 F8 40 73 0F 41 FF 40 24 FF C0 49 D1 60 28 "
    "41 89 40 30 C3";
/** Compiled pattern bytes of the zero-bit scheduler finalizer signature above. */
constexpr auto kSchedulerZeroFinalizer =
    signature<signature_length(kSchedulerZeroFinalizerText)>(kSchedulerZeroFinalizerText);

// Matches the entity scheduler finalizer. Its direct and slow paths append the entity lane's
// literal one terminal bit to the writer received in R8.
constexpr std::string_view kSchedulerEntityFinalizerText =
    "41 8B 40 30 4D 8B C8 83 F8 40 73 1A 41 FF 40 24 FF C0 41 89 40 30 "
    "49 8B 40 28 48 03 C0 48 83 C8 01 49 89 40 28 C3";
/** Compiled pattern bytes of the entity scheduler finalizer signature above. */
constexpr auto kSchedulerEntityFinalizer =
    signature<signature_length(kSchedulerEntityFinalizerText)>(kSchedulerEntityFinalizerText);

// Matches the replicated-entity scheduler candidate collector. It walks manager +0xC920 and
// appends packed entity/view/lane/priority candidates without writing their encoded bodies.
constexpr std::string_view kSchedulerEntityCollectorText =
    "44 89 44 24 18 89 54 24 10 56 41 57 48 81 EC C8 00 00 00 33 F6 "
    "4C 89 49 18 4C 8B F9 89 B4 24 F8 00 00 00";
/** Compiled pattern bytes of the entity candidate collector signature above. */
constexpr auto kSchedulerEntityCollector =
    signature<signature_length(kSchedulerEntityCollectorText)>(kSchedulerEntityCollectorText);

// Matches the 6-slot object resolver whose first instruction names the schema tables.
constexpr std::string_view kQueuezObjectResolverText =
    "4C 8B 1D ? ? ? ? 45 33 C9 48 63 C2 45 8B D0 48 05 1A 25 00 00 48 8D 14 40 48 C1 E2 05";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kQueuezObjectResolver =
    signature<signature_length(kQueuezObjectResolverText)>(kQueuezObjectResolverText);

// Matches the family-5 subscription path whose second call returns the object store.
constexpr std::string_view kQueuezFamily5SubscribeText =
    "40 57 48 83 EC ? 48 8B F9 E8 ? ? ? ? 0F B6 47 50 84 C0 75 ? E8 ? ? ? ?";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kQueuezFamily5Subscribe =
    signature<signature_length(kQueuezFamily5SubscribeText)>(kQueuezFamily5SubscribeText);

// Matches the config-init site that loads the bootstrap content-id token. The token address
// comes from the wildcarded rip-relative operand, so no token bytes live in Sunrise.
constexpr std::string_view kContentIdTokenLoadText =
    "0F 10 44 24 ? 41 B8 14 00 00 00 48 8D 15 ? ? ? ? 0F 10 4C 24 ? 48 8D 8C 24 ? ? ? ?";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kContentIdTokenLoad =
    signature<signature_length(kContentIdTokenLoadText)>(kContentIdTokenLoadText);

// Matches the item-stat lookup entry used by equipment scoring. The prologue alone is not
// unique once the frame size is wildcarded, so the match runs on into the argument moves.
constexpr std::string_view kGetItemStatValueText =
    "40 53 56 41 54 41 56 41 57 48 83 EC ? 45 33 E4 41 0F B6 D9 4C 8B 8C 24 ? ? ? ? 4D 8B F0 "
    "44 89 21";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kGetItemStatValue =
    signature<signature_length(kGetItemStatValueText)>(kGetItemStatValueText);

// Matches the light-level conversion entry used by equipment scoring.
constexpr std::string_view kLightValueToScalarText =
    "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 0F 29 74 24 ? 41 0F B6 D8";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kLightValueToScalar =
    signature<signature_length(kLightValueToScalarText)>(kLightValueToScalarText);

/** Game signatures in the exact target-slot order. */
constexpr std::array kDefinitions{
    patterns::Pattern{"transport_kind", kTransportKind},
    patterns::Pattern{"http_execute_request", kHttpExecuteRequest},
    patterns::Pattern{"signon_readiness_failure", signon::kReadinessFailure},
    patterns::Pattern{"signon_readiness_ready", signon::kReadinessReady},
    patterns::Pattern{"content_config_fetch", kContentConfigFetch},
    patterns::Pattern{"content_config_tick", kContentConfigTick},
    patterns::Pattern{"content_manifest_gate", kContentManifestGate},
    patterns::Pattern{"bubble_authority_decoder", kBubbleAuthorityDecoder},
    patterns::Pattern{"content_untracked_getter", kContentUntrackedGetter},
    patterns::Pattern{"view_signature_refresh", kViewSignatureRefresh},
    patterns::Pattern{"view_message_lookup", kViewMessageLookup},
    patterns::Pattern{"view_readiness_scan", kViewReadinessScan},
    patterns::Pattern{"view_slot_pump", kViewSlotPump},
    patterns::Pattern{"scheduler_signature_encoder", kSchedulerSignatureEncoder},
    patterns::Pattern{"sobject_create_encoder", kSobjectCreateEncoder},
    patterns::Pattern{"sobject_update_encoder", kSobjectUpdateEncoder},
    patterns::Pattern{"sobject_native_registration", kSobjectNativeRegistration},
    patterns::Pattern{"sobject_binder", kSobjectBinder},
    patterns::Pattern{"entity_create_encoder", kEntityCreateEncoder},
    patterns::Pattern{"entity_slot_decoder", kEntitySlotDecoder},
    patterns::Pattern{"scheduler_event_decoder", kSchedulerEventDecoder},
    patterns::Pattern{"scheduler_mask_decoder", kSchedulerMaskDecoder},
    patterns::Pattern{"scheduler_entity_prelude_decoder", kSchedulerEntityPreludeDecoder},
    patterns::Pattern{"scheduler_fixed_decoder", kSchedulerFixedDecoder},
    patterns::Pattern{"view_membership_sync", kViewMembershipSync},
    patterns::Pattern{"activity_membership_decoder", kActivityMembershipDecoder},
    patterns::Pattern{"activity_membership_queue", kActivityMembershipQueue},
    patterns::Pattern{"membership_update_encoder", kMembershipUpdateEncoder},
    patterns::Pattern{"account_soid_validator", kAccountSoidValidator},
    patterns::Pattern{"account_soid_publisher", kAccountSoidPublisher},
    patterns::Pattern{"account_soid_source", kAccountSoidSource},
    patterns::Pattern{"account_connection_source", kAccountConnectionSource},
    patterns::Pattern{"view_creator", kViewCreator},
    patterns::Pattern{"view_address_resolver", kViewAddressResolver},
    patterns::Pattern{"view_channel_validator", kViewChannelValidator},
    patterns::Pattern{"view_channel_accessor", kViewChannelAccessor},
    patterns::Pattern{"activity_host_decoder", kActivityHostDecoder},
    patterns::Pattern{"activity_host_connection_state", kActivityHostConnectionState},
    patterns::Pattern{"activity_route_record", kActivityRouteRecord},
    patterns::Pattern{"activity_route_local", kActivityRouteLocal},
    patterns::Pattern{"activity_route_authored", kActivityRouteAuthored},
    patterns::Pattern{"activity_mode_selector", kActivityModeSelector},
    patterns::Pattern{"activity_mode_setter", kActivityModeSetter},
    patterns::Pattern{"activity_type_resolver", kActivityTypeResolver},
    patterns::Pattern{"sobject_apply_job", kSobjectApplyJob},
    patterns::Pattern{"sobject_kind0_constructor", kSobjectKind0Constructor},
    patterns::Pattern{"sobject_record_promotion", kSobjectRecordPromotion},
    patterns::Pattern{"sobject_dirty_service", kSobjectDirtyService},
    patterns::Pattern{"sobject_backend_busy", kSobjectBackendBusy},
    patterns::Pattern{"sobject_dirty_row", kSobjectDirtyRow},
    patterns::Pattern{"sobject_type2_job", kSobjectType2Job},
    patterns::Pattern{"active_manager_refresh", kActiveManagerRefresh},
    patterns::Pattern{"citizen_session_ready", kCitizenSessionReady},
    patterns::Pattern{"citizen_join_status", kCitizenJoinStatus},
    patterns::Pattern{"z_leg_state", kZLegState},
    patterns::Pattern{"scheduler_zero_finalizer", kSchedulerZeroFinalizer},
    patterns::Pattern{"scheduler_entity_finalizer", kSchedulerEntityFinalizer},
    patterns::Pattern{"scheduler_entity_collector", kSchedulerEntityCollector},
    patterns::Pattern{"content_id_token_load", kContentIdTokenLoad},
    patterns::Pattern{"queuez_object_resolver", kQueuezObjectResolver},
    patterns::Pattern{"queuez_family5_subscribe", kQueuezFamily5Subscribe},
    patterns::Pattern{"get_item_stat_value", kGetItemStatValue},
    patterns::Pattern{"light_value_to_scalar", kLightValueToScalar},
    patterns::Pattern{"retail_log_enqueue", retail_log::kEnqueue},
    patterns::Pattern{"retail_log_set_category_verbosity", retail_log::kSetCategoryVerbosity},
};

static_assert(kDefinitions.size() == static_cast<std::size_t>(Id::count));

} // namespace

/** @return Immutable Game signature definitions in target-slot order. */
std::span<const patterns::Pattern> definitions() noexcept {
    return kDefinitions;
}

} // namespace sunrise::client::patterns::game
