#include "peer_transport.h"

#include <Windows.h>

#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string_view>

#include "../../../client/hooks/network/entity_slot_probe.h"
#include "../../../client/hooks/network/scheduler_handler_probe.h"
#include "../../../client/hooks/network/scheduler_signature_probe.h"
#include "../../../client/hooks/network/sobject_bind_probe.h"
#include "../../../client/hooks/network/sobject_rsat_probe.h"
#include "../../../client/hooks/network/sobject_update_probe.h"
#include "../../../middleware/content/packages/tables/region_reader.h"
#include "../../../middleware/crypto/random_bytes.h"
#include "../../../middleware/encoding/bit_reader.h"
#include "../../../middleware/encoding/bit_writer.h"
#include "../../../middleware/gameplay/descriptor/join_descriptor.h"
#include "../../../middleware/gameplay/peer/connect_messages.h"
#include "../../../middleware/gameplay/peer/established_packet.h"
#include "../../../middleware/gameplay/peer/join_messages.h"
#include "../../../middleware/gameplay/peer/peer_container.h"
#include "../../../middleware/gameplay/peer/reliable_assembly.h"
#include "../../../state/activity/membership/activity_membership_query.h"
#include "../../../state/build_data/runtime.h"
#include "../association/association_host.h"
#include "../dtls/dtls_host.h"
#include "../endpoint/gameplay_endpoint.h"
#include "../gameplay_log.h"
#include "../group/group_host.h"
#include "../group/group_host_sessions.h"

namespace sunrise::server::gameplay::peer {

namespace {

namespace wire = middleware::gameplay::peer;
namespace bits = middleware::encoding::bits;
namespace tables = middleware::content::packages::tables;

/**
 * Sends one payload over whichever transport the peer arrived on.
 * Records go first. The engine association answers only when no record association exists.
 * @param to Peer endpoint.
 * @param payload Payload bytes.
 * @return True when one of the two carried it.
 */
[[nodiscard]] bool send_transport(const state::gameplay::Endpoint& to,
                                  std::span<const std::byte> payload) noexcept {
    return dtls::send_payload(to, payload) || association::send_payload(to, payload);
}

/** One out-of-band reply fits well inside a single unfragmented payload. */
constexpr std::size_t kReplyCapacity = 1024;
/** Bit position of the payload marker inside its first byte. */
constexpr unsigned kMarkerShift = 7;
/** Bits in one byte. */
constexpr unsigned kByteBits = 8;
/** Mask of one byte. */
constexpr std::uint32_t kByteMask = 0xFF;
/** The connection guard is the sequence modulo four. */
constexpr std::uint32_t kSequenceGuardBase = 4;
/** Delay sentinel used until a round trip has been measured. */
constexpr std::uint16_t kDelaySentinel = 1023;
/** Packet sequences are published as ten bits. */
constexpr std::uint16_t kPacketSequenceModulus = state::gameplay::kPacketSequenceModulus;
/** Sequence the first packet to a peer carries, because the head advances before it is written. */
constexpr std::uint16_t kFirstPacketSequence = 1;
/** Smallest head-minus-cursor the peer accepts. This host keeps at most one packet in flight. */
constexpr std::uint8_t kMinimumHeadCursor = 1;
/** Scheduler prefix retained by the external-body diagnostic probe. */
constexpr std::size_t kExternalProbeWords = 4;
/** Bits retained in each diagnostic scheduler-prefix word. */
constexpr std::uint8_t kExternalProbeWordBits = 64;
/** Complete scheduler-body bytes retained for one bounded diagnostic line. */
constexpr std::size_t kExternalProbeByteCapacity = 256;
/**
 * Milliseconds between two resends of the same queue. The peer discards a packet more than 128
 * sequences ahead of its window, so this host must not send faster than the peer does.
 */
constexpr std::uint64_t kResendInterval = 250;
/** The first EDZ entity view owns thirteen native objects before a server-authored slot is safe. */
constexpr std::uint32_t kFirstEntityBaselineOccupied = 13;
/** A pristine slot's first native allocation advances object generation zero to two. */
constexpr std::uint8_t kFirstObjectGeneration = 2;
/** Package-backed tag discriminator used by schema 0x80800014. */
constexpr std::uint8_t kInstalledTagDiscriminator = 0x16;
/** A failed decode queues the RSAT; retry the exact same slot after loader service has run. */
constexpr std::uint64_t kEntityCreateRetryInterval = 2000;
/** The native baseline is ready before occupancy publishes; follow up on the first accepted tick.
 */
constexpr std::uint64_t kEntityFollowupReadyInterval = 0;
/** Exact shared-Vandal transform width produced by the native encoder. */
constexpr std::uint16_t kFirstEntityUpdateBits = 130;
/** Reject a scheduler layout that only agrees for one transition sample. */
constexpr std::uint64_t kEntityCreateReadyInterval = 500;
/** Keeps resource-readiness retries bounded even when the selected RSAT cannot load. */
constexpr std::uint8_t kEntityCreateAttemptLimit = 4;
/** Runtime currently proves complete inbound acceptance only for one registered scheduler view. */
constexpr std::uint8_t kProvenSchedulerViewCount = 1;
/** One-view scheduler wire is its update bit plus schema 0x80806AEA's 202-bit body. */
constexpr std::uint16_t kProvenSchedulerWireBits = 203;
/** The captured transition layout has exactly two registered scheduler views. */
constexpr std::uint8_t kTwoViewProbeViewCount = 2;
/** Captured two-view signature, including its one-bit update gate. */
constexpr std::uint16_t kTwoViewProbeWireBits = 275;
/** The transition capture orders the outgoing EDZ view before the current Basin authority view. */
constexpr std::uint8_t kTwoViewProbeEntityView = 0;
constexpr std::uint8_t kTwoViewProbeAuthorityView = 1;
/** Namespace owned by the live outgoing EDZ view in the captured transition. */
constexpr std::int32_t kTwoViewProbeEntityNamespace = 1;
/** Current Basin authority required before the bounded outgoing-view control may run. */
constexpr std::int32_t kTwoViewProbeAuthorityRegion = 24;
constexpr std::uint8_t kTwoViewProbeAuthorityBubble = 3;
constexpr std::uint8_t kTwoViewProbeAuthorityCell = 11;
/** Outgoing namespace 1's active Town cell, used only to confirm native construction. */
constexpr std::int32_t kTwoViewProbeRegion = 408;
constexpr std::uint8_t kTwoViewProbeBubble = 51;
constexpr std::uint8_t kTwoViewProbeCell = 145;
/**
 * Stored 275-bit signature, target view's 220-bit body, and current view's six-bit empty tail.
 * The native schema consumes 274 signature bits; stored bit 274 supplies view 0's event lane.
 */
constexpr std::uint16_t kTwoViewProbeBodyBits = 501;
/** Fail before the native four-second corrupt-packet timeout can close the channel. */
constexpr std::uint64_t kTwoViewProbeTimeout = 3000;
/** Stay below half of the 128-entry packet ring while waiting for direct acknowledgement. */
constexpr std::uint8_t kTwoViewProbeMaximumPacketsAfter = 63;
SRWLOCK g_lock{SRWLOCK_INIT};
std::array<state::gameplay::PeerLink, state::gameplay::kAssociationCapacity> g_peers;
/** Channel ids this host hands out. The peer refuses one that does not increase. */
std::uint32_t g_channelId{0};

/** One in-flight native fragmented established packet. */
struct FragmentAssembly {
    std::array<std::byte, wire::kFragmentedPacketCapacity> bytes{};
    std::array<std::size_t, wire::kMaximumPacketFragments> sizes{};
    std::array<bool, wire::kMaximumPacketFragments> received{};
    state::gameplay::Endpoint endpoint{};
    std::uint64_t touched{};
    std::uint8_t setId{};
    std::uint8_t guard{};
    std::uint8_t count{};
    bool active{};
};

/** Native keeps eight fragment sets per channel so adjacent large packets may overlap. */
constexpr std::size_t kFragmentAssemblyCapacity =
    state::gameplay::kAssociationCapacity * wire::kMaximumPacketFragments;
std::array<FragmentAssembly, kFragmentAssemblyCapacity> g_fragmentAssemblies{};

/** Read-only summary of the native simulation gate and replication-scheduler prefix. */
struct ExternalProbe {
    std::array<std::uint64_t, kExternalProbeWords> words{};
    std::array<std::uint8_t, kExternalProbeWords> widths{};
    std::array<std::byte, kExternalProbeByteCapacity> schedulerBytes{};
    std::size_t schedulerBitsBeforeProbe{};
    std::size_t schedulerBitsAfterProbe{};
    std::size_t schedulerCapturedBits{};
    std::size_t schedulerByteCount{};
    state::gameplay::SchedulerSignature schedulerSignature{};
    std::size_t schedulerSignatureBits{};
    wire::ExternalStatus status{};
    std::uint8_t wordCount{};
    std::uint8_t schedulerTailBits{};
    bool schedulerSignatureUpdate{};
    bool schedulerSignatureValid{};
};

/** Fully guarded inputs for one server-authored create in one registered scheduler view. */
struct EntityCreatePlan {
    std::uint64_t token{};
    std::uint64_t schedulerKey{};
    std::uint32_t rsat{};
    std::uint16_t slot{};
    std::uint8_t schedulerTag{};
    std::uint8_t handleGeneration{};
    std::uint8_t objectGeneration{};
    std::uint8_t viewIndex{};
    std::int32_t namespaceId{-1};
    std::int32_t spatialRegion{state::activity::membership::kAbsentRegionIndex};
    std::array<std::byte, client::hooks::network::sobject_update_probe::kNearbyUpdateCapacity>
        updateWire{};
    std::uint16_t updateBits{};
    std::uint8_t spatialCell{};
    std::uint8_t spatialBubble{};
    bool spatialCellPresent{};
    bool bootstrapScheduler{};
    bool combinedCreate{};
    bool updateOnly{};
    bool present{};
};

/** One fail-contained scheduler validation carrying an atomic create in a distinct live view. */
struct TwoViewProbePlan {
    state::gameplay::SchedulerSignature scheduler{};
    /** Current Basin authority token retained for topology and acknowledgement proof. */
    std::uint64_t token{};
    /** Live scheduler-view token whose entity manager receives the bounded record. */
    std::uint64_t entityToken{};
    /** Current Basin authority view; this remains the globally selected spatial context. */
    std::uint8_t selectedView{};
    std::uint8_t entityView{};
    std::uint8_t remoteViews{};
    bool remoteAlreadyMatches{};
    bool present{};
};

/** Deferred probe diagnostic so logging never occurs while the gameplay lock is held. */
struct TwoViewProbeReport {
    enum class Kind : std::uint8_t {
        remoteMutation,
        unacknowledged,
    };

    std::uint64_t token{};
    std::uint64_t entityToken{};
    std::uint64_t elapsed{};
    std::uint16_t packet{};
    std::uint8_t packetsAfter{};
    std::uint8_t remoteViews{};
    Kind kind{Kind::remoteMutation};
    const char* reason{};
};

/** Stable reason the first server-authored create is not ready to leave. */
enum class EntityCreateGate : std::uint8_t {
    ready,
    controlQueue,
    view,
    schedulerShape,
    schedulerTrace,
    captureMissing,
    candidate,
    baseline,
    slot,
    generation,
    rsat,
    spatialCell,
    signature,
    schedulerIdentity,
    remoteLayout,
    schedulerEntry,
    settling,
    attempted,
};

/** One transition-only snapshot of the guarded entity-create inputs. */
struct EntityCreateGateReport {
    std::uint64_t token{};
    std::uint64_t entityToken{};
    std::uint64_t readyAge{};
    std::size_t queueCount{};
    std::uint32_t occupied{};
    std::uint32_t occupiedLow{};
    std::uint32_t available{};
    std::uint16_t wireBits{};
    std::uint16_t slot{};
    std::int32_t namespaceId{-1};
    std::int32_t spatialRegion{state::activity::membership::kAbsentRegionIndex};
    EntityCreateGate gate{EntityCreateGate::view};
    std::uint8_t signatureViews{};
    std::uint8_t authorityView{};
    std::uint8_t entityView{};
    std::uint8_t localViews{};
    std::uint8_t remoteViews{};
    std::uint8_t handleGeneration{};
    std::uint8_t reservedGeneration{};
    std::uint8_t objectGeneration{};
    std::uint8_t spatialCell{};
    std::uint8_t spatialBubble{};
    std::uint8_t attempts{};
    bool spatialCellPresent{};
    bool awaitingAcknowledgement{};
    bool capturePresent{};
    bool candidatePresent{};
    bool localSignatureValid{};
    bool remoteSignatureValid{};
};

/** Best per-session view for replication on one multiplexed gameplay link. */
struct SelectedReplicationView {
    state::gameplay::ViewSignature signature{};
    client::hooks::network::entity_slot_probe::ViewCapture capture{};
    state::activity::membership::WorldSnapshot world{};
    bool capturePresent{};
    bool worldPresent{};
    bool present{};
};

/**
 * Chooses the bound view owned by the player's current advertised region.
 * Before that region has a group row, preserves the populated-view fallback used during setup.
 */
[[nodiscard]] SelectedReplicationView
select_replication_view(const state::gameplay::PeerLink& peer) noexcept {
    SelectedReplicationView output{};
    output.worldPresent = state::activity::membership::primary_world(output.world);
    if (output.worldPresent) {
        const std::uint64_t currentGroup = group::advertised_group_session(output.world.region);
        if (currentGroup != 0) {
            // State's current region is authoritative during overlap. Once its group is known,
            // sending through an older populated view targets the world the player just left.
            for (std::size_t index = 0; index < peer.sessions.size(); ++index) {
                if (peer.sessions[index] != currentGroup) {
                    continue;
                }
                const state::gameplay::ViewSignature& view = peer.views[index];
                if (view.token == 0 || !view.bound
                    || group::holding_group_session(view.token) != currentGroup) {
                    return output;
                }
                output.signature = view;
                output.capturePresent =
                    client::hooks::network::entity_slot_probe::find(view.token, output.capture);
                output.present = true;
                return output;
            }
            // A claimed current-world group without a view is not permission to use a stale one.
            return output;
        }
    }
    // Before State has mapped the current region, preserve the prior populated-view fallback.
    for (const state::gameplay::ViewSignature& view : peer.views) {
        if (view.token == 0) {
            continue;
        }
        client::hooks::network::entity_slot_probe::ViewCapture capture{};
        const bool captured = client::hooks::network::entity_slot_probe::find(view.token, capture);
        const bool better = !output.present
                            || (captured
                                && (!output.capturePresent
                                    || capture.occupiedCount > output.capture.occupiedCount));
        if (!better) {
            continue;
        }
        output.signature = view;
        output.capture = capture;
        output.capturePresent = captured;
        output.present = true;
    }
    return output;
}

[[nodiscard]] const char* entity_create_gate_name(EntityCreateGate gate) noexcept {
    switch (gate) {
    case EntityCreateGate::ready:
        return "ready";
    case EntityCreateGate::controlQueue:
        return "control-queue";
    case EntityCreateGate::view:
        return "view";
    case EntityCreateGate::schedulerShape:
        return "scheduler-shape";
    case EntityCreateGate::schedulerTrace:
        return "scheduler-trace";
    case EntityCreateGate::captureMissing:
        return "capture-missing";
    case EntityCreateGate::candidate:
        return "candidate";
    case EntityCreateGate::baseline:
        return "baseline";
    case EntityCreateGate::slot:
        return "slot";
    case EntityCreateGate::generation:
        return "generation";
    case EntityCreateGate::rsat:
        return "rsat";
    case EntityCreateGate::spatialCell:
        return "spatial-cell";
    case EntityCreateGate::signature:
        return "signature";
    case EntityCreateGate::schedulerIdentity:
        return "scheduler-identity";
    case EntityCreateGate::remoteLayout:
        return "remote-layout";
    case EntityCreateGate::schedulerEntry:
        return "scheduler-entry";
    case EntityCreateGate::settling:
        return "settling";
    case EntityCreateGate::attempted:
        return "attempted";
    }
    return "unknown";
}

/**
 * Resolves the selected activity-host view to the destination's map-global bubble index.
 * The destination comes from the signed-in player's world, while the region comes from the
 * selected token's host-session row so an overlap cannot pair a new region with an old view.
 */
[[nodiscard]] bool resolve_entity_spatial_cell(const SelectedReplicationView& selected,
                                               EntityCreatePlan& output) noexcept {
    output.spatialRegion = state::activity::membership::kAbsentRegionIndex;
    output.spatialBubble = 0;
    output.spatialCell = 0;
    output.spatialCellPresent = false;
    if (!selected.present || selected.signature.token == 0 || !selected.worldPresent
        || !group::holding_region_index(selected.signature.token, output.spatialRegion)
        || output.spatialRegion < 0
        || static_cast<std::uint32_t>(output.spatialRegion) >= tables::kRegionIndexBound) {
        return false;
    }

    const state::activity::destination::DestinationSelection& destination =
        selected.world.destination;
    const std::size_t nameLength = destination.packageNameLength;
    if (nameLength == 0 || nameLength > destination.packageName.size()) {
        return false;
    }
    const std::string_view name(reinterpret_cast<const char*>(destination.packageName.data()),
                                nameLength);
    state::build_data::scenarios::Definition layout{};
    if (!state::build_data::find_scenario_layout(name, layout)) {
        return false;
    }

    const auto bubble =
        static_cast<std::uint32_t>(output.spatialRegion) / tables::kSliceSetIndexFactor;
    if (bubble >= layout.bubbleCount || bubble >= layout.bubbleMapIndices.size()) {
        return false;
    }
    const std::uint16_t cell = layout.bubbleMapIndices[bubble];
    if (cell > (std::numeric_limits<std::uint8_t>::max)()) {
        return false;
    }
    output.spatialBubble = static_cast<std::uint8_t>(bubble);
    output.spatialCell = static_cast<std::uint8_t>(cell);
    output.spatialCellPresent = true;
    return true;
}

/**
 * Measures schema 0x80806AEA with the bit count observed around the native encoder call.
 * The schema is variable-length and does not contain the scheduler's logical view list. No live
 * reader or replication state moves.
 */
void probe_scheduler_signature(bits::Reader reader, ExternalProbe& output) noexcept {
    const std::size_t before = reader.remaining_bits();
    std::uint64_t update = 0;
    if (!reader.read(1, update)) {
        return;
    }
    output.schedulerSignatureUpdate = update != 0;
    if (!output.schedulerSignatureUpdate) {
        output.schedulerSignatureBits = before - reader.remaining_bits();
        output.schedulerSignatureValid = true;
        return;
    }

    client::hooks::network::scheduler_signature_probe::Capture native{};
    if (!client::hooks::network::scheduler_signature_probe::latest(native) || native.bitCount == 0
        || native.bitCount > reader.remaining_bits()
        || native.bitCount + 1 > output.schedulerSignature.wire.size() * kByteBits
        || !reader.skip(native.bitCount)) {
        return;
    }
    output.schedulerSignature.value = native.value;
    output.schedulerSignatureBits = before - reader.remaining_bits();
    output.schedulerSignature.present = true;
    output.schedulerSignatureValid = true;
}

/** Retains the client's exact encoded signature instead of attempting to re-encode its schema. */
[[nodiscard]] bool
capture_scheduler_signature_wire(bits::Reader reader,
                                 std::size_t bitCount,
                                 state::gameplay::SchedulerSignature& signature) noexcept {
    if (bitCount == 0 || bitCount > signature.wire.size() * kByteBits) {
        return false;
    }
    signature.wire.fill(std::byte{});
    std::size_t remaining = bitCount;
    std::size_t byteIndex = 0;
    while (remaining != 0) {
        const auto width = static_cast<std::uint8_t>(remaining < kByteBits ? remaining : kByteBits);
        std::uint64_t value = 0;
        if (!reader.read(width, value)) {
            return false;
        }
        signature.wire[byteIndex++] = static_cast<std::byte>(value);
        remaining -= width;
    }
    signature.wireBits = static_cast<std::uint16_t>(bitCount);
    return true;
}

/** Replays the exact schema-encoded client scheduler signature update. */
[[nodiscard]] bool
write_scheduler_signature(bits::Writer& writer,
                          const state::gameplay::SchedulerSignature& signature) noexcept {
    if (!signature.present || signature.viewCount == 0
        || signature.viewCount > signature.views.size() || signature.wireBits == 0
        || signature.wireBits > signature.wire.size() * kByteBits) {
        return false;
    }
    std::size_t remaining = signature.wireBits;
    std::size_t byteIndex = 0;
    while (remaining != 0) {
        const auto width = static_cast<std::uint8_t>(remaining < kByteBits ? remaining : kByteBits);
        if (!writer.write(std::to_integer<std::uint8_t>(signature.wire[byteIndex++]), width)) {
            return false;
        }
        remaining -= width;
    }
    return true;
}

/** Writes the five observed empty-handler bits for one registered view. */
[[nodiscard]] bool write_empty_scheduler_view(bits::Writer& writer) noexcept {
    // The native entity-list boundary begins after a combined three-zero handler prelude.
    return writer.write(0, 1) && writer.write(0, 1) && writer.write(0, 1) && writer.write(1, 1)
           && writer.write(0, 1);
}

/** Writes the complete six-bit empty body proven by the per-handler two-view trace. */
[[nodiscard]] bool write_complete_empty_scheduler_view(bits::Writer& writer) noexcept {
    // Each view owns event and mask absence bits, then two separate empty entity-prelude bits
    // (index count and spatial-cell presence), the empty entity-list terminator, and fixed absence.
    // The earlier 000100 order made the predecoder read a present cell from the final view.
    return writer.write(0, 1) && writer.write(0, 1) && writer.write(0, 1) && writer.write(0, 1)
           && writer.write(1, 1) && writer.write(0, 1);
}

/** Writes a captured MSB-first native body whose final byte is left-aligned. */
[[nodiscard]] bool write_native_update(bits::Writer& writer,
                                       const EntityCreatePlan& plan) noexcept {
    if (plan.updateBits == 0 || plan.updateBits > plan.updateWire.size() * kByteBits) {
        return false;
    }
    std::size_t remaining = plan.updateBits;
    std::size_t byteIndex = 0;
    while (remaining != 0) {
        const auto width = static_cast<std::uint8_t>(remaining < kByteBits ? remaining : kByteBits);
        auto value = std::to_integer<std::uint8_t>(plan.updateWire[byteIndex++]);
        if (width < kByteBits) {
            value = static_cast<std::uint8_t>(value >> (kByteBits - width));
        }
        if (!writer.write(value, width)) {
            return false;
        }
        remaining -= width;
    }
    return true;
}

/** Writes one update-only shortcut for the exact native slot accepted by the preceding create. */
[[nodiscard]] bool write_entity_update_view(bits::Writer& writer,
                                            const EntityCreatePlan& plan) noexcept {
    client::hooks::network::entity_slot_probe::arm_decoder_trace();
    return writer.write(0, 1) && writer.write(0, 1)
           && writer.write(0, 1)
           // Entity lane, one direct 17-bit handle.
           && writer.write(0, 1) && writer.write(1, 1) && writer.write(plan.slot, 13)
           && writer.write(plan.handleGeneration, 4)
           // The flag shortcut denotes update-only. Publish the selected world bubble explicitly;
           // the active decode context otherwise inherits the global 0xFFFF cell.
           && writer.write(1, 1) && writer.write(1, 1) && writer.write(1, 1)
           && writer.write(plan.spatialCell, 8)
           && write_native_update(writer, plan)
           // End entity lane and leave the fixed-control handler empty.
           && writer.write(1, 1) && writer.write(0, 1);
}

/**
 * Writes one direct kind-0 shared Vandal create, optionally with its initial native update. The
 * first control record establishes the RSAT-specific update shape; the second reuses that exact
 * native wire in one atomic create/update.
 */
[[nodiscard]] bool write_entity_create_view(bits::Writer& writer,
                                            const EntityCreatePlan& plan) noexcept {
    if (plan.updateOnly) {
        return write_entity_update_view(writer, plan);
    }
    // Trace only the bounded native decoder calls that follow this guarded server emission.
    client::hooks::network::entity_slot_probe::arm_decoder_trace();
    const bool hasUpdate = plan.updateBits != 0;

    // The exact native boundary consumes three zero bits before entering the entity list.
    if (!writer.write(0, 1) || !writer.write(0, 1)
        || !writer.write(0, 1)
        // Entity lane continues with a direct (non-anchor) 17-bit handle.
        || !writer.write(0, 1) || !writer.write(1, 1) || !writer.write(plan.slot, 13)
        || !writer.write(plan.handleGeneration, 4)
        // Explicit flags: create, optional initial update, no remove/lifecycle state/anchor.
        || !writer.write(0, 1) || !writer.write(1, 1) || !writer.write(hasUpdate ? 1 : 0, 1)
        || !writer.write(0, 1) || !writer.write(0, 1)
        || !writer.write(0, 1)
        // Publish the selected world bubble explicitly. Inheritance currently resolves to the
        // global 0xFFFF cell, which allocates the object outside the streamed destination.
        || !writer.write(1, 1) || !writer.write(1, 1)
        || !writer.write(plan.spatialCell, 8)
        // Kind-0 core: object generation, codec kind, installed RSAT schema, identity flag.
        || !writer.write(plan.objectGeneration, 8) || !writer.write(0, 2)
        || !writer.write(kInstalledTagDiscriminator, 6) || !writer.write(1, 1)
        || !writer.write(plan.rsat, 32)
        // Enable the named spatial layout so the injected native baseline exposes its exact shape.
        || !writer.write(1, 1)) {
        return false;
    }
    if (hasUpdate && !write_native_update(writer, plan)) {
        return false;
    }
    // End entity lane and leave the fixed-control handler empty.
    return writer.write(1, 1) && writer.write(0, 1);
}

/** Writes one proven signature and either an empty or guarded create body for every view. */
[[nodiscard]] bool write_scheduler(bits::Writer& writer,
                                   const state::gameplay::SchedulerSignature& signature,
                                   const EntityCreatePlan& plan) noexcept {
    const bool oneView = signature.viewCount == kProvenSchedulerViewCount
                         && signature.wireBits == kProvenSchedulerWireBits;
    const bool twoView = signature.viewCount == kTwoViewProbeViewCount
                         && signature.wireBits == kTwoViewProbeWireBits;
    if ((!oneView && !twoView) || !write_scheduler_signature(writer, signature)) {
        return false;
    }
    for (std::size_t index = 0; index < signature.viewCount; ++index) {
        if (plan.present && index == plan.viewIndex) {
            // The stored signature's final bit supplies view 0's event lane. Every later view
            // must publish that event-absence bit before the common post-event entity body.
            if ((twoView && index != 0 && !writer.write(0, 1))
                || !write_entity_create_view(writer, plan)) {
                return false;
            }
        } else {
            // View 0 likewise needs only its five-bit post-event remainder; subsequent empty
            // views own all six handler bits.
            const bool written = twoView && index != 0 ? write_complete_empty_scheduler_view(writer)
                                                       : write_empty_scheduler_view(writer);
            if (!written) {
                return false;
            }
        }
    }
    return true;
}

/** Writes the exact two-view signature, one old-view create, and an empty current-view tail. */
[[nodiscard]] bool write_two_view_probe(bits::Writer& writer,
                                        const TwoViewProbePlan& plan,
                                        const EntityCreatePlan& entityCreate) noexcept {
    if (!plan.present || !plan.scheduler.present
        || plan.scheduler.viewCount != kTwoViewProbeViewCount
        || plan.scheduler.wireBits != kTwoViewProbeWireBits
        || plan.selectedView != kTwoViewProbeAuthorityView
        || plan.entityView != kTwoViewProbeEntityView || plan.entityToken == 0
        || plan.entityToken == plan.token || !entityCreate.present || !entityCreate.combinedCreate
        || entityCreate.updateOnly || entityCreate.updateBits != kFirstEntityUpdateBits
        || entityCreate.token != plan.entityToken || entityCreate.viewIndex != plan.entityView
        || entityCreate.namespaceId != kTwoViewProbeEntityNamespace
        || entityCreate.schedulerKey != plan.scheduler.views[plan.entityView].key
        || entityCreate.schedulerTag != plan.scheduler.views[plan.entityView].tag
        || entityCreate.rsat != state::gameplay::kFirstEntityRsat
        || !entityCreate.spatialCellPresent || entityCreate.spatialRegion != kTwoViewProbeRegion
        || entityCreate.spatialBubble != kTwoViewProbeBubble
        || entityCreate.spatialCell != kTwoViewProbeCell
        || !write_scheduler_signature(writer, plan.scheduler)) {
        return false;
    }
    for (std::size_t index = 0; index < kTwoViewProbeViewCount; ++index) {
        if (index == plan.entityView ? !write_entity_create_view(writer, entityCreate)
                                     : !write_complete_empty_scheduler_view(writer)) {
            return false;
        }
    }
    return true;
}

/** @return True when the captured local scheduler is the exact stored logical layout. */
[[nodiscard]] bool scheduler_matches_local_capture(
    const state::gameplay::SchedulerSignature& scheduler,
    const client::hooks::network::entity_slot_probe::ViewCapture& capture) noexcept {
    if (!scheduler.present || !capture.schedulerSignatureValid
        || capture.schedulerSignature != scheduler.value
        || capture.schedulerViewCount != scheduler.viewCount
        || capture.schedulerViewCount > capture.schedulerViewKeys.size()) {
        return false;
    }
    for (std::size_t index = 0; index < scheduler.viewCount; ++index) {
        if (capture.schedulerViewKeys[index] != scheduler.views[index].key
            || capture.schedulerViewTags[index] != scheduler.views[index].tag) {
            return false;
        }
    }
    return true;
}

/** @return True when the readable remote scheduler is the exact stored logical layout. */
[[nodiscard]] bool scheduler_matches_remote_capture(
    const state::gameplay::SchedulerSignature& scheduler,
    const client::hooks::network::entity_slot_probe::ViewCapture& capture) noexcept {
    if (!scheduler.present || !capture.schedulerRemoteSignatureValid
        || capture.schedulerRemoteSignature != scheduler.value
        || capture.schedulerRemoteViewCount != scheduler.viewCount
        || capture.schedulerRemoteViewCount > capture.schedulerRemoteViewKeys.size()) {
        return false;
    }
    for (std::size_t index = 0; index < scheduler.viewCount; ++index) {
        if (capture.schedulerRemoteViewKeys[index] != scheduler.views[index].key
            || capture.schedulerRemoteViewTags[index] != scheduler.views[index].tag) {
            return false;
        }
    }
    return true;
}

/** @return True only when one server view row owns the requested scheduler token. */
[[nodiscard]] bool unique_view_token(const state::gameplay::PeerLink& peer,
                                     std::uint64_t token) noexcept {
    if (token == 0) {
        return false;
    }
    bool found = false;
    for (const state::gameplay::ViewSignature& view : peer.views) {
        if (view.token != token) {
            continue;
        }
        if (found) {
            return false;
        }
        found = true;
    }
    return found;
}

/**
 * Builds the one-shot two-view validation only for the bound view owned by the current world.
 * The remote layout need only be readable: direct packet acknowledgement, not mutation, is proof.
 */
[[nodiscard]] bool prepare_two_view_probe(const state::gameplay::PeerLink& peer,
                                          const SelectedReplicationView& selected,
                                          TwoViewProbePlan& output) noexcept {
    output = {};
    if (!selected.present || !selected.signature.bound || selected.signature.token == 0
        || !selected.capturePresent || !selected.worldPresent
        || selected.world.region == state::activity::membership::kAbsentRegionIndex) {
        return false;
    }
    const std::uint64_t currentGroup = group::advertised_group_session(selected.world.region);
    if (currentGroup == 0
        || group::holding_group_session(selected.signature.token) != currentGroup) {
        return false;
    }

    const state::gameplay::SchedulerSignature& scheduler = peer.schedulerSignature;
    const client::hooks::network::entity_slot_probe::ViewCapture& capture = selected.capture;
    if (!scheduler.present || scheduler.viewCount != kTwoViewProbeViewCount
        || scheduler.wireBits != kTwoViewProbeWireBits || capture.token != selected.signature.token
        || capture.schedulerKey != capture.token
        || !scheduler_matches_local_capture(scheduler, capture)
        || !capture.schedulerRemoteSignatureValid
        || capture.schedulerRemoteViewCount > capture.schedulerRemoteViewKeys.size()) {
        return false;
    }

    std::size_t selectedIndex = scheduler.views.size();
    for (std::size_t index = 0; index < scheduler.viewCount; ++index) {
        if (scheduler.views[index].key != capture.schedulerKey
            || scheduler.views[index].tag != capture.schedulerTag) {
            continue;
        }
        // Duplicate logical entries make the selected per-view handler ambiguous.
        if (selectedIndex != scheduler.views.size()) {
            return false;
        }
        selectedIndex = index;
    }
    const std::uint64_t entityToken = scheduler.views[kTwoViewProbeEntityView].key;
    if (selectedIndex == scheduler.views.size() || selectedIndex != kTwoViewProbeAuthorityView
        || entityToken == 0 || entityToken == capture.schedulerKey
        || !unique_view_token(peer, entityToken)) {
        return false;
    }

    output.scheduler = scheduler;
    output.token = selected.signature.token;
    output.entityToken = entityToken;
    output.selectedView = static_cast<std::uint8_t>(selectedIndex);
    output.entityView = kTwoViewProbeEntityView;
    output.remoteViews = capture.schedulerRemoteViewCount;
    output.remoteAlreadyMatches = scheduler_matches_remote_capture(scheduler, capture);
    output.present = true;
    return true;
}

/** @return True when two guarded plans describe the exact same atomic create. */
[[nodiscard]] bool same_entity_create_plan(const EntityCreatePlan& left,
                                           const EntityCreatePlan& right) noexcept {
    return left.token == right.token && left.schedulerKey == right.schedulerKey
           && left.rsat == right.rsat && left.slot == right.slot
           && left.schedulerTag == right.schedulerTag
           && left.handleGeneration == right.handleGeneration
           && left.objectGeneration == right.objectGeneration && left.viewIndex == right.viewIndex
           && left.namespaceId == right.namespaceId && left.spatialRegion == right.spatialRegion
           && left.updateWire == right.updateWire && left.updateBits == right.updateBits
           && left.spatialCell == right.spatialCell && left.spatialBubble == right.spatialBubble
           && left.spatialCellPresent == right.spatialCellPresent
           && left.bootstrapScheduler == right.bootstrapScheduler
           && left.combinedCreate == right.combinedCreate && left.updateOnly == right.updateOnly
           && left.present == right.present;
}

/**
 * Builds an outgoing-view atomic create under the current Basin authority/spatial context.
 * A retained plan supplies only the already-consumed native update during send revalidation;
 * every topology, slot, resource, and cell field is rebuilt from current live state.
 */
[[nodiscard]] bool
prepare_entity_create_with_two_view_probe(const state::gameplay::PeerLink& peer,
                                          const SelectedReplicationView& selected,
                                          const TwoViewProbePlan& probe,
                                          const EntityCreatePlan* retained,
                                          EntityCreatePlan& output,
                                          EntityCreateGate& gate) noexcept {
    output = {};
    output.namespaceId = -1;
    if (!probe.present || !probe.scheduler.present
        || probe.scheduler.viewCount != kTwoViewProbeViewCount
        || probe.scheduler.wireBits != kTwoViewProbeWireBits
        || probe.selectedView != kTwoViewProbeAuthorityView
        || probe.entityView != kTwoViewProbeEntityView || probe.entityToken == 0
        || probe.entityToken == probe.token
        || probe.scheduler.views[probe.entityView].key != probe.entityToken) {
        gate = EntityCreateGate::schedulerShape;
        return false;
    }
    if (!selected.present || !selected.signature.bound || selected.signature.token == 0
        || selected.signature.token != probe.token || !selected.worldPresent
        || selected.world.region != kTwoViewProbeAuthorityRegion) {
        gate = EntityCreateGate::view;
        return false;
    }
    const std::uint64_t currentGroup = group::advertised_group_session(selected.world.region);
    if (currentGroup == 0
        || group::holding_group_session(selected.signature.token) != currentGroup) {
        gate = EntityCreateGate::view;
        return false;
    }
    if (!unique_view_token(peer, probe.entityToken)) {
        gate = EntityCreateGate::view;
        return false;
    }

    client::hooks::network::entity_slot_probe::ViewCapture capture{};
    if (!client::hooks::network::entity_slot_probe::find(probe.entityToken, capture)) {
        gate = EntityCreateGate::captureMissing;
        return false;
    }
    if (capture.token != probe.entityToken || capture.schedulerKey != capture.token
        || capture.namespaceId != kTwoViewProbeEntityNamespace || capture.manager == nullptr) {
        gate = EntityCreateGate::schedulerIdentity;
        return false;
    }
    if (!capture.candidatePresent) {
        gate = EntityCreateGate::candidate;
        return false;
    }
    if (capture.occupiedCount != kFirstEntityBaselineOccupied
        || capture.slot != kFirstEntityBaselineOccupied) {
        gate = EntityCreateGate::baseline;
        return false;
    }
    if (capture.slot >= 0x2000 || capture.availableCount == 0) {
        gate = EntityCreateGate::slot;
        return false;
    }
    if (capture.handleGeneration != 0 || capture.reservedGeneration != 0
        || capture.objectGeneration != 0) {
        gate = EntityCreateGate::generation;
        return false;
    }
    if (!client::hooks::network::sobject_rsat_probe::first_entity_ready()) {
        gate = EntityCreateGate::rsat;
        return false;
    }
    if (!resolve_entity_spatial_cell(selected, output)
        || output.spatialRegion != kTwoViewProbeAuthorityRegion
        || output.spatialBubble != kTwoViewProbeAuthorityBubble
        || output.spatialCell != kTwoViewProbeAuthorityCell) {
        gate = EntityCreateGate::spatialCell;
        return false;
    }
    // Namespace 1 does not own Basin cell 11: its native dirty-row processor rejects that cell
    // before type-2 construction. Keep current Basin authority as the hard gate, but address the
    // outgoing view's proven active Town cell as a bounded end-to-end construction control.
    output.spatialRegion = kTwoViewProbeRegion;
    output.spatialBubble = kTwoViewProbeBubble;
    output.spatialCell = kTwoViewProbeCell;
    if (!scheduler_matches_local_capture(probe.scheduler, capture)) {
        gate = EntityCreateGate::schedulerIdentity;
        return false;
    }

    std::size_t match = probe.scheduler.views.size();
    for (std::size_t index = 0; index < probe.scheduler.viewCount; ++index) {
        if (probe.scheduler.views[index].key != capture.schedulerKey
            || probe.scheduler.views[index].tag != capture.schedulerTag) {
            continue;
        }
        if (match != probe.scheduler.views.size()) {
            gate = EntityCreateGate::schedulerEntry;
            return false;
        }
        match = index;
    }
    if (match != probe.entityView) {
        gate = EntityCreateGate::schedulerEntry;
        return false;
    }

    output.token = capture.token;
    output.schedulerKey = capture.schedulerKey;
    output.schedulerTag = capture.schedulerTag;
    output.rsat = state::gameplay::kFirstEntityRsat;
    output.slot = capture.slot;
    output.handleGeneration = capture.handleGeneration;
    output.objectGeneration = kFirstObjectGeneration;
    output.viewIndex = static_cast<std::uint8_t>(match);
    output.namespaceId = capture.namespaceId;
    if (retained == nullptr) {
        client::hooks::network::sobject_update_probe::NearbyUpdateCapture update{};
        if (!client::hooks::network::sobject_update_probe::take_nearby_player_update(
                state::gameplay::kFirstEntityRsat, update)
            || update.bitCount != kFirstEntityUpdateBits) {
            gate = EntityCreateGate::rsat;
            return false;
        }
        output.updateWire = update.wire;
        output.updateBits = update.bitCount;
    } else {
        output.updateWire = retained->updateWire;
        output.updateBits = retained->updateBits;
    }
    output.combinedCreate = true;
    output.present = true;
    if (output.updateBits != kFirstEntityUpdateBits
        || (retained != nullptr && !same_entity_create_plan(output, *retained))) {
        gate = EntityCreateGate::rsat;
        return false;
    }
    gate = EntityCreateGate::ready;
    return true;
}

/** @return True when the client has accepted the exact local logical scheduler order. */
[[nodiscard]] bool scheduler_layouts_agree(
    const client::hooks::network::entity_slot_probe::ViewCapture& capture) noexcept {
    if (!capture.schedulerRemoteSignatureValid
        || capture.schedulerRemoteSignature != capture.schedulerSignature
        || capture.schedulerRemoteViewCount != capture.schedulerViewCount) {
        return false;
    }
    for (std::size_t index = 0; index < capture.schedulerViewCount; ++index) {
        if (capture.schedulerRemoteViewKeys[index] != capture.schedulerViewKeys[index]
            || capture.schedulerRemoteViewTags[index] != capture.schedulerViewTags[index]) {
            return false;
        }
    }
    return true;
}

/** @return True only before this host has ever populated the client's scheduler view list. */
[[nodiscard]] bool scheduler_remote_is_pristine(
    const client::hooks::network::entity_slot_probe::ViewCapture& capture) noexcept {
    using ViewCapture = client::hooks::network::entity_slot_probe::ViewCapture;
    constexpr std::array<std::byte, 16> kEmptySignature{};
    constexpr std::array<std::uint64_t, ViewCapture::kSchedulerViewCapacity> kEmptyKeys{};
    constexpr std::array<std::uint8_t, ViewCapture::kSchedulerViewCapacity> kEmptyTags{};
    return capture.schedulerRemoteSignatureValid && capture.schedulerRemoteViewCount == 0
           && capture.schedulerRemoteSignature == kEmptySignature
           && capture.schedulerRemoteViewKeys == kEmptyKeys
           && capture.schedulerRemoteViewTags == kEmptyTags;
}

/**
 * @return True while the exact two-view frame proven by the client handler trace is still live.
 *
 * The trace completes inside the receive call, before the transport acknowledgement returns to
 * the server.  Waiting for that acknowledgement lets the root view join the local scheduler and
 * closes the short two-view transition window.  A pristine remote capture is allowed only after
 * all ten native handler calls completed; the capture pump can trail that synchronous proof by a
 * frame.  Any local-layout change fails closed.
 */
[[nodiscard]] bool two_view_layout_ready(const state::gameplay::PeerLink& peer,
                                         const SelectedReplicationView& selected) noexcept {
    const state::gameplay::SchedulerSignature& scheduler = peer.twoViewProbeScheduler;
    if (!peer.twoViewProbeAttempted
        || !client::hooks::network::scheduler_handler_probe::completed(kTwoViewProbeViewCount)
        || !scheduler.present || scheduler.viewCount != kTwoViewProbeViewCount
        || scheduler.wireBits != kTwoViewProbeWireBits || !selected.present
        || selected.signature.token == 0 || selected.signature.token != peer.twoViewProbeToken
        || !selected.capturePresent) {
        return false;
    }
    const client::hooks::network::entity_slot_probe::ViewCapture& capture = selected.capture;
    return capture.token == selected.signature.token && capture.schedulerKey == capture.token
           && scheduler_matches_local_capture(scheduler, capture)
           && (scheduler_matches_remote_capture(scheduler, capture)
               || scheduler_remote_is_pristine(capture));
}

/** Builds a create only when the bound token, scheduler entry, and pristine slot all agree. */
[[nodiscard]] bool prepare_entity_create(const state::gameplay::PeerLink& peer,
                                         const SelectedReplicationView& selected,
                                         EntityCreatePlan& output,
                                         EntityCreateGate& gate) noexcept {
    output = {};
    output.namespaceId = -1;
    if (!selected.present || selected.signature.token == 0) {
        gate = EntityCreateGate::view;
        return false;
    }
    if (!peer.schedulerSignature.present
        || peer.schedulerSignature.viewCount != kProvenSchedulerViewCount
        || peer.schedulerSignature.viewCount > peer.schedulerSignature.views.size()
        || peer.schedulerSignature.wireBits != kProvenSchedulerWireBits) {
        gate = EntityCreateGate::schedulerShape;
        return false;
    }

    if (!selected.capturePresent) {
        gate = EntityCreateGate::captureMissing;
        return false;
    }
    const client::hooks::network::entity_slot_probe::ViewCapture& capture = selected.capture;
    if (!capture.candidatePresent || capture.namespaceId < 0) {
        gate = EntityCreateGate::candidate;
        return false;
    }
    if (capture.occupiedCount < kFirstEntityBaselineOccupied) {
        gate = EntityCreateGate::baseline;
        return false;
    }
    if (capture.slot >= 0x2000 || capture.availableCount == 0) {
        gate = EntityCreateGate::slot;
        return false;
    }
    if (capture.handleGeneration != 0 || capture.reservedGeneration != 0
        || capture.objectGeneration != 0) {
        gate = EntityCreateGate::generation;
        return false;
    }
    if (!client::hooks::network::sobject_rsat_probe::first_entity_ready()) {
        gate = EntityCreateGate::rsat;
        return false;
    }
    if (!resolve_entity_spatial_cell(selected, output)) {
        gate = EntityCreateGate::spatialCell;
        return false;
    }

    if (!capture.schedulerSignatureValid
        || capture.schedulerSignature != peer.schedulerSignature.value) {
        gate = EntityCreateGate::signature;
        return false;
    }
    if (capture.schedulerViewCount != peer.schedulerSignature.viewCount
        || capture.schedulerKey != capture.token || peer.schedulerSignature.wireBits == 0) {
        gate = EntityCreateGate::schedulerIdentity;
        return false;
    }
    // The local list drives outbound ordering, while the remote list is what the client currently
    // uses to decode this host. A transition can leave them temporarily different even when their
    // 128-bit values already match. Only the pristine empty state may bootstrap from the exact
    // one-view client signature; a stale or partially populated mismatch remains unsafe.
    const bool schedulerAgrees = scheduler_layouts_agree(capture);
    const bool bootstrapScheduler = !schedulerAgrees && scheduler_remote_is_pristine(capture);
    if (!schedulerAgrees && !bootstrapScheduler) {
        gate = EntityCreateGate::remoteLayout;
        return false;
    }
    std::size_t match = capture.schedulerViewKeys.size();
    for (std::size_t index = 0; index < capture.schedulerViewCount; ++index) {
        if (capture.schedulerViewKeys[index] != capture.schedulerKey
            || capture.schedulerViewTags[index] != capture.schedulerTag) {
            continue;
        }
        if (match != capture.schedulerViewKeys.size()) {
            gate = EntityCreateGate::schedulerEntry;
            return false;
        }
        match = index;
    }
    if (match == capture.schedulerViewKeys.size()) {
        gate = EntityCreateGate::schedulerEntry;
        return false;
    }
    output.token = capture.token;
    output.schedulerKey = capture.schedulerKey;
    output.schedulerTag = capture.schedulerTag;
    output.rsat = state::gameplay::kFirstEntityRsat;
    output.slot = capture.slot;
    output.handleGeneration = capture.handleGeneration;
    output.objectGeneration = kFirstObjectGeneration;
    output.viewIndex = static_cast<std::uint8_t>(match);
    output.namespaceId = capture.namespaceId;
    output.bootstrapScheduler = bootstrapScheduler;
    output.present = true;
    gate = EntityCreateGate::ready;
    return true;
}

/** Builds one create in the current view after the exact cached two-view frame completed. */
[[nodiscard]] bool prepare_entity_create_after_two_view(const state::gameplay::PeerLink& peer,
                                                        const SelectedReplicationView& selected,
                                                        EntityCreatePlan& output,
                                                        EntityCreateGate& gate) noexcept {
    output = {};
    output.namespaceId = -1;
    if (!two_view_layout_ready(peer, selected)) {
        gate = EntityCreateGate::schedulerTrace;
        return false;
    }
    const state::gameplay::SchedulerSignature& scheduler = peer.twoViewProbeScheduler;
    if (!scheduler.present || scheduler.viewCount != kTwoViewProbeViewCount
        || scheduler.wireBits != kTwoViewProbeWireBits) {
        gate = EntityCreateGate::schedulerShape;
        return false;
    }
    if (!selected.present || !selected.signature.bound || selected.signature.token == 0
        || selected.signature.token != peer.twoViewProbeToken) {
        gate = EntityCreateGate::view;
        return false;
    }
    if (!selected.capturePresent) {
        gate = EntityCreateGate::captureMissing;
        return false;
    }

    const client::hooks::network::entity_slot_probe::ViewCapture& capture = selected.capture;
    if (!capture.candidatePresent || capture.namespaceId < 0) {
        gate = EntityCreateGate::candidate;
        return false;
    }
    if (capture.slot >= 0x2000 || capture.availableCount == 0) {
        gate = EntityCreateGate::slot;
        return false;
    }
    if (capture.handleGeneration != 0 || capture.reservedGeneration != 0
        || capture.objectGeneration != 0) {
        gate = EntityCreateGate::generation;
        return false;
    }
    if (!client::hooks::network::sobject_rsat_probe::first_entity_ready()) {
        gate = EntityCreateGate::rsat;
        return false;
    }
    if (!resolve_entity_spatial_cell(selected, output)) {
        gate = EntityCreateGate::spatialCell;
        return false;
    }
    std::size_t match = scheduler.views.size();
    for (std::size_t index = 0; index < scheduler.viewCount; ++index) {
        if (scheduler.views[index].key != capture.schedulerKey
            || scheduler.views[index].tag != capture.schedulerTag) {
            continue;
        }
        if (match != scheduler.views.size()) {
            gate = EntityCreateGate::schedulerEntry;
            return false;
        }
        match = index;
    }
    if (match == scheduler.views.size()) {
        gate = EntityCreateGate::schedulerEntry;
        return false;
    }

    client::hooks::network::sobject_update_probe::NearbyUpdateCapture update{};
    if (!client::hooks::network::sobject_update_probe::take_nearby_player_update(
            state::gameplay::kFirstEntityRsat, update)
        || update.bitCount != kFirstEntityUpdateBits) {
        gate = EntityCreateGate::rsat;
        return false;
    }

    output.token = capture.token;
    output.schedulerKey = capture.schedulerKey;
    output.schedulerTag = capture.schedulerTag;
    output.rsat = state::gameplay::kFirstEntityRsat;
    output.slot = capture.slot;
    output.handleGeneration = capture.handleGeneration;
    output.objectGeneration = kFirstObjectGeneration;
    output.viewIndex = static_cast<std::uint8_t>(match);
    output.namespaceId = capture.namespaceId;
    output.updateWire = update.wire;
    output.updateBits = update.bitCount;
    output.combinedCreate = true;
    output.present = true;
    gate = EntityCreateGate::ready;
    return true;
}

/**
 * Sends the decoded baseline's native transform back to the same two-view slot immediately.
 * Occupancy publication trails decoding by several frames, while the root view can join sooner;
 * the completed entity-handler trace and exact live scheduler layout are therefore the safe proof
 * for this staged update.  The normal accepted-slot path remains unchanged for one-view layouts.
 */
[[nodiscard]] bool prepare_entity_update_after_two_view(const state::gameplay::PeerLink& peer,
                                                        const SelectedReplicationView& selected,
                                                        EntityCreatePlan& output) noexcept {
    output = {};
    output.namespaceId = -1;
    if (peer.entityCreateAttempts == 0 || peer.entityFollowupSent || peer.entityCreateToken == 0
        || peer.entityCreateSlot >= 0x2000 || !two_view_layout_ready(peer, selected)) {
        return false;
    }

    const state::gameplay::SchedulerSignature& scheduler = peer.entityCreateScheduler;
    const client::hooks::network::entity_slot_probe::ViewCapture& capture = selected.capture;
    if (!scheduler.present || scheduler.viewCount != kTwoViewProbeViewCount
        || scheduler.wireBits != kTwoViewProbeWireBits || capture.namespaceId < 0
        || capture.token != peer.entityCreateToken || capture.schedulerKey != capture.token
        || capture.handleGeneration != peer.entityCreateHandleGeneration) {
        return false;
    }

    std::size_t match = scheduler.views.size();
    for (std::size_t index = 0; index < scheduler.viewCount; ++index) {
        if (scheduler.views[index].key != capture.schedulerKey
            || scheduler.views[index].tag != capture.schedulerTag) {
            continue;
        }
        if (match != scheduler.views.size()) {
            return false;
        }
        match = index;
    }
    if (match == scheduler.views.size() || !resolve_entity_spatial_cell(selected, output)) {
        return false;
    }

    client::hooks::network::sobject_update_probe::NearbyUpdateCapture update{};
    if (!client::hooks::network::sobject_update_probe::take_nearby_player_update(
            state::gameplay::kFirstEntityRsat, update)
        || update.bitCount != kFirstEntityUpdateBits) {
        return false;
    }

    output.token = capture.token;
    output.schedulerKey = capture.schedulerKey;
    output.schedulerTag = capture.schedulerTag;
    output.rsat = state::gameplay::kFirstEntityRsat;
    output.slot = peer.entityCreateSlot;
    output.handleGeneration = peer.entityCreateHandleGeneration;
    output.objectGeneration = kFirstObjectGeneration;
    output.viewIndex = static_cast<std::uint8_t>(match);
    output.namespaceId = capture.namespaceId;
    output.updateWire = update.wire;
    output.updateBits = update.bitCount;
    output.updateOnly = true;
    output.present = true;
    return true;
}

/**
 * Rebuilds the exact selected create against the one-view layout the client already accepted.
 * Local overlap views may appear while the RSAT loads, but the client's remote scheduler remains
 * on this cached layout until this host successfully sends a different signature.
 */
[[nodiscard]] bool prepare_entity_retry(const state::gameplay::PeerLink& peer,
                                        const SelectedReplicationView& selected,
                                        EntityCreatePlan& output) noexcept {
    output = {};
    output.namespaceId = -1;
    const state::gameplay::SchedulerSignature& scheduler = peer.entityCreateScheduler;
    if (peer.entityCreateAttempts == 0 || peer.entityCreateToken == 0 || !selected.present
        || selected.signature.token != peer.entityCreateToken || !selected.capturePresent
        || !scheduler.present || scheduler.viewCount != kProvenSchedulerViewCount
        || scheduler.wireBits != kProvenSchedulerWireBits) {
        return false;
    }

    const client::hooks::network::entity_slot_probe::ViewCapture& capture = selected.capture;
    if (!capture.candidatePresent || capture.namespaceId < 0 || capture.availableCount == 0
        || capture.schedulerKey != capture.token || capture.token != peer.entityCreateToken
        || capture.slot != peer.entityCreateSlot
        || capture.handleGeneration != peer.entityCreateHandleGeneration
        || capture.reservedGeneration != 0 || capture.objectGeneration != 0
        || !capture.schedulerRemoteSignatureValid
        || capture.schedulerRemoteSignature != scheduler.value
        || capture.schedulerRemoteViewCount != scheduler.viewCount
        || capture.schedulerRemoteViewKeys[0] != scheduler.views[0].key
        || capture.schedulerRemoteViewTags[0] != scheduler.views[0].tag
        || scheduler.views[0].key != capture.schedulerKey
        || scheduler.views[0].tag != capture.schedulerTag) {
        return false;
    }
    if (!resolve_entity_spatial_cell(selected, output)) {
        return false;
    }

    output.token = capture.token;
    output.schedulerKey = capture.schedulerKey;
    output.schedulerTag = capture.schedulerTag;
    output.rsat = state::gameplay::kFirstEntityRsat;
    output.slot = capture.slot;
    output.handleGeneration = capture.handleGeneration;
    output.objectGeneration = kFirstObjectGeneration;
    output.viewIndex = 0;
    output.namespaceId = capture.namespaceId;
    output.present = true;
    return true;
}

/** @return True once the native occupied map contains the exact slot this host created. */
[[nodiscard]] bool entity_create_accepted(const state::gameplay::PeerLink& peer) noexcept {
    if (peer.entityCreateAttempts == 0 || peer.entityCreateToken == 0
        || peer.entityCreateSlot >= 32) {
        return false;
    }
    client::hooks::network::entity_slot_probe::ViewCapture capture{};
    return client::hooks::network::entity_slot_probe::find(peer.entityCreateToken, capture)
           && capture.token == peer.entityCreateToken && capture.namespaceId >= 0
           && (capture.occupiedLow & (1U << peer.entityCreateSlot)) != 0;
}

/**
 * Builds a second create with its initial transform inline. Slot 13 remains the create-then-update
 * control; the next pristine slot tests whether gameworld instantiation requires update atomicity.
 */
[[nodiscard]] bool prepare_entity_combined_create(const state::gameplay::PeerLink& peer,
                                                  const SelectedReplicationView& selected,
                                                  EntityCreatePlan& output) noexcept {
    output = {};
    output.namespaceId = -1;
    const state::gameplay::SchedulerSignature& scheduler = peer.entityCreateScheduler;
    const bool oneView = scheduler.present && scheduler.viewCount == kProvenSchedulerViewCount
                         && scheduler.wireBits == kProvenSchedulerWireBits;
    const bool twoView =
        scheduler.present && scheduler.viewCount == kTwoViewProbeViewCount
        && scheduler.wireBits == kTwoViewProbeWireBits && peer.twoViewProbeAccepted
        && client::hooks::network::scheduler_handler_probe::completed(kTwoViewProbeViewCount);
    if (!peer.entityCreateAccepted || peer.entityFollowupSent || peer.entityCreateToken == 0
        || !selected.present || selected.signature.token != peer.entityCreateToken
        || !selected.capturePresent || (!oneView && !twoView)) {
        return false;
    }

    const client::hooks::network::entity_slot_probe::ViewCapture& capture = selected.capture;
    if (peer.entityCreateSlot >= 32 || capture.namespaceId < 0 || !capture.candidatePresent
        || capture.slot >= 0x2000 || capture.availableCount == 0 || capture.handleGeneration != 0
        || capture.reservedGeneration != 0 || capture.objectGeneration != 0
        || capture.schedulerKey != capture.token || capture.token != peer.entityCreateToken
        || (capture.occupiedLow & (1U << peer.entityCreateSlot)) == 0
        || !capture.schedulerSignatureValid || capture.schedulerSignature != scheduler.value
        || !scheduler_matches_remote_capture(scheduler, capture)) {
        return false;
    }
    if (oneView && !scheduler_matches_local_capture(scheduler, capture)) {
        return false;
    }
    std::size_t match = scheduler.views.size();
    for (std::size_t index = 0; index < scheduler.viewCount; ++index) {
        if (scheduler.views[index].key != capture.schedulerKey
            || scheduler.views[index].tag != capture.schedulerTag) {
            continue;
        }
        if (match != scheduler.views.size()) {
            return false;
        }
        match = index;
    }
    if (match == scheduler.views.size()) {
        return false;
    }
    if (!resolve_entity_spatial_cell(selected, output)) {
        return false;
    }

    client::hooks::network::sobject_update_probe::NearbyUpdateCapture update{};
    if (!client::hooks::network::sobject_update_probe::take_nearby_player_update(
            state::gameplay::kFirstEntityRsat, update)
        || update.bitCount != kFirstEntityUpdateBits) {
        return false;
    }

    output.token = capture.token;
    output.schedulerKey = capture.schedulerKey;
    output.schedulerTag = capture.schedulerTag;
    output.rsat = state::gameplay::kFirstEntityRsat;
    output.slot = capture.slot;
    output.handleGeneration = capture.handleGeneration;
    output.objectGeneration = kFirstObjectGeneration;
    output.viewIndex = static_cast<std::uint8_t>(match);
    output.namespaceId = capture.namespaceId;
    output.updateWire = update.wire;
    output.updateBits = update.bitCount;
    output.combinedCreate = true;
    output.present = true;
    return true;
}

/** Captures one diagnostic without moving any native or peer state. Callers hold the peer lock. */
[[nodiscard]] EntityCreateGateReport
capture_entity_create_gate(const state::gameplay::PeerLink& peer,
                           const SelectedReplicationView& selected,
                           const TwoViewProbePlan& probe,
                           const EntityCreatePlan& candidate,
                           EntityCreateGate gate,
                           std::uint64_t now) noexcept {
    EntityCreateGateReport output{};
    output.token = selected.present ? selected.signature.token : 0;
    output.entityToken = candidate.present ? candidate.token : output.token;
    output.authorityView = candidate.present ? candidate.viewIndex : 0;
    output.entityView = candidate.present ? candidate.viewIndex : 0;
    output.gate = gate;
    output.queueCount = peer.outbound.count;
    output.awaitingAcknowledgement = peer.outbound.awaitingAcknowledgement;
    output.signatureViews = peer.schedulerSignature.viewCount;
    output.wireBits = peer.schedulerSignature.wireBits;
    output.attempts = peer.entityCreateAttempts;
    output.readyAge = peer.entityCreateReadySince == 0 ? 0 : now - peer.entityCreateReadySince;

    EntityCreatePlan spatial = candidate;
    if (!spatial.spatialCellPresent) {
        static_cast<void>(resolve_entity_spatial_cell(selected, spatial));
    }
    output.spatialCellPresent = spatial.spatialCellPresent;
    output.spatialRegion = spatial.spatialRegion;
    output.spatialBubble = spatial.spatialBubble;
    output.spatialCell = spatial.spatialCell;

    client::hooks::network::entity_slot_probe::ViewCapture entityCapture{};
    const client::hooks::network::entity_slot_probe::ViewCapture* capture = nullptr;
    if (probe.present) {
        output.entityToken = probe.entityToken;
        output.authorityView = probe.selectedView;
        output.entityView = probe.entityView;
        output.capturePresent =
            client::hooks::network::entity_slot_probe::find(probe.entityToken, entityCapture);
        if (output.capturePresent) {
            capture = &entityCapture;
        }
    } else {
        output.capturePresent = selected.capturePresent;
        if (output.capturePresent) {
            capture = &selected.capture;
        }
    }
    if (!output.capturePresent) {
        return output;
    }
    output.candidatePresent = capture->candidatePresent;
    output.namespaceId = capture->namespaceId;
    output.occupied = capture->occupiedCount;
    output.occupiedLow = capture->occupiedLow;
    output.available = capture->availableCount;
    output.slot = capture->slot;
    output.handleGeneration = capture->handleGeneration;
    output.reservedGeneration = capture->reservedGeneration;
    output.objectGeneration = capture->objectGeneration;
    output.localSignatureValid = capture->schedulerSignatureValid;
    output.remoteSignatureValid = capture->schedulerRemoteSignatureValid;
    output.localViews = capture->schedulerViewCount;
    output.remoteViews = capture->schedulerRemoteViewCount;
    return output;
}

/** Pairs an exact schema encoding with the current native logical view order. */
[[nodiscard]] bool synchronise_scheduler_layout(state::gameplay::PeerLink& peer,
                                                const SelectedReplicationView& selected) noexcept {
    peer.schedulerSignature.viewCount = 0;
    peer.schedulerSignature.views = {};
    if (!selected.present || selected.signature.token == 0 || !peer.schedulerSignature.present) {
        return false;
    }

    if (!selected.capturePresent) {
        return false;
    }
    const client::hooks::network::entity_slot_probe::ViewCapture& capture = selected.capture;
    if (!capture.schedulerSignatureValid || capture.schedulerViewCount == 0
        || capture.schedulerViewCount > peer.schedulerSignature.views.size()
        || capture.schedulerSignature != peer.schedulerSignature.value) {
        return false;
    }
    peer.schedulerSignature.viewCount = capture.schedulerViewCount;
    for (std::size_t index = 0; index < capture.schedulerViewCount; ++index) {
        peer.schedulerSignature.views[index].key = capture.schedulerViewKeys[index];
        peer.schedulerSignature.views[index].tag = capture.schedulerViewTags[index];
    }
    return true;
}

/** Converts a bounded scheduler capture to uppercase hexadecimal. */
void scheduler_hex(std::span<const std::byte> input, std::span<char> output) noexcept {
    constexpr char kDigits[] = "0123456789ABCDEF";
    if (output.empty()) {
        return;
    }
    const std::size_t count =
        input.size() < (output.size() - 1) / 2 ? input.size() : (output.size() - 1) / 2;
    for (std::size_t index = 0; index < count; ++index) {
        const auto value = std::to_integer<std::uint8_t>(input[index]);
        output[index * 2] = kDigits[value >> 4];
        output[index * 2 + 1] = kDigits[value & 0x0F];
    }
    output[count * 2] = '\0';
}

/**
 * Reads the external trailer without accepting or mutating any replication state.
 * The first bit is c_network_channel_simulation_gatekeeper. The second is the
 * replication-scheduler body-presence bit. Words after those bits remain MSB-first.
 */
[[nodiscard]] bool probe_external(std::span<const std::byte> payload,
                                  std::size_t bitOffset,
                                  ExternalProbe& output) noexcept {
    ExternalProbe candidate{};
    bits::Reader reader(payload);
    if (!reader.skip(bitOffset) || !wire::read_external_status(reader, candidate.status)) {
        return false;
    }
    candidate.schedulerBitsBeforeProbe = reader.remaining_bits();
    if (candidate.status.schedulerPresent) {
        bits::Reader signatureStart = reader;
        probe_scheduler_signature(reader, candidate);
        if (candidate.schedulerSignatureUpdate && candidate.schedulerSignatureValid
            && candidate.schedulerSignature.present
            && !capture_scheduler_signature_wire(
                signatureStart, candidate.schedulerSignatureBits, candidate.schedulerSignature)) {
            candidate.schedulerSignatureValid = false;
            candidate.schedulerSignature.present = false;
        }
    }
    bits::Reader capture = reader;
    while (candidate.schedulerByteCount < candidate.schedulerBytes.size()
           && capture.remaining_bits() >= kByteBits) {
        std::uint64_t value = 0;
        if (!capture.read(kByteBits, value)) {
            return false;
        }
        candidate.schedulerBytes[candidate.schedulerByteCount++] = static_cast<std::byte>(value);
        candidate.schedulerCapturedBits += kByteBits;
    }
    if (candidate.schedulerByteCount < candidate.schedulerBytes.size()
        && capture.remaining_bits() != 0) {
        candidate.schedulerTailBits = static_cast<std::uint8_t>(capture.remaining_bits());
        std::uint64_t value = 0;
        if (!capture.read(candidate.schedulerTailBits, value)) {
            return false;
        }
        candidate.schedulerBytes[candidate.schedulerByteCount++] =
            static_cast<std::byte>(value << (kByteBits - candidate.schedulerTailBits));
        candidate.schedulerCapturedBits += candidate.schedulerTailBits;
    }
    while (candidate.wordCount < candidate.words.size() && reader.remaining_bits() != 0) {
        const std::size_t remaining = reader.remaining_bits();
        const auto width = static_cast<std::uint8_t>(
            remaining < kExternalProbeWordBits ? remaining : kExternalProbeWordBits);
        std::uint64_t word = 0;
        if (!reader.read(width, word)) {
            return false;
        }
        candidate.words[candidate.wordCount] = word;
        candidate.widths[candidate.wordCount] = width;
        ++candidate.wordCount;
    }
    candidate.schedulerBitsAfterProbe = reader.remaining_bits();
    output = candidate;
    return true;
}

/** @return True when both endpoints name the same address and port. */
[[nodiscard]] bool same_endpoint(const state::gameplay::Endpoint& left,
                                 const state::gameplay::Endpoint& right) noexcept {
    return left.address == right.address && left.port == right.port;
}

/** Result of adding one native fragment to its bounded packet assembly. */
enum class FragmentResult : std::uint8_t { invalid, held, complete };

/**
 * Adds one established-packet fragment and returns the original packet once every piece arrived.
 * Ghidra `FUN_1416D4B00` proves the six-bit set id, eight-set overlap, 1,238-byte stride, and the
 * count/index pair. The reassembled body includes its original marker and unfragmented selector.
 */
[[nodiscard]] FragmentResult assemble_fragment(const state::gameplay::Endpoint& from,
                                               std::span<const std::byte> payload,
                                               std::uint64_t now,
                                               std::span<std::byte> output,
                                               std::size_t& outputSize,
                                               wire::PacketFragment& decoded) noexcept {
    outputSize = 0;
    if (!wire::decode_packet_fragment(payload, decoded)) {
        return FragmentResult::invalid;
    }

    AcquireSRWLockExclusive(&g_lock);
    FragmentAssembly* assembly = nullptr;
    FragmentAssembly* free = nullptr;
    FragmentAssembly* oldest = &g_fragmentAssemblies.front();
    for (FragmentAssembly& candidate : g_fragmentAssemblies) {
        if (candidate.active && same_endpoint(candidate.endpoint, from)
            && candidate.setId == decoded.setId && candidate.guard == decoded.connectionSequenceLow2
            && candidate.count == decoded.count) {
            assembly = &candidate;
            break;
        }
        if (!candidate.active && free == nullptr) {
            free = &candidate;
        }
        if (candidate.touched < oldest->touched) {
            oldest = &candidate;
        }
    }
    if (assembly == nullptr) {
        assembly = free == nullptr ? oldest : free;
        *assembly = {};
        assembly->endpoint = from;
        assembly->setId = decoded.setId;
        assembly->guard = decoded.connectionSequenceLow2;
        assembly->count = decoded.count;
        assembly->active = true;
    }
    assembly->touched = now;
    const std::size_t index = decoded.index;
    if (!assembly->received[index]) {
        std::memcpy(assembly->bytes.data() + index * wire::kPacketFragmentStride,
                    decoded.body.data(),
                    decoded.body.size());
        assembly->sizes[index] = decoded.body.size();
        assembly->received[index] = true;
    }

    bool complete = true;
    for (std::size_t piece = 0; piece < assembly->count; ++piece) {
        complete = complete && assembly->received[piece];
    }
    if (!complete) {
        ReleaseSRWLockExclusive(&g_lock);
        return FragmentResult::held;
    }
    const std::size_t size = (assembly->count - 1U) * wire::kPacketFragmentStride
                             + assembly->sizes[assembly->count - 1U];
    if (size == 0 || size > output.size()) {
        *assembly = {};
        ReleaseSRWLockExclusive(&g_lock);
        return FragmentResult::invalid;
    }
    std::memcpy(output.data(), assembly->bytes.data(), size);
    outputSize = size;
    *assembly = {};
    ReleaseSRWLockExclusive(&g_lock);
    return FragmentResult::complete;
}

/** @return Peer for one endpoint, or null. Callers already hold the lock. */
[[nodiscard]] state::gameplay::PeerLink*
find_locked(const state::gameplay::Endpoint& from) noexcept {
    for (state::gameplay::PeerLink& peer : g_peers) {
        if (peer.stage != state::gameplay::PeerStage::absent
            && same_endpoint(peer.endpoint, from)) {
            return &peer;
        }
    }
    return nullptr;
}

/** @return True when the link carries one group session. Callers hold the lock. */
[[nodiscard]] bool carries_locked(const state::gameplay::PeerLink& peer,
                                  std::uint64_t sessionId) noexcept {
    for (const std::uint64_t held : peer.sessions) {
        if (held == sessionId) {
            return true;
        }
    }
    return false;
}

/** @return Link carrying one group session, or null. Callers hold the lock. */
[[nodiscard]] state::gameplay::PeerLink* find_session_locked(std::uint64_t sessionId) noexcept {
    if (sessionId == 0) {
        return nullptr;
    }
    for (state::gameplay::PeerLink& peer : g_peers) {
        if (peer.stage != state::gameplay::PeerStage::absent && carries_locked(peer, sessionId)) {
            return &peer;
        }
    }
    return nullptr;
}

/**
 * Resolves the session a message that does not name one belongs to.
 * A link carrying more than one session leaves it unresolved rather than guessing.
 * @param peer Link the message arrived on.
 * @return The session id, or zero when the link carries none or several.
 */
[[nodiscard]] std::uint64_t sole_session_locked(const state::gameplay::PeerLink& peer) noexcept {
    std::uint64_t only = 0;
    for (const std::uint64_t held : peer.sessions) {
        if (held == 0) {
            continue;
        }
        if (only != 0) {
            return 0;
        }
        only = held;
    }
    return only;
}

/**
 * Resolves the session an out-of-band message at one endpoint belongs to.
 * @param from Peer endpoint.
 * @return The session id, or zero when it cannot be resolved.
 */
[[nodiscard]] std::uint64_t session_for_endpoint(const state::gameplay::Endpoint& from) noexcept {
    AcquireSRWLockShared(&g_lock);
    const state::gameplay::PeerLink* const peer = find_locked(from);
    const std::uint64_t only = peer == nullptr ? 0 : sole_session_locked(*peer);
    ReleaseSRWLockShared(&g_lock);
    return only;
}

/** @return A free peer slot, or null. Callers already hold the lock. */
[[nodiscard]] state::gameplay::PeerLink* allocate_locked() noexcept {
    for (state::gameplay::PeerLink& peer : g_peers) {
        if (peer.stage == state::gameplay::PeerStage::absent) {
            return &peer;
        }
    }
    return nullptr;
}

/** Fills the address blob that names this host on the direct path. */
void local_address(std::array<std::byte, wire::kAddressBlobSize>& output) noexcept {
    const state::gameplay::Endpoint advertised = endpoint::advertised();
    middleware::gameplay::descriptor::write_net_addr(advertised.address, advertised.port, output);
}

/** @return A random 32-bit sequence, or zero when Windows refused. */
[[nodiscard]] std::uint32_t random_sequence() noexcept {
    std::array<std::byte, sizeof(std::uint32_t)> bytes{};
    if (!middleware::crypto::random::fill(bytes)) {
        return 0;
    }
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        value |= std::to_integer<std::uint32_t>(bytes[index]) << (index * kByteBits);
    }
    return value;
}

/**
 * Answers the peer's connect establish with this host's own.
 * It goes on the reliable queue because that is where the peer sends its own.
 * @param to Peer endpoint.
 * @param remoteChannelId Channel id the request carried. The link must still hold it.
 * @param body Both channel ids.
 */
void answer_establish(const state::gameplay::Endpoint& to,
                      std::uint32_t remoteChannelId,
                      const wire::ConnectEstablish& body) noexcept {
    std::array<std::byte, kReplyCapacity> buffer{};
    bits::Writer writer(buffer);
    std::size_t size = 0;
    if (!wire::write_establish(writer, body) || !writer.finish(size)) {
        report(core::log::Level::warn, "ev=gameplay stage=establish result=fail reason=encode");
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    // The endpoint's link. A channel the peer has retired has no link of its own to answer on.
    state::gameplay::PeerLink* peer = find_locked(to);
    const bool queued =
        peer != nullptr && peer->remoteConnectionSequence == remoteChannelId
        && wire::enqueue_message(peer->outbound,
                                 static_cast<std::uint8_t>(wire::ConnectId::establish),
                                 wire::kEstablishSize,
                                 {buffer.data(), size},
                                 writer.bit_count());
    if (queued) {
        peer->acknowledgementOwed = true;
        peer->outbound.awaitingAcknowledgement = false;
    }
    ReleaseSRWLockExclusive(&g_lock);
    report(core::log::Level::info,
           "ev=gameplay stage=establish result=%s local=0x%08X remote=0x%08X",
           queued ? "queued" : "fail",
           body.channelId,
           body.remoteChannelId);
}

/**
 * Answers one connect request with a connect response.
 * @param from Requesting endpoint.
 * @param request Decoded request body.
 * @param now Monotonic tick count.
 */
void answer_connect(const state::gameplay::Endpoint& from,
                    const wire::ConnectRequest& request,
                    std::uint64_t now) noexcept {
    wire::ConnectResponse response{};
    // The peer checks both echoed fields and closes the connection on a wrong sequence.
    response.remoteChannelId = request.channelId;
    response.remoteSequence = request.sequence;
    local_address(response.address);

    AcquireSRWLockExclusive(&g_lock);
    // Keyed by endpoint. The client holds one channel per host peer, so a second link would stamp
    // packets with a channel id the client has already retired.
    state::gameplay::PeerLink* peer = find_locked(from);
    // A repeat of the same request is a retransmission and leaves the link alone. A different
    // channel or sequence is a new incarnation the peer built without announcing the teardown.
    const bool rebuilt = peer != nullptr
                         && (peer->remoteConnectionSequence != request.channelId
                             || peer->remoteTransportSequence != request.sequence);
    if (peer == nullptr) {
        peer = allocate_locked();
    }
    const bool fresh =
        peer != nullptr && (peer->stage == state::gameplay::PeerStage::absent || rebuilt);
    if (fresh) {
        // The sessions outlive the channel. The client rebuilds one channel under every group
        // session it holds and rejoins none of them, so dropping them here strands each one.
        const std::array<std::uint64_t, state::gameplay::kSessionsPerLink> held =
            peer->stage == state::gameplay::PeerStage::absent
                ? std::array<std::uint64_t, state::gameplay::kSessionsPerLink>{}
                : peer->sessions;
        const std::array<state::gameplay::ViewSignature, state::gameplay::kSessionsPerLink> views =
            peer->stage == state::gameplay::PeerStage::absent
                ? std::array<state::gameplay::ViewSignature, state::gameplay::kSessionsPerLink>{}
                : peer->views;
        *peer = {};
        peer->sessions = held;
        peer->views = views;
        peer->endpoint = from;
        // The channel id is an incarnation counter: the peer refuses one that does not
        // increase, and reads all ones as unset.
        peer->localConnectionSequence = ++g_channelId;
        // The peer builds its receive window from the announced sequence and expects the first
        // packet one past it. This announces the sequence before the first packet, not the first
        // packet itself.
        peer->localTransportSequence =
            (random_sequence() & ~static_cast<std::uint32_t>(kPacketSequenceModulus - 1))
            | static_cast<std::uint32_t>(kFirstPacketSequence - 1);
    }
    if (peer != nullptr) {
        peer->remoteConnectionSequence = request.channelId;
        peer->remoteTransportSequence = request.sequence;
        // The membership update must name the peer's own address, so its own blob is kept.
        peer->remoteAddress = request.address;
        peer->remoteAddressPresent = true;
        // A retransmission must not move an established link back a stage.
        if (fresh) {
            peer->stage = state::gameplay::PeerStage::connecting;
        }
        peer->lastTick = now;
        response.channelId = peer->localConnectionSequence;
        response.sequence = peer->localTransportSequence;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (peer == nullptr) {
        report(core::log::Level::warn, "ev=gameplay stage=connect result=fail reason=capacity");
        return;
    }

    std::array<std::byte, kReplyCapacity> buffer{};
    bits::Writer writer(buffer);
    wire::MessageHeader header{static_cast<std::uint8_t>(wire::ConnectId::response),
                               wire::kResponseSize};
    std::size_t size = 0;
    if (!wire::open_container(writer) || !wire::write_header(writer, header)
        || !wire::write_response(writer, response) || !wire::close_container(writer)
        || !writer.finish(size) || !send_transport(from, {buffer.data(), size})) {
        report(core::log::Level::warn, "ev=gameplay stage=connect result=fail reason=send");
        return;
    }
    // A rebuilt link is invisible otherwise: the peer closes the old one silently.
    report(core::log::Level::info,
           "ev=gameplay stage=connect result=ok peer=%u local=0x%08X remote=0x%08X rebuilt=%u",
           from.port,
           response.channelId,
           request.channelId,
           rebuilt ? 1U : 0U);
    // The peer refuses any first reliable record that is not a connect establish, so this must be
    // enqueued before anything else the join produces.
    wire::ConnectEstablish establish{};
    establish.remoteChannelId = response.remoteChannelId;
    establish.channelId = response.channelId;
    answer_establish(from, request.channelId, establish);
}

/**
 * Binds one group session to the link the peer opened for it.
 * @param from Peer endpoint.
 * @param sessionId Session the join request named.
 * @return True when a link now carries that session.
 */
[[nodiscard]] bool bind_session(const state::gameplay::Endpoint& from,
                                std::uint64_t sessionId) noexcept {
    if (sessionId == 0) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    // The endpoint's link, whatever it already carries. A join for a second region arrives on the
    // same channel as the first, and out of band when that channel is still being rebuilt.
    state::gameplay::PeerLink* const peer = find_locked(from);
    const char* result = "nolink";
    bool bound = false;
    std::uint32_t channel = 0;
    if (peer != nullptr) {
        channel = peer->localConnectionSequence;
        result = "full";
        for (std::size_t index = 0; index < peer->sessions.size(); ++index) {
            if (peer->sessions[index] == sessionId) {
                result = "held";
                bound = true;
                break;
            }
            if (peer->sessions[index] == 0) {
                peer->sessions[index] = sessionId;
                peer->views[index] = {};
                result = "bound";
                bound = true;
                break;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    report(bound ? core::log::Level::info : core::log::Level::warn,
           "ev=gameplay stage=link result=%s session=0x%016llX peer=%u local=0x%08X",
           result,
           static_cast<unsigned long long>(sessionId),
           from.port,
           channel);
    return bound;
}

/**
 * Applies the admission rules to one join request and answers it.
 * A protocol mismatch is dropped with no reply.
 * @param from Requesting endpoint.
 * @param request Decoded admission prefix.
 */
void answer_join(const state::gameplay::Endpoint& from, const wire::JoinRequest& request) noexcept {
    const std::uint64_t hostSession = endpoint::identity().onlineSessionId;
    wire::RefuseReason reason = wire::RefuseReason::notFound;
    if (wire::admit(request, hostSession, reason)) {
        // The join is the first thing on this link that names the session. A link already
        // carrying it is a retry.
        const bool bound = bind_session(from, request.sessionId);
        const bool published =
            bound && group::publish_membership(from, request.joinId, request.sessionId);
        // The peer needs both before it finishes: the snapshot names it, and the parameter update
        // releases the latch its own tick waits on.
        const bool parameters = bound && group::publish_join_parameters(request.sessionId);
        // Nothing else names what the peer thinks it is joining.
        report(core::log::Level::info,
               "ev=gameplay stage=join result=admit build=%u..%u exe=%u session=0x%016llX "
               "host=0x%016llX join=0x%016llX membership=%s parameters=%s",
               request.minimumBuild,
               request.maximumBuild,
               static_cast<unsigned>(request.executableType),
               static_cast<unsigned long long>(request.sessionId),
               static_cast<unsigned long long>(hostSession),
               static_cast<unsigned long long>(request.joinId),
               published ? "queued" : "fail",
               parameters ? "queued" : "fail");
        return;
    }
    if (!wire::answerable(request)) {
        report(core::log::Level::warn,
               "ev=gameplay stage=join result=drop reason=protocol value=0x%04X",
               static_cast<unsigned>(request.protocolVersion));
        return;
    }
    report(core::log::Level::warn,
           "ev=gameplay stage=join result=refuse reason=%u build=%u..%u exe=%u session=0x%016llX "
           "host=0x%016llX",
           static_cast<unsigned>(reason),
           request.minimumBuild,
           request.maximumBuild,
           static_cast<unsigned>(request.executableType),
           static_cast<unsigned long long>(request.sessionId),
           static_cast<unsigned long long>(hostSession));
    wire::JoinRefuse refusal{};
    refusal.sessionId = request.sessionId;
    refusal.joinId = request.joinId;
    refusal.reason = reason;

    std::array<std::byte, kReplyCapacity> buffer{};
    bits::Writer writer(buffer);
    const wire::MessageHeader header{static_cast<std::uint8_t>(wire::JoinId::refuse),
                                     wire::kJoinRefuseSize};
    std::size_t size = 0;
    if (!wire::open_container(writer) || !wire::write_header(writer, header)
        || !wire::write_join_refuse(writer, refusal) || !wire::close_container(writer)
        || !writer.finish(size) || !send_transport(from, {buffer.data(), size})) {
        report(core::log::Level::warn, "ev=gameplay stage=join result=fail reason=send");
        return;
    }
    report(core::log::Level::info,
           "ev=gameplay stage=join result=refuse reason=%u",
           static_cast<unsigned>(refusal.reason));
}

/**
 * Consumes one out-of-band message container.
 * @param from Peer endpoint.
 * @param payload Whole decrypted payload.
 * @param now Monotonic tick count.
 */
void consume_container(const state::gameplay::Endpoint& from,
                       std::span<const std::byte> payload,
                       std::uint64_t now) noexcept {
    bits::Reader reader(payload);
    if (!wire::read_marker(reader)) {
        return;
    }
    for (;;) {
        wire::MessageHeader header{};
        bool present = false;
        if (!wire::read_header(reader, header, present)) {
            report(core::log::Level::debug, "ev=gameplay stage=oob result=drop reason=header");
            return;
        }
        if (!present) {
            return;
        }
        if (header.id == static_cast<std::uint8_t>(wire::ConnectId::request)) {
            wire::ConnectRequest request{};
            if (!wire::read_request(reader, request)) {
                return;
            }
            answer_connect(from, request, now);
            continue;
        }
        if (header.id == static_cast<std::uint8_t>(wire::JoinId::request)) {
            wire::JoinRequest request{};
            if (wire::read_join_request(reader, request)) {
                answer_join(from, request);
            }
            // The rest of the request is address and player tables this host does not decode,
            // so no later message in this container can be located.
            return;
        }
        if (header.id == static_cast<std::uint8_t>(wire::ConnectId::closed)) {
            wire::ConnectEnd closed{};
            if (!wire::read_closed(reader, closed)) {
                return;
            }
            // The link goes, the sessions stay. The client rebuilds the channel and rejoins none
            // of them, so releasing their activity host sessions here strands every one.
            drop_endpoint(from);
            report(core::log::Level::info,
                   "ev=gameplay stage=peer result=closed reason=%u",
                   static_cast<unsigned>(closed.reason));
            return;
        }
        if (group::consume(from, session_for_endpoint(from), header.id, reader, now)) {
            continue;
        }
        // A message this host does not decode ends the chain: its body width is unknown, so
        // every message behind it would be read at the wrong offset.
        report(core::log::Level::debug, "ev=gameplay stage=oob result=stop id=%u", header.id);
        return;
    }
}

/**
 * Records one received packet sequence in the acknowledgement history.
 * @param peer Peer receiving the packet.
 * @param sequence Sequence the packet published.
 */
void record_sequence(state::gameplay::PeerLink& peer, std::uint16_t sequence) noexcept {
    if (!peer.ringInitialized) {
        peer.ringInitialized = true;
        peer.receiveHead = sequence;
        peer.received = {};
        return;
    }
    // Add the modulus before subtracting. A bare difference is signed and goes negative on a wrap.
    const std::uint16_t advance = static_cast<std::uint16_t>(
        (sequence + state::gameplay::kPacketSequenceModulus - peer.receiveHead)
        % state::gameplay::kPacketSequenceModulus);
    if (advance == 0 || advance >= state::gameplay::kPacketSequenceHalf) {
        // A repeat or an older packet leaves the published history alone.
        return;
    }
    std::array<bool, state::gameplay::kAckHistory> shifted{};
    for (std::size_t index = 0; index < shifted.size(); ++index) {
        // Entry `index` is the packet `index + 1` before the new head, so the old head lands at
        // `advance - 1`. Anything newer than the old head and older than this packet was skipped.
        if (index + 1 < advance) {
            continue;
        }
        if (index + 1 == advance) {
            shifted[index] = true;
            continue;
        }
        const std::size_t source = index - advance;
        shifted[index] = source < peer.received.size() && peer.received[source];
    }
    peer.received = shifted;
    peer.receiveHead = sequence;
}

/**
 * Applies one reassembled reliable message.
 * @param peer Peer that sent it, held under the lock.
 * @param message Reassembled message and its inner header.
 */
void apply_message(state::gameplay::PeerLink& peer,
                   const wire::AssembledMessage& message) noexcept {
    if (message.id == static_cast<std::uint8_t>(wire::ConnectId::establish)
        && peer.stage == state::gameplay::PeerStage::connecting) {
        // The reliable establish is what moves a connected peer past the out-of-band pair.
        peer.stage = state::gameplay::PeerStage::connected;
    }
}

/**
 * Clears the send queue once the peer acknowledges the packet that carried it.
 * @param peer Peer whose acknowledgement arrived, held under the lock.
 * @param ack Acknowledgement state the packet published.
 * @return True when this acknowledgement emptied the queue.
 */
bool apply_acknowledgement(state::gameplay::PeerLink& peer, const wire::AckState& ack) noexcept {
    if (!peer.outbound.awaitingAcknowledgement
        || !wire::acknowledgement_covers(ack, peer.outbound.sentInPacket)) {
        return false;
    }
    // The peer has the packet, so every fragment in it is delivered. The next sequence is kept
    // because message sequences continue across messages.
    for (state::gameplay::OutboundFragment& fragment : peer.outbound.fragments) {
        fragment = {};
    }
    peer.outbound.count = 0;
    peer.outbound.awaitingAcknowledgement = false;
    return true;
}

/**
 * Consumes one established packet.
 * @param from Peer endpoint.
 * @param payload Whole decrypted payload.
 * @param now Monotonic tick count.
 */
void consume_established(const state::gameplay::Endpoint& from,
                         std::span<const std::byte> payload,
                         std::uint64_t now) noexcept {
    wire::EstablishedPacket packet{};
    if (!wire::decode_established(payload, false, packet)) {
        report(core::log::Level::debug, "ev=gameplay stage=packet result=drop reason=grammar");
        return;
    }
    ExternalProbe external{};
    const bool externalReadable = probe_external(payload, packet.externalBitOffset, external);
    const std::size_t messageCapacity = packet.large.count + packet.small.count;
    std::unique_ptr<wire::AssembledMessage[]> bodies;
    if (messageCapacity != 0) {
        bodies.reset(new (std::nothrow) wire::AssembledMessage[messageCapacity]);
        if (bodies == nullptr) {
            report(core::log::Level::warn,
                   "ev=gameplay stage=packet result=drop reason=message-storage count=%zu",
                   messageCapacity);
            return;
        }
    }
    std::size_t bodyCount = 0;
    unsigned stage = 0;
    bool queueCleared = false;
    std::uint16_t clearedPacket = 0;
    std::uint64_t sessionId = 0;
    std::uint64_t twoViewProbeToken = 0;
    std::uint64_t twoViewProbeEntityToken = 0;
    std::uint64_t twoViewProbeElapsed = 0;
    std::uint16_t twoViewProbePacket = 0;
    std::uint8_t twoViewProbePacketsAfter = 0;
    bool twoViewProbeAccepted = false;
    bool twoViewProbeExpired = false;
    // The reliable window never resynchronises, so a stalled queue is only visible as a refused
    // record against the sequence it is still waiting for.
    std::size_t largeDropped = 0;
    std::uint16_t largeNext = 0;
    std::uint16_t largeFirst = 0;
    AcquireSRWLockExclusive(&g_lock);
    // One link per endpoint, so the packet's two-bit guard identifies nothing this host has to
    // resolve. Every session-scoped message names its own session instead.
    state::gameplay::PeerLink* peer = find_locked(from);
    if (peer != nullptr) {
        sessionId = sole_session_locked(*peer);
        if (packet.ack.outboundHeadPresent) {
            record_sequence(*peer, packet.ack.outboundHead);
        }
        clearedPacket = peer->outbound.sentInPacket;
        queueCleared = apply_acknowledgement(*peer, packet.ack);
        if (peer->twoViewProbeAwaitingAcknowledgement) {
            const std::uint64_t elapsed = now - peer->twoViewProbeSentAt;
            if (elapsed >= kTwoViewProbeTimeout) {
                twoViewProbeToken = peer->twoViewProbeToken;
                twoViewProbeEntityToken =
                    peer->twoViewProbeScheduler.views[kTwoViewProbeEntityView].key;
                twoViewProbePacket = peer->twoViewProbePacket;
                twoViewProbePacketsAfter = peer->twoViewProbePacketsAfter;
                twoViewProbeElapsed = elapsed;
                peer->twoViewProbeAwaitingAcknowledgement = false;
                twoViewProbeExpired = true;
            } else if (wire::acknowledgement_covers(packet.ack, peer->twoViewProbePacket)) {
                twoViewProbeToken = peer->twoViewProbeToken;
                twoViewProbeEntityToken =
                    peer->twoViewProbeScheduler.views[kTwoViewProbeEntityView].key;
                twoViewProbePacket = peer->twoViewProbePacket;
                twoViewProbePacketsAfter = peer->twoViewProbePacketsAfter;
                twoViewProbeElapsed = elapsed;
                peer->twoViewProbeAwaitingAcknowledgement = false;
                peer->twoViewProbeAccepted = true;
                twoViewProbeAccepted = true;
            }
        }
        peer->acknowledgementOwed = true;
        peer->lastTick = now;
        if (externalReadable && external.schedulerSignatureUpdate
            && external.schedulerSignatureValid && external.schedulerSignature.present) {
            peer->schedulerSignature = external.schedulerSignature;
        }
        largeDropped = wire::accept_records(packet.large, peer->large);
        largeFirst = packet.large.count == 0 ? 0 : packet.large.records[0].sequence;
        wire::accept_records(packet.small, peer->small);
        wire::AssembledMessage message{};
        while (wire::drain_message(peer->large, message)) {
            apply_message(*peer, message);
            if (bodyCount < messageCapacity) {
                bodies[bodyCount++] = message;
            }
        }
        while (wire::drain_message(peer->small, message)) {
            apply_message(*peer, message);
            if (bodyCount < messageCapacity) {
                bodies[bodyCount++] = message;
            }
        }
        largeNext = peer->large.nextSequence;
        stage = static_cast<unsigned>(peer->stage);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (peer == nullptr) {
        return;
    }
    if (twoViewProbeAccepted) {
        report(core::log::Level::info,
               "ev=gameplay stage=scheduler-two-view-probe result=transport-accepted proof=ack "
               "authority=0x%016llX/%u entity=0x%016llX/%u packet=%u "
               "ack_base=%u ack_entries=%u packets_after=%u "
               "elapsed_ms=%llu",
               static_cast<unsigned long long>(twoViewProbeToken),
               static_cast<unsigned>(kTwoViewProbeAuthorityView),
               static_cast<unsigned long long>(twoViewProbeEntityToken),
               static_cast<unsigned>(kTwoViewProbeEntityView),
               static_cast<unsigned>(twoViewProbePacket),
               static_cast<unsigned>(packet.ack.receiveHead),
               static_cast<unsigned>(packet.ack.reportedCount),
               static_cast<unsigned>(twoViewProbePacketsAfter),
               static_cast<unsigned long long>(twoViewProbeElapsed));
    } else if (twoViewProbeExpired) {
        report(core::log::Level::warn,
               "ev=gameplay stage=scheduler-two-view-probe result=unacknowledged reason=timeout "
               "authority=0x%016llX/%u entity=0x%016llX/%u packet=%u packets_after=%u "
               "elapsed_ms=%llu",
               static_cast<unsigned long long>(twoViewProbeToken),
               static_cast<unsigned>(kTwoViewProbeAuthorityView),
               static_cast<unsigned long long>(twoViewProbeEntityToken),
               static_cast<unsigned>(kTwoViewProbeEntityView),
               static_cast<unsigned>(twoViewProbePacket),
               static_cast<unsigned>(twoViewProbePacketsAfter),
               static_cast<unsigned long long>(twoViewProbeElapsed));
    }
    if (externalReadable) {
        const bool viewAccepted = sessionId != 0 && group::view_accepted(sessionId);
        // The scheduler boundary is proven. Retain its large diagnostic body only when the
        // signature changes instead of synchronously writing it for every simulation packet.
        if (external.schedulerSignatureUpdate
            && (viewAccepted || external.status.gatekeeperEnabled
                || external.status.schedulerPresent)) {
            report(core::log::Level::debug,
                   "ev=gameplay stage=external-probe view=%u offset=%zu tail=%zu gate=%u "
                   "scheduler=%u words=%u widths=%u,%u,%u,%u "
                   "data=%016llX,%016llX,%016llX,%016llX remain=%zu",
                   viewAccepted ? 1U : 0U,
                   packet.externalBitOffset,
                   external.schedulerBitsBeforeProbe,
                   external.status.gatekeeperEnabled ? 1U : 0U,
                   external.status.schedulerPresent ? 1U : 0U,
                   static_cast<unsigned>(external.wordCount),
                   static_cast<unsigned>(external.widths[0]),
                   static_cast<unsigned>(external.widths[1]),
                   static_cast<unsigned>(external.widths[2]),
                   static_cast<unsigned>(external.widths[3]),
                   static_cast<unsigned long long>(external.words[0]),
                   static_cast<unsigned long long>(external.words[1]),
                   static_cast<unsigned long long>(external.words[2]),
                   static_cast<unsigned long long>(external.words[3]),
                   external.schedulerBitsAfterProbe);
        }
        if (external.schedulerSignatureUpdate && viewAccepted && external.status.schedulerPresent) {
            std::array<char, kExternalProbeByteCapacity * 2 + 1> schedulerHex{};
            scheduler_hex(std::span<const std::byte>{external.schedulerBytes}.first(
                              external.schedulerByteCount),
                          schedulerHex);
            report(core::log::Level::info,
                   "ev=gameplay stage=scheduler-body bits=%zu captured=%zu bytes=%zu "
                   "tail=%u truncated=%u hex=%s",
                   external.schedulerBitsBeforeProbe,
                   external.schedulerCapturedBits,
                   external.schedulerByteCount,
                   static_cast<unsigned>(external.schedulerTailBits),
                   external.schedulerCapturedBits < external.schedulerBitsBeforeProbe ? 1U : 0U,
                   schedulerHex.data());
        }
        if (external.status.schedulerPresent && external.schedulerSignatureUpdate) {
            std::uint64_t signatureFirst = 0;
            std::uint64_t signatureSecond = 0;
            std::memcpy(
                &signatureFirst, external.schedulerSignature.value.data(), sizeof signatureFirst);
            std::memcpy(&signatureSecond,
                        external.schedulerSignature.value.data() + sizeof signatureFirst,
                        sizeof signatureSecond);
            report(core::log::Level::info,
                   "ev=gameplay stage=scheduler-signature valid=%u bits=%zu "
                   "value=%016llX%016llX count=%u "
                   "e0=0x%016llX/%u e1=0x%016llX/%u e2=0x%016llX/%u",
                   external.schedulerSignatureValid ? 1U : 0U,
                   external.schedulerSignatureBits,
                   static_cast<unsigned long long>(signatureFirst),
                   static_cast<unsigned long long>(signatureSecond),
                   static_cast<unsigned>(external.schedulerSignature.viewCount),
                   static_cast<unsigned long long>(external.schedulerSignature.views[0].key),
                   static_cast<unsigned>(external.schedulerSignature.views[0].tag),
                   static_cast<unsigned long long>(external.schedulerSignature.views[1].key),
                   static_cast<unsigned>(external.schedulerSignature.views[1].tag),
                   static_cast<unsigned long long>(external.schedulerSignature.views[2].key),
                   static_cast<unsigned>(external.schedulerSignature.views[2].tag));
        }
    }
    for (std::size_t index = 0; index < bodyCount; ++index) {
        const wire::AssembledMessage& body = bodies[index];
        report(core::log::Level::info,
               "ev=gameplay stage=message result=ok id=%u peerstage=%u",
               static_cast<unsigned>(body.id),
               stage);
        // The connect establish belongs to this layer and apply_message already took it, so
        // handing it to the group layer would only report it as undecoded on every connection.
        if (body.id == static_cast<std::uint8_t>(wire::ConnectId::establish)) {
            continue;
        }
        // Group handling runs outside the lock because answering takes it again.
        bits::Reader reader({body.bytes.data(), state::gameplay::kReassemblyCapacity});
        if (reader.skip(body.bodyBitOffset)
            && !group::consume(from, sessionId, body.id, reader, now)) {
            report(core::log::Level::debug,
                   "ev=gameplay stage=message result=undecoded id=%u",
                   static_cast<unsigned>(body.id));
        }
    }
    if (queueCleared) {
        report(core::log::Level::info,
               "ev=gameplay stage=sendqueue result=cleared packet=%u base=%u entries=%u",
               static_cast<unsigned>(clearedPacket),
               static_cast<unsigned>(packet.ack.receiveHead),
               static_cast<unsigned>(packet.ack.reportedCount));
    }
    report(core::log::Level::debug,
           "ev=gameplay stage=packet result=ok seq=%u base=%u entries=%u large=%u small=%u "
           "first=%u next=%u drop=%zu",
           static_cast<unsigned>(packet.ack.outboundHead),
           static_cast<unsigned>(packet.ack.receiveHead),
           static_cast<unsigned>(packet.ack.reportedCount),
           static_cast<unsigned>(packet.large.count),
           static_cast<unsigned>(packet.small.count),
           static_cast<unsigned>(largeFirst),
           static_cast<unsigned>(largeNext),
           largeDropped);
}

/**
 * Builds and sends one acknowledgement-only packet.
 * @param peer Peer state copied under the lock before the send.
 * @param entityCreate Optional guarded entity body in a proven one- or two-view layout.
 * @param twoViewProbe Optional one-shot two-view validation carrying entityCreate.
 * @return True when the packet left the endpoint.
 */
[[nodiscard]] bool send_acknowledgement(const state::gameplay::PeerLink& peer,
                                        const EntityCreatePlan& entityCreate,
                                        const TwoViewProbePlan& twoViewProbe) noexcept {
    wire::AckState ack{};
    ack.outboundHead = peer.outboundHead;
    ack.outboundHeadPresent = peer.outboundHeadPresent;
    // The peer subtracts this from the decoded sequence to place its receive window. A zero
    // collapses that window and the peer discards every packet.
    ack.headMinusCursor = kMinimumHeadCursor;
    ack.receiveHead = peer.receiveHead;
    ack.ringInitialized = peer.ringInitialized;
    ack.received = peer.received;
    // No round trip is timed, so the delay field carries its sentinel.
    ack.delay = kDelaySentinel;

    std::array<std::byte, kReplyCapacity> buffer{};
    bits::Writer writer(buffer);
    const auto guard = static_cast<std::uint8_t>(peer.localConnectionSequence % kSequenceGuardBase);
    // One-view packets remain accepted after signature convergence and after a successful create.
    // In both captured two-view transitions the client applied the remote signature, then marked
    // that packet and every repeated scheduler packet corrupt until its four-second timeout. Keep
    // ordinary transport acknowledgements healthy while the multi-view handler tail is unmapped.
    const SelectedReplicationView selected = select_replication_view(peer);
    const bool viewPresent = selected.present && selected.signature.token != 0;
    if (twoViewProbe.present) {
        TwoViewProbePlan verified{};
        EntityCreatePlan verifiedEntity{};
        EntityCreateGate ignoredGate = EntityCreateGate::view;
        if (!entityCreate.present || !prepare_two_view_probe(peer, selected, verified)
            || !prepare_entity_create_with_two_view_probe(
                peer, selected, verified, &entityCreate, verifiedEntity, ignoredGate)
            || verified.token != twoViewProbe.token
            || verified.entityToken != twoViewProbe.entityToken
            || verified.selectedView != twoViewProbe.selectedView
            || verified.entityView != twoViewProbe.entityView
            || verified.scheduler.value != twoViewProbe.scheduler.value
            || verified.scheduler.wire != twoViewProbe.scheduler.wire
            || verified.scheduler.wireBits != twoViewProbe.scheduler.wireBits
            || verified.scheduler.viewCount != twoViewProbe.scheduler.viewCount
            || verified.scheduler.views[0].key != twoViewProbe.scheduler.views[0].key
            || verified.scheduler.views[0].tag != twoViewProbe.scheduler.views[0].tag
            || verified.scheduler.views[1].key != twoViewProbe.scheduler.views[1].key
            || verified.scheduler.views[1].tag != twoViewProbe.scheduler.views[1].tag) {
            return false;
        }
    }
    const state::gameplay::SchedulerSignature& scheduler = peer.schedulerSignature;
    const bool oneViewScheduler = scheduler.present
                                  && scheduler.viewCount == kProvenSchedulerViewCount
                                  && scheduler.wireBits == kProvenSchedulerWireBits;
    const bool twoViewEntityScheduler =
        !twoViewProbe.present && entityCreate.present && scheduler.present
        && scheduler.viewCount == kTwoViewProbeViewCount
        && scheduler.wireBits == kTwoViewProbeWireBits && two_view_layout_ready(peer, selected);
    if (twoViewEntityScheduler) {
        if (!selected.capturePresent || entityCreate.token != selected.signature.token
            || entityCreate.viewIndex >= scheduler.viewCount
            || scheduler.views[entityCreate.viewIndex].key != entityCreate.schedulerKey
            || scheduler.views[entityCreate.viewIndex].tag != entityCreate.schedulerTag) {
            return false;
        }
    }
    if (entityCreate.present && !twoViewProbe.present && !oneViewScheduler
        && !twoViewEntityScheduler) {
        return false;
    }
    // Once an entity create has been attempted, its native decoder may retain a pending body
    // while the requested RSAT loads. Repeating an otherwise empty scheduler body on every
    // acknowledgement makes each of those packets enter the pending decoder again and the
    // channel eventually classifies the stream as corrupt. Keep ordinary acknowledgements free
    // of scheduler data between bounded attempts; an actual retry carries the exact cached
    // one-view layout through entityCreate.present.
    const bool schedulerWanted =
        entityCreate.present || (peer.entityCreateAttempts == 0 && !peer.twoViewProbeAttempted);
    const bool schedulerBodyPresent = !twoViewProbe.present && schedulerWanted && viewPresent
                                      && (oneViewScheduler || twoViewEntityScheduler);
    const bool schedulerPresent = schedulerBodyPresent || twoViewProbe.present;
    std::size_t size = 0;
    // Only the 32-byte queue carries this host's messages; the 6-byte queue stays empty.
    if (!wire::write_head_and_ack(writer, guard, ack) || !wire::write_queue(writer, peer.outbound)
        || !wire::write_empty_queue(writer)
        || !wire::write_external_status(writer, viewPresent, schedulerPresent)
        || (twoViewProbe.present && !write_two_view_probe(writer, twoViewProbe, entityCreate))
        || (schedulerBodyPresent && !write_scheduler(writer, scheduler, entityCreate))
        || !writer.finish(size)) {
        return false;
    }
    const bool traceTwoView = twoViewProbe.present || twoViewEntityScheduler;
    if (traceTwoView) {
        client::hooks::network::scheduler_handler_probe::arm(kTwoViewProbeViewCount);
    }
    // Publish the plan before loopback transport can synchronously enter the client decoder.
    if (entityCreate.present && !entityCreate.updateOnly) {
        client::hooks::network::sobject_bind_probe::record_plan(entityCreate.token,
                                                                entityCreate.namespaceId,
                                                                entityCreate.viewIndex,
                                                                entityCreate.slot,
                                                                entityCreate.rsat,
                                                                entityCreate.spatialRegion,
                                                                entityCreate.spatialBubble,
                                                                entityCreate.spatialCell,
                                                                peer.entityCreateAttempts,
                                                                false);
    }
    const bool sent = send_transport(peer.endpoint, {buffer.data(), size});
    if (traceTwoView && !sent) {
        client::hooks::network::scheduler_handler_probe::cancel();
    }
    return sent;
}

} // namespace

/** Consumes one decrypted transport payload. */
void deliver(const state::gameplay::Endpoint& from,
             std::span<const std::byte> payload,
             std::uint64_t now) noexcept {
    if (payload.empty()) {
        return;
    }
    if ((std::to_integer<unsigned>(payload[0]) >> kMarkerShift) != 0) {
        consume_container(from, payload, now);
        return;
    }
    // The second bit selects the native fragment header. Each piece carries a byte-aligned slice
    // of the original unfragmented packet, so only the completed body reaches the normal decoder.
    if (((std::to_integer<unsigned>(payload[0]) >> (kMarkerShift - 1U)) & 1U) != 0) {
        std::array<std::byte, wire::kFragmentedPacketCapacity> assembled{};
        std::size_t assembledSize = 0;
        wire::PacketFragment fragment{};
        const FragmentResult result =
            assemble_fragment(from, payload, now, assembled, assembledSize, fragment);
        report(result == FragmentResult::invalid ? core::log::Level::warn : core::log::Level::debug,
               "ev=gameplay stage=fragment result=%s set=%u guard=%u index=%u count=%u "
               "bytes=%zu assembled=%zu",
               result == FragmentResult::complete
                   ? "complete"
                   : (result == FragmentResult::held ? "held" : "invalid"),
               static_cast<unsigned>(fragment.setId),
               static_cast<unsigned>(fragment.connectionSequenceLow2),
               static_cast<unsigned>(fragment.index),
               static_cast<unsigned>(fragment.count),
               fragment.body.size(),
               assembledSize);
        if (result == FragmentResult::complete) {
            consume_established(from, {assembled.data(), assembledSize}, now);
        }
        return;
    }
    consume_established(from, payload, now);
}

/** Sends one already-encoded out-of-band body in its own container. */
bool send_container(const state::gameplay::Endpoint& to,
                    std::uint8_t id,
                    std::uint32_t declaredSize,
                    std::span<const std::byte> body,
                    std::size_t bodyBits) noexcept {
    std::array<std::byte, kReplyCapacity> buffer{};
    bits::Writer writer(buffer);
    const wire::MessageHeader header{id, declaredSize};
    if (!wire::open_container(writer) || !wire::write_header(writer, header)) {
        return false;
    }
    bits::Reader reader(body);
    std::size_t remaining = bodyBits;
    while (remaining != 0) {
        const auto width = static_cast<std::uint8_t>(remaining < kByteBits ? remaining : kByteBits);
        std::uint64_t value = 0;
        if (!reader.read(width, value) || !writer.write(value, width)) {
            return false;
        }
        remaining -= width;
    }
    std::size_t size = 0;
    if (!wire::close_container(writer) || !writer.finish(size)) {
        return false;
    }
    return send_transport(to, {buffer.data(), size});
}

/** Queues one reliable message for a peer. */
bool enqueue_reliable(std::uint64_t sessionId,
                      std::uint8_t id,
                      std::uint32_t declaredSize,
                      std::span<const std::byte> body,
                      std::size_t bodyBits) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    state::gameplay::PeerLink* peer = find_session_locked(sessionId);
    const bool queued =
        peer != nullptr && wire::enqueue_message(peer->outbound, id, declaredSize, body, bodyBits);
    if (queued) {
        // The next service slice carries it, so the acknowledgement path also flushes sends.
        peer->acknowledgementOwed = true;
        // The queue changed, so the packet it was stamped against no longer carries all of it.
        peer->outbound.awaitingAcknowledgement = false;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return queued;
}

/** Reports the NetAddr one peer sent in its own connect request. */
bool remote_address(std::uint64_t sessionId,
                    std::array<std::byte, state::gameplay::kNetAddrBlobSize>& output) noexcept {
    AcquireSRWLockShared(&g_lock);
    const state::gameplay::PeerLink* peer = find_session_locked(sessionId);
    const bool present = peer != nullptr && peer->remoteAddressPresent;
    if (present) {
        output = peer->remoteAddress;
    }
    ReleaseSRWLockShared(&g_lock);
    return present;
}

/** Clears the retry state when the selected per-session replication view changes. */
static void reset_entity_create(state::gameplay::PeerLink& peer) noexcept {
    peer.entityCreateScheduler = {};
    peer.entityCreateToken = 0;
    peer.entityCreateSlot = 0;
    peer.entityCreateHandleGeneration = 0;
    peer.entityCreateReadyToken = 0;
    peer.entityCreateReadySlot = 0;
    peer.entityCreateReadyHandleGeneration = 0;
    peer.entityCreateReadyBootstrap = false;
    peer.entityCreateReadySince = 0;
    peer.entityCreateAttempts = 0;
    peer.lastEntityCreate = 0;
    peer.entityCreateAccepted = false;
    peer.entityFollowupSent = false;
    peer.entityCreateAcceptedSince = 0;
    peer.entityCreateGate = 0xFF;
}

/** Publishes one session's provisional or bound view signature. */
void bind_view(std::uint64_t sessionId, const state::gameplay::ViewSignature& signature) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    state::gameplay::PeerLink* peer = find_session_locked(sessionId);
    if (peer != nullptr) {
        const std::uint64_t before = select_replication_view(*peer).signature.token;
        for (std::size_t index = 0; index < peer->sessions.size(); ++index) {
            if (peer->sessions[index] == sessionId) {
                peer->views[index] = signature;
                break;
            }
        }
        const std::uint64_t after = select_replication_view(*peer).signature.token;
        if (before != after) {
            reset_entity_create(*peer);
        } else {
            peer->entityCreateGate = 0xFF;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Clears one session's provisional or bound replication view. */
void clear_view(std::uint64_t sessionId) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    state::gameplay::PeerLink* peer = find_session_locked(sessionId);
    if (peer != nullptr) {
        const std::uint64_t before = select_replication_view(*peer).signature.token;
        std::uint64_t removed = 0;
        for (std::size_t index = 0; index < peer->sessions.size(); ++index) {
            if (peer->sessions[index] == sessionId) {
                removed = peer->views[index].token;
                peer->views[index] = {};
                break;
            }
        }
        const std::uint64_t after = select_replication_view(*peer).signature.token;
        if (before != after || (removed != 0 && peer->entityCreateToken == removed)) {
            reset_entity_create(*peer);
        } else {
            peer->entityCreateGate = 0xFF;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Reports whether a view signature is bound. */
bool view_bound(std::uint64_t sessionId) noexcept {
    AcquireSRWLockShared(&g_lock);
    const state::gameplay::PeerLink* peer = find_session_locked(sessionId);
    bool bound = false;
    if (peer != nullptr) {
        for (std::size_t index = 0; index < peer->sessions.size(); ++index) {
            if (peer->sessions[index] == sessionId) {
                bound = peer->views[index].bound;
                break;
            }
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return bound;
}

/** Reports how far the link carrying one group session has got. */
bool link_stage(std::uint64_t sessionId, state::gameplay::PeerStage& stage) noexcept {
    stage = state::gameplay::PeerStage::absent;
    AcquireSRWLockShared(&g_lock);
    const state::gameplay::PeerLink* peer = find_session_locked(sessionId);
    const bool present = peer != nullptr;
    if (present) {
        stage = peer->stage;
    }
    ReleaseSRWLockShared(&g_lock);
    return present;
}

/** Sends any owed acknowledgement. */
void service(std::uint64_t now) noexcept {
    std::array<state::gameplay::PeerLink, state::gameplay::kAssociationCapacity> owed{};
    std::array<EntityCreatePlan, state::gameplay::kAssociationCapacity> entityCreates{};
    std::array<TwoViewProbePlan, state::gameplay::kAssociationCapacity> twoViewProbes{};
    std::array<EntityCreateGateReport, state::gameplay::kAssociationCapacity> gateReports{};
    std::array<TwoViewProbeReport, state::gameplay::kAssociationCapacity> twoViewProbeReports{};
    std::size_t count = 0;
    std::size_t gateReportCount = 0;
    std::size_t twoViewProbeReportCount = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (state::gameplay::PeerLink& peer : g_peers) {
        if (peer.stage == state::gameplay::PeerStage::absent) {
            continue;
        }
        if (peer.twoViewProbeAwaitingAcknowledgement) {
            const std::uint64_t elapsed = now - peer.twoViewProbeSentAt;
            const bool timedOut = elapsed >= kTwoViewProbeTimeout;
            const bool packetBoundReached =
                peer.twoViewProbePacketsAfter >= kTwoViewProbeMaximumPacketsAfter;
            if ((timedOut || packetBoundReached)
                && twoViewProbeReportCount < twoViewProbeReports.size()) {
                TwoViewProbeReport& probe = twoViewProbeReports[twoViewProbeReportCount++];
                probe.token = peer.twoViewProbeToken;
                probe.entityToken = peer.twoViewProbeScheduler.views[kTwoViewProbeEntityView].key;
                probe.packet = peer.twoViewProbePacket;
                probe.packetsAfter = peer.twoViewProbePacketsAfter;
                probe.elapsed = elapsed;
                probe.kind = TwoViewProbeReport::Kind::unacknowledged;
                probe.reason = timedOut ? "timeout" : "ack-window";
                peer.twoViewProbeAwaitingAcknowledgement = false;
            } else if (!peer.twoViewProbeMutationReported) {
                client::hooks::network::entity_slot_probe::ViewCapture capture{};
                if (client::hooks::network::entity_slot_probe::find(peer.twoViewProbeToken, capture)
                    && scheduler_matches_remote_capture(peer.twoViewProbeScheduler, capture)
                    && twoViewProbeReportCount < twoViewProbeReports.size()) {
                    TwoViewProbeReport& probe = twoViewProbeReports[twoViewProbeReportCount++];
                    probe.token = peer.twoViewProbeToken;
                    probe.entityToken =
                        peer.twoViewProbeScheduler.views[kTwoViewProbeEntityView].key;
                    probe.packet = peer.twoViewProbePacket;
                    probe.packetsAfter = peer.twoViewProbePacketsAfter;
                    probe.elapsed = elapsed;
                    probe.remoteViews = capture.schedulerRemoteViewCount;
                    probe.kind = TwoViewProbeReport::Kind::remoteMutation;
                    peer.twoViewProbeMutationReported = true;
                }
            }
        }
        // A settled native slot may become ready after the last packet was acknowledged. Poll the
        // first guarded create directly so an idle zone does not need movement traffic to wake it.
        EntityCreatePlan candidate{};
        const SelectedReplicationView selected = select_replication_view(peer);
        const bool firstAttempt = peer.entityCreateAttempts == 0;
        if (!peer.entityCreateAccepted && entity_create_accepted(peer)) {
            peer.entityCreateAccepted = true;
            peer.entityCreateAcceptedSince = now;
        }
        const bool controlQueueSettled =
            peer.outbound.count == 0 && !peer.outbound.awaitingAcknowledgement;
        if (firstAttempt) {
            (void)synchronise_scheduler_layout(peer, selected);
        }
        const bool validatedTwoView = two_view_layout_ready(peer, selected);
        TwoViewProbePlan twoViewProbe{};
        const bool twoViewProbeReady = !peer.twoViewProbeAttempted && firstAttempt
                                       && !peer.entityCreateAccepted
                                       && peer.stage == state::gameplay::PeerStage::connected
                                       && prepare_two_view_probe(peer, selected, twoViewProbe);
        bool prepared = false;
        EntityCreateGate gate = firstAttempt ? EntityCreateGate::view : EntityCreateGate::attempted;
        if (firstAttempt) {
            // Reliable membership, join, and view records establish the topology the scheduler
            // describes. Never let an entity body overtake one, but allow an otherwise identical
            // candidate to accumulate its settle age while the record is acknowledged. Initial
            // EDZ control bursts recur faster than the conservative settle interval; resetting
            // the timer for each burst makes a stationary create impossible even though both
            // native scheduler layouts, the token, and the slot remain unchanged.
            bool candidatePrepared = false;
            if (twoViewProbeReady) {
                if (!controlQueueSettled) {
                    gate = EntityCreateGate::controlQueue;
                } else {
                    candidatePrepared = prepare_entity_create_with_two_view_probe(
                        peer, selected, twoViewProbe, nullptr, candidate, gate);
                }
            } else {
                candidatePrepared =
                    validatedTwoView
                        ? prepare_entity_create_after_two_view(peer, selected, candidate, gate)
                        : prepare_entity_create(peer, selected, candidate, gate);
            }
            if (!candidatePrepared) {
                peer.entityCreateReadyToken = 0;
                peer.entityCreateReadySlot = 0;
                peer.entityCreateReadyHandleGeneration = 0;
                peer.entityCreateReadyBootstrap = false;
                peer.entityCreateReadySince = 0;
            } else {
                if (peer.entityCreateReadyToken != candidate.token
                    || peer.entityCreateReadySlot != candidate.slot
                    || peer.entityCreateReadyHandleGeneration != candidate.handleGeneration
                    || peer.entityCreateReadyBootstrap != candidate.bootstrapScheduler) {
                    peer.entityCreateReadyToken = candidate.token;
                    peer.entityCreateReadySlot = candidate.slot;
                    peer.entityCreateReadyHandleGeneration = candidate.handleGeneration;
                    peer.entityCreateReadyBootstrap = candidate.bootstrapScheduler;
                    peer.entityCreateReadySince = now;
                }
                if (!controlQueueSettled) {
                    gate = EntityCreateGate::controlQueue;
                } else if (twoViewProbeReady || validatedTwoView) {
                    // The transition window is short. Send its first proven signature with the
                    // fully guarded atomic create instead of adding the one-view settle age.
                    prepared = true;
                } else if (candidate.bootstrapScheduler
                           || now - peer.entityCreateReadySince < kEntityCreateReadyInterval) {
                    gate = EntityCreateGate::settling;
                } else {
                    prepared = true;
                }
            }
        }
        const bool twoViewProbeDue = twoViewProbeReady && prepared;
        const auto gateValue = static_cast<std::uint8_t>(gate);
        if (peer.entityCreateGate != gateValue && gateReportCount < gateReports.size()) {
            peer.entityCreateGate = gateValue;
            gateReports[gateReportCount++] =
                capture_entity_create_gate(peer, selected, twoViewProbe, candidate, gate, now);
        }
        // An unacknowledged send queue keeps the packet going out until the peer confirms it.
        // Every packet burns one sequence, so the resend is paced.
        const bool resendDue = peer.outbound.count != 0 && now - peer.lastSend >= kResendInterval;
        const bool entityRetryDue =
            !peer.entityCreateAccepted && peer.entityCreateAttempts != 0
            && peer.entityCreateAttempts < kEntityCreateAttemptLimit
            && peer.entityCreateScheduler.viewCount == kProvenSchedulerViewCount
            && now - peer.lastEntityCreate >= kEntityCreateRetryInterval;
        const bool entityFirstDue = firstAttempt && prepared && !twoViewProbeDue;
        const bool twoViewUpdateReady =
            peer.entityCreateAttempts != 0
            && peer.entityCreateScheduler.viewCount == kTwoViewProbeViewCount
            && !peer.entityFollowupSent && controlQueueSettled
            && two_view_layout_ready(peer, selected);
        const bool entityFollowupReady =
            twoViewUpdateReady
            || (peer.entityCreateAccepted && !peer.entityFollowupSent && controlQueueSettled
                && now - peer.entityCreateAcceptedSince >= kEntityFollowupReadyInterval);
        const bool entityFollowupDue =
            entityFollowupReady
            && (twoViewUpdateReady ? prepare_entity_update_after_two_view(peer, selected, candidate)
                                   : prepare_entity_combined_create(peer, selected, candidate));
        const bool due = peer.acknowledgementOwed || resendDue || entityFirstDue || entityRetryDue
                         || entityFollowupDue || twoViewProbeDue;
        if (!due) {
            continue;
        }
        peer.acknowledgementOwed = false;
        peer.lastSend = now;
        // Only the first send of the current contents is stamped. A resend carries the same
        // fragments, so re-stamping would move the target past what the peer can acknowledge.
        if (peer.outbound.count != 0 && !peer.outbound.awaitingAcknowledgement) {
            peer.outbound.sentInPacket =
                static_cast<std::uint16_t>((peer.outboundHead + 1) % kPacketSequenceModulus);
            peer.outbound.awaitingAcknowledgement = true;
        }
        // The packet sequence advances here so the copy carries the value it will publish.
        peer.outboundHead =
            static_cast<std::uint16_t>((peer.outboundHead + 1) % kPacketSequenceModulus);
        peer.outboundHeadPresent = true;
        peer.lastTick = now;
        if (peer.twoViewProbeAwaitingAcknowledgement && !twoViewProbeDue) {
            ++peer.twoViewProbePacketsAfter;
        }
        if (entityRetryDue) {
            prepared = prepare_entity_retry(peer, selected, candidate);
        }
        const bool sameAttempt = entityRetryDue && prepared
                                 && candidate.token == peer.entityCreateToken
                                 && candidate.slot == peer.entityCreateSlot
                                 && candidate.handleGeneration == peer.entityCreateHandleGeneration;
        if (entityFollowupDue) {
            peer.entityFollowupSent = true;
            entityCreates[count] = candidate;
        } else if (twoViewProbeDue || entityFirstDue || sameAttempt) {
            if (firstAttempt) {
                peer.entityCreateScheduler =
                    twoViewProbeDue
                        ? twoViewProbe.scheduler
                        : (validatedTwoView ? peer.twoViewProbeScheduler : peer.schedulerSignature);
                peer.entityCreateToken = candidate.token;
                peer.entityCreateSlot = candidate.slot;
                peer.entityCreateHandleGeneration = candidate.handleGeneration;
                if (candidate.combinedCreate) {
                    peer.entityFollowupSent = true;
                }
            }
            ++peer.entityCreateAttempts;
            peer.lastEntityCreate = now;
            entityCreates[count] = candidate;
        } else if (entityRetryDue) {
            // A changed first candidate means the selected slot was consumed or recycled.
            peer.entityCreateAttempts = kEntityCreateAttemptLimit;
        }
        if (twoViewProbeDue) {
            peer.twoViewProbeScheduler = twoViewProbe.scheduler;
            peer.twoViewProbeToken = twoViewProbe.token;
            peer.twoViewProbeSentAt = now;
            peer.twoViewProbePacket = peer.outboundHead;
            peer.twoViewProbePacketsAfter = 0;
            peer.twoViewProbeAttempted = true;
            peer.twoViewProbeAwaitingAcknowledgement = true;
            peer.twoViewProbeAccepted = false;
            peer.twoViewProbeMutationReported = twoViewProbe.remoteAlreadyMatches;
            twoViewProbes[count] = twoViewProbe;
        }
        owed[count] = peer;
        if (entityCreates[count].present) {
            owed[count].schedulerSignature = peer.entityCreateScheduler;
        }
        ++count;
    }
    ReleaseSRWLockExclusive(&g_lock);
    for (std::size_t index = 0; index < twoViewProbeReportCount; ++index) {
        const TwoViewProbeReport& probe = twoViewProbeReports[index];
        if (probe.kind == TwoViewProbeReport::Kind::remoteMutation) {
            report(core::log::Level::info,
                   "ev=gameplay stage=scheduler-two-view-probe result=remote-mutated proof=none "
                   "authority=0x%016llX/%u entity=0x%016llX/%u packet=%u remote=%u "
                   "packets_after=%u elapsed_ms=%llu",
                   static_cast<unsigned long long>(probe.token),
                   static_cast<unsigned>(kTwoViewProbeAuthorityView),
                   static_cast<unsigned long long>(probe.entityToken),
                   static_cast<unsigned>(kTwoViewProbeEntityView),
                   static_cast<unsigned>(probe.packet),
                   static_cast<unsigned>(probe.remoteViews),
                   static_cast<unsigned>(probe.packetsAfter),
                   static_cast<unsigned long long>(probe.elapsed));
            continue;
        }
        report(core::log::Level::warn,
               "ev=gameplay stage=scheduler-two-view-probe result=unacknowledged reason=%s "
               "authority=0x%016llX/%u entity=0x%016llX/%u packet=%u packets_after=%u "
               "elapsed_ms=%llu",
               probe.reason,
               static_cast<unsigned long long>(probe.token),
               static_cast<unsigned>(kTwoViewProbeAuthorityView),
               static_cast<unsigned long long>(probe.entityToken),
               static_cast<unsigned>(kTwoViewProbeEntityView),
               static_cast<unsigned>(probe.packet),
               static_cast<unsigned>(probe.packetsAfter),
               static_cast<unsigned long long>(probe.elapsed));
    }
    for (std::size_t index = 0; index < gateReportCount; ++index) {
        const EntityCreateGateReport& gate = gateReports[index];
        report(gate.gate == EntityCreateGate::ready ? core::log::Level::info
                                                    : core::log::Level::debug,
               "ev=gameplay stage=entity-create-gate result=%s authority=0x%016llX/%u "
               "entity=0x%016llX/%u "
               "queue=%zu/%u signature=%u/%u capture=%u candidate=%u namespace=%d "
               "occupied=%u occupied_low=0x%08X available=%u slot=%u gen=%u/%u/%u "
               "local=%u/%u remote=%u/%u spatial=%u region=%d bubble=%u cell=%u "
               "ready_ms=%llu attempts=%u",
               entity_create_gate_name(gate.gate),
               static_cast<unsigned long long>(gate.token),
               static_cast<unsigned>(gate.authorityView),
               static_cast<unsigned long long>(gate.entityToken),
               static_cast<unsigned>(gate.entityView),
               gate.queueCount,
               gate.awaitingAcknowledgement ? 1U : 0U,
               static_cast<unsigned>(gate.signatureViews),
               static_cast<unsigned>(gate.wireBits),
               gate.capturePresent ? 1U : 0U,
               gate.candidatePresent ? 1U : 0U,
               gate.namespaceId,
               gate.occupied,
               gate.occupiedLow,
               gate.available,
               static_cast<unsigned>(gate.slot),
               static_cast<unsigned>(gate.handleGeneration),
               static_cast<unsigned>(gate.reservedGeneration),
               static_cast<unsigned>(gate.objectGeneration),
               gate.localSignatureValid ? 1U : 0U,
               static_cast<unsigned>(gate.localViews),
               gate.remoteSignatureValid ? 1U : 0U,
               static_cast<unsigned>(gate.remoteViews),
               gate.spatialCellPresent ? 1U : 0U,
               gate.spatialRegion,
               static_cast<unsigned>(gate.spatialBubble),
               static_cast<unsigned>(gate.spatialCell),
               static_cast<unsigned long long>(gate.readyAge),
               static_cast<unsigned>(gate.attempts));
    }
    for (std::size_t index = 0; index < count; ++index) {
        const bool sent =
            send_acknowledgement(owed[index], entityCreates[index], twoViewProbes[index]);
        if (twoViewProbes[index].present) {
            if (sent) {
                report(core::log::Level::info,
                       "ev=gameplay stage=scheduler-two-view-probe result=sent "
                       "authority=0x%016llX/%u entity=0x%016llX/%u packet=%u "
                       "signature_bits=%u body_bits=%u local=%u remote=%u "
                       "e0=0x%016llX/%u e1=0x%016llX/%u",
                       static_cast<unsigned long long>(twoViewProbes[index].token),
                       static_cast<unsigned>(twoViewProbes[index].selectedView),
                       static_cast<unsigned long long>(twoViewProbes[index].entityToken),
                       static_cast<unsigned>(twoViewProbes[index].entityView),
                       static_cast<unsigned>(owed[index].outboundHead),
                       static_cast<unsigned>(twoViewProbes[index].scheduler.wireBits),
                       static_cast<unsigned>(kTwoViewProbeBodyBits),
                       static_cast<unsigned>(twoViewProbes[index].scheduler.viewCount),
                       static_cast<unsigned>(twoViewProbes[index].remoteViews),
                       static_cast<unsigned long long>(twoViewProbes[index].scheduler.views[0].key),
                       static_cast<unsigned>(twoViewProbes[index].scheduler.views[0].tag),
                       static_cast<unsigned long long>(twoViewProbes[index].scheduler.views[1].key),
                       static_cast<unsigned>(twoViewProbes[index].scheduler.views[1].tag));
            } else {
                bool reportFailure = false;
                AcquireSRWLockExclusive(&g_lock);
                state::gameplay::PeerLink* const peer = find_locked(owed[index].endpoint);
                if (peer != nullptr && peer->twoViewProbeAwaitingAcknowledgement
                    && peer->twoViewProbePacket == owed[index].outboundHead) {
                    peer->twoViewProbeAwaitingAcknowledgement = false;
                    reportFailure = true;
                }
                ReleaseSRWLockExclusive(&g_lock);
                if (reportFailure) {
                    report(core::log::Level::warn,
                           "ev=gameplay stage=scheduler-two-view-probe "
                           "result=unacknowledged reason=send-fail "
                           "authority=0x%016llX/%u entity=0x%016llX/%u packet=%u "
                           "packets_after=0 elapsed_ms=0",
                           static_cast<unsigned long long>(twoViewProbes[index].token),
                           static_cast<unsigned>(twoViewProbes[index].selectedView),
                           static_cast<unsigned long long>(twoViewProbes[index].entityToken),
                           static_cast<unsigned>(twoViewProbes[index].entityView),
                           static_cast<unsigned>(owed[index].outboundHead));
                }
            }
        }
        if (entityCreates[index].present) {
            if (!entityCreates[index].updateOnly) {
                client::hooks::network::sobject_bind_probe::record_plan(
                    entityCreates[index].token,
                    entityCreates[index].namespaceId,
                    entityCreates[index].viewIndex,
                    entityCreates[index].slot,
                    entityCreates[index].rsat,
                    entityCreates[index].spatialRegion,
                    entityCreates[index].spatialBubble,
                    entityCreates[index].spatialCell,
                    owed[index].entityCreateAttempts,
                    sent);
            }
            if (entityCreates[index].updateOnly) {
                report(sent ? core::log::Level::info : core::log::Level::warn,
                       "ev=gameplay stage=entity-update-out result=%s token=0x%016llX "
                       "namespace=%d view=%u key=0x%016llX tag=%u slot=%u hgen=%u "
                       "rsat=0x%08X region=%d bubble=%u cell=%u update_bits=%u",
                       sent ? "sent" : "fail",
                       static_cast<unsigned long long>(entityCreates[index].token),
                       entityCreates[index].namespaceId,
                       static_cast<unsigned>(entityCreates[index].viewIndex),
                       static_cast<unsigned long long>(entityCreates[index].schedulerKey),
                       static_cast<unsigned>(entityCreates[index].schedulerTag),
                       static_cast<unsigned>(entityCreates[index].slot),
                       static_cast<unsigned>(entityCreates[index].handleGeneration),
                       entityCreates[index].rsat,
                       entityCreates[index].spatialRegion,
                       static_cast<unsigned>(entityCreates[index].spatialBubble),
                       static_cast<unsigned>(entityCreates[index].spatialCell),
                       static_cast<unsigned>(entityCreates[index].updateBits));
            } else {
                report(sent ? core::log::Level::info : core::log::Level::warn,
                       "ev=gameplay stage=entity-create-out result=%s token=0x%016llX "
                       "attempt=%u namespace=%d view=%u key=0x%016llX tag=%u slot=%u hgen=%u "
                       "ogen=%u rsat=0x%08X region=%d bubble=%u cell=%u update=%s update_bits=%u "
                       "bootstrap=%u combined=%u",
                       sent ? "sent" : "fail",
                       static_cast<unsigned long long>(entityCreates[index].token),
                       static_cast<unsigned>(owed[index].entityCreateAttempts),
                       entityCreates[index].namespaceId,
                       static_cast<unsigned>(entityCreates[index].viewIndex),
                       static_cast<unsigned long long>(entityCreates[index].schedulerKey),
                       static_cast<unsigned>(entityCreates[index].schedulerTag),
                       static_cast<unsigned>(entityCreates[index].slot),
                       static_cast<unsigned>(entityCreates[index].handleGeneration),
                       static_cast<unsigned>(entityCreates[index].objectGeneration),
                       entityCreates[index].rsat,
                       entityCreates[index].spatialRegion,
                       static_cast<unsigned>(entityCreates[index].spatialBubble),
                       static_cast<unsigned>(entityCreates[index].spatialCell),
                       entityCreates[index].updateBits == 0 ? "none" : "inline",
                       static_cast<unsigned>(entityCreates[index].updateBits),
                       entityCreates[index].bootstrapScheduler ? 1U : 0U,
                       entityCreates[index].combinedCreate ? 1U : 0U);
            }
        }
        if (!sent) {
            report(core::log::Level::debug, "ev=gameplay stage=ack result=fail");
        }
    }
}

/** Drops one group session, leaving the link and its other sessions alone. */
void drop(std::uint64_t sessionId) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    state::gameplay::PeerLink* const peer = find_session_locked(sessionId);
    if (peer != nullptr) {
        // The channel outlives the session. A leave names one region, and the client keeps playing
        // the other over the same channel.
        const std::uint64_t selected = select_replication_view(*peer).signature.token;
        for (std::size_t index = 0; index < peer->sessions.size(); ++index) {
            if (peer->sessions[index] == sessionId) {
                const std::uint64_t removed = peer->views[index].token;
                peer->sessions[index] = 0;
                peer->views[index] = {};
                if (removed == selected || removed == peer->entityCreateToken) {
                    reset_entity_create(*peer);
                } else {
                    peer->entityCreateGate = 0xFF;
                }
                break;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Drops every link at one endpoint, which is what a connect-closed names. */
void drop_endpoint(const state::gameplay::Endpoint& endpoint) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    for (state::gameplay::PeerLink& peer : g_peers) {
        if (peer.stage != state::gameplay::PeerStage::absent
            && same_endpoint(peer.endpoint, endpoint)) {
            peer = {};
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Drops every peer. */
void reset() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    for (state::gameplay::PeerLink& peer : g_peers) {
        peer = {};
    }
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace sunrise::server::gameplay::peer
