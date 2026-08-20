#include "group_host.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdio>

#include "../../../client/hooks/network/view_signature_capture.h"
#include "../../../core/settings/settings.h"
#include "../../../middleware/gameplay/descriptor/join_descriptor.h"
#include "../../../middleware/gameplay/group/member_messages.h"
#include "../../../middleware/gameplay/group/parameter_messages.h"
#include "../../../middleware/gameplay/group/parameter_registry.h"
#include "../../../middleware/gameplay/group/session_messages.h"
#include "../../../middleware/gameplay/group/session_state.h"
#include "../../../middleware/gameplay/group/view_message.h"
#include "../../../state/activity/runtime.h"
#include "../endpoint/gameplay_endpoint.h"
#include "../gameplay_log.h"
#include "../peer/peer_transport.h"
#include "group_host_sessions.h"

namespace sunrise::server::gameplay::group {

namespace {

namespace wire = middleware::gameplay::group;
namespace bits = middleware::encoding::bits;
namespace descriptor = middleware::gameplay::descriptor;

/** One reliable body staged before it is split into fragments. */
constexpr std::size_t kBodyCapacity = 128;
/** A membership snapshot is far larger, and the peer's reliable send queue bounds it. */
constexpr std::size_t kMembershipBodyCapacity = 512;
/** Only the low 25 bitmap bits name a registry parameter. */
constexpr std::uint64_t kParameterMaskBits = 0x1FFFFFF;
/**
 * Values safe to synthesize when the peer requests them.
 * Parameter 13 has a proven encoder now, but its authored-route fields are deliberately withheld
 * until one native record establishes their values. Sending a cleared record would select the
 * local route and recreate the missing-director failure.
 */
constexpr std::uint64_t kAnswerableParameters =
    wire::kEncodableParameters
    & ~(std::uint64_t{1} << static_cast<std::uint8_t>(wire::Parameter::remoteJoinData));
/** Room for every registry name plus its separators. */
constexpr std::size_t kParameterNameCapacity = 640;
/** Member index this host takes, and the index it nominates to succeed it. */
constexpr std::uint32_t kHostMemberIndex = 0;
/** Member index the admitted peer takes. */
constexpr std::size_t kPeerMemberIndex = 1;
/** Members one snapshot names: this host and the admitted peer. */
constexpr std::size_t kSnapshotMemberCount = 2;
/** Registry index the join-latch update names. Any of the 25 would do; none is ever filled. */
constexpr std::uint8_t kJoinLatchParameter = 0;
/** Peers this host tracks at once. The public POC admits one. */
constexpr std::size_t kAdmittedCapacity = 4;
/** Every member index the `activity-host` parameter covers. The peer needs its own bit set. */
constexpr std::uint32_t kAllMembers = 0xFFFFFFFF;
/** Shortest gap between two retries of an owed publish. */
constexpr std::uint64_t kRetryInterval = 250;
/** Player slot the admitted peer's player takes. */
constexpr std::uint32_t kPeerPlayerSlot = 0;
/** Counter the first player of a session carries. The consumer's own add starts here too. */
constexpr std::uint32_t kFirstAddSequence = 0;
/** Native view establishment begins with both sides publishing stage one. */
constexpr std::uint8_t kInitialViewStage = 1;
/** Only stage two carries the runtime compatibility signature. */
constexpr std::uint8_t kSignatureViewStage = 2;
/** Stage four owns the native readiness scan and may already consume replication. */
constexpr std::uint8_t kReplicationViewStage = 4;
/** Both sides reaching stage five opens the simulation gatekeeper. */
constexpr std::uint8_t kFinalViewStage = 5;
/** Storage for the embedded host's bounded native peer-session-id. */
constexpr std::size_t kPeerSessionIdCapacity = 128;

/** Per-session host side of native message 40's five-stage handshake. */
struct ViewHandshake {
    client::hooks::network::view_signature::CapturedSignature signature{};
    /** Native activity-session token carried inside message 40. */
    std::uint64_t token{};
    std::int32_t index{-1};
    std::uint8_t localStage{};
    std::uint8_t remoteStage{};
    bool started{};
    bool signatureReady{};
    bool replicationPublished{};
    bool bound{};
};

/** One admitted peer and the player it asked this host to add. */
struct Admitted {
    state::gameplay::Endpoint endpoint{};
    std::uint64_t joinId{};
    std::uint64_t playerId{};
    /** Group-session id the peer named in its join request, which its parameters must echo. */
    std::uint64_t sessionId{};
    bool occupied{};
    bool hasPlayer{};
    /** Set once the peer reports its join finished, which is what promotes it to `established`. */
    bool joinComplete{};
    /** Set once a snapshot carrying that promotion is on the peer's reliable channel. */
    bool joinPublished{};
    /** Set once the `activity-host` parameter is on the peer's reliable channel. */
    bool activityHostPublished{};
    /** Set once a snapshot naming the peer's player is on that channel. The queue can refuse it. */
    bool playerPublished{};
    /** Set once this host has answered the peer's establish announcement on the reliable link. */
    bool peerEstablishPublished{};
    /** Message-40 state is per group session even when one channel carries several sessions. */
    ViewHandshake view{};
    /** Tick of the last retry, so a full queue is retried on a timer rather than every packet. */
    std::uint64_t lastRetry{};
    /** View retry clock is independent of the membership/parameter queue. */
    std::uint64_t viewLastRetry{};
    /** Order in which the peer last named this session. The lowest is the least recently used. */
    std::uint64_t lastUse{};
};

/**
 * Public group sessions the peer holds at once: one current and one target.
 * The peer resolves a session through a two-element array, so a third is one it left.
 */
constexpr std::size_t kPublicSessionCapacity = 2;

/** Revision of the last published snapshot. The consumer refuses one that does not increase. */
std::atomic<std::uint32_t> g_membershipRevision{0};
/** Stamps `Admitted::lastUse`. It only has to order the records, so it never has to be a clock. */
std::atomic<std::uint64_t> g_admitClock{0};
/** Guards the admitted table against the worker and the callback pump. */
SRWLOCK g_admittedLock{SRWLOCK_INIT};
/** Admitted peers. A join claims a slot and a leave never reclaims one in this POC. */
std::array<Admitted, kAdmittedCapacity> g_admitted{};
/** Recently promoted public groups; retained after their admitted row is released. */
std::array<std::uint64_t, kPublicSessionCapacity> g_activityHostPublished{};

/** Tests the durable activity-host history while the admitted lock is held. */
[[nodiscard]] bool activity_host_was_published(std::uint64_t sessionId) noexcept {
    return sessionId != 0
           && (g_activityHostPublished[0] == sessionId || g_activityHostPublished[1] == sessionId);
}

/** Retains one promotion across the client's subsequent leave. */
void remember_activity_host(std::uint64_t sessionId) noexcept {
    if (sessionId == 0 || g_activityHostPublished[0] == sessionId) {
        return;
    }
    g_activityHostPublished[1] = g_activityHostPublished[0];
    g_activityHostPublished[0] = sessionId;
}

/** A fresh attempt for the same group must earn activity-host promotion again. */
void forget_activity_host(std::uint64_t sessionId) noexcept {
    if (g_activityHostPublished[0] == sessionId) {
        g_activityHostPublished[0] = g_activityHostPublished[1];
        g_activityHostPublished[1] = 0;
    } else if (g_activityHostPublished[1] == sessionId) {
        g_activityHostPublished[1] = 0;
    }
}

/** Member state this host publishes for every member carrying the join id. */
constexpr wire::MemberState kJoinMemberState = wire::MemberState::ready;

// Three peer checks pin this to exactly `ready`. The joining peer's entry must be at least
// `joined`, must not be `established`, and the request waits until every member carrying the
// join id reads `ready`.
static_assert(static_cast<std::uint8_t>(kJoinMemberState)
                  >= static_cast<std::uint8_t>(wire::MemberState::joined),
              "the published member state must clear the peer's own join bar");
static_assert(kJoinMemberState == wire::MemberState::ready,
              "the request advances only when every member carrying the join id reads ready");

/**
 * Sends one reliable group-session message.
 * @param sessionId Group session whose reliable channel carries it.
 * @param id Registry message id.
 * @param declaredSize Decoded structure size the registry declares.
 * @param write Callback that writes the body.
 * @return True when the message was queued.
 */
template <typename Body>
[[nodiscard]] bool send_reliable(std::uint64_t sessionId,
                                 std::uint8_t id,
                                 std::uint32_t declaredSize,
                                 Body write) noexcept {
    std::array<std::byte, kBodyCapacity> body{};
    bits::Writer writer(body);
    std::size_t size = 0;
    if (!write(writer) || !writer.finish(size)) {
        return false;
    }
    return peer::enqueue_reliable(
        sessionId, id, declaredSize, {body.data(), size}, writer.bit_count());
}

/**
 * Finds or claims the record for one peer.
 * @param peer Peer endpoint.
 * @param sessionId Group session the record is keyed by. Zero claims nothing.
 * @return Record for that session, or null when the table is full.
 */
[[nodiscard]] Admitted* claim(const state::gameplay::Endpoint& peer,
                              std::uint64_t sessionId) noexcept {
    if (sessionId == 0) {
        return nullptr;
    }
    // Keyed by session, not endpoint: one client holds a record per public region and both records
    // name the same endpoint.
    const std::uint64_t use = g_admitClock.fetch_add(1) + 1;
    for (Admitted& entry : g_admitted) {
        if (entry.occupied && entry.sessionId == sessionId) {
            entry.lastUse = use;
            return &entry;
        }
    }
    for (Admitted& entry : g_admitted) {
        if (!entry.occupied) {
            entry.occupied = true;
            entry.endpoint = peer;
            entry.sessionId = sessionId;
            entry.lastUse = use;
            return &entry;
        }
    }
    return nullptr;
}

/** @return Admitted record whose native view carries one activity token. */
[[nodiscard]] Admitted* find_admitted_view(std::uint64_t token) noexcept {
    for (Admitted& entry : g_admitted) {
        if (entry.occupied && entry.view.token == token) {
            return &entry;
        }
    }
    return nullptr;
}

/**
 * Publishes one snapshot naming this host, one admitted peer, and that peer's player if it has
 * one. The caller holds the admitted lock.
 * @param record Admitted peer the snapshot names.
 * @return True when the snapshot was queued on the peer's reliable channel.
 */
[[nodiscard]] bool publish_snapshot(const Admitted& record) noexcept {
    const state::gameplay::Endpoint host = endpoint::advertised();
    std::array<wire::MembershipMember, kSnapshotMemberCount> members{};
    std::array<char, kPeerSessionIdCapacity> hostPeerSessionId{};
    const int hostPeerSessionIdSize =
        std::snprintf(hostPeerSessionId.data(),
                      hostPeerSessionId.size(),
                      "Sunrise_%016llX@x64@activity_host",
                      static_cast<unsigned long long>(record.sessionId));
    if (hostPeerSessionIdSize <= 0
        || static_cast<std::size_t>(hostPeerSessionIdSize) >= hostPeerSessionId.size()) {
        return false;
    }
    descriptor::write_net_addr(host.address, host.port, members[kHostMemberIndex].address);
    // The session id is the machine id this region's descriptor advertised, and the client joined
    // through it. The whole-process identity would name a host this session never saw.
    members[kHostMemberIndex].machineId = record.sessionId;
    // Retail complete snapshots always carry the host's native peer identity. Only the
    // peer-session-id field is known for the embedded host; the remaining optional fields stay
    // absent instead of borrowing the local player's platform/account identity.
    members[kHostMemberIndex].peerSessionId = {hostPeerSessionId.data(),
                                               static_cast<std::size_t>(hostPeerSessionIdSize)};
    // The consumer refuses a table with no entry it recognises as itself, so the peer's own blob is
    // echoed. A blob rebuilt from the endpoint it arrived from is not the same bytes.
    if (!peer::remote_address(record.sessionId, members[kPeerMemberIndex].address)) {
        descriptor::write_net_addr(
            record.endpoint.address, record.endpoint.port, members[kPeerMemberIndex].address);
    }
    // The peer refuses a snapshot whose entry for itself carries another join id. Its real
    // machine id sits in the address table this host does not decode, so the join id stands in.
    members[kPeerMemberIndex].machineId = record.joinId;
    members[kPeerMemberIndex].joinId = record.joinId;
    // The peer ends its join request once no session holds more than one member with that id, so
    // both entries carry it. A table naming it once says the join is over.
    members[kHostMemberIndex].joinId = record.joinId;
    for (wire::MembershipMember& member : members) {
        // The connection group is what makes the consumer resolve the member's peer link. This
        // host has no value for join compatibility or the join timestamp, so both stay cleared.
        member.connectionPresent = true;
    }
    // Both entries carry the join id, so both take the same state. Once the peer reports its join
    // finished they move to `established`, which is what stops it re-sending that report.
    const wire::MemberState state =
        record.joinComplete ? wire::MemberState::established : kJoinMemberState;
    members[kHostMemberIndex].state = state;
    members[kPeerMemberIndex].state = state;

    std::array<wire::MembershipPlayer, 1> players{};
    players[0].slot = kPeerPlayerSlot;
    players[0].playerId = record.playerId;
    players[0].memberIndex = static_cast<std::uint32_t>(kPeerMemberIndex);
    players[0].addSequence = kFirstAddSequence;
    if (record.hasPlayer) {
        members[kPeerMemberIndex].ownsPlayerSlot = true;
        members[kPeerMemberIndex].playerSlot = kPeerPlayerSlot;
    }

    wire::MembershipUpdate update{};
    // The same per-region machine id the member table carries.
    update.hostMachineId = record.sessionId;
    update.revision = g_membershipRevision.fetch_add(1) + 1;
    update.hostMemberIndex = kHostMemberIndex;
    update.successionIndex = kHostMemberIndex;
    update.members = members;
    if (record.hasPlayer) {
        update.players = players;
    }

    std::array<std::byte, kMembershipBodyCapacity> body{};
    bits::Writer writer(body);
    std::size_t size = 0;
    if (!wire::write_membership_update(writer, update) || !writer.finish(size)) {
        return false;
    }
    // The peer logs the hash it wanted, so ours has to be logged next to it to read a mismatch.
    report(core::log::Level::info,
           "ev=gameplay stage=membership result=built revision=%u members=%zu players=%zu "
           "hash=0x%08X",
           update.revision,
           update.members.size(),
           update.players.size(),
           wire::session_state_hash(update));
    return peer::enqueue_reliable(
        record.sessionId,
        static_cast<std::uint8_t>(wire::SessionMessageId::membershipUpdate),
        wire::kMembershipUpdateSize,
        {body.data(), size},
        writer.bit_count());
}

/**
 * Fills the `activity-host` body this host publishes.
 * The peer creates no activity client until it holds this parameter, and the public-region
 * slice-set switch waits behind that client.
 * @param body Cleared body to fill.
 * @param groupSessionId Group session whose region this parameter is published for.
 */
void fill_activity_host(wire::ActivityHostParameter& body, std::uint64_t groupSessionId) noexcept {
    // The peer's `current-activity` carries this host's empty delta, so its nonce is the
    // descriptor default and the comparand is the empty id.
    body.selectionId = 0;
    // The peer addresses its activity join request to this id, and the activity route refuses one
    // that names no committed activity session. A gameplay identity is not one.
    body.hostId = held_host_session(groupSessionId);
    // The peer tests only the bit for its own member index, and this host does not decode which
    // index that is, so every bit is set.
    body.memberMask = kAllMembers;
    // This parameter constructs a plain-UDP peer address. It must name the same gameplay endpoint
    // as the join descriptor; advertising the BAP service port creates a second, unestablished
    // channel that the native view creator resolves and then rejects.
    const state::gameplay::Endpoint host = endpoint::advertised();
    body.address = host.address;
    body.port = host.port;
}

/**
 * Publishes the `activity-host` parameter for one admitted peer. The caller holds the lock.
 * @param record Admitted peer the parameter is published to.
 * @return True when the update was queued on the peer's reliable channel.
 */
[[nodiscard]] bool publish_activity_host(const Admitted& record) noexcept {
    if (held_host_session(record.sessionId) == state::activity::kAbsentSessionId) {
        // Publishing a zero host id latches an unusable parameter on the peer, and the peer only
        // reads it once. The region's advertisement allocates and this retries.
        report(core::log::Level::debug, "ev=gameplay stage=activityhost result=nosession");
        return false;
    }
    wire::ParameterUpdate update{};
    update.sessionId = record.sessionId;
    // Both go in one update, so the peer never holds the host without the activity it belongs to.
    // `current-activity` carries an empty delta, which leaves the peer's own descriptor defaults.
    update.carriedMask =
        (std::uint64_t{1} << static_cast<std::uint8_t>(wire::Parameter::activityHost))
        | (std::uint64_t{1} << static_cast<std::uint8_t>(wire::Parameter::currentActivity));
    fill_activity_host(update.activityHost, record.sessionId);

    const bool sent = send_reliable(
        record.sessionId,
        wire::kParameterUpdateId,
        wire::kParameterUpdateSize,
        [&update](bits::Writer& writer) { return wire::write_parameter_update(writer, update); });
    std::array<char, kParameterNameCapacity> names{};
    report(sent ? core::log::Level::info : core::log::Level::debug,
           "ev=gameplay stage=activityhost result=%s host=0x%llX address=0x%08X port=%u names=%s",
           sent ? "queued" : "deferred",
           static_cast<unsigned long long>(update.activityHost.hostId),
           update.activityHost.address,
           static_cast<unsigned>(update.activityHost.port),
           wire::parameter_names(update.carriedMask, names.data(), names.size()));
    return sent;
}

/**
 * Answers the peer's establish announcement.
 *
 * Peer-establish is symmetric: each endpoint publishes the session id after its local membership
 * reaches the establish gate. The client's inbound half raises the native session event that binds
 * the corresponding replication slot. Message 40 cannot resolve a view until that event runs.
 */
[[nodiscard]] bool publish_peer_establish(const Admitted& record) noexcept {
    const bool sent =
        send_reliable(record.sessionId,
                      static_cast<std::uint8_t>(wire::SessionMessageId::peerEstablish),
                      wire::kPeerEstablishSize,
                      [&record](bits::Writer& writer) noexcept {
                          return wire::write_session_only(writer, record.sessionId);
                      });
    report(sent ? core::log::Level::info : core::log::Level::debug,
           "ev=gameplay stage=establish result=%s direction=out session=0x%016llX",
           sent ? "queued" : "deferred",
           static_cast<unsigned long long>(record.sessionId));
    return sent;
}

/** Queues one local stage of message 40 and advances only after the queue accepts it. */
[[nodiscard]] bool send_view_stage(Admitted& record, std::uint8_t stage) noexcept {
    if ((stage >= kSignatureViewStage && record.view.index < 0)
        || (stage == kSignatureViewStage && !record.view.signatureReady)) {
        return false;
    }
    wire::ViewEstablishment body{};
    body.kind = stage;
    body.sessionToken = record.view.token;
    if (stage >= kSignatureViewStage) {
        body.hasOptionalValue = true;
        body.optionalValue = record.view.index;
    }
    if (stage == kSignatureViewStage) {
        body.hasList = record.view.signatureReady;
        body.listCount = record.view.signature.count;
        body.list = record.view.signature.bytes;
    }
    const bool sent = send_reliable(
        record.sessionId,
        wire::kViewMessageId,
        wire::kViewMessageSize,
        [&body](bits::Writer& writer) noexcept { return wire::write_view(writer, body); });
    if (sent) {
        record.view.localStage = stage;
    }
    report(sent ? core::log::Level::info : core::log::Level::debug,
           "ev=gameplay stage=view result=%s direction=out local=%u remote=%u index=%d "
           "token=0x%llX list=%u",
           sent ? "queued" : "deferred",
           static_cast<unsigned>(stage),
           static_cast<unsigned>(record.view.remoteStage),
           record.view.index,
           static_cast<unsigned long long>(record.view.token),
           static_cast<unsigned>(body.listCount));
    return sent;
}

/** Captures the signature the native client built for one view, once it becomes available. */
[[nodiscard]] bool refresh_view_signature(ViewHandshake& view) noexcept {
    if (view.signatureReady) {
        return true;
    }
    client::hooks::network::view_signature::CapturedSignature captured{};
    if (!client::hooks::network::view_signature::find(view.token, captured)) {
        return false;
    }
    view.signature = captured;
    view.signatureReady = true;
    return true;
}

/** Publishes the stage-four view to replication without reporting the session fully bound. */
void publish_replication_view(Admitted& record) noexcept {
    if (record.view.replicationPublished || !record.view.signatureReady
        || record.view.localStage < kReplicationViewStage
        || record.view.remoteStage < kReplicationViewStage) {
        return;
    }
    state::gameplay::ViewSignature signature{};
    signature.token = record.view.token;
    signature.kind = kReplicationViewStage;
    signature.listCount = record.view.signature.count;
    signature.hasList = true;
    signature.list = record.view.signature.bytes;
    signature.bound = false;
    peer::bind_view(record.sessionId, signature);
    record.view.replicationPublished = true;
    report(core::log::Level::info,
           "ev=gameplay stage=view result=replication-ready local=%u remote=%u index=%d "
           "token=0x%llX",
           static_cast<unsigned>(record.view.localStage),
           static_cast<unsigned>(record.view.remoteStage),
           record.view.index,
           static_cast<unsigned long long>(record.view.token));
}

/** Marks the peer bound only after both sides complete native stage five. */
void complete_view(Admitted& record) noexcept {
    if (record.view.bound) {
        return;
    }
    state::gameplay::ViewSignature signature{};
    signature.token = record.view.token;
    signature.kind = kFinalViewStage;
    signature.listCount = record.view.signature.count;
    signature.hasList = record.view.signatureReady;
    signature.list = record.view.signature.bytes;
    signature.bound = true;
    peer::bind_view(record.sessionId, signature);
    record.view.replicationPublished = true;
    record.view.bound = true;
    report(core::log::Level::info,
           "ev=gameplay stage=view result=bound local=%u remote=%u index=%d token=0x%llX",
           static_cast<unsigned>(record.view.localStage),
           static_cast<unsigned>(record.view.remoteStage),
           record.view.index,
           static_cast<unsigned long long>(record.view.token));
}

/**
 * Drives the receptor half of the native handshake without advancing ahead of the client.
 * The client initiator chooses the view index in stage two. This host validates and echoes that
 * stage, then echoes each later stage until both sides reach five. Only stage one is retried:
 * duplicate later stages are establishment errors to the native initiator while it waits for its
 * local readiness gates.
 */
void progress_view(Admitted& record) noexcept {
    if (!record.view.started || record.view.bound) {
        return;
    }
    if (record.view.token == state::activity::kAbsentSessionId) {
        record.view.token = held_host_session(record.sessionId);
        if (record.view.token == state::activity::kAbsentSessionId) {
            return;
        }
        report(core::log::Level::info,
               "ev=gameplay stage=view result=routed group=0x%016llX token=0x%016llX",
               static_cast<unsigned long long>(record.sessionId),
               static_cast<unsigned long long>(record.view.token));
    }
    if (record.view.localStage == 0) {
        (void)send_view_stage(record, kInitialViewStage);
        return;
    }
    if (record.view.localStage == kFinalViewStage && record.view.remoteStage == kFinalViewStage) {
        complete_view(record);
        return;
    }
    if (record.view.remoteStage > record.view.localStage) {
        if (send_view_stage(record, record.view.remoteStage)) {
            publish_replication_view(record);
        }
        return;
    }
    publish_replication_view(record);
    // Before stage two, the native view lookup may not exist yet and deliberately drops message 40.
    // Once the initiator has answered, the reliable channel and its advancing stage prove receipt.
    // Repeating stage 2, 3, or 4 is not harmless: in particular, a second remote stage 4 makes the
    // initiator report an establishment error while it is waiting to advance its local side to 5.
    if (record.view.localStage == kInitialViewStage) {
        (void)send_view_stage(record, kInitialViewStage);
    }
}

/** Validates one receptor stage and advances the matching host view. */
void accept_view(const state::gameplay::Endpoint& from,
                 const wire::ViewEstablishment& body,
                 std::uint64_t now) noexcept {
    const char* result = "missing-session";
    std::uint8_t localStage = 0;
    std::uint8_t remoteStage = 0;
    std::int32_t index = -1;
    bool restarted = false;
    AcquireSRWLockExclusive(&g_admittedLock);
    Admitted* const record = find_admitted_view(body.sessionToken);
    if (record != nullptr && record->endpoint.address == from.address
        && record->endpoint.port == from.port) {
        // Stage one after a later remote stage is a new native handshake, not a harmless repeat.
        // Keeping the old local stage makes the two peers answer one another forever (for example,
        // client stage 1 versus server stage 4) and floods the reliable channel needed by joins.
        if (!record->view.bound && body.kind == kInitialViewStage
            && record->view.remoteStage > kInitialViewStage) {
            record->view.signature = {};
            record->view.index = -1;
            record->view.localStage = 0;
            record->view.remoteStage = 0;
            record->view.signatureReady = false;
            record->view.replicationPublished = false;
            peer::clear_view(record->sessionId);
            restarted = true;
        }
        result = "invalid-stage";
        const bool stageValid = body.kind >= kInitialViewStage && body.kind <= kFinalViewStage;
        // A view can begin before its activity record is claimed. Its repeated stage two is enough
        // to recover because that stage carries both the chosen index and the complete signature.
        const bool stageTwoBootstrap = body.kind == kSignatureViewStage
                                       && record->view.remoteStage == 0
                                       && record->view.localStage >= kInitialViewStage;
        const bool ordered = body.kind <= record->view.remoteStage + 1 || stageTwoBootstrap;
        bool formValid = false;
        if (body.kind == kInitialViewStage) {
            formValid = !body.hasOptionalValue && !body.hasList;
        } else if (body.kind == kSignatureViewStage) {
            const bool signatureReady = refresh_view_signature(record->view);
            const bool indexValid =
                body.hasOptionalValue && body.optionalValue >= 0
                && (record->view.index < 0 || body.optionalValue == record->view.index);
            formValid = signatureReady && indexValid && body.hasList
                        && body.listCount == record->view.signature.count
                        && body.list == record->view.signature.bytes;
        } else {
            formValid =
                body.hasOptionalValue && !body.hasList && body.optionalValue == record->view.index;
        }
        if (stageValid && ordered && formValid) {
            if (body.kind == kSignatureViewStage && record->view.index < 0) {
                record->view.index = body.optionalValue;
            }
            if (body.kind > record->view.remoteStage) {
                record->view.remoteStage = body.kind;
            }
            record->view.started = true;
            record->viewLastRetry = now;
            progress_view(*record);
            result = "accepted";
        } else if (stageValid && body.kind <= record->view.remoteStage && formValid) {
            result = "repeat";
        }
        localStage = record->view.localStage;
        remoteStage = record->view.remoteStage;
        index = record->view.index;
    }
    ReleaseSRWLockExclusive(&g_admittedLock);
    report(result[0] == 'a' || result[0] == 'r' ? core::log::Level::info : core::log::Level::warn,
           "ev=gameplay stage=view result=%s direction=in local=%u remote=%u got=%u index=%d "
           "token=0x%llX list=%u restart=%u",
           result,
           static_cast<unsigned>(localStage),
           static_cast<unsigned>(remoteStage),
           static_cast<unsigned>(body.kind),
           index,
           static_cast<unsigned long long>(body.sessionToken),
           static_cast<unsigned>(body.listCount),
           restarted ? 1U : 0U);
}

/**
 * Answers one parameter request with the parameters this host can encode.
 * An empty answer leaves the peer waiting, so the answer carries every requested parameter that
 * has an encoder and names the rest as unheld.
 * @param sessionId Session the request named, which is also the link it goes back on.
 * @param requested Requested parameter mask, already reduced to its meaningful bits.
 */
void answer_parameters(std::uint64_t sessionId, std::uint64_t requested) noexcept {
    std::uint64_t carried = requested & kAnswerableParameters;
    // Claims the region's slot rather than only reading it, so a request arriving before the
    // advertisement still makes the service slice allocate one.
    if (activity_host_session(sessionId, kUnknownRegion) == state::activity::kAbsentSessionId) {
        // See publish_activity_host: a zero host id is worse than no answer for this one.
        carried &= ~(std::uint64_t{1} << static_cast<std::uint8_t>(wire::Parameter::activityHost));
    }
    if (carried == 0) {
        report(core::log::Level::debug,
               "ev=gameplay stage=parameters result=unheld mask=0x%08X",
               static_cast<unsigned>(requested));
        return;
    }

    wire::ParameterUpdate update{};
    update.sessionId = sessionId;
    update.carriedMask = carried;
    // A zero host id latches an unusable parameter on the peer, so the answer carries the same
    // body the unsolicited publish does.
    fill_activity_host(update.activityHost, sessionId);

    const bool sent = send_reliable(
        sessionId,
        wire::kParameterUpdateId,
        wire::kParameterUpdateSize,
        [&update](bits::Writer& writer) { return wire::write_parameter_update(writer, update); });
    std::array<char, kParameterNameCapacity> names{};
    report(sent ? core::log::Level::info : core::log::Level::warn,
           "ev=gameplay stage=parameters result=%s carried=0x%08X names=%s",
           sent ? "answered" : "fail",
           static_cast<unsigned>(carried),
           wire::parameter_names(carried, names.data(), names.size()));
}

/**
 * Answers one time-synchronize probe with the same form it arrived in.
 * @param from Peer endpoint.
 * @param probe Decoded probe.
 */
void answer_time(const state::gameplay::Endpoint& from,
                 const wire::TimeSynchronize& probe) noexcept {
    // The exchange must never block the event loop, so the samples are echoed unchanged.
    if (!peer::send_out_of_band(from,
                                static_cast<std::uint8_t>(wire::SessionMessageId::timeSynchronize),
                                wire::kTimeSynchronizeSize,
                                [&probe](bits::Writer& writer) noexcept {
                                    return wire::write_time_synchronize(writer, probe);
                                })) {
        report(core::log::Level::debug, "ev=gameplay stage=time result=fail");
    }
}

/**
 * Drops one session's link and its admitted record together.
 * A leave names one region's session, and the client's other region must keep its own link.
 * @param sessionId Session the peer is leaving.
 */
void release(std::uint64_t sessionId) noexcept {
    peer::drop(sessionId);
    // The region's activity host stays. A leave is also how the peer fast travels to the region it
    // is already in, and a fresh id there is `public_activity_host_mismatch`.
    AcquireSRWLockExclusive(&g_admittedLock);
    for (Admitted& entry : g_admitted) {
        if (entry.occupied && entry.sessionId == sessionId) {
            entry = {};
        }
    }
    ReleaseSRWLockExclusive(&g_admittedLock);
}

} // namespace

/** Frees every admitted record at one endpoint. */
void release_endpoint(const state::gameplay::Endpoint& endpoint) noexcept {
    std::size_t count = 0;
    AcquireSRWLockExclusive(&g_admittedLock);
    for (Admitted& entry : g_admitted) {
        if (entry.occupied && entry.endpoint.address == endpoint.address
            && entry.endpoint.port == endpoint.port) {
            ++count;
            entry = {};
        }
    }
    ReleaseSRWLockExclusive(&g_admittedLock);
    if (count != 0) {
        report(core::log::Level::info,
               "ev=gameplay stage=admitted result=dropped endpoint=0x%08X:%u sessions=%zu",
               endpoint.address,
               static_cast<unsigned>(endpoint.port),
               count);
    }
}

/** Consumes one group-session message. */
bool consume(const state::gameplay::Endpoint& from,
             std::uint64_t sessionId,
             std::uint8_t id,
             bits::Reader& reader,
             std::uint64_t now) noexcept {
    if (id == static_cast<std::uint8_t>(wire::SessionMessageId::timeSynchronize)) {
        wire::TimeSynchronize probe{};
        if (!wire::read_time_synchronize(reader, probe)) {
            return false;
        }
        answer_time(from, probe);
        return true;
    }
    if (id == wire::kViewMessageId) {
        wire::ViewEstablishment view{};
        if (!wire::read_view(reader, view)) {
            return false;
        }
        // Message 40 names its own group session. A link may carry several overlapping regions,
        // so the fallback session is deliberately not used here.
        accept_view(from, view, now);
        return true;
    }
    if (id == static_cast<std::uint8_t>(wire::SessionMessageId::leaveSession)) {
        std::uint64_t leaving = 0;
        if (!wire::read_session_only(reader, leaving)) {
            return false;
        }
        const bool sent = peer::send_out_of_band(
            from,
            static_cast<std::uint8_t>(wire::SessionMessageId::leaveAcknowledge),
            wire::kLeaveAcknowledgeSize,
            [leaving](bits::Writer& writer) noexcept {
                return wire::write_session_only(writer, leaving);
            });
        report(core::log::Level::info,
               "ev=gameplay stage=leave result=%s session=0x%016llX",
               sent ? "acknowledged" : "fail",
               static_cast<unsigned long long>(leaving));
        release(leaving);
        return true;
    }
    if (id == static_cast<std::uint8_t>(wire::SessionMessageId::peerEstablish)) {
        std::uint64_t established = 0;
        if (!wire::read_session_only(reader, established)) {
            return false;
        }
        bool establishPublished = false;
        AcquireSRWLockExclusive(&g_admittedLock);
        Admitted* const record = claim(from, established);
        if (record != nullptr) {
            record->view.started = true;
            if (!record->peerEstablishPublished) {
                record->peerEstablishPublished = publish_peer_establish(*record);
            }
            establishPublished = record->peerEstablishPublished;
            // Even when the echo queues immediately, wait for the service slice before sending
            // message 40. Its handler otherwise runs before the client's native session event has
            // had a frame in which to bind the replication slot.
            record->viewLastRetry = now;
        }
        ReleaseSRWLockExclusive(&g_admittedLock);
        report(core::log::Level::info,
               "ev=gameplay stage=establish result=ok direction=in session=0x%016llX echo=%s",
               static_cast<unsigned long long>(established),
               establishPublished ? "queued" : "deferred");
        return true;
    }
    if (id == static_cast<std::uint8_t>(wire::SessionMessageId::joinComplete)) {
        wire::JoinComplete body{};
        if (!wire::read_join_complete(reader, body)) {
            return false;
        }
        // The peer repeats this until its membership shows every member of the join at
        // `established`, so the answer is a snapshot that promotes them. Keyed by the body's
        // session, not the link's: one link carries every region the client joined over it.
        AcquireSRWLockExclusive(&g_admittedLock);
        Admitted* const record = claim(from, body.sessionId);
        bool queued = false;
        const bool owed = record != nullptr && !record->joinPublished;
        if (record != nullptr) {
            record->joinComplete = true;
            if (owed) {
                queued = publish_snapshot(*record);
                record->joinPublished = queued;
            }
            // The peer only reads the parameter once its join is finished, and the queue is at its
            // fullest right here, so a refusal is expected and the service slice retries it.
            if (record->joinPublished && !record->activityHostPublished) {
                record->activityHostPublished = publish_activity_host(*record);
                if (record->activityHostPublished) {
                    remember_activity_host(record->sessionId);
                }
                record->lastRetry = now;
            }
        }
        ReleaseSRWLockExclusive(&g_admittedLock);
        report(queued ? core::log::Level::info : core::log::Level::debug,
               "ev=gameplay stage=join result=%s session=0x%llX machine=0x%llX update=%u",
               queued              ? "completed"
               : record == nullptr ? "fail"
               : owed              ? "deferred"
                                   : "repeat",
               static_cast<unsigned long long>(body.sessionId),
               static_cast<unsigned long long>(body.machineId),
               body.joinSequence);
        return true;
    }
    if (id == static_cast<std::uint8_t>(wire::SessionMessageId::joinAbort)) {
        wire::SessionNotice notice{};
        if (!wire::read_join_abort(reader, notice)) {
            return false;
        }
        report(core::log::Level::info,
               "ev=gameplay stage=join result=abort session=0x%016llX",
               static_cast<unsigned long long>(notice.sessionId));
        release(notice.sessionId);
        return true;
    }
    if (id == wire::kParameterRequestId) {
        wire::ParameterRequestHeader header{};
        if (!wire::read_parameter_request(reader, header)) {
            return false;
        }
        // The selected bodies after the header have per-parameter codecs this host does not
        // write, so their widths are unknown and this container cannot be walked further.
        const std::uint64_t mask = header.requestedMask & kParameterMaskBits;
        std::array<char, kParameterNameCapacity> names{};
        report(core::log::Level::info,
               "ev=gameplay stage=parameters result=request mask=0x%08X mode=%u names=%s",
               static_cast<unsigned>(mask),
               static_cast<unsigned>(header.modeFlag ? 1U : 0U),
               wire::parameter_names(mask, names.data(), names.size()));
        answer_parameters(header.sessionId, mask);
        return false;
    }
    if (id == wire::kPeerPropertiesId) {
        wire::PeerPropertiesHeader header{};
        if (!wire::read_peer_properties_header(reader, header)) {
            return false;
        }
        // The 304-byte property block behind the address is not decoded, so the body is
        // reported and not consumed.
        report(core::log::Level::info,
               "ev=gameplay stage=properties result=read session=0x%llX method=%u",
               static_cast<unsigned long long>(header.sessionId),
               static_cast<unsigned>(header.addressMethod));
        return false;
    }
    if (id == wire::kPlayerAddId) {
        wire::PlayerAddRequest request{};
        if (!wire::read_player_add(reader, request)) {
            return false;
        }
        // The published row carries the identity group only. The profile block behind it has no
        // encoder here, and the peer's clear-flag arm accepts a row without one.
        AcquireSRWLockExclusive(&g_admittedLock);
        // The body's session, for the same reason join-complete uses its own.
        Admitted* const record = claim(from, request.sessionId);
        bool published = false;
        if (record != nullptr) {
            record->hasPlayer = true;
            record->playerId = request.playerId;
            published = publish_snapshot(*record);
            // The queue is at its fullest here, right after the join promotion, so a refusal is
            // ordinary and the service slice retries it.
            record->playerPublished = published;
        }
        ReleaseSRWLockExclusive(&g_admittedLock);
        // The player block and its tail are not decoded, so the body is reported and not consumed.
        report(core::log::Level::info,
               "ev=gameplay stage=player result=%s session=0x%llX player=0x%llX seq=%u kind=%u",
               published ? "added" : "fail",
               static_cast<unsigned long long>(request.sessionId),
               static_cast<unsigned long long>(request.playerId),
               request.sequence,
               static_cast<unsigned>(request.kind));
        return false;
    }
    return false;
}

/** Publishes the membership snapshot that completes one peer's join. */
bool publish_membership(const state::gameplay::Endpoint& peer,
                        std::uint64_t peerJoinId,
                        std::uint64_t sessionId) noexcept {
    AcquireSRWLockExclusive(&g_admittedLock);
    Admitted* const record = claim(peer, sessionId);
    const bool admitted = record != nullptr;
    bool published = false;
    if (record != nullptr) {
        forget_activity_host(sessionId);
        // A retried join brings a new join id and drops any player the previous attempt added.
        // It also starts again at `ready`, so the previous attempt's completion does not carry.
        record->joinId = peerJoinId;
        record->sessionId = sessionId;
        record->hasPlayer = false;
        record->playerId = 0;
        record->joinComplete = false;
        record->joinPublished = false;
        record->activityHostPublished = false;
        record->playerPublished = false;
        record->peerEstablishPublished = false;
        record->view = {};
        record->lastRetry = 0;
        record->viewLastRetry = 0;
        published = publish_snapshot(*record);
    }
    ReleaseSRWLockExclusive(&g_admittedLock);
    if (admitted) {
        note_session_admission(sessionId);
    }
    return published;
}

/** Retries any publish a full reliable queue refused. */
void service(std::uint64_t now) noexcept {
    // Outside any staged push, so the state revision it advances cannot fail a transaction guard.
    allocate_claimed_host_sessions();
    // The peer drops a stale target locally and sends no leave for it. Such a record shows up
    // only as the least recently named one over the capacity.
    std::uint64_t retired = 0;
    AcquireSRWLockExclusive(&g_admittedLock);
    std::size_t occupied = 0;
    Admitted* oldest = nullptr;
    for (Admitted& record : g_admitted) {
        if (!record.occupied) {
            continue;
        }
        ++occupied;
        if (oldest == nullptr || record.lastUse < oldest->lastUse) {
            oldest = &record;
        }
    }
    if (occupied > kPublicSessionCapacity && oldest != nullptr) {
        retired = oldest->sessionId;
        *oldest = {};
    }
    for (Admitted& record : g_admitted) {
        if (!record.occupied || !record.view.started || record.view.bound
            || now - record.viewLastRetry < kRetryInterval) {
            continue;
        }
        record.viewLastRetry = now;
        if (!record.peerEstablishPublished) {
            record.peerEstablishPublished = publish_peer_establish(record);
            continue;
        }
        progress_view(record);
    }
    for (Admitted& record : g_admitted) {
        const bool owed =
            record.occupied
            && ((record.joinComplete && !record.joinPublished)
                || (record.joinPublished && !record.activityHostPublished)
                || (record.joinPublished && record.hasPlayer && !record.playerPublished));
        if (!owed || now - record.lastRetry < kRetryInterval) {
            continue;
        }
        record.lastRetry = now;
        if (!record.joinPublished) {
            record.joinPublished = publish_snapshot(record);
            continue;
        }
        if (!record.activityHostPublished) {
            record.activityHostPublished = publish_activity_host(record);
            if (record.activityHostPublished) {
                remember_activity_host(record.sessionId);
            }
            continue;
        }
        // Last, so the order the join needs is unchanged. The snapshot carries the player row the
        // peer's own add asked for, and a refused one leaves the peer's player unnamed.
        record.playerPublished = publish_snapshot(record);
    }
    ReleaseSRWLockExclusive(&g_admittedLock);
    // Outside the lock, in the order `release` already uses. The region's activity host stays: the
    // peer rotates back into a region it has not left, and a fresh id there is a hard error.
    if (retired != 0) {
        peer::drop(retired);
        report(core::log::Level::info,
               "ev=gameplay stage=admitted result=retired session=0x%016llX held=%zu",
               static_cast<unsigned long long>(retired),
               occupied - 1);
    }
}

/** Publishes the parameter update a joining peer needs before it will finish its join. */
bool publish_join_parameters(std::uint64_t sessionId) noexcept {
    // A joining peer finishes only once it has applied one parameter update. Any update with a
    // named parameter and no body sets that latch. This host has no values, so it releases a slot
    // the peer never filled, which leaves the peer's state alone.
    wire::ParameterUpdate update{};
    update.sessionId = sessionId;
    update.releasedMask = std::uint64_t{1} << kJoinLatchParameter;

    const bool sent = send_reliable(
        sessionId,
        wire::kParameterUpdateId,
        wire::kParameterUpdateSize,
        [&update](bits::Writer& writer) { return wire::write_parameter_update(writer, update); });
    std::array<char, kParameterNameCapacity> names{};
    report(sent ? core::log::Level::info : core::log::Level::warn,
           "ev=gameplay stage=parameters result=%s released=0x%08X names=%s",
           sent ? "queued" : "fail",
           static_cast<unsigned>(update.releasedMask),
           wire::parameter_names(update.releasedMask, names.data(), names.size()));
    return sent;
}

/** Reports whether replication may produce entity output for one peer. */
bool view_accepted(std::uint64_t sessionId) noexcept {
    return peer::view_bound(sessionId);
}

/** Reports whether join completion and the activity-host update have both been published. */
bool activity_host_published(std::uint64_t sessionId) noexcept {
    bool published = false;
    AcquireSRWLockShared(&g_admittedLock);
    published = activity_host_was_published(sessionId);
    if (!published) {
        for (const Admitted& entry : g_admitted) {
            if (entry.occupied && entry.sessionId == sessionId) {
                published =
                    entry.joinComplete && entry.joinPublished && entry.activityHostPublished;
                break;
            }
        }
    }
    ReleaseSRWLockShared(&g_admittedLock);
    return published;
}

/** Reports whether a peer has joined one advertised gameplay group. */
bool session_admitted(std::uint64_t sessionId) noexcept {
    bool admitted = false;
    AcquireSRWLockShared(&g_admittedLock);
    for (const Admitted& entry : g_admitted) {
        if (entry.occupied && entry.sessionId == sessionId) {
            admitted = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_admittedLock);
    return admitted;
}

/** Copies every admitted group-session record. */
void snapshot_admitted(std::span<AdmittedRow> output, std::size_t& count) noexcept {
    count = 0;
    AcquireSRWLockShared(&g_admittedLock);
    for (const Admitted& entry : g_admitted) {
        if (!entry.occupied || count >= output.size()) {
            continue;
        }
        output[count] = {entry.sessionId,
                         entry.endpoint,
                         entry.joinComplete,
                         entry.activityHostPublished,
                         entry.playerPublished};
        ++count;
    }
    ReleaseSRWLockShared(&g_admittedLock);
}

/** Clears every group-session record. */
void reset() noexcept {
    g_membershipRevision.store(0);
    // Every host session goes back to State as well, or its records are stranded there.
    reset_host_sessions();
    AcquireSRWLockExclusive(&g_admittedLock);
    g_admitted = {};
    g_activityHostPublished = {};
    ReleaseSRWLockExclusive(&g_admittedLock);
}

} // namespace sunrise::server::gameplay::group
