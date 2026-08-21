# Server Entity Spawning / Native View Reverse-Engineering Notes

Last updated: 2026-08-20

## Goal

Make a Sunrise-hosted Destiny 2 activity create the native per-peer replication view required for
server-authored enemies and other entities to appear on the client.

The immediate milestone is completing message 40's five-stage view handshake. Native view
creation and token lookup are now proven; the next layer is the replication scheduler and its
entity-create lane.

## Latest ownership proof and UI diagnostics

The restored broad EDZ arrival produces the audible construction control again. The client-side
overlay snapshot captures the successful object as bound RSAT `0x815B204B` in namespace 1/view 0,
region 408/bubble 51/cell 145, while `primary_world()` reports the player in region 24/bubble 3.
The resulting `OWNER MISMATCH` is the narrowest current explanation for positional audio with no
rendered Vandal. Do not change the proven create/update/RSAT payload until the entity is created in
the renderer's actual current owner or the authored parent/stream-source path is captured.

The location overlay was audited at the same checkpoint:

- Activity and region come from the signed-in player's original joined membership record.
- Bubble is `region / 8`, validated against that destination's scenario layout and bubble hash.
- The former Slice-set line was wrong: it duplicated Region and displayed `region % 8` as state.
  It now uses the actual client-authored teleport fields when present and otherwise a fresh native
  world-manager slice-set observation published on the game thread.
- Closest spawn uses the live player physics position and the destination's spawn bank; its short
  cache now includes the destination stem.
- Session rows formerly came directly from the durable host-session reconnect cache, so departed
  rows could appear to be active. The overlay now filters cache-only rows and labels the remaining
  rows `current` or `overlap`; channel and join still come from the live peer/admitted tables.

Overlay/debug Release SHA-256:
`5ac62efcbd6f0db1c880a32d6783355a17ac62fa478544b822b2d3115d0bf670`.

## Current-region simulation-manager promotion

The owner mismatch is upstream of the dirty service. The runtime contains three manager
containers at `runtime + 0x206C8 + namespace * 0x11E08`; each replicated-object manager is at
container `+0x270`. `FUN_1416CCA40` services only the container whose identity is stored at runtime
`+0x560E0`. That is why a current-view namespace-2 record could decode, promote, and occupy slot 0
without ever reaching `FUN_141717790`, while an outgoing namespace-1/Town record reached type-2,
kind-0, native registration, binding, and positional audio.

Ghidra confirms `FUN_1416EC250(runtime)` is the sole non-constructor writer of `+0x560E0`. Before
changing the identity it calls `FUN_1417030F0(container+0x30)`; that helper only writes byte 1 at
container `+0x15C`. The new `active_manager_refresh` hook preserves the original call and then
reconciles these same two manager-local fields with the player's coherent current region. The
gameplay service reapplies the same guarded choice because native PUBLIC CURRENT may continue to
prefer the outgoing region.

Promotion requires all of the following, otherwise it performs no write:

- bootflow is freshly `in_world`; this keeps initial slice loading fail-closed;
- `primary_world().region` is present. A normal z-leg may legitimately report the new region while
  the retiring native slice still names the old one, so slice equality is diagnostic rather than
  a promotion prerequisite;
- that region resolves to an advertised group with a bound view and held host token;
- the host token has a live entity-manager capture with namespace 0..2;
- the captured manager pointer equals `runtime + 0x20938 + namespace * 0x11E08`;
- the manager container's own identity equals the captured namespace and the old active identity
  is in range.

The two-view experiment now places the create in the selected current view (normally entry 1),
not the outgoing view. Its proven 501-bit body is unchanged in width: the carried signature bit
supplies view-0 event, view 0 writes the five-bit empty remainder, view 1 writes one event-absence
bit plus the 220-bit atomic entity body. The entity region/bubble/cell come from the same current
view. Every create/retry/follow-up path also requires a fresh matching active-manager observation.

Release SHA-256:
`54c55d9b2e6cdc40ac3634181d36506d23f3bc734d7632355bd0909d3c419edf`.

The preceding strict-slice candidate
`8d72493f382fb5382e3a05570eba87892c9b504082ba54509c3741d38a49cfdd` reached the exact
two-view window at `t=89037` with current token `.003`, namespace 2, region 24/bubble 3/cell 11,
but refused promotion because the retiring slice was still 408. The scheduler expanded to three
views 61 ms later, so no entity packet was sent. This proves slice equality cannot gate the normal
in-world z-leg reconciliation.

## Environment

- Repository: `/home/zeex64/Documents/Sunrise`
- Branch: `feat_entity_spawning`
- Ghidra program: `/destiny2_runtime_dump.exe`
- Runtime log: `/home/zeex64/Games/Sunrise/bin/x64/Sunrise/logs/sunrise.log`
- Build directory: `/tmp/sunrise-entity-probe-release-codex`
- Built DLL: `/home/zeex64/Documents/Sunrise/build/x64/Release/steam_api64.dll`
- Deployment remains manual; do not copy the DLL into the game directory automatically.

Last committed entity DLL at this checkpoint (`2ce86a3d test atomic Vandal create update`):

```text
SHA-256 d946ccd816276734d864f0ded9b6da02350ce444266f7798fe5c55d35335c941
```

The uncommitted passive registration/glue-dispatch diagnostic was manually deployed with SHA-256
`bee5df9bdf30023ba09a2d59b543355233d46984415fddc177d65be94197c67c`. That tested DLL reports
dispatcher entry but not its table-write postcondition. Current unbuilt working-tree work adds the
postcondition check; neither candidate is an eventual commit identifier.

The prior 502-bit active-manager test DLL, SHA-256
`5262387a228cf793c17aed6b1d2c54b4d3dcd618636bb02bad5db27ca9163d5b`, exposed the extra appended
target bit described below. The repo and game directory now both contain the runtime-untested final
501-bit Release DLL, SHA-256
`28b14320728d4d2cabd0d0ba8384a4847449ea8f50b37b08e2112573b141bf03`.

## Confirmed high-level path

1. The client joins Sunrise's gameplay group through the advertised method-0 UDP descriptor.
2. The gameplay channel establishes successfully.
3. Group membership and `activity-host` parameters are published.
4. The client creates and joins the public activity client through BAP.
5. Activity message type 12 publishes the root activity membership.
6. `FUN_141702580` synchronizes that membership into native views.
7. For each eligible non-local member, it calls `FUN_141703910`, the native view creator.
8. The creator resolves the member's network address to a channel and requires that channel to be
   valid and established before allocating the view.
9. Only after the view exists can message 40 resolve the token and complete its five-stage
   compatibility handshake.

## Native functions and layouts

Addresses below are from `/destiny2_runtime_dump.exe`.

### Membership-to-view synchronization

- `FUN_141702580`: membership-to-view synchronization pass.
- It iterates occupied/eligible membership peers.
- `FUN_1404dd470`: reads the per-peer view gate.
- The gate is member byte `+0x38`, bit `0x10`.
- The creator receives the member address at `member + 0x142`.
- The creator receives another identity object at `member + 0x1EE`.

### View creator

- `FUN_141703910`: native per-peer view creator.
- `FUN_1417c40f0`: resolves a `0x56`-byte address to a channel index.
- `FUN_1417c35f0`: validates the resolved channel.
- `FUN_1417c4230`: returns the resolved channel object.

Creator prerequisites recovered from decompilation:

```text
resolved channel index != -1
channel lifecycle == 4
channel connection state == 5
```

Channel-manager layout:

```text
slot stride                         0x41F0
lifecycle (slot base relative)      0x30E8
family bitmap                       0x3112
native network address              0x3144 (0x56 bytes)
accessor result                     slot base + 0xA8
connection state                    accessor result + 0x1D18
```

The address resolver ignores lifecycle zero slots, requires the requested native-family bit, and
compares the `0x56`-byte address with `FUN_1403df020`.

`FUN_1417C08F0` is the channel association path that owns those family bits. It searches live slots
by the same `0x56`-byte address; when it finds an exact match, it ORs the requested family bit into
that slot's mask instead of allocating a new channel. With field 11 carrying slot 0's address, the
family-6 and family-7 associations should therefore merge into established slot 0 rather than
creating the zero-address slot 1 seen in the previous run.

## Activity-membership schema discoveries

The relevant nested identity schema is referred to here as B2.

- B2 layout table: `0x1437E2000`.
- B2 has presence-coded fields 0 through 14.
- B2 field 0 is a six-bit scalar and controls the native gate byte.
- Writing field 0 as `0x10` produces `p1_gate=0x10` at the exact native predicate.
- B2 field 1 is type `0x80809EE1`, which wraps a 128-byte block. It is not the gate.
- B2 fields 10, 11, and 12 are type `0x80807C82` and are each `0x56` bytes.
- Their schema storage offsets are `0xBC`, `0x112`, and `0x168`.
- Type `0x80807C82` schema data is at `0x143924878`.

Field 10 was initially encoded as an 86-byte raw NetAddr and the native decoder consumed the packet
cleanly, but the exact `member + 0x142` address passed to the creator remained zero. Parsing the B2
records from their true `0x80` table start resolved the mismatch: field 10 starts at B2 `+0xBC`,
while field 11 starts at B2 `+0x112`. B2 itself starts at member `+0x30` (membership-lane `+0x38`),
so field 11 lands exactly at member `+0x142` (membership-lane `+0x14A`). The writer now places the
NetAddr in field 11.

## Type-12 encoding milestones

### Initial state

The client decoded only its local activity member:

```text
occupied=0x00000001
eligible=0x00000001
p1=0
```

No view-creation call occurred.

### Reflected host member

A second synthetic activity member was added using the held gameplay group session identity.
This produced:

```text
occupied=0x00000003
eligible=0x00000003
```

### View gate

An early experiment set the wrong optional subtree and made the native decoder stop at 1541 bits
because that subtree required an additional 29-bit payload.

The correct six-bit field-0 encoding is:

```text
field present = 1
field value   = 0x10 (6 bits)
```

After that change:

```text
p1_gate=0x10
view-membership ... p1[...] create=1
```

The native view creator began firing.

### Confirmed meaningful bit counts

Before the attempted address field:

```text
foreign two-member membership       30647 bits
root membership with descriptor     31671 bits
```

After adding the 86-byte field-10 payload:

```text
foreign two-member membership       31335 bits
root membership with descriptor     32359 bits
```

The increase is exactly 688 bits (`86 * 8`), and the native decoder reports success. This proves
the raw fixed-array wire grammar is correct.

## Latest runtime diagnosis

The 2026-08-18 EDZ free-roam run reached the real in-world state and completed the native view
handshake. Token `0x9EAA300100200006` progressed through local stages 1, 2, and 5; Sunrise then
reported the view bound with both halves at stage 5 and index 3:

```text
world_controller:state_manager: Entering state 'activity:in_world'
Starting activity ... (grognok: edz_freeroam)
client ... view-state ... local=5 ... compatible=1
server ... stage=view result=bound local=5 remote=5 index=3 token=0x9EAA300100200006
```

The live codec registry exactly matched the four functions recovered in Ghidra. No
`entity-create`, `sobject-create`, or `sobject-update` event occurred, however. The missing enemies
are therefore not evidence of a failed session or view: Sunrise has not yet published an entity
record.

The first `entity-slots` build attached its inbound decoder detour successfully but produced no
snapshot. This is a useful negative result: `FUN_141718510` is not entered until the remote
scheduler actually supplies the direct-entity lane, so it cannot discover a safe handle before
the first server-authored record. The probe now also observes the same authoritative manager
through an existing native view (`view + 0xB8`) during message-40 lookup. This removes the circular
dependency while leaving the inbound detour in place to validate the eventual record.

The corrected probe then captured two live managers and their world-load transition:

```text
namespace=1 before world: free=149 occupied=1  first safe slot=1
namespace=1 in world:     free=137 occupied=13 first safe slot=13
namespace=2 bound view:   free=150 occupied=0  first safe slot=0
```

All reported candidates had an unclaimed descriptor and zero handle, reserved, and object
generations. The EDZ activity view token `0x9EAA300100200003` is the namespace-2 manager and reached
`result=bound`; slot zero is therefore the first authoritative candidate for that view. The
namespace-1 transition independently validates that the free and occupied map offsets are real
rather than constant padding.

`FUN_1417A96D0` supplies the remaining scheduler-lane mapping. Registration stores the view key
from `*(view + 0x48) + 0x20`, stores the tag from `*(view + 0xB8) + 0xD024`, and sorts both the
scheduler-view records and signature entries by that same key. The probe now captures token, key,
tag, namespace, and safe slot together. Sunrise refuses a create unless exactly one entry in the
client's current scheduler signature matches the bound token's captured key and tag.

The first guarded-create run correctly emitted nothing because the client supplied only a
zero-entry scheduler update before the activity view recycled near the end of the run. It also
proved that the native logical scheduler keys are the session tokens themselves, while the old
server probe's 64-bit wire slices were not those logical token values. Schema `0x80806AEA`
transforms or frames those values even though its total width remains fixed.

The scheduler address is now corrected directly from `FUN_1416E80B0` and
`FUN_14172B453`: the view stores its scheduler owner at `view + 0x68`, and the scheduler begins at
owner `+0x38`. The client probe reads the logical header/count/key/tag object there on every view
pump. Separately, Sunrise retains the exact MSB-first encoded signature update received from the
client and replays it after the external scheduler-body-presence bit. A create is allowed only
when the native logical count equals the wire count and the bound token/key/tag has one unique
logical lane. No lane-order guess or server-side schema re-encoding remains in the path.

This run also proves the scheduler signature is dynamic. It first sent an empty 131-bit update
(the one-bit update gate plus a 130-bit zero-entry signature), then a valid 347-bit update with
three registered entries (one update bit plus a 346-bit signature). Any create writer must size
itself from the captured count and echo the complete captured signature; it must not assume the
earlier one-view 203-bit update-plus-schema width.

The field-11 address correction resolved native view creation. The latest run proves all of the
following:

```text
membership-decode ... p1_gate=0x10 addr_hit=330
creator_addr=7F000001007900000000000000000000
view-create result=ok ... channel=0 valid=1 lifecycle=4 state=5 established=1
s0[life=4 state=5 family=0x00D0 addr=1]
view-slots ... views=1 first=00000000047462E0
view-lookup result=found ... view=00000000047462E0
view-signature result=captured ... bytes=15
```

The `view-state` probe resolved the stage-2 deadlock. The native client reported no protocol error
and no signature mismatch. Instead, the two session views exposed opposite halves of an
initiator/receptor mistake:

```text
token ...002 error=0 local=2 local_index=0 remote=1 remote_index=-1 signature=1 bytes=15
token ...003 error=0 local=1 local_index=-1 remote=2 remote_index=1 signature=1 bytes=15
```

For token `...002`, Sunrise sent stage 1 and the native client independently selected index `0`
and advanced its local half to stage 2. Sunrise rejected that repeated stage 2 because the server
record had missed the earlier client stage 1 and still had no chosen index/signature. For token
`...003`, Sunrise prematurely sent stage 2 with index `1`; the native receptor accepted it as its
remote stage, but the client remained at local stage 1 because only an initiator advances the
later stages. Neither side could progress.

Static analysis confirms the roles:

- `FUN_1416F6770` owns the initiator's stage-1-to-stage-2 transition. It waits for remote stage 1,
  chooses the next allowed index, stores it locally, and sends stage 2.
- `FUN_1416F6810` runs only when the view's initiator flag at `view + 0x45` is set. It advances
  stages 2 through 5 and sends every transition.
- Sunrise therefore has to be the receptor: publish/retry stage 1, adopt and validate the
  client's stage-2 index/signature, then echo stages 2 through 5.

The server state machine now implements that receptor shape. It also permits a stage-2 bootstrap
when the activity record was claimed after the native view began: stage 2 is self-contained because
it carries both the selected index and the full 15-byte compatibility signature.

The established slot-0 NetAddr remains:

```text
7F00000100790000000000000000000000000000000000000000000000007F00000100790000000001000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
```

The earlier pre-fix log repeatedly showed the exact receptor failure the new state machine removes:

```text
client ... local=2 local_index=0 remote=1
server ... result=invalid-stage local=1 remote=0 got=2 index=-1 token=...002
```

That earlier pre-probe log has now been superseded by the EDZ run above. The newer log validates
view binding, dynamic scheduler signatures, and the runtime codec table, but still has no native
entity-create activity to mirror.

## Endpoint clarification

The `activity-host` gameplay parameter now publishes the gameplay endpoint (`30976`) instead of the
BAP listener (`30974`). This is correct because the parameter constructs a plain-UDP address.

The activity client's BAP service resolution still reports `127.0.0.1:30974`; that is a separate
service-transport path and is expected.

## Message-40 and post-view path

- `FUN_1416E13E0` is message 40's native receptor. It first resolves the body token through
  `FUN_1416FC4A0`, then passes the decoded stage, view index, optional signature length/bytes, and
  token into `FUN_1416EB4D0`.
- `FUN_1416ECDD0` builds the native outbound 40-byte body. Only stage 2 includes the compatibility
  signature; stages 3 through 5 keep the agreed view index but omit that list.
- `FUN_1416F6CE0` exposes the two useful completion thresholds: both sides at stage 2 open the
  compatible-view gate, while both sides at stage 5 mark replication fully open.
- `FUN_1416F6770` proves the native client initiator chooses the stage-2 index. Sunrise must not
  guess it or advance first.
- `FUN_1416F6810` proves stages 3 through 5 are initiator-driven. Sunrise's receptor echoes each
  stage and marks the view bound only after both halves reach stage 5.
- Once Sunrise marks the view bound, established packets send the native simulation gatekeeper bit
  as enabled. The guarded scheduler path now keeps its presence bit clear until this peer has also
  supplied a valid nonzero registered-view signature; it then echoes that exact signature in an
  empty scheduler frame. Entity records remain disabled.

## Replication scheduler and entity lane

The scheduler is now mapped far enough to exchange a self-gated empty frame and identify the exact
entity record path, but not yet far enough to safely emit a fabricated enemy. Sunrise publishes
`schedulerPresent=1` only after both view binding and a captured nonzero client scheduler signature;
otherwise it keeps the bit clear.

### Scheduler ownership and framing

- The scheduler descriptor is anchored by the `replication-scheduler` string at `0x141C9AD78`.
- `FUN_1416E80B0` obtains the shared scheduler from `FUN_14172B453(viewOwner)`, which is
  `viewOwner + 0x38`; the view stores that owner pointer at `+0x68`.
- `FUN_1417A96D0` registers one established view and its four replication handlers.
- `FUN_1417B0D70` is the outbound scheduler/budget pass. It gathers up to 256 candidates, chooses
  records under the packet budget, writes a one-bit signature-update flag, optionally writes the
  scheduler signature, then appends each handler's pre-encoded bit buffer.
- `FUN_1417A8CE0` is the inbound counterpart. It consumes the signature-update flag, verifies the
  registered-view signature, asks each handler to decode pending 0x84-byte records, applies them,
  and finally releases the temporary records.

The handlers are registered in scheduler order as follows:

| Order | View field | Candidate collector | Outbound encoder | Inbound decoder | Meaning |
| --- | --- | --- | --- | --- | --- |
| 0 | `+0x100` | `FUN_14170C3F0` | `FUN_14171F650` | `FUN_141718AE0` | linked event queue |
| 1 | `+0x140` | `FUN_14170BF30` | `FUN_14171F050` | `FUN_1417183C0` | 32-bit mask/control lane |
| 2 | `+0x0A8` | `FUN_14170C080` | `FUN_14171F200` | `FUN_141718510` | replicated entities |
| 3 | `+0x1680` | `FUN_14170CA60` | `FUN_14171F7F0` | `FUN_141718CB0` | fixed object/control lane |

The client already emits scheduler bodies before the view is bound. The read-only external probe
has observed bodies from 105 to more than 1500 bits, commonly beginning with values such as
`01C8`, `026C`, and `1830`. These are useful captures, but they are pre-bind scheduling traffic and
not yet evidence of an accepted server-authored entity.

The signature object is larger than its 128-bit header. `FUN_1417A96D0`, `FUN_1417B0D70`, and
`FUN_1417A8CE0` establish this scheduler-relative layout:

```text
+0x10  local signature object (0x48 bytes)
  +00  128-bit signature header
  +10  signed per-view entry count
  +18  three 0x10-byte entries: 64-bit view key, 8-bit tag, 7 bytes padding
+0x58  remote signature object (same 0x48-byte layout)
+0xC0  registered scheduler-view count
+0x1DC signature dirty/state flags
```

Registration appends the view key and tag to the local object. The outbound scheduler writes schema
`0x80806AEA` whenever its signature-update bit is set; the inbound scheduler decodes the same schema
into the remote object and then compares the complete header, count, entry key, and entry tag. A
server cannot establish compatibility by echoing only the 128-bit header. The `view-slots` probe
now logs both counts and all three local/remote entries in addition to the headers.

The schema metadata makes the signature wire widths exact, but its 64-bit slices are not raw
logical token values:

```text
128 bits  two schema-encoded 64-bit header slices
  2 bits  registered-view count (0 through 3)
 72 bits  per active entry: encoded 64-bit key slice followed by 8-bit tag
```

The signature object size is `130 + 72 * count` bits; the scheduler's update gate adds one bit.
The observed totals therefore agree exactly with the schema:

| Registered views | Signature object | Update gate plus signature |
| ---: | ---: | ---: |
| 0 | 130 bits | 131 bits |
| 1 | 202 bits | 203 bits |
| 2 | 274 bits | 275 bits |
| 3 | 346 bits | 347 bits |

For one registered view the signature object is therefore 202 bits. `FUN_14171EFE0` writes the
entity lane's one-bit end marker as one, while `FUN_14171F020` writes the other lanes' zero
terminators. Static inspection initially attributed four zero bits to the combined handlers before
`FUN_141718510`; the armed native-reader trace below proves the actual wire boundary consumes
three. A signature-update-only empty scheduler frame for one view is therefore exactly
`1 + 202 + 5 = 208` bits. In general, a signature-update-only empty frame is
`131 + 77 * count` bits, so the three-entry EDZ form is 362 bits.

The empty-stream terminators are also recovered from the four inbound decoders:

- Event lane (`FUN_141718AE0`): a zero presence bit ends the stream.
- Mask/control lane (`FUN_1417183C0`): a zero presence bit ends the stream.
- Entity lane: `FUN_141718D90` first reads a one-bit auxiliary-entity count plus an optional 8-bit
  generation; `FUN_141718510` then uses a one bit as its no-more-records sentinel.
- Fixed control lane (`FUN_141718CB0`): a zero presence bit means no object.

Therefore the minimum known-signature, no-record scheduler body costs one signature-update bit plus
five observed handler bits per registered view. A view can additionally publish one auxiliary
entity index and an 8-bit generation in the entity prelude even when it schedules no entity record.
The remaining prerequisite is knowing the client's registered-view count and current scheduler
signature. The
`view-slots` now follows `view + 0x68 -> owner + 0x38`, logging the scheduler view count,
local/remote in-memory signature objects, and signature flags.

### Entity record grammar

`FUN_14171F200` writes scheduled entity records. It chooses either `anchor-index` or
`entity-index`, then delegates the per-object body to `FUN_14171E5A0`. The receive side mirrors
this in `FUN_141718510`, `FUN_141717EB0`, and `FUN_141718080`.

The decoded 0x84-byte pending record contains these protocol flags:

- bit 0: object creation
- bit 1: object update
- bit 2: a trailing per-object boolean
- bit 3: additional lifecycle state
- bit 6: optional `anchor-entity-index`

An initial one-bit compact form represents an update-only record. Otherwise five explicit bits
carry the flags above. The record then carries the optional anchor entity, an optional 8-bit
generation/variant value, and, for one lifecycle form, a signed 16-bit nested-body length.

The direct, non-anchor record framing is now exact. `FUN_14171F200` writes a zero lane-end bit,
then a one direct-entity selector, followed by `FUN_1404C30D0`'s entity handle. That helper writes
13 low slot bits and then four generation bits; its paired decoder `FUN_1404C16C0` confirms this is
a 17-bit field. After the object body, `FUN_14171EFE0` writes a one to terminate the entity lane.

A create-only record's explicit flag prefix is:

```text
0  use explicit flags (not compact update-only form)
1  create
0  update
0  trailing per-object boolean absent
0  additional lifecycle state absent
0  anchor entity absent
```

With no anchor, `FUN_14171E5A0` next writes the generation-change decision. The create sub-body in
`FUN_14171E240` then writes an 8-bit object generation, the 2-bit codec kind (`0` for sobject), and
the codec's creation payload.

The first-use generations are now exact. The per-slot manager entry is
`manager + 0x114 + slot * 6`. `FUN_14171B1C0` initializes all 0x2000 slots with descriptor index
`0xFFFF`, handle-generation byte `0x10`, reserved-generation byte `0x10`, and object generation
zero. `FUN_141711D10` allocates a free 13-bit slot without changing its handle generation; it
changes object generation zero to two, otherwise incrementing while skipping a wrap to zero.
`FUN_14170FC90` increments the handle generation when the slot is freed. Consequently a pristine
slot's first wire handle is its 13-bit slot plus four-bit handle generation zero, and its first
create core carries object generation two. This proves the generation fields, but does not make an
arbitrary slot safe: Sunrise still needs authoritative vacant-slot tracking before it can choose
one.

The spatial-cell prefix between the explicit flags and the core body is also exact. The native
record descriptor stores a 16-bit cell at descriptor offset `+2`, initialized to `0xFFFF`.
`FUN_14171F200` passes the current world cell as stack argument eight to `FUN_14171E5A0`; it
defaults to `0xFFFF`, or comes from the current local-world entry at `+0x3C` when one is valid. The
encoder writes:

```text
1 bit  record cell differs from current world cell
1 bit  record cell is less than 0x100              (only when different)
8 bits record cell                                 (only when different and small)
```

The receiver in `FUN_141717EB0` mirrors that grammar. When the first bit is zero it copies the
caller's current cell. When it is one and the second bit is zero it uses `0xFFFF`; when both are
one it reads the eight-bit cell. A server-authored create can therefore force the global/default
cell independently of the client's current world with the robust two-bit sequence `1,0`. The
one-bit native minimum `0` is valid only when the current cell is already known to be `0xFFFF`.

For one registered view, a robust create-only kind-0 scheduler frame is therefore 285 bits:

```text
203 bits  signature-update flag plus one-view signature
  3 bits  combined empty-handler prelude before the entity-list decoder
 78 bits  direct create-only entity record, including the explicit default-cell prefix
  1 bit   fixed-control absence
```

The 78-bit record consists of lane-continue `0`, direct selector `1`, the 17-bit entity handle,
six explicit create-only flags, default-cell `1,0`, the 50-bit kind-0 core body, and lane-end `1`.
If the current cell is independently proven to be `0xFFFF`, the unchanged-cell form is 284 bits.
The compact single-bit flag form is update-only and cannot represent this create.

For a dynamic signature count, the target view replaces its five-bit empty body with the 82-bit
create body, adding 77 bits to the empty frame. The currently expected three-view form is therefore
`362 + 77 = 439` bits.

`FUN_14171E240` is the core outbound object-body encoder and `FUN_141718080` is its inbound mirror:

- A create writes an 8-bit generation, a 2-bit replicated-object kind, and calls that kind's
  codec vtable slot `+0x58` for the creation payload.
- An update calls codec slot `+0x68`; the decoder uses the matching slot `+0x70`.
- The decoder resolves the 2-bit kind through `FUN_1416EAE90`, allocates the codec's declared
  create/update buffers, and injects a baseline when creation arrives without an update.
- `FUN_141714840` is the later apply/commit path that merges the decoded records into the local
  object table and clears lifecycle/dirty state.

The encoder's six-argument Windows x64 ABI is now confirmed directly from its body:

```text
uint8 FUN_14171E240(manager, writer, uint32 entity, record,
                    uint64 update_context, uint32 auxiliary)
```

Its entry has the unique fixed pattern
`40 53 55 56 57 41 56 41 57 48 81 EC A8 01 00 00`. A bounded client detour preserves the call
and reports the first successful create body for up to 64 distinct entity handles as
`stage=entity-create`, including the record flags, exact bit delta, writer state, and any bounded
bytes flushed by the call. For a create-only kind-0 sobject, the expected core-body boundary is
exactly 50 bits: 8 generation bits, 2 kind bits, and the 40-bit sobject creation payload. This
probe does not include the outer record flags or the 17-bit entity handle written by its callers.

The 2-bit codec registry is now resolved. An AI enemy's primary replicated object uses the
`sobject` codec (kind 0). A valid, loadable enemy sobject RSAT identifier is the only codec-specific
input now proven necessary for a first create-only record: the native receiver explicitly injects
the codec baseline when no update accompanies creation. A later update is still needed to choose
placement and drive non-baseline state.

The create-body encoder is now tied directly to the scheduler's entity collector rather than a
separate transport:

- `FUN_14170B660` walks dirty replicated objects and builds the scheduler's create/update entries.
- A create-pending object calls `FUN_1417084B0`, which packages `FUN_1417003C0`'s kind, entity id,
  0x400-bit authority/presence mask, optional anchor, and codec `+0x58` payload.
- An update-pending object calls `FUN_141708C40` and the codec's update path.
- `FUN_1416FF790` decodes the create body through codec slot `+0x60`.
- `FUN_1417085C0` validates the decoded record, then calls codec slot `+0xB0` to instantiate or bind
  the native runtime object.
- The collector explicitly warns and schedules a baseline when creation is pending without an
  update, matching the baseline behavior already found in `FUN_14171E240`.

The client-side application chain is now concrete:

```text
FUN_1416FF790 (decode entity creation through codec +0x60)
  -> FUN_1417085C0 (validate and apply through codec +0xB0)
    -> FUN_1417242F0 (sobject instantiate/apply)
      -> FUN_140A01030 (resolve/load RSAT resource reference)
      -> native sobject allocation/construction
      -> FUN_141704870 (bind runtime object id to replicated entity id)
```

The create-only baseline path is equally explicit. `FUN_141718080` emits
`receiving sobject creation without update, will inject baseline`, allocates a zeroed update
record, and calls `FUN_14170B600`; that helper invokes codec slot `+0x80`, the baseline
initializer. The outbound mirror in `FUN_14171E240` has the corresponding send-side warning.
Sunrise should therefore test a minimal kind-0 create before attempting to reproduce the full
transform/update body.

`FUN_1416EFD70`, called during network-manager startup for manager `+0x59820`, initializes the
global codec registry and proves that all four 2-bit values are registered:

```text
+0x00  signed codec count
+0x08  codec pointer 0
+0x10  codec pointer 1
+0x18  codec pointer 2
+0x20  codec pointer 3
+0x28  signed event-codec count
+0x30  event-codec pointer table (22 entries in this build)
```

The concrete registrations are:

| Kind | Name from vtable `+0x08` | Codec object | Vtable | Create out/in | Update out/in |
| --- | --- | --- | --- | --- | --- |
| 0 | `sobject` | `0x142038460` | `0x141CA1550` | `FUN_141726900` / `FUN_1417266B0` | `FUN_141725140` / `FUN_141724FD0` |
| 1 | `squad` | `0x1420484A0` | `0x141CA1758` | `0x141726980` / `FUN_1417268B0` | `0x1417252A0` / `FUN_1417250E0` |
| 2 | `player_broadcast` | `0x142038458` | `0x141CA1420` | `0x1417268E0` / `FUN_141726680` | `0x141725130` / `FUN_141724F80` |
| 3 | `test_entity` | `0x1420484A8` | `0x141CA1858` | default/no-op | default/no-op |

The non-test creation schemas and codec-declared create-buffer sizes are:

| Kind | Creation schema | Buffer size |
| --- | --- | --- |
| `sobject` | `0x80800014` | `0x10` bytes |
| `squad` | `0x80809C42` | `0x08` bytes |
| `player_broadcast` | `0x80806ABD` | `0x08` bytes |

The earlier `+0x18` per-family registration interpretation belonged to a different object. The
view stores a per-family entity manager at `view + 0xB8`; that manager stores the global registry
pointer at `+0x10` and its per-family entity-handler pointers at `+0x18 + family * 8`.

The sobject creation path narrows the server payload substantially:

- `FUN_141726900` encodes schema `0x80800014` from the 16-byte create buffer and appends the bit
  `(buffer[4] & 1)`.
- `FUN_1417266B0` clears the 16-byte destination, decodes that schema, reads the trailing bit into
  byte 4, and resolves the first 32-bit field through `FUN_140A020E0`.
- `FUN_140A020E0` is the sobject RSAT loader and contains the diagnostic
  `sobject rsat loader globals is full!`. The first create field is therefore an sobject RSAT id,
  not an arbitrary entity or activity hash.
- `FUN_140A01030`, `FUN_140A01080`, and `FUN_140A00FE0` index that id with the normal package-tag
  split: the low 13 bits select an entry and the higher bits select the package table. They return
  the resolved native resource handle, baseline byte size, and baseline bit size respectively.
  The RSAT field is therefore a `0x80800000`-based installed tag handle, not a Bungie API hash.
- `FUN_140A020E0` only queues the RSAT in the loader's bounded 0x800-entry list. `FUN_140A02590`
  and `FUN_140A02B50` later batch those ids into native resource-manager requests whose entries are
  `{type=2, id=RSAT}`. A usable create therefore requires that exact installed resource tag to be
  present in the client's package set; substituting an API hash cannot reach object construction.
- General native construction reaches `FUN_1405943F0`, which resolves the construction resource
  and passes its `+0x88` RSAT field to `FUN_140A03020`. The latter has one executable caller, takes
  the new native object id in `ECX` and the resolved RSAT directly in `EDX`, and begins with the
  normal package-tag split. Its entry signature is unique at `0x140A03020` in this image.
- `FUN_141723FD0` derives the remaining local creation data from the resolved RSAT definition.
  The id must already name valid loadable sobject data on the client.
- `FUN_1417269D0` is the matching native outbound create-buffer constructor. It zeroes all 16
  bytes, resolves the replicated entity to its runtime sobject through `FUN_1417042E0`, copies the
  sobject record's RSAT id from offset `+0x88` through `FUN_1409FED40`, computes the byte-4 flag,
  and then calls `FUN_141723FD0`.

The resulting local create-buffer layout is:

```text
+0x00  uint32 sobject RSAT id       (wire schema 0x80800014)
+0x04  uint8 trailing identity flag (separate one-bit wire field)
+0x08  uint32 derived byte offset/size
+0x0C  uint32 derived bit offset/size
```

The last two fields are derived from the resolved RSAT on both sender and receiver. They are local
codec bookkeeping, not values Sunrise should choose. The `sobject-create` boundary probe can
confirm whether a native outbound exemplar uses an identity or runtime-remapped tag.

### Installed-package RSAT investigation

The installed package set contains 528 current package families and roughly 2.9 million entry
records. A complete metadata scan found zero entries whose package class is `0x80800014`. That id
is the sobject creation wire schema, not the resource class of an sobject RSAT, so class-scanning
for it cannot produce spawn definitions.

The working `edz_freeroam:scenario_client` (`0x80B2F00A`) was then scanned through its complete
installed package graph. Its 37 registries contain 773 object containers and 13,149 placed
handles. Of those, 431 placements use the NPC component class `0x80806382`, reducing to 24 unique
loadable `0x80809BB6` sobject RSATs:

| RSAT | Object definition | Placements | RSAT | Object definition | Placements |
| --- | --- | ---: | --- | --- | ---: |
| `0x80C4FE6B` | `0x80B8341A` | 1 | `0x80C4FE6C` | `0x80B8341C` | 2 |
| `0x80C4FE81` | `0x80B837B0` | 1 | `0x80C4FE9F` | `0x80B837E3` | 3 |
| `0x80C4FEAD` | `0x80B83809` | 24 | `0x80C4FEB5` | `0x80B8381A` | 1 |
| `0x80C4FEC5` | `0x80B8382E` | 1 | `0x80C4FEC7` | `0x80B83830` | 6 |
| `0x80C4FEEC` | `0x80B83865` | 29 | `0x80C58297` | `0x80B83DA3` | 1 |
| `0x80C58320` | `0x80C5CE43` | 3 | `0x80C58334` | `0x80BEA05E` | 21 |
| `0x80C58335` | `0x80BEA060` | 4 | `0x80C58337` | `0x80BEA062` | 23 |
| `0x80C5833D` | `0x80BEA064` | 42 | `0x80C5833E` | `0x80BEA066` | 30 |
| `0x80F44DC1` | `0x80BEA90E` | 27 | `0x80F44DC2` | `0x80BEA910` | 3 |
| `0x80FB4182` | `0x80C0090B` | 2 | `0x80FB4185` | `0x80C0090D` | 6 |
| `0x80FB418D` | `0x80C00914` | 19 | `0x80FB4B95` | `0x80C0190C` | 176 |
| `0x80FB4B96` | `0x80C0190E` | 3 | `0x80FB9FC7` | `0x80F67021` | 3 |

The same runtime registered 18 native RSAT dependencies. Every tag resolves to class
`0x80809BB6` and points back to a `0x80809C0F` definition, validating the static class and
back-reference interpretation. None is one of the 24 placed EDZ NPC RSATs above, so those live
registrations are generic activity/sandbox dependencies rather than a trustworthy enemy exemplar.
They must not be selected blindly for the first create.

A concrete combat activity was then traced locally without running the game:

```text
scenario       0x80F363C6  gambit_badlands_cabal:scenario_client
scenario class 0x80809994
slice          0x80F363C5
registry       0x80F366CD
objects        0x80FA262B, 0x80F366D7, 0x80F366CC
```

The large object `0x80F366CC` has registry key `0x2A97039E` and hundreds of placed handles across
its global and bubble-0 groups. Representative handles follow the existing Sunrise package-reader
chain exactly:

```text
0x80809468 indirect handle
  -> 0x80809B14 redirect
    -> 0x80809C36 descriptor blob
```

For example, handle `0x80F366D5` reaches descriptor blob `0x80F366CB`, which declares component
class `0x80809A3B`, sense schema `0x80807ECC`, and auth schema `0x80807EC9` for registry key
`0x2A97039E`. Environment handle `0x80FD3CE2` reaches `0x80FD3CE0`, which declares component class
`0x80806382` with sense/auth schemas `0x8080626A` and `0x8080626B` for the same key.

The full sweep reduced 732 placed handles to 22 distinct component/schema layouts. One layout is
NPC-specific and crosses the final static RSAT boundary:

| Placed handle | Component descriptor | Object definition (`+0x60`) | Object class | Definition `+0x88` | RSAT class |
| --- | --- | --- | --- | --- | --- |
| `0x80FD3CE2` | `0x80FD3CE0` | `0x80EC0835` | `0x80809C0F` | `0x80FCC6C6` | `0x80809BB6` |
| `0x80FD3CE5` | `0x80FD3CE3` | `0x80EC0839` | `0x80809C0F` | `0x80FCC6C7` | `0x80809BB6` |
| `0x80FD3CE8` | `0x80FD3CE6` | `0x80EC083D` | `0x80809C0F` | `0x80FCC6C8` | `0x80809BB6` |

All three placed descriptors use component class `0x80806382`, sense schema `0x8080626A`, auth
schema `0x8080626B`, and slot type 43. Their three `0x80809C0F` definitions are equal-sized
1,284-byte entries in `w64_npcs_0360_5.pkg`. At exact serialized offset `+0x88`, they carry the
three sequential 160-byte `0x80809BB6` resources above from `w64_npcs_03e6_5.pkg`. Each RSAT
resource points back to its corresponding object definition at its own offset `+0x08`.

This matches the native path instruction-for-instruction: `FUN_1417269D0` resolves the replicated
entity to its native object, and `FUN_1409FED40` copies the tag at the resolved object definition's
runtime offset `+0x88` into the sobject create buffer. The Ghidra class registry gives
`0x80809C0F` a decoded runtime size of `0xA0`; `FUN_14055AA30` registers its loader callback and the
adjacent `0x80809BB6` resource callback. `FUN_140556EC0` then prepares the `0x80809C0F` object's
RSAT-derived byte-size/alignment layout at runtime offsets `+0x90` and `+0x94`. The package offset
and runtime access therefore agree: `0x80FCC6C6`, `0x80FCC6C7`, and `0x80FCC6C8` are the first
statically derived, loadable sobject RSAT candidates for this Cabal activity.

The activity's handles, redirects, descriptor blobs, component classes, schemas, and
`0x80809C0F` definitions are upstream metadata and must not be substituted into the 40-bit create
payload. The three `0x80809BB6` candidates still need one native `sobject-native` or
`sobject-create` capture to identify the exact combatant represented by each and to confirm whether
the runtime remap table preserves the installed tag on wire. Static derivation is strong enough to
focus the next run, but entity emission remains disabled until that confirmation and safe slot
ownership exist.

The generic schema dispatcher now makes that width exact. Schema `0x80800014`'s only field uses
dispatcher type `0x18`; `FUN_1409F9350` first writes `FUN_1409FEC90`'s six-bit value. An installed
package tag whose top bits are `0x80000000` selects discriminator `0x16`. Its case
`FUN_1409F90A0` then writes a one-bit non-null marker and the 32-bit tag returned by
`FUN_1404CE050`. The latter is an identity transform when the runtime remap table is empty, but can
substitute a mapped tag when that table is populated. Thus a valid RSAT consumes 39 schema bits,
and the codec's separate identity flag makes the complete sobject create payload exactly 40 bits.
The native probe still needs to confirm whether the remap table is identity in this runtime.

Static schema metadata now separates the value presented to the generic codec from its local
scratch allocation. The same metadata fields agree with the known raw 86-byte NetAddr schema and
with the other two replicated-object creation codecs:

| Creation schema | Value bytes | Local scratch bytes |
| --- | ---: | ---: |
| sobject `0x80800014` | `0x04` | `0x10` |
| squad `0x80809C42` | `0x08` | `0x08` |
| player-broadcast `0x80806ABD` | `0x08` | `0x08` |
| raw NetAddr `0x80807C82` | `0x56` | `0x56` |

This proves that only the first four bytes of the sobject create scratch record are handed to the
schema before the separate flag bit. The table-driven encoder's six-bit discriminator and one-bit
non-null marker mean the field is not merely an unframed 32-bit scalar, even when the final mapped
tag value is unchanged.

The sobject update path also identifies the baseline state an enemy needs. `FUN_141725140` encodes
the named `transform`, `parent`, and `stream-source` components before its remaining sobject update
body; `FUN_141724FD0` decodes the same components. Squad updates use a named `squad` component, and
player-broadcast updates use a named `player` component. A squad relationship for AI is plausible
but not yet proven as a mandatory creation prerequisite.

The three named sobject component schema ids are now resolved from their descriptor tables:

| Component | Schema | Value bytes | Scratch bytes | Scratch offset |
| --- | --- | ---: | ---: | ---: |
| transform | `0x80809F75` | `0x20` | `0x20` | `0x00` |
| parent | `0x8080949B` | `0x28` | `0x4A` | `0x20` |
| stream-source | `0x8080949A` | `0x08` | `0x14` | `0x70` |

`FUN_141725140` aligns those scratch records to 16 bytes. After the three named records, the
RSAT-defined update body begins at scratch offset `0x90`. Each named component is encoded as a
presence decision followed immediately by its schema payload when dirty; the corresponding sent
mask is updated after successful encoding. This means a valid baseline is not simply three fixed
presence bits followed by three unconditional structures.

Kind 1 (`squad`) is a separate replicated-object lane, not an embedded requirement in the sobject
create payload. Its native outbound constructor `FUN_141726A50` fills the eight-byte creation
buffer as follows:

```text
+0x00  uint32 native squad field +0x30
+0x04  uint8  native squad field +0x34
+0x05  uint8  padding/reserved
+0x06  uint16 native squad field +0x36
```

The create schema is `0x80809C42`. The named squad update component uses schema `0x80807EB9`,
with a `0x300`-byte value and `0x505`-byte local scratch record. Most importantly, squad codec slot
`+0xB0` is `FUN_141724A80`: it resolves an already-existing native squad from that eight-byte
identity and binds the replicated id; it does not construct a native squad from scratch. Nothing
currently proves that a kind-1 squad record must precede a first enemy sobject. Defer squad
replication until a minimal sobject create has been accepted, then inspect whether the spawned
definition expects an existing activity-owned squad relationship.

The next diagnostic build follows `view + 0xB8 -> entity manager +0x10 -> global registry` without
calling native code. It logs the live count plus Ghidra-relative vtable/create/update RVAs as
`stage=view-codecs` and `stage=view-codec`. A correct runtime capture should report `count=4` and
match the four static registrations above.

The same build hooks only the kind-0 outbound create encoder and reports each distinct native
16-byte create input once as `stage=sobject-create`. It records the RSAT id, trailing flag, exact
bit-count delta, accumulator state, and any bytes flushed by the call. It does not change the
payload or enable scheduler injection.

The follow-up diagnostic build also hooks `FUN_141725140`, the kind-0 outbound update encoder, as
`stage=sobject-update`. It records at most 32 distinct inputs, including the create identity, the
first 16 bytes of both native dirty/sent mask objects, the component scratch base, exact bit-count
delta, accumulator state, and up to 64 flushed bytes. The hook calls the original encoder first
and never edits its context, masks, or writer.

The server's external-body probe records the entire remaining external trailer after the two
gatekeeper/presence bits. It retains up to 256 bytes, preserves a final partial byte as an
MSB-aligned value, and reports the original/captured bit counts plus a truncation flag. This data
was originally labeled `stage=scheduler-body`, but later native lane-finalizer measurements prove
that the retained range extends beyond the scheduler into reserve/padding. New builds label it
`stage=scheduler-tail`; only its starting boundary is proven.

The copied-reader path originally treated the bits after schema `0x80806AEA` as a two-bit view
count followed by 64/8-bit view entries. The 2026-08-18 run disproved that layout: while the native
scheduler held one logical view, that parser reported three entries and values unrelated to any
session token. The create gate correctly refused to emit an object.

Ghidra confirms why. `FUN_1417B0D70` writes one dirty/update bit and, when set, calls
`FUN_1404C78B0(schema=0x80806AEA, value=scheduler+0x10, writer, mode=1)`. The logical count at
`scheduler+0x20` and entries at `scheduler+0x28` are not fields in that schema; handler output
begins immediately after the variable-length schema value. The apparent count was therefore the
first two handler bits.

The client now hooks only the generic schema wrapper, filters for `0x80806AEA` in output mode, and
measures the exact native writer bit delta as `stage=scheduler-native-signature`. The server uses
that observed delta to retain the dirty bit plus exactly one encoded schema value. It pairs those
wire bits with the native logical count/key/tag list only when the retained 16-byte value matches
the current scheduler value. This keeps variable-length encoding and logical lane identity
separate and prevents stale signatures from selecting a recycled view.

The first exact-signature emission reached the intended namespace-2 scheduler lane. The server
reported `stage=entity-create-out result=sent`, and the client accepted the echoed remote
signature and logical view entry. It then reported that the replication scheduler prematurely
stopped reading the packet; slot 13 remained pristine and no entity/create codec boundary fired.
This moves the fault past signature framing and lane selection into the four-handler body.

The entity-list hook now arms only when the server writes a guarded create and records up to four
subsequent namespace-2 decoder calls as `stage=entity-list-decode`. Each report includes the
native reader's total/loaded/pending bit state and accumulator before and after
`FUN_141718510`, plus its result and decoded-record count. This distinguishes an entity-lane
alignment error, an early body rejection, and trailing unread bits without changing live reader
state.

The first armed trace resolved that ambiguity. At the server-authored create boundary,
`FUN_141718510` returned result 2 with count 0 after consuming exactly 19 bits. That is its
`0,0` anchor form followed by one 17-bit handle; it never entered `FUN_141717EB0`. The reader
accumulator also began with `00`, while Sunrise intended `0,1` for a direct record. Therefore
the combined handler prefix contained one extra zero immediately before the entity lane. Sunrise
now emits three zero prefix bits instead of four for both empty and create views, moving the direct
selector to the exact native boundary without changing the proven 78-bit entity record.

The next run reached the intended codec boundary. The first namespace-2 create consumed 77 bits
before `FUN_141718510` returned result 2, and the client emitted the decisive diagnostic:
`sobject rsat tag 0x80c4fead not loaded, can't decode this packet`. This proves the selector,
17-bit handle, explicit flags, default-cell prefix, generation, kind, and 40-bit sobject payload
all reached the native sobject decoder. The missing 78th bit is the entity-lane terminator, which
the decoder does not reach after the codec rejects the unloaded resource.

`FUN_1417266B0` calls `FUN_140A020E0` before testing the tag with `FUN_140A01C70`; the first
decode therefore queues the RSAT even though that packet cannot complete. Sunrise now makes at
most four attempts, two seconds apart, against the exact same view token, slot, and handle
generation. Every retry revalidates the native pristine candidate. If the slot is accepted,
occupied, or recycled, its first-candidate identity changes and retries stop rather than advancing
to a different slot.

The first retry run exposed a separate zone-load race. Creates for namespace 2 began at pristine
slot 0 while the activity view was still empty. Native EDZ initialization then populated slots
0 through 12, so the exact-slot safety check correctly stopped each retry sequence before its
fourth attempt. Sunrise now waits until the namespace-2 occupied map reaches the observed EDZ
baseline of 13 objects before sending the first guarded create. This keeps the probe on the first
post-baseline candidate, normally slot 13, instead of competing with native zone setup.

The settled-baseline run reached slot 13 and kept the exact token, slot, and generations across all
four attempts. It also exposed why creation appeared to require moving between zones: the first
attempt was only evaluated while the peer already owed an acknowledgement, resend, or retry. The
slot became ready at `t=141324`, but unrelated transition traffic did not wake the create until
`t=141342`. The service loop now polls only the guarded first-attempt predicate, so a ready idle EDZ
view emits immediately without movement.

That run was force-closed after the main activity/network path stopped during a public-bubble
session-ID transition. It did not reach the entity-list decoder, so the freeze was not caused by
native sobject decoding. Diagnostic pressure was nevertheless excessive: 9,390 cosmetic channel
name-change lines plus roughly 1,400 full external scheduler tails were synchronously copied to the
log in 157 seconds. Sunrise now suppresses that cosmetic retail line and retains the external-tail
diagnostic only on signature updates. If the freeze reproduces with that pressure removed, capture
the stopped thread rather than attributing it to the entity codec.

The reduced-log run reproduced the freeze and provided the native watchdog diagnosis:
`network_update` stalled in `activity:in_world` immediately after the target `group_target` session
identifier changed. No namespace-2 create or entity-list decode had occurred. Both frozen runs ended
on the first retail line emitted by that session reset. The retail-log detour used to call all 26
native category-verbosity setters after capturing every outer log line; when its two-second refresh
became due inside this critical transition, a setter could re-enter native logging while
`network_update` still held its subsystem state. Moving that refresh to Sunrise's Steam callback
service removed the direct retail-log re-entry, but the next run still froze because the callback
service itself runs inside `network_update`. Sunrise no longer calls the native verbosity setter at
all; the retail observer now captures only the categories Destiny emits under its own configuration.

The same run exposed a second unsafe transition-time mutation. Destiny's family-zero producer had
already upserted the account entry at `t=33150` (`countA=1`, `countB=1`, `mask=4`) and later advanced
it to `mask=5`. When public-bubble churn changed `countA` to 2 while `countB` remained 1, Sunrise
continued replacing the entire source list every few frames; rewrite 358 landed only 380 ms before
the final `group_target` reset stalled. Ghidra confirms the hooked sweep at `0x140BE6240` obtains the
source list through the call at `0x140BE6284`, compares all `0x210` bytes against its held copy, and
later copies the complete `0x210`-byte source record into that copy. The seed is now strictly a
bootstrap: producer mask bit 4 permanently latches native ownership, after which Sunrise observes
the list but never rewrites it, including during legitimate counter mismatches.

A live Linux GDB attachment confirmed the Wine host was waiting but could not recover useful Windows
frames because this Proton process presents the x64-32 target mismatch. A scripted WineDbg attach did
not detach safely and the helper cleanup terminated the already-hung game. Do not repeat that attach
method; use code breadcrumbs or a prepared WineDbg GDB proxy for any later stopped-thread capture.

The native-ownership run confirmed the family-zero retirement works: `native-owned` appeared once at
`t=72164` with mask 4, mask 5 appeared later, and no seed rewrite followed. The client then completed
several public-region joins and reached namespace-2 decode. Its only guarded entity create was sent at
`t=198430` and failed on the already-known unloaded `0x80C4FEAD` RSAT. The eventual stall began more
than 17 seconds later, so neither that create record nor its decoder call caused this freeze.

The post-watchdog run no longer froze and separated scheduler delivery from RSAT readiness. A create
for token `0x9EAA300100200002` at logical scheduler entry zero reached `FUN_141718510`, consumed the
expected 77 bits, and again queued unloaded RSAT `0x80C4FEAD`. By contrast, four nominal sends for
token `0x9EAA30010020000A` targeted entry one and then entry two but never reached the create codec;
the only immediate namespace-2 decode consumed one empty-list bit. The later 19-bit failures occurred
during group-session teardown with the reader already past its loaded bits and are not evidence of a
different create grammar.

`FUN_1417B0D70` confirms that the scheduler writes each registered view's four handler bodies in
logical order after the signature schema. Runtime evidence currently proves the entity boundary only
for logical entry zero; the guessed empty body for a preceding view is not proven. The guarded probe
now waits until namespace 2 is scheduler entry zero before starting its bounded retry sequence. This
ensures every counted attempt can actually enter the native sobject decoder and give the RSAT loader
time to service the queued dependency.

The next run converted that provisional entry-zero result into a successful native object. Attempt
one for token `0x9EAA30010020000F` was emitted while the local scheduler list was `[00F]` but the
remote list still held `[00C,00F]`; namespace 2 therefore decoded one empty-list bit. Attempt two
arrived after both sides converged and completed the full 78-bit record with `result=0 count=1`.
The client invoked the kind-0 sobject create codec for `0x80C4FEAD`, and the namespace-2 manager
advanced from 13 to 14 occupied objects with slot 14 becoming the next candidate. Attempts three
and four correctly did not fire because slot 13 had been accepted rather than remaining pristine.

This proves that movement was only exposing transient scheduler arrangements, not a spawning
requirement. It also weakens the earlier entry-zero-only inference: even entry zero decoded empty
while the remote logical list lagged. The create gate now requires the native local and remote
signature values, counts, keys, and tags to agree before any view index may emit. The decoder probe
also captures the successful record's bounded create and baseline-update scratch so the next step can
construct a transform update for the accepted but currently invisible sobject.

The later freeze has a narrower boundary. A PUB448-to-PUB96 move preempted the previous transition,
force-disconnected its still-leaving group, and successfully established the reused PUB96 session.
The last native line at `t=215700` says it is queuing join-complete. Every successful join immediately
followed that line with `sending initial join-complete`; the failed one never reached that line, and
the first `network_update` watchdog assert arrived exactly 20 seconds later. Retail capture now keeps
lock-free entry/return serials around the original native enqueue. The first watchdog occurrence emits
`stage=network-hitch`. Nonzero `native` proves the original native enqueue is still inside; nonzero
`observer` with `native=0` isolates Sunrise's capture after that return; both zero place the stall
after the last completed retail-log call.

That run also exposed an independent reliable-channel flood. One unfinished view remained at local
stage 4 / remote stage 4 while the client repeatedly restarted at stage 1. Sunrise treated each lower
stage as accepted but answered it with stage 4 about 15 times per second, so the pair could never
converge. A stage-1 regression on an unbound view now clears the stale stage, index, and signature and
restarts both sides at stage 1. The inbound view report carries `restart=1` when this recovery fires.

The following run isolated a second, more precise stage-4 failure. The client and Sunrise advanced
normally through stages 1, 2, 3, and 4. If the client's local readiness work took longer than the
host's 250 ms retry interval, Sunrise resent stage 4. The native client accepted the first stage 4
but rejected the duplicate immediately:

```text
client ... view-state ... local=4 ... remote=4 ... compatible=1 open=0
client ... view ... mode=4/id=66 -> mode=4/id=66 establishment error ... dying
server ... view ... got=1 ... restart=1
```

`FUN_1416EB4D0` confirms that an initiator accepts the ordered 3-to-4 remote transition but treats a
second remote stage 4 as invalid. `FUN_1416F6810` may legitimately leave the local side at stage 4
while its readiness scan and simulation lifecycle gate are pending, then advances it to stage 5.
Message 40 already rides the reliable gameplay channel, and an inbound later stage proves that the
previous receptor stage arrived. Sunrise therefore retries only stage 1, whose body may be dropped
before the native token-to-view lookup exists; stages 2 through 4 are emitted exactly once per
initiator advance.

The frozen session did not enter the experimental entity path: it contained no
`entity-create-out`, `entity-list-decode`, or `entity-record` event. It instead stopped during a
PUB96-to-PUB24 citizen transition after `peer join successful`, `peer-established`, and the bubble
reservation update, while the stage-4 view reset loop was saturating the same reliable gameplay
channel. The entity-record capture cannot be the cause of that lockup because its hook never ran.

With duplicate stage retries removed, the next run proved that view recovery and the transition
freeze are separate. The new namespace-2 view advanced directly through stages 1 to 5 and bound at
index 2 without an establishment error. A later PUB24-to-PUB96 citizen join again stopped at
`Queuing join-complete`, before `sending initial join-complete`; no entity create or record decoder
ran near the stop. The stage-4 fix is therefore effective, but not sufficient for this older native
`network_update` stall.

Retail capture now records the main-image caller RVA and Windows thread id for every native line.
The application-state site that remains live after the networking thread stops also embeds the
previous entry/return snapshot before replacing it. On the next freeze this directly identifies the
Ghidra function containing the join-complete queue log and distinguishes a native enqueue stall, a
Sunrise post-enqueue capture stall, and a return into the surrounding session update.

## Instrumentation added

Client hooks now cover:

- View-message lookup and message-40 routing.
- Native message-40 error, local/remote stage, index, signature, compatibility, and open state.
- Live replicated-object codec count, vtable, create, and update entry points.
- Bounded, read-only native sobject creation inputs and their exact encoded bit deltas.
- Bounded, read-only native sobject update masks and their exact encoded bit deltas.
- Bounded, read-only successful native entity-create bodies, including the six-argument encoder
  context and exact body bit delta (`stage=entity-create`).
- The first native RSAT dependency registration observed for up to 4096 distinct tags, before any
  dependency on a functioning replication view (`stage=sobject-native`).
- A bounded, read-only snapshot of each view or inbound decoder entity manager's 8,192-slot free
  and occupied maps, including the first eight slots whose descriptor is unclaimed and their three
  generation fields (`stage=entity-slots`).
- A token-bound capture of the native scheduler's logical count/key/tag list and safe slot, used
  only when the target has one unique logical lane; it now includes the full 16-byte local
  signature value (`stage=entity-view`).
- The exact bit delta of native schema `0x80806AEA` output plus its 16-byte input value
  (`stage=scheduler-native-signature`).
- Up to 2048 exact remaining external-trailer bits after the gatekeeper/presence prefix.
- View-slot manager state plus scheduler view count, complete local/remote signature objects, and
  flags.
- Membership-to-view synchronization predicates.
- Type-12 native wire decoder bit consumption.
- Fully decoded type-12 membership snapshots.
- Native view-creator result and channel prerequisites.
- Activity-host parameter decoding and host-link callbacks.
- Capturing the runtime view compatibility signature.
- One-shot full address dumps for requested, slot-0, and slot-1 NetAddr blobs.

Server-side diagnostics cover:

- Gameplay association and reliable transport establishment.
- Group membership, player, parameter, and peer-establish messages.
- Activity-host session allocation/publication.
- Message-40 routing and staged responses.
- Type-12 membership publication and bit-count-sensitive bodies.

## Zone-transition reliable packet fragmentation

The run that eventually disconnected from EDZ isolated the transition failure below the group and
entity layers. The inbound large reliable queue advanced normally through sequence `1360`. At
`t=103269`, three established payloads were rejected as `reason=grammar`. The next decoded packet
began at reliable sequence `1628`, while Sunrise still waited for `1361`; all 14 of its records and
every later reliable record were consequently outside the 32-slot receive window. The server still
acknowledged the newer packet sequence, so the missing transition packet aged out and the reliable
stream could never recover. Later citizen joins reached `sending initial join-complete` but their
messages were refused against the same stale `next=1361`, ultimately producing the EDZ disconnect.

Ghidra resolves the three grammar failures as native fragmented established packets:

- `FUN_1416D4A30` consumes the established marker and selects the fragmented path with the second
  bit.
- `FUN_1416D4B00` reads a 6-bit fragment-set id, 2-bit connection guard, 3-bit
  fragment-count-minus-one, and 3-bit fragment index. Together with the first two bits this is an
  exact two-byte header.
- That function keeps eight overlapping sets, strips the two-byte header, copies each body at a
  1,238-byte stride, and passes the completed original packet back through `FUN_1416D56C0`.

Sunrise previously required the fragmented bit to be zero and therefore discarded every piece.
The server now reconstructs the native eight-piece format before ordinary established decoding.
The decoded-record and reliable receive capacities are 512, covering the observed 267-record
transition burst, and reliable message assembly is bounded by the complete reconstructed packet.
All messages drained from the burst are retained for group dispatch; the former eight-message
report buffer no longer silently discards the remainder. The next runtime proof should show
`stage=fragment result=held` followed by `result=complete`, then a packet whose reliable `next`
advances beyond `1361` with `drop=0`.

The next run supplied that proof. Fragment set 1 completed from 1,238-byte and 688-byte pieces into
a 1,926-byte established packet. Both the citizen join already in flight and a later public-region
join advanced from queued through sending to received, while the reliable packet reports remained
at `drop=0`.

## Multi-view scheduler timeout containment

The same run separated a later disconnect from DTLS and reliable fragmentation. A one-view
namespace-2 scheduler packet first queued `0x80C4FEAD`, and its second guarded attempt decoded one
complete 78-bit entity record. The client invoked the sobject create codec and advanced the manager
from 14 to 15 occupied slots. This is the first complete server-authored object allocation.

The disconnect began only when the scheduler expanded from one logical view to two. At `t=146142`
the client partially applied the server packet: its remote signature and view list converged to the
two local entries. Native channel statistics nevertheless treated that packet and the following 68
repeated scheduler packets as corrupt. The last valid receive remained the preceding stage-2 view
packet, so the channel closed on its exact four-second receive timeout. An earlier run has the same
shape: two-view convergence is visible, but the receive clock never advances and 126 packets are
reported corrupt.

That partial mutation is not proof of full packet acceptance. The signature prefix is correct, but
at least one multi-view handler or final scheduler boundary remains wrong. Sunrise now publishes a
scheduler body only when the native registered-view count is exactly one. Entity-create preparation
uses the same gate, so a suppressed multi-view packet cannot consume a retry or claim a create was
sent. Ordinary acknowledgements continue with `schedulerPresent=0` during multi-view transitions,
which should keep the gameplay channel alive while that tail is reverse engineered.

The next run confirmed multi-view containment: it crossed several EDZ regions without a corrupt
gameplay packet timeout. Its final `connection_failure_suicide` followed the client disabling the
world and gracefully leaving its sessions, so it is not the earlier four-second channel failure.
That run also exposed a namespace-1 interval with one scheduler view, matching local and remote
lists, and a pristine slot. A provisional build allowed the guarded create there to test whether the
four repeated handlers were namespace-independent.

That provisional runtime condition is not safe for the currently reconstructed body. At `t=116583`
the server sent the first namespace-1 create after both layouts converged on token
`0x9EAA300100200006` and slot 13. No `entity-list-decode`, kind-0 codec, or `entity-record` event
followed. Every later gameplay packet was rejected, producing 91 corrupt reads and an exact
4,001-ms receive timeout at `t=120554`. Attempt two was merely sent into the already-stalled receive
path. Namespace 2 remains the only lane that has completed the entity-list and kind-0 create
decoders, so entity emission is again restricted to namespace 2 as well as exactly one scheduler
view.

Ghidra adds an important qualification: inbound `FUN_1417A8CE0` iterates the registered views at
`scheduler + 0xC0` and invokes the same four handler virtuals for every view; it has no namespace
branch. The namespace-1 failure is therefore evidence against that *transition sample*, not proof
of a different namespace wire grammar. The scheduler lists first agreed at `t=116578`, only 5 ms
before Sunrise emitted the create, while a citizen handoff was still in progress. The server did
not finish the group join until `t=116801`, and the replacement activity view appeared immediately
afterward. The server kept retrying that replacement view roughly every 255 ms while the poisoned
channel was still receiving ordinary packets. Sunrise now refuses to arm entity creation while any
reliable membership, join, or view control record is queued or awaiting acknowledgement. Once that
control queue is empty, the exact token, slot, generation, and all existing preparation gates must
remain continuously valid for another 500 ms before the first create may leave. An idle stable zone
is polled during that interval; a handoff cannot make entity data overtake its topology records.

Seven later native registrations and create-codec captures resolved to tags `0x815AA673`,
`0x80FC45CE`, `0x815AA68E`, `0x8157E74D`, `0x8157E747`, `0x815A6C5E`, and `0x815B16E9`.
Their package ids are `0x06D5`, `0x03E2`, `0x06D5`, `0x06BF`, `0x06BF`, `0x06D3`, and `0x06D8`;
all installed files are `w64_environments_*`. They are zone-streamed environment objects rather
than combatant exemplars. The delayed `sobject-create` calls reflect later authority/transition
serialization of objects first registered during streaming. `sobject-native` deliberately records
each RSAT only once per process, so remaining in or revisiting an already-seen zone is expected to
produce no repeated line. Future reports include the decoded package and entry ids directly.

The next safe-build run produced no `entity-create-out`, entity-list, or sobject-create event before
its freeze, excluding the guarded entity body as the trigger. It instead completed one `PUB448.4`
transition, immediately started the same region and group session again, force-disconnected the
still-leaving target, and logged `Cannot create: Managed session with this identifier already
exists`. The replacement peer join reached `Queuing join-complete` at `t=116024` but never reached
`sending initial join-complete`. After several application suspend-state changes, the native
watchdog reported `network_update` continuously stalled from `t=136023` through at least
`t=159395`. Its progress snapshot had `observer=0 native=0`, so neither retail-log capture nor a
native function called by that observer held the thread.

Ghidra resolves `network_update` to the job body at `FUN_1416D2320`. Its direct call
`FUN_14175E520` is the managed-session state pump, and that pump calls `FUN_141792840`, whose retail
site emitted the duplicate-identifier warning. A diagnostic-only detour now records lock-free
enter/return serials around `FUN_14175E520`. The first watchdog line reports them as
`managed=active/entered/returned/thread`: `active=1` with `entered=returned+1` proves the freeze is
inside this pump, while `active=0` excludes it and directs the next split to another
`network_update` child.

## External authored-content research

`D2-Server-Infrastructure.pdf` describes a separate client-side requirement for complete activity
content. Its central distinction agrees with the runtime boundary seen here: a public/peer route can
create a live world session and replication slots, while identity 1 must enter authored mode 1 for
the mission director, activity script, encounters, AI, and cinematics to be constructed. A single
accepted kind-0 sobject therefore proves transport and object replication, but does not by itself
prove that an enemy actor or its encounter logic exists.

The document is Towerfall-specific, so its conclusion cannot yet be applied wholesale to EDZ patrol.
Its address base is compatible with this pinned executable (`read_bits` RVA `0x3513B0` matches
`FUN_1403513B0`), making the listed route/pump functions useful Ghidra anchors. One implementation
claim does not match this repository: the paper says `activity_forced_destination.cpp` synthesizes a
full roughly 620-bit authored descriptor, while this branch only renames a captured descriptor and
clears the bits when no compatible capture exists. Treat that builder as external/unpublished work,
not as code already available here.

The paper's manager split is independently valid in this image. Its local initializer RVA
`0x1772440` is `FUN_141772440`, and its authored initializer RVA `0x1773200` is
`FUN_141773200`. Both receive the manager as their first argument; the local initializer directly
reads the identity index at `manager+0x854`, while the authored initializer uses the same manager
layout throughout its setup. Each entry has a unique position-independent prologue in the pinned
image. A passive pair of detours now reports
`stage=activity-route result=called|ok|fail route=local|authored identity=...` once per distinct
boundary. The call is logged before native initialization and completion after it returns, so an
authored constructor that hangs is distinguishable from one that never runs. This is the direct
runtime test for the paper's identity-1 claim; the separate `activity-mode` probe remains a
content-definition selector and must not be confused with this manager route.

The exact constructor branch is now proven in `FUN_14175E520`, not inferred from the paper. The
managed-session pump snapshots three 0xA8-byte activity-start records through `FUN_141751370`.
Byte `+0x12` of each record is the route selector: zero calls `FUN_1417723C0`, which forwards to the
local initializer `FUN_141772440`; nonzero takes the compatibility check and calls
`FUN_14178FE00`, which forwards to the authored initializer `FUN_141773200`. `FUN_141751370`
returns either the cached record at the identity's pump slot `+0x388` or the live record exposed by
the manager component at `manager+0x19068`. A third passive detour now reports the raw boundary as
`stage=activity-route-record result=ok identity=... selector=... route=local|authored` once per
identity and branch. This separates a wrong decoded start record from a correct authored branch
whose initializer or downstream director fails. No selector is forced.

Several other labels in the external paper do not survive direct decompilation of this image.
`FUN_1417ADA60` copies and commits a 0x1B0-byte activity-selection descriptor into one of two
manager buffers; it is not the local/authored constructor switch. `FUN_1417B8C50` is the manager's
state-1 worker, while `FUN_141766A30` performs throttled per-component work. RVA `0xC210F0` resolves
only to an external jump thunk here. These addresses remain useful landmarks, but they are not safe
entity-construction hook points. The verified `+0x12` branch above is the route authority for this
runtime.

The separately supplied `Decrypt_and_label_encrypted_ptrs.py` is IDAPython. It discovers two
encrypted-string stub forms, decodes their text, then heuristically labels one associated global
pointer; it does not decrypt arbitrary function pointers or recover missing function boundaries.
Its default `APPLY=True` rename mode and fixed Windows JSON output path are also unsuitable for the
current Ghidra project. A read-only standalone port of its string formula found 1,227 stubs and
decoded 1,001 strings in this exact runtime dump. Those strings supplied useful manager names but
no parameter-body codec. The script has not modified the Ghidra database and does not replace the
native route analysis above.

### `remote-join-data` parameter and exact wire codec

The 0xA8 activity-start record is not merely adjacent to group parameters: it is the exact value of
registry parameter 13, `remote-join-data`. `FUN_1417A9820` constructs the 25 wrapper table at
parameter-manager subobject `+0xB968`; entry 13 points at the wrapper whose current value is exposed
at native manager `+0x19068`. `FUN_14175A880` tests both parameters 12 and 13, and the managed-session
pump obtains the live record through the parameter-13 wrapper before choosing the `+0x12` route.

Message 38 (`parameters-update`) is registered by `FUN_1416E1640` with decoded size `0xAC20`.
Its encoder `FUN_1417A28F0` writes grouped release/carry masks and dispatches each carried body
through a 0x50-byte codec-registry entry. Its decoder `FUN_1417A2670` performs the inverse. The
registry is initialized by `FUN_1417BAE60`; parameter 13 has size `0xA8`, encoder
`FUN_1417BC570`, and decoder `FUN_1417BC410`. This proves the complete body format:

- `+0x00`: 4 bits, valid 0 through 8;
- `+0x04`: 2 bits, valid 0 through 2;
- `+0x08`: 2 bits, valid 0 through 2;
- `+0x0C`: one presence bit, then a 2-bit value when present; absent decodes as `UINT32_MAX`;
- `+0x10`: 3 bits;
- `+0x12`: one route bit;
- `+0x11`: 2 bits;
- `+0x13`: one bit;
- `+0x18`: one raw memory-order 64-bit value;
- `+0xA0`: 3 bits, valid 0 through 4;
- `+0xA1` and `+0xA2`: 5 bits each, valid 0 through 18;
- `+0xA3`: one bit;
- `+0x20`: the 128-byte endpoint descriptor, encoded last through `FUN_1403EB460`.

The endpoint descriptor is the same layout Sunrise already publishes for a region: 8-byte machine
id, 86-byte `NetAddr`, 16-byte join key, and 18-byte online-session tail. Its codec writes one bit
for an entirely empty descriptor. Otherwise it writes the machine id and join key raw, the four-bit
NetAddr method plus 41 direct-method bytes (85 bytes for relay methods 6/7), and a presence bit plus
the 18-byte session tail. This lets Sunrise reuse `descriptor::build` rather than manufacture a
second address layout.

The decoded update then reaches `FUN_141788050`, which passes the carry mask and message body to
`FUN_1417A4BE0`. That routine walks all 25 wrappers, advances each value by
`align8(size + 8)`, and applies the value through wrapper vtable slot `+0x58`. The native outbound
path `FUN_14176E5A0` performs the corresponding wrapper copy and sends message 38. Parameter 13 is
therefore the normal authority-to-peer call that supplies the activity-start record; a separate
custom entity RPC is not involved.

Sunrise now contains the exact parameter-13 encoder, including native range checks and descriptor
compression. It is intentionally not included in automatic parameter-request answers yet. A zeroed
record is valid on the wire but chooses the local route and can recreate the missing-director state.
The passive route probe now logs the complete 0xA8 record once per identity/branch, along with all
bounded fields. One native EDZ exemplar is required to populate the server record without guessing.

There are two different native name tables. The wrapper table calls entries 22 and 23
`clan-lobby-data` and entry 24 `network_quality`; the codec registry calls the same indices
`matchmaking-data`, `matchmaking-peer-data`, and `network-quality`. Sunrise's diagnostic registry
uses the codec names and is therefore not renamed from the wrapper labels.

### Complete activity-logic archive

The separate `destiny2-complete-activity-logic-archive` adds strong static package evidence, but it
does not contain the missing server runtime. Its manifest covers 140,816 entity resources and 53
component-class pairs. The archive itself distinguishes strong serialized links from name-affinity
heuristics and explicitly cannot prove engine-only or server-only behavior.

The active Sunrise destination is the archive's `edz_freeroam`, not `ambient_edz`: runtime logged
both `successfully changed world to: edz_freeroam` and root roster bodies with
`dest=edz_freeroam`. The archive identifies it as scenario `80B2F00A`, root `80B2EF39`, with
12,376 entity definitions. Those include 1,443 spawn definitions, 1,863 squad definitions, 543
trigger sources, 2,861 condition monitors, 1,011 action targets, and 651 objectives. This is direct
evidence that the loaded patrol content contains enemy encounter logic; the absence of enemies is
not because Sunrise accidentally selected the smaller `ambient_edz` scenario.

One compact authored example under activity resource `80B2F02A` is:

- `80B2E99F`: `pf2_simple_encounter._trigger_volume`, class `808099C8/808099C9`;
- `80B2E997`: `pf2_simple_encounter._spawn_rule`, class `808094CF/808094D0`;
- `80B2E99A`: the corresponding fallback spawn rule;
- `80B2E9A2`: `pf2_simple_encounter._squad[0]`, class `80809A3B/8080948F`;
- `80B9ABAE`: `pf2_simple_encounter._objective`;
- a serialized WorldID placement links `80B2E997` to placed entity tag `80C5111C`.

These hashes occupy different layers. `80B2E997` is an authored spawn-rule definition and
`80C5111C` is a placed map entity; neither is evidence for the 40-bit RSAT field consumed by the
runtime kind-0 sobject-create codec. The currently proven runtime RSAT `80C4FEAD` does not occur in
this archive, which is expected because the archive inventories authored activity logic rather than
the live sobject catalog observed by the client hook. Do not substitute an archive tag into the
entity packet merely because its name says `spawn_rule`.

The combined boundary is now clearer: Sunrise's 78-bit kind-0 create proves the gameplay transport,
scheduler, handle, and sobject allocation path. Actual enemies should originate after the authored
activity/director instance evaluates a trigger, spawn rule, and squad definition, then creates the
native squad/member sobjects that use that replication path. The next spawning milestone is
therefore reconstruction of the identity-1 authored-mode descriptor/director startup, followed by
capturing the native sobject sequence it produces. A hand-built minimal create remains useful as a
wire probe, but it is not an encounter substitute.

The local `DemonWare` source tree is release 1.80-era generic middleware for Win32, PS2, PSP,
Xbox 360, PS3, Matchmaking+, and the State Engine. It is useful background for DemonWare buffer,
transport, lobby, and peer conventions, but repository-wide searches contain no Destiny activity
mode, activity-global-state, destination-selection, or activity-host schema. It therefore cannot
supply the missing D2-specific authored descriptor directly.

Service-6 selection diagnostics now report the client-authored request kind, activity and element
indices, skull count, descriptor width, package-name bit offset, and trailing flag. The next native
EDZ launch can therefore establish whether Sunrise captured and replayed a wide authored descriptor
or fell back to its 372-bit minimal form. This is evidence collection only: the archive does not
define the unknown mode field, and no field is forced until an EDZ exemplar identifies it.

Ghidra provides another guard against treating “mode 1” as a literal wire bit. The retail string
`Failed to set activity mode.` is referenced by `FUN_140BEA6D0`. Its caller `FUN_140BEA850` selects
a 32-bit mode definition from an activity record (an indexed 0x38-byte entry with a `+0xDC`
fallback) and passes that definition to the setter. This proves at least one engine-level activity
mode is content-defined. It does not yet prove that this function is the same identity-1 mode named
by the external paper, so its definition lookup must be traced to the service-6/global-state input
before changing the wire format.

The selector's exact ABI is `void(uint16 sourceActivity, int32 elementIndex,
uint16 destinationActivity)`. Both initialization callers (`FUN_140DC2340` and `FUN_140DC23A0`)
load those values from the activity singleton at offsets `+0x354A`, `+0x3550`, and `+0x354C`,
respectively. These offsets are the `+0x02`, `+0x08`, and `+0x04` fields of the 0x118-byte startup
selection initialized by `FUN_1404D53A0`. `FUN_1404D58A0` formats that object as source,
`(element %d) -->`, destination, which proves the wire mapping independently of Sunrise's parser.
`FUN_1404F8FD0` copies the same object into the service-6 request. Its leading layout is reason at
`+0x00`, source activity at `+0x02`, destination activity at `+0x04`, and optional element index at
`+0x08`, exactly matching the decoded service-6 descriptor order.

The selector resolves the source and destination IDs through the activity-definition table. When
the source record has a mode array, it uses `elementIndex * 0x38` and reads the definition at entry
`+0x30`; otherwise it tries the source record's `+0xDC`, then the destination record's `+0xDC`, then
the global default. This maps the content-defined activity mode to the authored request without
assuming that an external paper's identity-1 mode is a literal standalone wire bit.

Two passive network-group hooks now capture this boundary without changing it. The selector probe
logs `stage=activity-mode source=... element=... destination=...`; the only callee's setter probe logs
`stage=activity-mode-definition result=ok|fail definition=0x...`. The selector signature is the
unique 27-byte prefix at `FUN_140BEA850`; the setter signature is the unique 15-byte prefix at
`FUN_140BEA6D0`. The current service-6 request selected activity 8 from activity 8 and omitted its
element index, but only the runtime probe can show whether native `modeIndex` is also absent/default
or was populated independently later.

The same initialization paths call `FUN_140DDD2A0` with one or both activity IDs. That resolver
maps an activity index through the installed activity definition and returns its signed-byte type at
record offset `+0x78`. Callers use zero to disable a large initialized subsystem; values 1 through 6
select one of six downstream configuration definitions. A third passive hook logs each distinct
pair as `stage=activity-type result=enabled|disabled activity=... type=...`. Its wildcarded entry
signature and fixed sentinel-check tail uniquely match `FUN_140DDD2A0`. This separates a missing
activity definition/type gate from a missing mode definition and from failures later in encounter
startup.

### Same-region citizen-advertisement replay fix

The no-entity freeze run isolated a separate control-plane bug. After the first `PUB448.4` join had
completed, the client gracefully began leaving it. A root membership refresh then carried the same
citizen descriptor again, causing a second transition to the same `PUB448.4` group session. The
world controller force-disconnected the still-leaving target, the managed-session pump reported
`Cannot create: Managed session with this identifier already exists`, and the replacement join
stopped after `Queuing join-complete`. `network_update` later remained stalled. Ghidra confirms that
the duplicate-identifier path is below managed-session pump `FUN_14175E520`; it is not in the entity
sender, which emitted nothing in that run.

The server bug was that `includeCitizenAdvertisement` also selected root-versus-foreign membership.
Every root refresh consequently rebuilt the descriptor even after the region join had completed.
Membership serialization now has two independent decisions: root records always retain their root
identity and admitted gameplay-host reflection, while the citizen descriptor has its own lifecycle.

The first one-shot implementation retired that descriptor as soon as its first frame reached the
client. The next EDZ run proved that boundary was too early. Revision 1 consumed 30,992 bits and
started the `PUB80.80` citizen join; the service-8 identity refresh arrived before the join settled,
and revision 2 consumed only 29,968 bits. The client interpreted the absent 1,024-bit descriptor as
the ambassador switching to `00000000:00000000`, aborted the join, and timed out entering the
prologue filler. This is why the run remained in orbit/loading even though the initial descriptor
was correct and used the captured 716-bit `edz_freeroam` activity selection.

The marker now means **settled**, not merely **sent**. Every root membership revision continues to
carry the citizen descriptor until the native gameplay view is bound and join-complete has queued
its activity-host promotion. The first membership after that combined gate omits the descriptor and
records the settled group only after the frame reaches the caller. Remembering the settled group
prevents the old same-session replay even if its advertisement row is later released. Transaction
and keepalive publication use the same three-state lifecycle: advertise while joining, retire after
view and host promotion, retain the retired marker. Session wiping explicitly restores the `-1`
region sentinel because `SecureZeroMemory` bypasses the member initializer and region zero is valid.

The next EDZ run validated the retention boundary: revisions 1 and 2 both consumed 30,992 bits,
the client started the `PUB80.80` citizen join, sent and received join-complete, and never emitted the
ambassador-zero abort or the duplicate-managed-session warning. The run eventually entered EDZ and
captured native sobjects. It also exposed a cadence bug in the first three-state implementation.
Using the unsettled marker as the urgent-send trigger produced 320 descriptor-bearing keepalives,
with revisions advancing roughly every 12--67 ms while each gameplay view bound. That flood explains
the repeated transition churn and can itself delay the prologue filler even though the join succeeds.

Publication and retirement now use separate delivered markers. `activityPublishedRegion` disarms
the urgent send as soon as one descriptor-bearing membership reaches the client; transaction-driven
membership refreshes still include the descriptor until the group reaches its combined retirement
gate. A separate retirement-due gate emits the one descriptor-free membership needed to settle the
group. This preserves revision-2 retention without turning every network pump into a membership
revision. Keepalive diagnostics report the legacy region markers plus group-keyed state.

The first cadence-fixed run exposed two more ordering constraints. `PUB96.96` completed its gameplay
join and view first and retired normally at `t=88428`. The older `PUB80.80` view did not reach stage
5 until `t=92610`. Its late completion overwrote the single settled-region marker from 96 back to
80. Because the reported region was still 96, the region-only urgent trigger then emitted roughly
284 descriptor-free membership revisions in about 6.5 seconds. Public transitions can therefore
complete out of order; one scalar region cannot describe the lifecycle of the client's current and
target public groups.

`PUB280.2` proved that view bind is also too early a retirement boundary. The server accepted its
stage-5 view and removed the descriptor at `t=99349`, but did not receive the client's join-complete
or queue `activity-host,current-activity` until `t=99476`. In between, at `t=99409`, the client
declared the transition complete and began gracefully leaving the target group. The late
activity-host update caused another transition to the same `PUB280.2` at `t=99482`; at `t=99512`
the world controller found the previous target still leaving and force-disconnected it. Repeated
same-region transitions then left the client in transition loading.

Ghidra corroborates that sequence. The transition starter is `FUN_140E2B120`; it stores the target
slice-set, transition type, and incremented token on the manager. Its stop routine
`FUN_140E2B660` clears the active transition and citizen-join state, calls the region handoff, and
reports `stopping transition`. The citizen-join pump `FUN_140E1E040` owns persistent target session
description and join-handle fields, waits on the native group join state, and contains the logged
force-disconnect path. Consequently, a descriptor retirement that lets the stop routine run before
activity-host promotion is not benign: the subsequent host update is processed against cleared
transition state and can reopen the same target.

Citizen publication state is now keyed by the advertised gameplay group session. Each BAP session
remembers the two most recent published and settled public groups, matching the server's two public
session records and the client's observed current/target handoff. A late completion for the old
group can settle that group without changing whether the newer group is published or settled.
Descriptor retirement now additionally requires the gameplay host to have processed join-complete,
published the join membership, and queued the activity-host update. Keepalive diagnostics expose
the selected `group`, `group_published`, `group_settled`, and combined `ready` gate. The gameplay
host retains its last two successful activity-host promotions after an admitted row is released;
a fresh join attempt for the same group clears that remembered promotion before it can qualify.

The next long patrol run exposed a different lifecycle generation bug. The initial visits to
regions 80, 96, and 24 completed, but a later return to region 96 reused its deterministic gameplay
group. That group still appeared in the durable settled-history array, so the keepalive suppressed
the new descriptor even though the scalar markers still named `published=88` and `settled=24`.
Once the client had no target descriptor, it repeatedly completed and restarted `PUB96.96`: tokens
20 through 122 advanced in about 2.5 seconds. This transition storm is the concrete cause of that
run's apparent lockup and is upstream of entity creation.

Group history is now qualified by the scalar region marker to form a visit generation. Returning
to a previously settled region therefore publishes its descriptor again. A reused group also keeps
durable view/host readiness, so retirement additionally requires that the descriptor was published
on the current visit; historical readiness can no longer erase it from the very first re-entry
membership. The following delivered membership may retire it normally. This retains group-keyed
out-of-order protection while rearming deterministic region sessions on patrol loops.

The same run explains why `entity-create-out` previously required extensive zone movement. The
initial EDZ content populated namespace 1 to 13 occupied native slots at `t=72588`, then that view
converged to the supported single-view scheduler layout. Namespace 2 did not exist until the next
public-region join and began with zero occupied slots. Sunrise nevertheless required namespace 2
in both its create planner and its decoder trace, so it ignored the ready initial-zone manager and
waited for unrelated movement to populate the hard-coded namespace.

Ghidra resolves the hooked direct entity-list decoder to `FUN_141718510`. Its manager comes from the
context and all slot/anchor/create handling is shared; the routine contains no namespace-specific
codec branch. The create writer also emits one selected scheduler view and carries no namespace
field. The planner now accepts any nonnegative native namespace that still passes the existing
one-view signature agreement, 13-object baseline, pristine slot generations, control-queue, and
500 ms stability gates. Decoder tracing follows the armed view's namespace for the same reason.

### Initial-zone scheduler bootstrap window

The first stationary EDZ test with the namespace restriction removed reached the intended initial
manager: namespace 1 populated exactly 13 native objects at `t=156509`, collapsed to one local
scheduler view at `t=156622`, and published its exact scheduler signature at `t=156671`. No create
left, because the client's host-decoded scheduler was still the pristine zero signature with zero
logical views. The PUBLIC TARGET activity then appeared automatically at `t=157083`, only 412 ms
after signature publication, so the existing 500 ms agreement timer could never complete even
without player movement.

This is a first-echo bootstrap case, not a stale transition mismatch. The create planner now accepts
an empty remote scheduler only when its 128-bit signature, view count, keys, and tags are all zero
and the local side already matches the exact captured one-view signature. It remembers that the
candidate began in this pristine state and waits one 250 ms transport resend interval before
attaching the create. A nonempty, partial, or stale remote scheduler still requires complete local
and remote list agreement plus the original 500 ms interval. `stage=entity-create-out` reports
`bootstrap=1` when this narrow path is used.

### Stationary disconnect is missing account SOID, not entity traffic

The stationary EDZ run after the bootstrap change never emitted `entity-create-out`, so no
experimental create packet caused the disconnect. Gameplay transport remained healthy, with no
corrupt or lost reliable frames. The initial namespace-1 manager populated 13 native objects and
reached the exact one-view scheduler signature, but its channel view stopped at local stage 4,
remote stage 3, `compatible=1`, `open=0`. Because the view was not bound, the create planner
correctly declined to send.

Ghidra identifies stage 4 as a real resource-readiness boundary. `FUN_1416F6810` calls
`FUN_141713980(view + 0xA8)`, which scans the entity manager's active RSAT/object rows and invokes
the registered kind's readiness virtual. A pending row calls `FUN_141710880` and returns true; only
after this scan clears and the owning manager state at `+0x2C` equals 4 may the native initiator
advance to stage 5. Forcing a bind or synthesizing stage 5 would therefore bypass game-owned
resource initialization and is not safe.

The actual disconnect occurred at `t=150134`, when retail networking reported that all peers still
lacked valid account SOIDs after the 91,000 ms grace period. One millisecond later world
prerequisite `ENUM(22)` failed as unavailable in context `ENUM(67)`, and the normal cleanup path
returned the client to orbit. Ghidra maps prerequisite 22 through `FUN_140D3EB60` to
`FUN_140E1A8B0`/`FUN_140BE2070`, the same account-SOID grace-period check. The later BAP
`_connection_failure_suicide` lines are consequences of cleanup, not its cause.

Sunrise's message-30 membership writer currently emits the full player identity arm but explicitly
writes the player-profile arm as absent. This remains a plausible upstream mismatch, but the later
native analysis below means it is not yet proven to be the direct account-SOID failure.

### Native message-30 profile and view-readiness capture

The native message-30 encoder is `FUN_14173E050`. Its player deltas begin at update-image offset
`0x4250`, use a `0x1B8` stride, and store the player count at `0x174A`. A full delta has an identity
presence byte at delta `+0x08` and a profile presence byte at delta `+0x21`. The profile payload
continues through three native helper codecs, so guessing a partial wire body is riskier than
capturing a game-authored local-session exemplar.

Two passive probes now preserve the original native calls and record the missing evidence:

- `stage=membership-native-profile` logs the first encoder call and the first complete `0x1B8`
  profile-bearing player delta as uppercase hex.
- `stage=view-readiness` logs the stage-4 helper's pending/ready transitions and samples a
  persistent pending result every five seconds, including token, namespace, manager, first active
  slot, and call count.
- `stage=view-slots` now includes the owning manager's `gate` field at `+0x2C`.

Both new signatures were checked against the pinned Ghidra image and each resolves uniquely at its
expected function entry. These probes do not mutate membership, entity state, or view stages.

The stationary solo run attached both hooks, but `membership-native-profile` never appeared. That
is expected for a solo fireteam: there is no remote fireteam peer to which the client must encode a
native message-30 membership update. Capturing a game-authored profile exemplar therefore requires
a genuine second client/peer and is not an efficient next step for the current solo server path.

The readiness hook did fire. Token `0x9EAA300100200002` remained pending for more than 90 seconds
with manager gate 4 and active slot 0, while token `...003` returned ready with no active slot. The
global view gate is therefore correct; the initial entity handler is specifically waiting for the
native resource/codec associated with slot 0.

### Account-SOID prerequisite table

Ghidra now identifies the exact data consumed by the 91-second disconnect predicate. The validator
`FUN_140BE2070` scans 32 records beginning at its manager `+0x210`, with a `0x20` stride. A nonzero
SOID whose low state byte at `+0x08` equals 3 is an individually expired/invalid record, not a valid
one: finding one immediately blocks the prerequisite. A separate timer at manager `+0x610` blocks
the prerequisite when the game concludes that no peer has a valid account SOID.

An upstream reconciler obtains a desired-SOID singleton from `FUN_140FCC6E0`. Its 32 desired IDs
begin at singleton `+0x10` with a `0x10` stride. The reconciler creates target records in state 1.
A matching class-0 connection record in native state 3 advances the account record to state 2 and
clears its per-record timer. Losing that connection starts the per-record grace timer; expiration
then advances the account record to invalid state 3. A new passive
`stage=account-soids` probe hooks only the final validator and compares both tables without changing
them. It reports target/source nonzero counts plus the first four records, their state, and timer
fields.

The first instrumented run showed the desired source and reconciled target both contain exactly
`0x9EAA300100100100`. The target is continuously in state 2 with its per-record timer cleared, so
message-30 profile omission did not prevent the native account identity from being published or
matched. The unresolved failure is the separate manager `+0x610` no-valid-peer timer. The
reconciler clears that timer when the desired table changes or when its two copied header qwords
compare equal; otherwise it starts and advances the timer. The expanded probe now captures those
manager/source header qwords, the global timer, and the complete matching `0x58` connection record.

The same run resolved active slot 0 to type key 0, mapped type 0, kind 2, namespace mask 1. Its
namespace flags moved from `0xC1` to `0xC3`, but the readiness method continued returning pending.
The probe now additionally resolves the kind-2 codec, vtable, and virtual method at vtable `+0x28`,
including its main-image RVA for direct Ghidra analysis. All added signatures resolve uniquely in
the pinned Ghidra image.

The next stationary run localized the global timer trigger. Before activity membership, both the
source and reconciled manager remain `1/1`, with one live state-2 target for local account SOID
`0x9EAA300100100100`. The revision-3 membership snapshot then adds the synthetic activity-host
peer (`occupied=eligible=3`, decoded `p1_gate=0x10`, `create=1`). At `t=68947`, the desired source
changes to `2/1`: its first header counts two peer/upsert inputs while its second header and entry
table still contain only the local account SOID. The manager copies `2/1` at `t=69015` and starts
its global grace timer. The family-zero bootstrap is not the writer: it permanently yields to the
native producer earlier at `1/1` after observing producer-owned mask bit 4. The unresolved protocol
defect is therefore one missing native account-identity association for the synthetic host peer,
not a missing local profile or a stale Sunrise seed.

Ghidra identifies the complete desired-snapshot publisher as `FUN_140FC9050`. It copies `0x210`
bytes from its second argument into the singleton and is reached indirectly through a virtual
table, so static callers are not available. A passive `stage=account-soid-publish` detour now logs
the native return-address RVA, input headers, and first desired identities whenever its input
changes. This should identify the subsystem that constructs `2/1` without modifying the account
table.

The kind-2 readiness virtual resolved at RVA `0x11B1DB0` is only `mov al,1; ret`; it has no hidden
state check. The pending result comes from the entity manager's active bitset at `+0xC920` and the
slot lifecycle around it. The readiness probe now reports the full active-bit count and first eight
active slots. Do not force stage 5 or clear those bits: they represent native entity initialization
that the server still needs to satisfy.

The publisher probe made the account failure causal rather than correlational. A healthy svc-23
translation at `t=37352` is followed by a native `1/1` snapshot. When the synthetic public host is
added, svc 23 returns `unpaired` at `t=66655`; the publisher emits `2/1` at `t=66754` and the global
timer starts immediately afterward. The server handler contained an explicit process-wide gate
that paired only the first identity and returned an empty svc-24 response for every distinct later
identity. That gate was removed, while malformed and zero identities still received an empty
response. The follow-up run established that the public synthetic host's request is structurally
valid but carries identity zero. Returning the empty response for that zero produces `2/1` again,
and the disconnect follows 91.5 seconds later. Request-shape validation is therefore separate from
the identity value.

The experiment that aliased this typed zero to the local account disproved that shortcut. Svc 24
accepted it and the source reached `2/2`, but both source slots contained the same account SOID
`0x9EAA300100100100`. The reconciler created two target records with that same key, both remained
in state 1 with the same countdown, and both expired together after 91 seconds with duplicate
`Failed peer-subscription validation` errors. A platform identity of zero is missing identity, not
another name for the local Steam account. Sunrise again leaves it unpaired; any future synthetic
account response must be distinct and must also have a real matching connection record.

### Native peer identity delta

The upstream membership mismatch is now exact. `FUN_14176B450`, the native complete-snapshot
builder, emits a full peer delta for every occupied peer and calls `FUN_14176BE20` with no old
identity. Consequently its outer identity flag is present for every peer. The resulting `0x130`
delta image describes a `0x108` peer identity. `FUN_1417A51E0` encodes it in this order:

- a `peer-session-id` presence bit and a zero-terminated string of at most 128 bytes;
- an optional 3-bit/7-bit pair;
- a four-bit change mask for the identity fields around native offsets `+0x88..+0xB4`;
- independently optional fields through the final native qword at `+0x100`.

The membership dump formatter at RVA `0x1776763` confirms the first string becomes the displayed
`session_id`, while native identity offset `+0xB8` becomes the displayed `flags`. The retail local
fireteam/posse peer has a nonempty process session id and later flags `0x2000006F`; both Sunrise
public peers had blank session ids and zero flags. This matches the writer: it previously emitted
the outer identity bit as absent for both peers even in a complete snapshot.

Sunrise now publishes a bounded `Sunrise_<group-session>@x64@activity_host` peer-session-id for the
embedded host. Only that proven string arm is sent. All platform, account, QoS, flags, and trailing
identity arms remain absent, and the admitted client's identity arm remains absent so the server
does not overwrite its local native identity with guessed values. This is the smallest retail-like
change that can test whether the native account publisher was classifying a blank-identity host as
an ordinary account-bearing peer.

The first identity run decoded the string exactly, but exposed a checksum dependency before the
join could proceed. The client calculated a different membership checksum, reported `no membership
information`, disconnected, and retried the same citizen join every half second. The native peer
table begins at state offset `0x1750`, has a `0x120` stride, and stores the decoded `0x108` identity
at peer offset `+0x18`. Sunrise's checksum replica still left that region zero. The replica now
copies the peer-session-id into the first 128 bytes of `peer+0x18`; every omitted optional identity
field remains zero, matching the decoded complete-snapshot state.

The checksum-corrected follow-up completed both public joins and loaded EDZ, proving that the
peer-session-id arm and its checksum replica are now valid. It did not change account
classification: the synthetic host still requested a typed identity zero, the desired-account
snapshot still changed from `1/1` to `2/1`, and cleanup still began about 91 seconds later. The
captured stack above the publisher was the generic game-tick event drain, not the snapshot builder.

### Desired-account source flags and activity-host pairing

The actual desired-account producer is `FUN_140FCF0B0`. It iterates native identity-map records,
materializes each 0xD0-byte view through `FUN_140BDE590`/`FUN_140BDE690`, and emits event `0x2F`
only when its rebuilt 0x210-byte snapshot changes. The byte at materialized offset `+0x40` comes
from identity-map record offset `+0xC2`. The producer increments header 0 when either flag `0x01`
or `0x10` is set, but it increments header 1 and writes a SOID/mask entry only for flag `0x01`.
This exactly explains the observed `2/1`: the synthetic host owns a `0x10`-only record.

`FUN_140BE5830` establishes the direction of those flags. `FUN_140E74F90` returns platform type
`0x0C` and `FUN_140E74BF0` returns SOID type `0xFF`. Merging a `0x0C -> 0xFF` translation sets
qword bit 16, which is byte flag `0x01`; merging the reverse `0xFF -> 0x0C` translation sets qword
bit 20, which is byte flag `0x10`. The host therefore has the reverse association but its zero
platform handle never receives the forward svc-24 association. The publisher itself is behaving
correctly; forcing its headers or validator would only hide an incomplete bidirectional mapping.

The next bounded experiment pairs a valid typed-zero request only with
`live_region_session(activitySessionId)`. It never aliases zero to the local account. The selected
SOID is distinct and already names the live public activity session associated with the embedded
region host, giving the native connection path a real server-owned key it can plausibly reconcile.
The account probe
now summarizes the first four active connection records as `cN[index=... soid=... state=...]`,
including zero-valued records, so the experiment can be rejected immediately if the new host SOID
has no matching connection.

The first run of that experiment did not exercise the pairing. Svc 23 is sent over the primary BAP
connection, and that connection's route-local `activitySessionId` remains zero even while private
and public activity clients are both established. An overly strict nonzero guard therefore left
every zero identity `source=none`, kept the source at `2/1`, and reproduced the same timeout. The
connection summary confirmed one local record and 31 empty records. The corrected lookup always
scans shared activity State for the newest region session; the zero route-local value is now only
the fallback when that scan finds nothing.

The corrected run proves the stationary disconnect fixed. At `t=86867`, typed identity zero paired
to the live public session SOID `0x9EAA300100200002`. The native producer briefly published `2/1`
while applying the response, then reached `2/2` with two distinct source entries. The new host
target first appeared in state 1 with connection record 1 already keyed to the same SOID; by
`t=87266`, both targets were state 2, both connection records were state 3, and every target and
global timer was disabled at `0xFFFFFFFFFFFFFFFF`. That state remained unchanged through at least
`t=262464`, more than 175 seconds after translation and well beyond the former 91-second cleanup.
There were no peer-subscription failures, prerequisite-22 cleanup, or world disconnect. A later
retired `GAH1` link raised `_connection_failure_suicide`, but the active gameplay and BAP links kept
exchanging packets and activity pushes, so that line is not the account failure returning.

With session stability established, the next blocking boundary is again entity initialization.
The public view remains pending with 13 active native slots while the server's gameplay packets
carry `entries=0`; a separate empty namespace view reports ready. The account fix removes the time
limit from this investigation but does not acknowledge or initialize those public native objects.

The same run explains why `entity-create-out` has mostly required zone travel. The public view first
reached the 13-object baseline at `t=93009`, but the native local scheduler still contained two
views. It collapsed to the supported single-view layout at `t=93207`, then the next regional group
queued fresh reliable control records at `t=93407`: only about 200 ms later. The guarded pristine
bootstrap currently requires one full 250 ms resend interval with both a stable one-view layout and
a settled control queue, so this safe window closes before the first create can leave. Once the
control queue settles, the new empty regional view makes the scheduler multi-view, which this host
intentionally refuses because earlier two-view packets were decoded as corrupt. Transition-only
`entity-create-gate` diagnostics now record which of these conditions blocks the next run; they do
not change replication behavior.

The following stationary run made the protocol cycle explicit. The populated public token
`0x9EAA300100200002` remained at message-40 stage four, while the empty transition token
`0x9EAA300100200003` alone reached stage five and became the peer transport's selected view. Ghidra
`FUN_1416F6810` confirms that stage four calls `FUN_141713980`, returns without advancing while any
entity handler reports pending, and only then moves the native view to stage five. Waiting for stage
five before publishing replication therefore waits on the readiness that replication must clear.
The transport now retains one view signature per carried group session, publishes a provisional
replication view once both message-40 sides reach stage four, keeps `view_accepted` false until the
same session reaches stage five, and selects the captured view with the highest native occupancy so
an empty overlap view cannot displace the 13-object public view. In this run the exact one-view
signature became valid at `t=67691`, but the next region queued control records at `t=67825`, leaving
only about 134 ms. The pristine-only settle interval is therefore 100 ms; every identity, baseline,
slot, generation, signature, scheduler-layout, and settled-control guard remains unchanged.

The next run validated that sequence without movement. Token `0x9EAA300100200002` published
`replication-ready` at stage four, its remote one-view scheduler converged, the gate settled for
133 ms, and the first create left at `t=67858` for namespace 1 slot 13. Native consumed 77 bits and
returned result 2, while retail named the remaining condition directly: RSAT `0x80C4FEAD` was not
loaded and the packet could not yet decode. The intended two-second retry never left because a new
local overlap view changed the scheduler to multi-view; the old retry path re-synchronized to that
unsupported local layout and jumped to the attempt limit. The client remote scheduler remained the
same accepted one-view token throughout. The selected create now caches that exact one-view wire
signature and validates the unchanged token, candidate slot/generations, remote signature, remote
view count, key, and tag before each bounded retry. It does not publish a multi-view layout.

The cached-layout run proved that the bounded retry itself now survives the local overlap view.
Without movement, the first create left at `t=65559` and attempt two left at `t=67573`, both for
token `0x9EAA300100200002`, namespace 1, slot 13, and the same key, generations, and RSAT. This
removes scheduler-layout drift as the reason the retry was missing.

That run also exposed an independent transport hazard. The native entity-list decoder returned
result 2 after consuming 19 bits from the first create, and every ordinary acknowledgement carrying
the echoed one-view scheduler continued entering the same pending decoder. It returned result 2
and consumed 19 bits repeatedly until the channel reported `79 discard-expected, 66 corrupt` and
timed out. The later namespace occupancy increase from 13 to 14 is inconclusive because no accepted
entity record or visible enemy accompanied it. Ordinary acknowledgements now suppress the scheduler
after the first entity attempt. Only an actual bounded create/retry packet carries the cached
one-view scheduler, isolating pending resource decodes while allowing acknowledgement-only traffic
to keep the channel alive.

The packet chronology localizes the corruption one step earlier than the create. The last valid
client receive was `t=64892`; at `t=64952` Sunrise replaced its cached signature wire from a new
client scheduler update, and the client partially applied that echo but never counted it as a valid
packet. The later entity-list calls all arrived with `capacity=255` and their readers already near
the zero-padded packet end, proving an earlier handler had consumed the intended entity body.

The next runtime test disproved the 202-bit boundary interpretation. Treating the external trailer's
second bit as the scheduler update caused `stage=scheduler-signature` to appear on almost every
packet, first at 202 bits and later at 274 bits. It is therefore the outer scheduler-body-presence
bit. The scheduler handler itself owns a nested update bit before schema `0x80806AEA`, exactly as
its native decoder's first read indicates. Each of four 202-bit create attempts reached the entity
list one bit out of phase: the decoder consumed only one false count bit (`result=0 count=0`), no
entity record was accepted, and the channel eventually reported `16 ok, 255 discard-expected, 87
corrupt`. The correct retained one-view scheduler wire is 203 bits: one nested update bit plus the
202-bit schema body. Sunrise now restores that prefix and rejects other widths for this guarded
one-view path. Scheduler isolation remains in place so only a create or bounded retry carries this
body after the first attempt; ordinary acknowledgements remain scheduler-free.

The corrected 203-bit build validated both scheduler widths in a stationary run. A one-view native
schema measured 202 bits and produced a 203-bit captured update; the later two-view schema measured
274 bits and produced 275 bits. Signature logging returned to actual nested updates instead of
appearing on nearly every packet. No create left in this run. Namespace 1 reached its 13-object
baseline at `t=77537`, and local/remote one-view layouts agreed by `t=77767`, but reliable control
records recurred roughly every 200 ms. Because the converged remote layout was no longer pristine,
the guarded path required its conservative 500-ms interval and reset that timer for every control
burst. The second view appeared before the timer could finish. Candidate validation now runs while
the queue is outstanding and retains settle age only while the token, slot, generations, signature,
and local/remote one-view layouts continue to agree. Transmission remains blocked until the reliable
queue is acknowledged and empty, so no entity body can overtake topology records.

The same run also resolved the two observed session-constructor branches, but the later chronology
corrects the initial interpretation. Identity 1's complete 0xA8 record has `selector=0` and creates
the local `posse` activity session. Identity 2 has `selector=1` and creates the remote public
`group_target` join. Both constructors return successfully, and those branches match their session
roles. This selector is not evidence that the EDZ content director chose local instead of authored
execution, so forcing identity 1 to `FUN_141773200` would be an unsupported and likely harmful
change.

### Stationary namespace-1 allocation

Commit `b46dabce` completed the stationary-create milestone in the initial EDZ zone. Namespace 1
reached 13 occupied native objects at `t=102579`, exposing pristine slot 13. The proven one-view
scheduler carried 203 bits, local and remote layouts agreed, and the create gate reached ready at
`t=102943` after retaining 133 ms of candidate settle age while reliable control records drained.

Attempt one left at `t=102943`. The client consumed 77 bits, reported that RSAT `0x80C4FEAD` was not
loaded, and returned result 2. Attempt two left at `t=104946`; the decoder consumed 78 bits and
returned `result=0 count=1`. Its complete record was:

```text
entity=0x0010000D flags=0x0001
create_size=16
create=ADFEC480000000004900000006000000
update_size=73
mask=00000000000000000040000000000000
update=000000000000000000000000000000000000000000000000000000000000803F00000000000000000000000000000000C59D1C81FF00FFFF0000000000000000000000000000000000
```

The kind-0 create codec accepted the RSAT in 40 bits, and namespace 1 advanced from 13 to 14
occupied objects at `t=104979`, with slot 14 becoming the next candidate. This is direct allocation
proof and no movement was required.

The object is not visible. The create buffer's byte at +4 has trailing flag zero. Ghidra confirms
that `FUN_141724fd0` tests this byte before calling the three named component decoders:

- `FUN_141720780`: transform schema `0x80809F75`;
- `FUN_1417203c0`: parent schema `0x8080949B`;
- `FUN_1417201e0`: stream-source schema `0x8080949A`.

With flag zero, all three are skipped and only `FUN_140A00970` decodes the RSAT-defined suffix.
The encoder `FUN_141725140` is the exact inverse. Static schema metadata places transform scratch
at `+0x00`, parent after `+0x20`, stream-source at aligned `+0x70`, and the RSAT-defined scratch at
aligned `+0x90` when named components are enabled.

The current diagnostic build uses those proven offsets without changing a live record. After one
successful decode it gives private copies to the native update encoder with an all-clear dirty mask:
once with the create flag clear and once with the flag set and the RSAT scratch moved to `+0x90`.
The `sobject-native-update-probe` lines will provide exact native bit counts, flushed bytes, pending
bits, and accumulator values for the plain and spatial-capable empty update shapes. Nothing from
this probe is published to the gameplay channel.

That probe completed exactly. `plain-clean` returned 1 without a fault and emitted two zero bits.
`spatial-clean` also returned 1 without a fault and emitted five zero bits. The three-bit difference
matches the transform, parent, and stream-source presence calls in `FUN_141725140`; the remaining
two bits are the RSAT-defined fields walked by `FUN_140A00AA0`. An all-clean update therefore has no
hidden alignment, length, or payload fields.

The next bounded record sets both create and update flags, changes the create trailing byte to one,
and appends those exact five zero bits. Its predicted entity-list size is 83 bits: the accepted
78-bit create plus the five-bit spatial-clean update. This is a framing experiment, not a visibility
claim. The decoder capture now retains 256 bytes so the expected `0x90 + 73 = 217` byte spatial
scratch is complete. A second private pass then sets only dirty bit zero and asks the native encoder
to emit `spatial-transform-default`, exposing the transform payload without sending it.

That experiment completed as predicted. Attempt two returned `result=0 count=1`, consumed 83 bits,
and produced a flags-`0x0003` record with a 217-byte update scratch. Its first 32 bytes are:

```text
0000000000000000000000000000803F00000000000000000000000000000000
```

Interpreted as eight little-endian floats, this is `[0, 0, 0, 1, 0, 0, 0, 0]`: an identity
quaternion followed by zero translation/auxiliary values. The private transform-only re-encode
returned 1 with `fault=0`, `bits=112`, `flushed=64`, `pending=48`, and accumulator zero. The first
eight bytes were `C010000000000000`; the pending 48 zero bits complete the exact native wire as:

```text
C010000000000000000000000000
```

Because the update codec is the top-level kind-0 spatial encoder, those 112 bits include transform
presence and payload followed by clear parent, stream-source, and two RSAT-defined presence fields.
The clean five-bit form was accepted by the decoder but then triggered a repeating client assertion:
`Injecting an update into gameworld with a blank update mask.` It is therefore useful only as a
framing proof and must not remain on the published path. The next build replaces it with the exact
112-bit native default-transform form. Its predicted accepted entity-list size is 190 bits.

The transform-bearing live run then returned `result=0 count=1` after exactly 190 bits. Its decoded
mask began with `01`, namespace occupancy advanced from 13 to 14, and the repeating blank-update
assertion disappeared. The decoded transform prefix was:

```text
0000000000000000880FC93BC4FE7F3F00000000000000000000000000000000
```

The first float4 is the quantized quaternion, approximately `[0, 0, 0.0061, 0.99998]`; the second
float4 remains zero. This validates both the reconstructed 112-bit wire and transform dirty-bit
behavior. The next diagnostic privately changes each element of that second float4 to `1.0` in
turn and invokes the same native encoder. It also combines flushed bytes with pending accumulator
bits into one complete wire string, so X/Y/Z/W payloads can be compared directly without mutating
the live object.

Those four probes succeeded. X, Y, and Z each retained the 112-bit total and independently changed
their own field:

```text
default  C010000000000000000000000000
X=1      C013F80000000000000000000000
Y=1      C01000000003F800000000000000
Z=1      C0100000000000000003F8000000
```

W=`1.0` instead produced `C020000000000000000000000000` in 111 bits, confirming that it selects
an auxiliary transform branch rather than world position. X/Y/Z are therefore the second float4's
first three lanes, and W stays zero for placement.

This run lasted more than two minutes without the earlier four-second gameplay receive timeout.
The report at `t=120600` still counted 76 corrupt reads, however, so packet hygiene remains open.
The later `_connection_failure_suicide` reports occur with session teardown and shutdown at
`t=130517`; they do not mark the accepted entity record failing.

The current EDZ launch selected bubble hash `0xB8459D59`, resolved from `build_data.bin` to `town`,
bubble ordinal 51 and map-global index 145. Spawn set `0x9617A6E7` has 54 extracted points across
map indices 30, 38, 63, 73, 82, 145, and 155. Sunrise's flat cache preserves the union bubble mask
on the set but not the individual mask on each point, so assigning one of the coordinate clusters
to Town by order would still be a guess. The existing player-position publisher is stronger: it
already reads the local player's rigid-body position through the teleport physics path and exposes
a seqlock snapshot. The next diagnostic privately native-encodes that exact position and X plus
three units without modifying the accepted object.

That diagnostic succeeded at player position `(509.15094, 30.1296005, 74.3163147)`. It produced:

```text
player       C0143FE935241F1096C4294A1F40
player X+3   C01440009A941F1096C4294A1F40
```

Both are complete 112-bit top-level spatial updates. The source second float4 for X+3 is
`[512.15094, 30.1296005, 74.3163147, 0]`. The measured player position also lands directly in the
cached nine-point cluster around `(509, 30, 74)`, independently confirming that the physics reader,
spawn-point cache, and transform schema share a world-coordinate basis. The next bounded live build
publishes the exact X+3 wire for the single already-guarded RSAT create.

That live create was accepted on attempt 2 after 190 bits. The decoded update scratch retained the
exact intended position, namespace 1 occupancy advanced from 13 to 14, and the blank-update assert
did not return, but no object was visible. Rechecking the record codec in Ghidra exposed a spatial
cell error: `FUN_141717eb0` interprets a leading zero as inheritance from the caller's active cell,
while the emitted `1,0` branch explicitly stores `0xFFFF`. The caller loads its inherited cell from
the active decode context at offset `+8` before invoking the record decoder. The inheritance build
then decoded successfully after the predicted 189 bits, but the decoded record still reported
`cell=0xFFFF`: the active context itself currently carries the default cell. The object remained
invisible, occupancy still advanced from 13 to 14, and no assert occurred. This rules out the cell
branch as the visibility blocker without guessing an unrelated bubble/map ordinal.

The top-level encoder's named-component calls start at dirty index zero and advance through
transform, parent, and stream source before the generic RSAT suffix. The clean update's five bits
therefore bound the valid indices to 0 through 4: transform is bit 0, parent bit 1, stream source bit
2, and the two RSAT fields bits 3 and 4. The next diagnostic sets each of bits 1 through 4
individually against a private copy of the decoded 217-byte scratch and records the native wire;
the accepted object is not modified.

At `t=105336`, during a later regional transition, the channel timed out after four seconds without
a valid receive and reported 146 corrupt reads. Server send calls continued to return success. The
accepted transform record did not leave a partial entity decode, but the channel result means packet
hygiene remains a parallel blocker and visibility experiments must stay bounded.

An intermediate channel report counted `14 ok, 290 discard-expected, 75 corrupt`, but there was no
four-second gameplay timeout. Valid gameplay and BAP traffic continued. The activity-host failure
at `t=173667` followed an activity-host change, and the later failures at shutdown were ordinary
session teardown; neither coincided with the accepted create.

### Comparison with upstream commit b8ccfb9b

The shared `b8ccfb9b80f072ae76cd84491d00c7310cd76287` commit is a large independent physics-host and
replication architecture change, not an implementation of the native sobject payload now under
test. Its generic external codec is useful because it formalizes the channel-2 entity envelope:
13-bit slot plus four-bit incarnation, explicit create/update/remove/lifecycle/anchor flags, raw
bubble handling, and type-specific baseline/update callbacks.

It does not close the remaining boundary:

- `external_entity_codec.h` says its four entry points have no caller yet;
- the world coordinator's scriptless callbacks accept only kind-0 and emit no payload bits;
- the corresponding feature gates are disabled by default;
- the physics host models simulation and replication planning but never encodes RSAT-defined,
  transform, parent, stream-source, squad, or enemy data;
- its start-activity parser deliberately stops at the unresolved nested selection tail.

The envelope implementation can be reused selectively after the real type payload is proven.
Cherry-picking the full commit now would combine 273 files of unrelated architecture with the
working scheduler path while still leaving the exact native payload empty.

### External entity-name map

The shared `EntityNames.json` contains 747 package entity-definition hashes with extracted display
or internal names. It identifies concrete combat definitions including Red Legion Legionary
`0x80C1A52D`, Red Legion Centurion `0x80C19B1F`, Red Legion Psion `0x80C1A8E4`, and Dreg v400
`0x80FDEBC6`. Neither the current sobject RSAT `0x80C4FEAD` nor its statically linked definition
`0x80B83809` appears in the map. That absence is not conclusive because the map can be incomplete,
but it supports the runtime observation that the allocated object may be a nonvisual placed-object
controller rather than a combat actor.

These hashes must not be substituted directly for the RSAT field. Archive component evidence places
the named actor hashes inside squad-definition data, while the sobject create codec expects an
installed replication-schema asset tag. Their value is as package-graph anchors: locate an EDZ
squad/member definition with a known name, recover the enclosing squad and spawn-rule records, then
capture or derive the corresponding runtime squad and sobject RSAT sequence.

The Simulated Vandal entry closes that package boundary directly. `0x815B5420` resolves to package
`0x06DA`, entry `0x1420` in `w64_activities_06da_5.pkg`. Its package class is `0x80809C0F`, and
the decoded 49,236-byte definition stores `0x815B5422` at serialized offset `+0x88`. Entry
`0x815B5422` is class `0x80809BB6`, is 1,856 bytes, and points back to definition `0x815B5420` at
its own offset `+0x08`. Therefore `0x815B5422` is the exact sobject RSAT corresponding to the
entity-name map's `Simulated Vandal`; `0x815B5420` itself must not be placed on the create wire.

This RSAT is structurally much larger than the old `0x80C4FEAD` probe. Its serialized component
count at `+0x30` is 55, versus 2 for the old 160-byte RSAT. Ghidra `FUN_140A00AA0` confirms that
the native update codec iterates the RSAT runtime descriptor table and emits component-presence
decisions from that table. The old 112-bit transform body consequently cannot be reused: it ends
after two RSAT-defined clean fields and would leave the Vandal decoder inside its component tail.
The next bounded build sends the Vandal create without an update, preserving the spatial create
flag. A successful native decode will expose the Vandal-derived byte and bit layout in the existing
16-byte `entity-record` create capture before any new update body is published.

That create-only run succeeded exactly at the bounded retry. Attempt two consumed 77 bits and
returned `result=0 count=1` for entity `0x0010000D`. The decoded create profile was
`22545B8101000000BC2200005E000000`: RSAT `0x815B5422`, spatial flag one, derived update scratch
size `0x22BC` (8,892 bytes), and derived profile value `0x5E`. The client injected its baseline,
then namespace 1 occupancy advanced from 13 to 14. The missing-update assertions are expected for
this diagnostic and disappear once the measured update is carried with the create.

The injected baseline was sufficient for the private native encoder even though the report copies
only the first 256 bytes of the 8,892-byte scratch. The Vandal all-clean update is 25 zero bits,
showing that only 22 of its RSAT-derived decisions join transform, parent, and stream-source at the
top update boundary. Marking only transform dirty produced 132 bits without a fault. At the current
EDZ player position `(509.15094, 30.129612, 74.3163147)`, the exact X+3 wire is 128 flushed bits
`C01440009A941F109724294A1F400000` plus four pending zero bits. The next build publishes those
exact 132 bits with the accepted Vandal create; it does not infer the count from the 55 serialized
descriptor records.

That combined Simulated Vandal create/update did not reach either codec body. Both bounded attempts
returned `result=2 count=0` after the 19-bit entity-handle boundary, produced no `entity-record`,
and left namespace occupancy at 13. Repeated pending entity-list entries then contributed 78 corrupt
reads and a four-second receive timeout before attempt three. The correct correction is staged
creation followed by a later update, not additional combined retries.

The broader entity-name entry `0x80C187BD` is a stronger EDZ Fallen target than the activity-specific
Simulated Vandal. It is class `0x80809C0F`, lives in `w64_sandbox_020c_5.pkg`, and is named plain
`vandal` as well as many real variants such as Assault Vandal, Resilient Vandal, and Dusk Walker
Mechanic. Its decoded 47,540-byte definition stores `0x815B204B` at serialized offset `+0x88`.
Tag `0x815B204B` is a 1,792-byte class `0x80809BB6` resource in
`w64_sandbox_06d9_6.pkg` and points back to `0x80C187BD` at its own `+0x08`. It is therefore the
exact shared-Vandal sobject RSAT. Its serialized descriptor count is 53, so even the Simulated
Vandal's measured 132-bit body cannot be reused. The next bounded build sends shared RSAT
`0x815B204B` create-only, then uses the injected baseline to measure that resource's own update.

The final old-RSAT component probe also clarified what not to add. Parent encoded in 49 bits and
stream-source in 18 bits, while marking the first RSAT-defined field dirty raised the game's
guarded `dirty bit inconsistency detected` assertion. The second RSAT bit produced only the clean
five-bit body. These results do not identify missing default data and are not carried into the
Vandal experiment; the one-shot non-transform dirty probes have been removed.

### Shared Vandal create acceptance and staged update

The shared Vandal create-only run succeeded on its first attempt. Namespace 1 decoded one record
after 77 bits for entity `0x0010000D` and accepted baseline
`4B205B8101000000AC2200005C000000`. This proves RSAT `0x815B204B`, spatial flag one, derived
scratch size `0x22AC` (8,876 bytes), and derived profile value `0x5C`. The native occupied count
advanced from 13 to 14, so slot 13 is an observed allocation rather than an inferred success.

The injected baseline produced a 23-bit all-clean update and a 130-bit transform-dirty update. At
player position `(509.150787, 30.1287689, 74.6452332)`, the native X+3 transform was 128 flushed
bits `C01440009A641F107B842954A5C00000` followed by two zero tail bits. This is the exact
shared-Vandal body; neither the old 112-bit probe nor the Simulated Vandal's 132-bit body is used.

The same run later hit a four-second gameplay receive timeout with 65 corrupt reads. The accepted
record itself decoded completely, but repeated post-create traffic still makes retries and combined
create/update packets unsafe. The next build therefore captures the exact low occupancy word,
stops create retries as soon as bit 13 is observed, waits 500 ms with an empty reliable-control
queue, and sends one update-only shortcut for that same handle. The update bytes come directly from
the game encoder in the current run, so they follow the current player position instead of a
hard-coded prior coordinate. Scheduler output remains absent from ordinary acknowledgements.

The first two-stage test did not reach that update. Both create-only attempts returned `result=2`
after the 19-bit entity handle and slot 13 remained clear (`occupied_low=0x00001FFF`). The first
attempt left only two milliseconds after the retail world changed from initial instantiation to
fully enabled, while the next public-region citizen join was also beginning. It accumulated 63
corrupt reads before that gameplay channel reset; a later replacement channel reported zero.

The server gate exposed a deterministic timing flaw: it latched the 100 ms bootstrap interval when
the remote scheduler was still pristine, then retained that short interval after the remote layout
became the agreed one-view signature. A create could therefore leave immediately at scheduler
convergence during a world transition. Bootstrap now only seeds an empty scheduler body. Changing
from pristine to agreed resets the candidate timer, and no entity body may leave until the agreed
layout remains stable for a fresh 500 ms with an empty reliable-control queue.

That correction produced the intended create on the next run. The agreed one-view layout remained
stable for 534 ms, the 77-bit shared-Vandal create decoded with `result=0 count=1`, and the low
occupancy word changed from `0x00001FFF` to `0x00003FFF`. The current player X+3 native update was
130 bits with flushed bytes `C01440009A941F109764294A1F400000` and two zero tail bits.

The staged update still did not enter the client entity decoder. Occupancy was observed at
`t=80713`, but the 500 ms update delay overlapped new view registration: the local scheduler grew
from one view to two at `t=81180`. A temporarily busy control queue delayed transmission until
`t=81480`, where the server sent the cached one-view body against a live two-view local layout. No
update `entity-record` followed, and nothing rendered. The channel stayed connected and its later
report counted only 11 corrupt reads, so this is a rejected scheduler context rather than another
four-second failure.

The baseline and exact update are already available by the time occupancy is published. The next
build therefore uses a 100 ms post-acceptance gap, which fits the observed 467 ms one-view overlap,
and refuses the update unless both the live local and remote scheduler layouts still exactly match
the cached one-view signature. A missed window now leaves the update unsent rather than publishing
an incompatible scheduler body.

The following run showed that the overlap window is not stable: the create again decoded and slot
13 was occupied, but the local scheduler expanded to two views only 18 ms later. The 100 ms build
correctly suppressed `entity-update-out`, so it caused no incompatible update packet, but there was
nothing new for the client to render. The native transform had already been captured before the
occupied bit appeared. The next build therefore has no artificial post-acceptance delay; it sends
on the first service tick that observes slot 13, still guarded by exact live one-view agreement on
both scheduler sides.

### Deterministic RSAT residency gate

The first-tick update run did not reach allocation. Both bounded creates for shared Vandal RSAT
`0x815B204B` returned result 2 after the direct entity handle, and slot 13 remained clear. The first
call arrived with aggregate capacity 255, but three subsequent traced calls had capacity 256 and
returned the same result, disproving capacity as the cause.

Ghidra localizes this result precisely. `FUN_141718080` selects the kind-0 sobject codec and calls
its inbound create method at vtable `+0x60`, `FUN_1417266B0`. That method decodes schema
`0x80800014`, reads the identity bit, calls `FUN_140A020E0(rsat)` to queue the resource, then calls
`FUN_140A01C70(rsat)` to test residency. A false readiness result makes the codec return false;
`FUN_141718080` maps that failure to result 2 in the normal runtime mode. This is the complete
explanation for the 19-bit rejection and for earlier nondeterminism: the same valid create succeeds
only when another path has already made its RSAT resident.

The client now resolves those adjacent private queue/readiness calls from the live kind-0 create
decoder. On a game-owned view-lookup call it queues `0x815B204B`, polls readiness, and publishes
only two transition logs: `sobject-rsat-preload result=queued` and then `result=ready`. The server
create planner has a new `rsat` gate and cannot start its 500 ms scheduler-settle interval until the
native predicate is ready. A resource miss therefore no longer uses a partially consumed entity
packet as a preload request.

The preloaded run then passed both remaining transport milestones without zone movement. The
client accepted the create after exactly 77 bits, occupied slot 13 as entity `0x0010000D`, and
accepted the separate 130-bit native transform update after exactly 152 top-level bits. The update
record retained the same handle and exact player-X-plus-three position. No framing or resource
failure remains between the server's guarded scheduler body and the client's native sobject
allocation/update path.

Both accepted records still decoded with `cell=0xFFFF`, however. Inheritance is functioning, but
the active entity-list context itself is the global/default cell; it does not attach a new object
to the streamed EDZ bubble. The current selected arrival hash `0xB8459D59` resolves from the same
scenario layout to Town bubble ordinal 51 and map-global index 145.

The first explicit-cell build incorrectly wrote the local ordinal 51. Both sends rolled back after
19 visible bits with result 2, produced no record, and their rejected retransmissions accumulated
78 corrupt reads before a four-second receive timeout. Ghidra resolves the index domain: the cell
initializer uses the decoded byte to index a 256-entry spatial table and the same 256-bit component
masks that authored containers key by map-global bubble index. The wire cell is therefore Town's
map-global index 145 (`0x91`), not scenario-local ordinal 51. The corrected build writes 145 on both
create and update. The predicted accepted sizes remain 86 and 161 bits, with both records reporting
`cell=0x0091`.

The corrected run matched all four predictions. Create decoded after 86 bits as entity
`0x0010000D`, slot 13 became occupied, and the staged transform decoded after 161 bits on the same
entity. Both records reported `cell=0x0091`, and the update scratch retained the exact nearby
position. The native runtime constructed shared-Vandal RSAT `0x815B204B`; nothing rendered.

Creation still raised both native missing-update diagnostics and instantiated the object from an
injected zero baseline. The transform arrived 66 ms later. The registration's third argument was
one, but Ghidra shows that value reflects the construction record's absent `+0x30` binding and the
same value occurs on several normal streamed objects; it is not the replicated slot-binding result.
The next bounded experiment retained slot 13 as the create-then-update control and used the captured
130-bit transform in a combined create/update for pristine slot 14.

### Slot-14 atomic acceptance and the post-decode boundary

The atomic run completed the current wire-format milestone. At `t=66530`, the slot-13 control on
token `0x9EAA300100200002` decoded one record after 86 bits as entity `0x0010000D`, cell `0x0091`,
flags `0x0001`; its two missing-update diagnostics were expected. At `t=66597`, slot 14 left with
`update=inline`, `update_bits=130`, and `combined=1`. The client returned `result=0 count=1` after
exactly 216 bits and reported entity `0x0010000E`, cell `0x0091`, flags `0x0003`, a transform-dirty
mask, and the intended player-X-plus-three transform. It added no missing-update diagnostic.
Namespace 1 advanced to 15 occupied objects (`occupied_low=0x00007FFF`).

This proves resource residency, the explicit map-global cell, atomic create/update framing,
transform decoding, and replicated-slot allocation. It does not by itself prove that the staged
record became a renderable native simulation object. Only one `sobject-native` line appeared for
RSAT `0x815B204B` because that logger deliberately emitted once per RSAT per process. The absence
of a second line was therefore a logging blind spot, not evidence that slot 14 skipped
construction. The next passive diagnostic keeps the general one-per-RSAT catalog while reporting
the first eight target-RSAT registrations with occurrence numbers.

Ghidra places the next boundary after the successful list decode:

```text
FUN_141718510 / FUN_141718080 decode and stage
  -> network job type 2
  -> FUN_1416E6ED0
  -> FUN_1417085C0 re-decode and validation
  -> FUN_1416FF790
  -> kind-0 codec +0xB0, FUN_1417242F0
  -> FUN_141704870 glue dispatch (direct table write or queued callback)
```

`FUN_141704870` writes the native object index into the simulation glue table, or queues the
corresponding glue-set operation. The latest run produced target-RSAT occurrences one and two and
then called this dispatcher with valid native indices for both experimental slots:

```text
decoded 0x0010000D -> dispatch 0x7CFBA00D slot 13 native 0x43FC400C
decoded 0x0010000E -> dispatch 0x2FFBA00E slot 14 native 0x2DFC400D
```

The generation bits change between staged decode and native application, while the low 13-bit slot
does not. These events prove both records reached native construction and the kind-0 glue
dispatcher. The user heard positional Vandal audio immediately beside the player, supporting the
nearby transform, but saw no model and the object neither reacted nor attacked. Native
construction, audio, and position therefore work; active-world/render ownership and AI do not.
The entry-only dispatch still does not prove that its table write completed. The revised probe
therefore arms the dynamic slot from each successful `entity-record`, preserves repeated dispatcher
calls, and reports `stage=sobject-bind-dispatch` with the exact glue-table value before and after
each call. `status=bound` means the post-call value matches the requested native index;
`status=deferred-or-skipped` means the initial call did not establish that postcondition. A queued
callback tail-jumps back through the detoured entry and should produce a later bounded occurrence
with `status=bound` when it applies.

Both experimental records were sent to the older populated token `...0002`. The client had already
begun the `PUB24.24` transition and routed newer token `...0003`; the newer empty view became
replication-ready at `t=67198`, bound at `t=67465`, and was the settled current public group by
`t=72536`. The selector retained `...0002` because it had 15 occupied slots while `...0003` had
none. This retired-populated versus settled-current split is now a stronger visibility hypothesis
than another payload change, although it is not yet causal proof because the atomic send occurred
while the transition was still in flight.

The corrected overlay is visually confirmed and reports physical slice 24, bubble 3. A later
screenshot shows old region-408 token `...0002` ready and current region-24 token `...0003`
unjoined after the newer row had left. That screenshot is not injection-time state: during the
injection, `...0003` joined and bound only after the atomic slot-14 dispatch. This timing supports,
but does not yet prove, that both native objects were constructed for a view that was retiring.

The newest run later hit one four-second receive timeout at `t=103241` with 84 corrupt reads. The
process rebuilt the connection and continued afterward. This transport defect remains open, but it
occurred more than 33 seconds after both native constructions and does not invalidate their decode,
registration, or dispatch evidence. The accepted 216-bit record should remain unchanged while the
glue-table postcondition and view ownership are observed.

The current test build resolves the leading ownership mismatch without changing that accepted
record. `select_replication_view()` now follows the primary-world region to its advertised group
session and exact bound activity-host view, and fails closed instead of selecting the most-populated
retiring view. Spatial cell selection is derived from the selected token's held region and the
destination scenario's `bubbleMapIndices`: EDZ region 24 -> bubble ordinal 3 -> map-global cell 11
(Basin), while region 408 -> ordinal 51 -> cell 145 (Town). This directly corrects the prior
`...0002` / cell-145 injection while the client was entering Basin on `...0003`.

Before enabling a create in that current view, the first probe emitted one empty two-view validation
packet. Its 275-bit signature plus two `00010` tails was directly acknowledged after 66 ms, and the
client's remote signature changed to both views. The handler trace proved that was necessary but not
sufficient: view 0 consumed `1,1,2,1,1` bits and completed by taking the first zero of view 1; view 1
then consumed `1,1,10,19`, returned result 2 in the entity lane, and never called the fixed handler.
The later packet-loss summary reported one corrupt read. This is the exact historical reason the
entity remained suppressed; the build intentionally carried no Vandal.

The historical correction appended two six-bit `000100` chunks, for 287 stored scheduler bits
total (275 + 6 + 6), while leaving the proven one-view helper unchanged. Its next run passed: both
views consumed `1,1,2,1,1`, every handler returned zero, ordinal 9/fixed reported complete, and
packet 135 was directly acknowledged after 66 ms. Later traces clarified that those chunks were
not independent view bodies: signature bit 274 supplied view 0's event, the first chunk's final
zero supplied view 1's event, and each view consumed a five-bit `00010` remainder. A target entity
body consumes its own fixed bit, so the following empty view instead needs the full `000010` body
used by the final writer. The selected view was current Basin token `...0003`, namespace 2, region
24, bubble 3, map-global cell 11. The later aggregate reported 536 delivered, zero lost, and one
corrupt read without a timeout or disconnect.

The first bounded Basin-create run proved that the cached remote list is not sufficient once the
local list changes. The empty probe completed all ten handler calls at `t=71208` and was directly
acknowledged at `t=71235`. At `t=71229`, however, root token `...0001` joined the scheduler: the
local logical list became `[...0001,...0002,...0003]` while the decoded remote list remained
`[...0002,...0003]`. The server sent its cached two-view create at `t=71235` to the correct current
Basin token `...0003`, namespace 2, view 1, slot 0, region 24/bubble 3/cell 11. No second handler
epoch ran, no `entity-list-decode` or `entity-record` followed, occupancy stayed zero, and there was
no target native registration or glue dispatch. The packet therefore failed before construction;
it is not evidence of an invisible current-view Vandal. The later packet-loss summary reported zero
corrupt reads. The accompanying overlay confirmed the physical Basin state and showed both the old
region-408 and current region-24 activity hosts connected and ready.

The revised bounded build treats the synchronous ten-handler completion as the application proof
and does not wait for the transport ACK before using the short two-view window. It requires the
live local layout to match the cached two-view order exactly; a stale remote capture is tolerated
only while still pristine and only after all ten native calls returned zero. The create can leave on
the next service tick. Its own completed handler epoch produces the exact 130-bit current-player
transform, which the following tick sends as an update-only record to the same slot without waiting
for the occupancy probe to publish. Both send-time checks fail closed as soon as root token
`...0001` expands the local scheduler.

That first pre-ACK run decoded the current-view create successfully. At `t=80823`, namespace 2
consumed 86 entity bits and produced entity `0x00200000`, cell `0x000B`, flags `0x0001`, with the
accepted shared-Vandal profile `4B205B8101000000AC2200005C000000`. Slot 0 was occupied by
`t=80856`, and the private native encode recovered the exact 130-bit player-X+3 transform. The
server sent the update-only record at `t=80856`; the root membership added token `...0001` to the
local scheduler during the same tick, however, and no third handler epoch or update record ran.
There was also no target-RSAT native registration or glue dispatch for the create-only slot. Thus
the correct current view and cell now accept allocation, but the split update still loses the
scheduler race before native construction. The later interval contained one corrupt read and zero
lost outgoing packets.

The current candidate pre-encodes the update before the two-view window. Once RSAT `0x815B204B` is
resident and a player position exists, it invokes the already-proven native update encoder against
the exact accepted create profile, identity transform baseline, and observed mask metadata
`0x00004000`. Readiness now requires the retained output to be exactly 130 bits. The first Basin
record consumes that capture as a combined create/update and marks the staged follow-up complete,
so root-view registration cannot split the creation from its transform.

That atomic record passed. At `t=68853`, the current Basin token `...0003` decoded namespace 2,
view 1, slot 0, cell `0x000B`, flags `0x0003` after exactly 216 entity bits. Its update scratch
contained the intended player-X+3 transform, there was no baseline-injection assertion, and
occupancy rose to one at `t=68859`. The ten handler calls all returned zero and the later health
summary reported zero corrupt reads. No `0x815B204B` native registration and no glue-dispatch line
followed, however. This proves the remaining failure is after record validation/occupancy but
before kind-0 native construction, not in the current packet grammar, cell, or transform.

The next diagnostic hooks the exact Ghidra chain without changing the record. `FUN_1417085C0` is
the asynchronous type-2 apply job; it calls `FUN_1416FF790` to re-decode retained creation state,
validates the entity/cell, and invokes codec virtual `+0xB0`. Kind 0 resolves that call to
`FUN_1417242F0`, which returns true only after obtaining a native object and dispatching
`FUN_141704870`. Watched decoded slots now produce bounded `sobject-apply` and `sobject-kind0`
lines. Their presence and return state separate missing job dispatch, pre-codec validation failure,
constructor rejection, and final glue binding in one run.

The diagnostic run stopped before both hooks. They attached successfully, but the valid namespace-2
record at `t=69242` produced neither `sobject-apply` nor `sobject-kind0`; it also produced no target
native registration or bind while occupied slot 0 persisted. The empty two-view proof left at
`t=69208`, the atomic record followed at `t=69241`, and root membership expanded the local scheduler
at `t=69274`. That is the same roughly 33-millisecond interval in which the older-view experiment
had reached native construction. The next candidate therefore carries the atomic record in the
first already-proven two-view signature packet instead of spending that packet on two empty tails.
It also hooks `FUN_141714840`, the pending-record batch commit/drain, and reports a watched record's
batch flags as `sobject-commit`. This separates failure to commit from failure to enqueue/dispatch
the later type-2 apply job.

The first-packet writer preserves the captured 275-bit signature, completes the outgoing view with
six empty bits, and gives the current view its full 220-bit handler body. The probe therefore logs
`body_bits=501`; the entity-list lane within that current-view body remains the proven 216 bits.
The Release candidate SHA-256 is
`7a5ae97befc9be4050403c9342c57b5a6a8d6a324da9835d92e8c5439a826211`.

The first-packet run passed the wire and timing test but still stopped before native construction.
At `t=74442`, namespace 2 decoded entity `0x00200000`, cell `0x000B`, flags `0x0003`, and the exact
nearby transform after 216 entity-list bits. All ten handler lanes completed, slot 0 became
occupied, and packet 139 was directly acknowledged 27 ms later. Root membership did not expand the
local scheduler to three views until 68 ms after the create. There was still no type-2 apply,
kind-0 constructor, target RSAT registration, or bind. This disproves the short scheduler-window
hypothesis while preserving the accepted packet grammar.

Ghidra also corrected the meaning of `FUN_141714840`: it is a rollback/merge path for a linked
batch list, not the ordinary accepted-record commit. The normal receive path invokes
`FUN_141716010(manager, record)` immediately after decoding. For internal flag `record+0x42 bit 0`,
that function allocates the 0x70-byte replicated row, copies cell/entity/creation state, sets
row `+0x68 bit 0`, walks to the root internal row, and sets its bit in manager `+0xCA20`.

Native construction then follows this per-frame path:

`FUN_1416CCA40 -> FUN_141717790 -> FUN_14170B660 -> FUN_1417084B0 -> type-2 job -> FUN_1417085C0`

`FUN_1416CCA40` services only the runtime's selected replication manager. `FUN_141717790` scans that
manager's root-dirty bitmap. A backend-busy predicate can divert every dirty row to reason 5 without
building jobs. Otherwise `FUN_14170B660` requires the replicated row to be eligible, including an
active spatial-cell bit for its 16-bit cell, before `FUN_1417084B0` serializes the row and allocates
job type 2. The builder returns 4 for serialization failure, 1/2/3 for allocation or queue refusal,
and 0 with a non-null node for success.

The next build therefore logs the exact accepted slot at `sobject-promote`, the selected manager and
root dirty transition at `sobject-dirty-service`, and the serializer/allocator result at
`sobject-type2-job`. These probes are passive and bounded; the entity body remains unchanged. The
Release DLL SHA-256 is
`f0467bca1b03b7023767a68f3225b9208ee4a0982a849058a0f2e18a4ebdc7c1`.

That build proved normal receive promotion and isolated the missing service boundary. At `t=79732`,
packet 137 carried the exact two-view 501-bit body and the namespace-2 view-1 create. At `t=79733`,
the client decoded entity `0x00200000`, Basin cell 11, wire flags `0x0003`, internal flags `0x0023`,
and object generation 2; `sobject-promote` reported manager `0x4730E48` occupancy changing `0 -> 1`.
The packet was directly acknowledged at `t=79799`, after 67 ms. No `sobject-dirty-service` ever ran
for that watched namespace-2 manager, and no type-2 job, apply, kind-0 construction, target native
registration, or bind followed during more than two minutes of otherwise healthy traffic. The
runtime is therefore servicing a different replication manager; the failure precedes dirty-row
eligibility and active-cell testing.

The next bounded proof preserved the bound/current Basin token `...0003` at scheduler view 1 as
the authority and spatial source (region 24, bubble 3, cell 11), while moving only the entity record
to scheduler view 0's namespace-1 token `...0002`. Its first Release SHA was
`13d441ff2878130ddd6e6c50a40b3a88a1a2df3ffa3512b76ccbe0e8d3c5b060`.
That deployed run did not emit a scheduler probe or entity record. The client-side target was live:
manager `0x471F040`, namespace 1, scheduler entry 0, 13 occupied objects (`0x00001FFF`), and pristine
next slot 13 with zero generations. On the server, however, token `...0002` stopped at
replication-ready (`local=4 remote=4`) and never reached the bound state. Requiring the target to be
server-bound made `prepare_two_view_probe` fail; the fallback gate reported `scheduler-shape` at
`t=79237`. There were consequently no handler lanes, entity decode, promotion, native jobs, or
synthetic occupancy change. The run was stable: 794 outgoing packets were delivered, none were
lost, and the client reported zero corrupt reads.

The corrected proof requires only one server view row with the exact target token; it does not
mistake target stage-5 binding for client-manager liveness. The target remains strictly validated
by a non-null client manager, namespace 1, exact scheduler key/tag and two-view local layout,
occupancy exactly 13, next slot exactly 13, zero handle/reserved/object generations, loaded Vandal
RSAT, and the exact 130-bit nearby update. The authority remains separately required to be the
bound current Basin group at scheduler view 1.

The deployed `f664c98f...` run passed those gates and sent packet 135 at `t=81002`. It carried
authority `...0003/1`, entity target `...0002/0`, namespace 1, slot 13, region 24, bubble 3, cell
11, and `body_bits=501`. View 0's entity-list decoder consumed the exact 216-bit atomic list and
emitted entity `0x0010000D`, flags `0x0003`, create
`4B205B8101000000AC2200005C000000`, and the intended nearby-player transform. That log line proves
the record payload itself decoded, but not that the enclosing scheduler transaction committed.

The earlier explanation for that failure was wrong. A nonempty target view does consume 221
handler bits (event 1, mask 1, entity prelude 2, entity list 216, fixed 1), but the replayed
signature and target body overlap that boundary intentionally. Sunrise stores 275 signature bits,
while `scheduler-native-signature` proves schema `0x80806AEA` consumes 274. Stored signature bit 274
therefore supplies view 0's event bit. The unappended 220-bit target body already supplies its mask,
two-bit entity prelude, 216-bit entity list, and fixed bit. The `f664c98f...` failure came from the
old empty-tail order `000100`, not from view 0 borrowing view 1's first bit.

The next correction changed the empty tail to `000010` but also appended an unnecessary zero after
the target, producing Release SHA
`5262387a228cf793c17aed6b1d2c54b4d3dcd618636bb02bad5db27ca9163d5b` and `body_bits=502`. The
deployed run sent packet 132 at `t=68651`. View 0 completed all five handlers: its total was 221
bits, its entity-list lane consumed exactly 216, and it emitted the correct namespace-1 record for
entity `0x0010000D`, Basin cell 11, flags `0x0003`, shared Vandal RSAT, and nearby transform.

The appended zero then became view 1's event bit and shifted the intended empty tail. View 1's
event, mask, and prelude consumed `1/1/2`; its entity-list decoder saw count 1, returned result 2
after 45 bits, and never reached the fixed handler. The enclosing transaction rolled back. There
was no `sobject-promote`, slot 13 stayed absent, dirty service remained
`internal=-1 mapped=0 dirty=0`, and no type-2 job, apply, kind-0 construction, native registration,
or bind followed. Packet 132's ACK after 133 ms again proved transport delivery only. No audio or
visible Vandal was expected.

The 502-bit run remained healthy for more than five minutes. The 120-second aggregate reported one
corrupt incoming read; later 180-, 240-, and 300-second reports each returned zero. Outgoing loss,
assert hits, and network hitches remained zero. At `t=308528` the client gracefully left its
sessions, stopped the transition, and completed `shutdown result=ok`; those disconnect lines were
normal shutdown, not a gameplay failure.

The final correction keeps the aligned six-bit empty body `000010`, removes the extra appended
target zero, and restores `body_bits=501`. This follows the 274/275 signature boundary and leaves
view 1 at its exact start. The installed Release DLL SHA-256 is
`28b14320728d4d2cabd0d0ba8384a4847449ea8f50b37b08e2112573b141bf03`.

The generic two-view writer now applies the same boundary by index. View 0 uses the signature's
carried event and writes only the five-bit `00010` remainder; a later empty view writes all six
`000010` bits, while a later entity view writes an explicit event zero before the common 220-bit
post-event body. Both two-view target positions therefore remain 501 bits. The special Basin test
still targets view 0, so this safety correction does not change its packet.

The `28b14320...` runtime test passed the complete wire and promotion path. At `t=78255`, packet
148 carried authority `...0003/1`, entity target `...0002/0`, namespace 1, slot 13, region 24,
bubble 3, cell 11, and `body_bits=501`. View 0 consumed handler widths `1,1,2,216,1`; view 1
consumed `1,1,2,1,1`. Every result was zero and ordinal 9/fixed reported `status=complete`.
Namespace 1 emitted the exact entity `0x0010000D`, flags `0x0003`, shared Vandal creation, and
130-bit player-X+3 transform. `sobject-promote` immediately returned on manager `0x471F040` with
internal flags `0x0023`, object generation 2, and occupied `0 -> 1`. Packet 148 was directly
acknowledged after 65 ms. Framing, record decoding, atomic update, active-manager selection, and
promotion are therefore proven and must not be changed again.

The promoted row then reached `FUN_141717790` eight observed times. Every entry and return mapped
slot 13 to internal row 13 and retained the manager dirty bit `1 -> 1`; no
`sobject-type2-job`, apply, kind-0 construction, target native registration, or bind followed.
Ghidra explains this exact signature. `FUN_141717790` resolves a context from the manager provider,
then calls `FUN_1416EC0F0(context)`. That leaf is exactly
`return *(int32_t*)(context + 0x560E4) > 0`. When true, dirty service walks dirty rows through
`FUN_141712850(..., reason=5, ...)`, never calls `FUN_14170B660`, and does not clear the manager's
dirty bit. The normal branch calls `FUN_14170B660`; absent a successful type-2 job, its row/cell
failure paths would not match the repeatedly retained dirty state as closely.

The lifecycle log independently supports the suppression interpretation. A normal z-leg to region
24 began at `t=76451`. The public-target group and activity host joined, but the transition never
reported `completed` or promoted that target to `PUBLIC CURRENT`; shutdown finally stopped it at
`t=170369` because the slice-set transition manager was disabled. Network health remained clean:
the 120-second packet summary reported 19 valid reads, 785 expected discards, and zero corrupt
reads. Terminal connection-suicide lines were teardown, followed by `shutdown result=ok`.

The deployed predicate hook at `FUN_1416EC0F0` conclusively returned `backend_count=0` and
`backend_busy=0` on all eight watched namespace-1 dirty-service calls. The 501-bit transaction
completed, promotion mapped slot 13 to internal row 13 with flags `0x0023`, and the dirty bit stayed
set, but `FUN_1417084B0` was never entered. The unfinished z-leg transition therefore is not the
immediate suppression condition; the row is retained inside `FUN_14170B660` before type-2 job
construction.

The deployed `FUN_14170B660` probe resolves the missing predicate. Cell 11's row is kind 0 with
control `0x00`, flags `0x0003`, defer `UINT64_MAX`, retry zero, `suppress_create=0`, and an intact
creation pointer. Across all eight calls the batch remains `0 -> 0`, no type-2 builder runs, and
the caller's state output becomes 1. The decompiled processor increments the batch before any
create/defer/serializer decision; the only eligible pre-increment failure here is its
`cell < 256 && active_cell_bit == 0` branch. Namespace 1 therefore rejects Basin cell 11 because
that manager still owns the outgoing Town world.

The next bounded control keeps Basin region 24/cell 11 as the current-authority gate, but writes
Town region 408/bubble 51/cell 145 into the namespace-1 entity record. No scheduler, RSAT, update,
slot, or transform bytes change. This should restore type-2 construction and positional audio but
is not expected to make the object visible in Basin. Pending Release SHA-256:
`1f3c7939e4b84d9337fc0cdbde41696a7ec13018fb0da623402f37ce727da0ac`.

This synthetic entity is scoped to the selected replication view and map cell. Sunrise does not
migrate it to a successor view or publish its removal yet, so an overlapping bubble may retain it
briefly and then cull it when that view leaves. The control is armed only while Basin region 24,
bubble 3, cell 11 is current, but its namespace-1 record deliberately names Town cell 145.

The deployed Town-cell control completed the full native path at `t=78967..78972`: the namespace-1
dirty row accepted cell `0x0091`, the type-2 builder returned result 0 with a job, target RSAT
`0x815B204B` registered, kind-0 returned 1, and glue slot 13 became native handle `0x58FC400C`.
The user heard clear positional Vandal audio but saw no model. That is an end-to-end positive
control for create/update framing, RSAT resolution, type-2 dispatch, native construction, and glue
binding. Its expected visual failure follows from ownership: namespace 1 and cell 145 still belong
to the outgoing Town view while the player and renderer are in Basin.

The upstream mismatch begins before entity replication. The captured EDZ selection names Town
hash `0xB8459D59`; Sunrise consequently publishes arrival slice 408 and the client completes an
initial Town transition. Broad spawn set `0x9617A6E7` has 54 points whose mask spans cells
30, 38, 63, 73, 82, 145, and 155. Its `(509.151,30.129,73.822)` cluster places this character on
the Basin side, so the client immediately begins a normal z-leg to region 24. Basin becomes a
public target but does not become the serviced simulation manager during the stationary test.

Directly selecting Basin bubble 3 did not provide a valid initial world. With SHA `f6aeb696...`,
the client completed slice 24 as PUBLIC CURRENT, but namespace 1 contained only one active kind-2
object rather than Town's normal 13-object baseline. View readiness remained pending and the local
player never instantiated, leaving a black screen. This disproves direct-Basin arrival as a safe
ownership shortcut and shows that the authored initial baseline is tied to the arrival pairing.

The cache provides a narrower coherent control without bypassing that baseline. Spawn set
`0xCB8903DF` is map-resident, has three points around `(527,159,75)`, and references only map cell
145. The current override pairs it with Town bubble 51. If the player remains in Town, the ordinary
one-view path should create the Vandal in active namespace 1/cell 145, removing the prior renderer
versus entity-owner mismatch without forcing the active manager or changing entity wire fields.

The shared *Destiny 2 Activity System & Authored-Content Internals* paper does not provide a peer
account or membership codec. Its sections 7 and 8 do corroborate the current sequencing: the
public/peer route is the transport milestone that creates a real session and entity slots but only
a live, empty world; authored mode and its server-published selection are the later layer that
creates encounters, AI, scripts, and objectives. Stabilizing this public group session is therefore
still prerequisite work, not a detour from enemy spawning.

The same run showed the public namespace active set start at slot 0, then grow to exactly 13
contiguous slots `0..12` when twelve additional native sobjects register. It never clears, so the
public view remains pending. A separate namespace-2 view with `active_count=0` immediately reports
ready. The next entity boundary is therefore completion/acknowledgment of those 13 public native
objects, not the readiness virtual or a general view flag.

### External NetDuma connection-table capture

The shared `destiny 2.pcapng` is not a packet capture from the Destiny host's gameplay interface.
It was recorded on a Windows machine while a browser repeatedly called a NetDuma router's
`com.netdumasoftware.ctwatch` JSON-RPC endpoint. The roughly 215-second file contains 230 decoded
connection-table snapshots, but the actual game datagrams never traverse the capturing interface.

One sustained connection on the same LAN device as active Steam traffic is consistent with the
reported Destiny session: client source port 2003 to remote UDP port 3074. Across the capture its
counters increased by 1,704 sent and 1,685 received packets, or about 1.30 MB sent and 0.69 MB
received. That supports a long-lived bidirectional retail gameplay route, but attribution to
Destiny is still an inference from the file's provenance and the concurrent Steam activity.

The capture contains no UDP/3074 payload bytes, traversal records, DTLS frames, scheduler bodies,
entity lists, or sobject create/update messages. It therefore cannot determine entity wire layout
and does not justify changing Sunrise's loopback gameplay or activity-host ports. A useful retail
comparison capture must be taken on the game PC's active NIC (or a real mirrored router port), must
include packet bytes rather than router statistics, and should begin before activity launch so the
initial handshake and first zone load are both present.

The file also records an HTTP Basic authorization header for the router in clear text. Do not
redistribute it; if the credential is still valid, rotate it before sharing any sanitized excerpt.

## Source areas changed

The current checkpoint includes work in:

- `Sunrise/src/client/hooks/network/`
- `Sunrise/src/client/patterns/` and `Sunrise/src/client/targets/`
- `Sunrise/src/middleware/bap/activity_message/`
- `Sunrise/src/middleware/gameplay/peer/`
- `Sunrise/src/server/bap/encrypted/`
- `Sunrise/src/server/gameplay/group/`
- `Sunrise/src/server/gameplay/peer/`
- `Sunrise/src/state/activity/membership/`

## Next investigation

1. Start a fresh EDZ session and remain in the loaded region after native PUBLIC CURRENT changes.
   Require `scheduler-post-handoff-probe result=sent handler_complete=1`, all 10 or 15 handler
   lanes to complete, and `result=transport-accepted proof=ack handler_complete=1`.
2. Require the following atomic create to use that same current host token, namespace, region,
   bubble, cell, and exact scheduler layout. A layout change must return to empty validation rather
   than sending through stale scheduler state.
3. Require the atomic create to use that same current host token, namespace, region, bubble, and
   cell, then confirm type-2 result 0, native registration, kind-0 success, and a completed bind.
   The overlay must say `owner matches` rather than `OWNER MISMATCH`.
4. If this makes the Vandal visible, retain coherent arrival/spawn pairing and generalize entity
   residency per bubble. If it remains audio-only, capture parent/stream-source state from a real
   authored biped before changing the payload.
5. Require `sobject-kind0 result=1`, target native registration, and
   `sobject-bind-dispatch status=bound` before treating the remaining failure as rendering.
6. If a correctly owned and bound object remains audible but invisible, capture a real authored
   biped's parent, stream-source, and RSAT suffix before changing the payload. Do not guess them.
7. Treat AI activation separately: trace EDZ spawn-rule/squad/director creation. Kind-1 receive only
   binds an already-existing native squad and does not turn a standalone kind-0 sobject into an
   active encounter enemy.
8. Trace the activity-host lifecycle after the already-successful EDZ mode selection
   (`definition=0x0109ED6B`, activity type 6) and public remote-session constructor. Identify which
   missing host state or server publication starts director/encounter evaluation. Do not modify the
   correct local-posse versus remote-public route selectors.
9. Use archive scenario `0x80B2F00A`, simple encounter `0x80B2F02A`, spawn rule `0x80B2E997`, and
   squad `0x80B2E9A2` only to validate authored relationships. None is interchangeable with the
   runtime sobject RSAT field.
10. Reuse the generic envelope from upstream `b8ccfb9b` only after the payload callback can emit the
   exact native body. Its physics host and activity receipts can then become useful downstream,
   after visible entity replication is proven.
11. If a retail comparison becomes available, capture real UDP gameplay bytes on the game PC from
   before activity launch through initial zone load. The existing NetDuma file contains counters,
   not entity packets.

## Build and verification

```bash
cmake --build /tmp/sunrise-entity-probe-release-codex --config Release --parallel 8
sha256sum build/x64/Release/steam_api64.dll
git diff --check
```

Previous bounded Basin-spawn Release candidate SHA-256:
`69ceeaee5d60b92091f97331ee0d21f2bd92d9185b5661e6ebcaf48de5fb096d`.
Committed as `2abfb562 test: create Vandal in current Basin view`.

Current pre-ACK Basin Release candidate SHA-256:
`a9afb46a3bd8e273f7346f312c333fcef18d61f2985bec692e157e922db5d3d6`.
Committed as `12968ec2 test: use proven two-view window for Vandal`.

Current atomic Basin Release candidate SHA-256:
`cba76fa8b262b02ff4453560ae5f7456ad00715f09db318bbf524f10fd76a9c2`.
Committed as `4a19acb test: atomically create current-view Vandal`.

Current post-decode diagnostic Release candidate SHA-256:
`b295f21dcaaf17706a05766f75ce477dde50e606b24f3aad4d2e6886fefc40ca`.

Current first-packet Basin Release candidate SHA-256:
`7a5ae97befc9be4050403c9342c57b5a6a8d6a324da9835d92e8c5439a826211`
(`scheduler-two-view-probe body_bits=501`).

Current promotion/service diagnostic Release candidate SHA-256:
`f0467bca1b03b7023767a68f3225b9208ee4a0982a849058a0f2e18a4ebdc7c1`.

Deployed target-bound active-manager proof SHA-256 (no packet sent):
`13d441ff2878130ddd6e6c50a40b3a88a1a2df3ffa3512b76ccbe0e8d3c5b060`.

Deployed 501-bit active-manager proof SHA-256 (old `000100` tail; transaction rolled back):
`f664c98f1a22f338eb41bc3ec62e3bc2b4d11a7e1220779ebe0af94d9894a330`.

Previously deployed 502-bit active-manager proof SHA-256 (extra target zero; rolled back):
`5262387a228cf793c17aed6b1d2c54b4d3dcd618636bb02bad5db27ca9163d5b`.

Runtime-proven final 501-bit active-manager proof SHA-256:
`28b14320728d4d2cabd0d0ba8384a4847449ea8f50b37b08e2112573b141bf03`.

Deployed passive backend-predicate probe SHA-256:
`c7eac83722020049a6dd9559241127efdc9c99a63d823a4d06ab6c7e7b040dae`.

Deployed passive dirty-row probe SHA-256:
`39f8cba810a0f2272c527e44589e0aee9657c753b5e66301dd8b3e6deefbb140`.

Successful Town-cell construction control SHA-256:
`1f3c7939e4b84d9337fc0cdbde41696a7ec13018fb0da623402f37ce727da0ac`.

Rejected direct-Basin-arrival SHA-256 (black screen; missing normal baseline):
`f6aeb6968e0251e32a66acb0eb250ed083016a82d4862e118667ea5a344a012e`.

Town bubble 51 plus Town-only spawn-set `0xCB8903DF` Release candidate SHA-256:
`f70cd002c75cf9e42d2345340f17d564b66b77ad51f6d00e241cafca3b68aa7f`.

Current-region simulation-manager promotion Release candidate SHA-256:
`54c55d9b2e6cdc40ac3634181d36506d23f3bc734d7632355bd0909d3c419edf`.

Transition-safe passive-manager Release candidate SHA-256:
`af9fd044c9d4aada42844c89ea065bdb567a8909e51395a00bcee35c8bd3e487`.

Citizen-acceptance passive diagnostic Release candidate SHA-256:
`120c0594ee2ab23d237c18f8e2f13e0fe1bd994fd3dd97678dc81154dd853d26`.

Visit-aware transaction-retirement Release candidate SHA-256:
`b2202d68ebdb850298fb4f734bf7edfda19aa60a6bcfcaa14dbd81a55679cc04`.

Post-handoff scheduler-validation Release candidate SHA-256:
`dabac3c2de0de14f7174c3beb32789ef822342f79f29316ce8c5f90f3b3332e8`.

Citizen-join-status diagnostic Release SHA-256:
`ad324006a54b2f2732e07e44db3819afeb8b27e102bfcc226daf703d59498215`.

Visit-safe descriptor-retirement Release candidate SHA-256:
`a59f8c047eda82adaab6f7e82a95c3ecd533cdeb424ee013d302a94592b1acc0`.

The `54c55d9b...` run separated replication ownership from native world residency. Namespace 2,
view 1, region 24, bubble 3, and cell 11 all matched; the record decoded, promoted, and was serviced
by manager 2. Nevertheless `FUN_14170B660` returned every frame without adding a batch or calling
the type-2 builder. In the same run the normal-z-leg stayed `PUBLIC TARGET`, never logged a
`PUBLIC CURRENT` swap or completed transition, and later target sessions stayed absent. A manual
write to `runtime+0x560E0` therefore services an unready manager but does not activate its spatial
world. The replacement now observes `FUN_1416EC250` passively. The server retains the citizen
descriptor and suppresses entity output until native selection reaches the exact current token.
The sessions overlay applies the same distinction: a player-reported row is `target` until its
captured namespace equals the native active manager, then and only then becomes `current`.

The `af9fd044...` run showed that the initial EDZ session reaches native `PUBLIC CURRENT` before its
server view ever reaches the later bound marker. The observer now publishes the guarded
`runtime+0x560E0` identity first, so `native_manager_active()` can correctly label that initial row
without weakening the entity/citizen-retirement gate. Later z-leg target sessions still establish
their group and activity host but never emit the world-controller `almost complete, accepting join`
line. Ghidra resolves its immediate tests to `FUN_141788810(session)`, which returns whether
`session+0x86C` is nonzero, followed by selected index `session+0xE93C` and a 0x120-stride peer row
whose state at `session+0x1FB8+index*0x120` must equal `10`. The new passive hook reports those
values on change as `citizen-acceptance`; it does not write session, slice, or manager state. The
roster still reports region 24 with destination-arrival spawn override slice 408, which remains a
candidate only until this probe identifies which native acceptance gate is actually false.

The `120c0594...` zone-swap run completed native selection for region 24 at `t=96891`, then exposed
an inconsistent descriptor-retirement path while returning to region 408. At `t=101941`, the root
keepalive still carried the region-408 citizen descriptor with `published=408`, `settled=24`, and
`ready=0`. Before the real region-408 transition began, an activity transaction retired that reused
group using only historical view-accepted and activity-host-published state. Later keepalives showed
`published=408 settled=408 group_published=1 group_settled=1 ready=0`; `PUB408` started at
`t=104969` and again at `t=105230`, but no citizen join followed. The transaction publication logic
now treats settled-group history as visit-specific through `activitySettledRegion` and requires the
same exact native-current activity-host predicate as the keepalive before emitting a
descriptor-free revision.

The `b2202d68...` traversal run proves visit-aware descriptor retention fixed that immediate
handoff failure. Region 24 and region 408 both issued citizen joins on revisits and both eventually
became native `PUBLIC CURRENT`; transport stayed at zero loss and corruption. Entity output remained
empty because native-current selection trailed the original two-view/275-bit transition frame. At
the usable post-handoff instants the client exposed 3-view/275-bit, 2-view/203-bit, or
3-view/203-bit logical layouts. Those shapes were intentionally outside the old one-view/203 and
two-view/275 create gates.

The new path validates those post-handoff layouts without risking an entity record first. It
replays the exact captured signature, then emits index-sensitive empty handler bodies: view 0 uses
the five-bit post-event remainder `00010` because the stored signature supplies its event bit;
later views use the complete six-bit body `000010`. Total validation width is
`wireBits + 5 + 6 * (viewCount - 1)`. The passive handler epoch must complete five lanes for every
view, and the exact packet must receive direct ACK coverage. Parser proof and transport proof are
stored separately. The next atomic create is permitted only while token, current manager, encoded
signature, logical key/tag order, selected view, and client capture remain identical. The same
failed layout is never retried; layout changes consume a bounded new validation attempt.

Ghidra resolves the asynchronous citizen-join query to `FUN_1417593A0(kind, handle)`. It has one
code caller, `FUN_140E1E040`, and the world controller advances only through its returned state.
The deployed diagnostic showed the successful initial region-408 join and the stuck region-24 join
both move through `1 -> 2 -> 3 -> 0`. Region 24 then had session lifecycle 4, nonzero readiness,
selected peer 1, and selected peer state 10. `FUN_14177A240(session)` is simply `session + 0x860`,
confirming the existing selected-peer offsets are coherent. The missing promotion was therefore not
an async join or peer-readiness failure.

At the stuck point, the root keepalive reported region 24 published for the current visit with the
matching group admitted, activity host published, and replication view bound. Both server
retirement paths nevertheless required that host to already be native `PUBLIC CURRENT`. This is a
cycle on non-initial handoffs: the client keeps the joined region as `PUBLIC TARGET` until the
citizen descriptor is retired, while Sunrise waits for `PUBLIC CURRENT` before retiring it. The
replacement gate retains the visit-generation protection from `154439a9`: descriptor-free
membership now requires `activityPublishedRegion == region`, the matching group in the published
history, an accepted view, and a published activity host. It no longer requires native current.
Thus a reused group's historical view/host state cannot retire before this visit publishes its
descriptor, but a fully prepared target can proceed to native promotion.

The `a59f8c...` run confirms the revised retirement gate does its server-side job. The initial
region-408 transition became `PUBLIC CURRENT` and completed normally. On the automatic move to
region 24, the root membership was republished descriptor-free at `t=65506`, and subsequent
keepalives reached `published=24 settled=24 group_published=1 group_settled=1 ready=1`. The client
still did not promote Basin, but the first new event at the transition boundary was an injected
post-handoff scheduler validation at `t=64530`: logical views 2, signature width 203, body width
214. Packet 135 received direct ACK coverage, while the synchronous five-lane handler epoch did
not complete. This means the replayed 203-bit signature does not share the 275-bit signature's
proven view-boundary overlap; ACK is only transport proof. The run reported zero aggregate corrupt
reads, but a decoder-incomplete experimental frame is unsafe during a world handoff and cannot be
used as an entity prerequisite.

The next build therefore narrows `post_handoff_scheduler_shape` to exactly two views and 275 wire
bits, the only multi-view grammar already accepted end-to-end. Two-/three-view 203-bit layouts and
three-view 275-bit layouts now fail closed and emit no scheduler body. Release SHA-256:
`d4df9a600a01285196f727615d5e5295206dec50e5befe69e1db2fa4da63f176`.

### Stationary overlap: native normal-z-leg completion gate

The stationary `d4df9a...` run removes the malformed post-handoff scheduler packet from the
timeline. The Basin target still passed every server and client session prerequisite: async join
`1 -> 2 -> 3 -> 0`, initialized session, selected-peer state 10, activity-host join, view bound,
visit-safe descriptor retirement, and root settlement at `published=24 settled=24 ready=1`. No
scheduler body or entity record left after the handoff began, and transport stayed at zero loss and
corruption. Namespace 1/region 408 nevertheless remained native current while namespace 2/region
24 remained target. This excludes entity framing, scheduler replay, citizen readiness, descriptor
retirement, and server settlement as the reason the native role did not swap.

`FUN_140E24C80(controller)` is the remaining normal-z-leg tick. It reads the active player's
authored z-leg entry and axis, maintains controller fields `+0x4EC..+0x514`, interpolates a scalar
coordinate, and compares it against three configuration thresholds. Its result is:

- `3` below the first threshold;
- `2` between the first and second thresholds;
- `1` between the second and completion threshold;
- `4` for the special authored two-entry case;
- `0` only after the coordinate leaves every transition band.

While transition mode `controller+0x209` is 3, every nonzero result calls
`FUN_140E12910(controller, state, "Z-leg transition update")`. That function preserves native side
effects and stores the band byte at `controller+0x352`. Result zero takes a separate branch and
calls `FUN_140E2B660(controller, "completed")`. The completion routine performs the coordinated
public-role/session swap whose downstream refresh is the only legitimate writer of the active
simulation-manager identity at `runtime+0x560E0`. It must not be called or emulated by Sunrise.

This explains the two session rows. Region 408 is the old/current overlap world; region 24 is the
joined target for the physical Basin side. They are not two players and are not stale overlay rows.
The target becomes current only after native position classification reaches zero. The earlier
visit-aware traversal run promoted region 24 and then region 408 while the player was moving; the
latest stationary run never left the overlap band. Directly starting on Basin is still unsafe
because the rejected build lacked the authored initial baseline and black-screened.

The observation-only diagnostic hooks `FUN_140E12910` with the unique 33-byte entry signature:

```text
48 89 5C 24 18 48 89 74 24 20 55 57 41 54 41 55 41 56
48 8D AC 24 90 FD FF FF 48 81 EC 70 03 00 00
```

Ghidra finds exactly one match at `0x140E12910`. Bounded `z-leg-state` reports include requested
and stored band, mode/flags, target region, raw `+0x2B0/+0x2B4` region fields, authored region,
entry index, axis, previous/target/current scalar coordinates, and position-reference state. The
Entity Debug overlay shows the same fresh classification even before a synthetic entity is sent.
No controller field is written by the hook. Release SHA-256:
`0e0cfc8c4f4173e0247bf99e3367d9edbc083ec9df8cb2409582d32a8d888d3f`.

### Preempted target and revisited-region publication failure

The ordered spawn / walk-forward / walk-back run identifies the first deterministic server fault
in this traversal. Native PUBLIC CURRENT remained region 408. The first region-24 target was fully
joined, but its normal z-leg had not completed when the client crossed into region 416. At
`t=102570` the client preempted the region-24 transition and began leaving that PUBLIC TARGET. The
root authoritative transaction reported region 416/token 3 at `t=102591`; Sunrise claimed and
allocated host `0x9EAA300100200004`, then sent the region-416 citizen descriptor in membership
revision 7 at `t=102638`. The old target slot was not actually cleared client-side until
`t=102686`. No region-416 citizen join followed.

This is a cross-transport ordering race. The gameplay leave acknowledgement and the root BAP
membership do not share one ordered stream. The client can apply the new root revision while the
single PUBLIC TARGET slot is still occupied. Before this fix, Sunrise treated successful frame
delivery as proof that the descriptor had been claimed. Later keepalives retained
`published=416`, `group_published=1`, `ready=0`, but did not advance the membership revision, so the
client never saw a claimable replacement.

Walking back exposed the independent A -> B -> A bug. Region 24 was now token 4, but its reused
group still existed in durable published/settled history and `activitySettledRegion` was still 24
from token 2. Because region 416 never settled, no scalar marker displaced that value. The old
predicate therefore concluded token-4 region 24 was already settled and sent `citizen=0` forever.
The target row's absent/unjoined state is real; the sessions overlay filters disconnected cache-only
rows but intentionally retains the player-reported current destination even when its join is
missing.

Published/settled markers now carry the exact client transition token. All current-visit tests in
both transaction and keepalive paths require group, region, and token. A published descriptor also
arms a bounded admission check. If no matching gameplay session is admitted after 500 ms, Sunrise
waits for acknowledgement, advances the membership revision, and republishes the descriptor. The
initial publication plus at most three retries prevents an unbounded revision loop. Admission or
settlement disarms the retry. No native controller, role, or active-manager field is modified.

The same run clarifies the native diagnostic fields. Controller `+0x210` stays 408 through
destinations 24 and 416, so it is the native-current anchor/source, not the requested target.
`+0x2B0` is the actual destination region and `+0x2B4` its hash. The stuck load eventually
oscillated between state 4 / entry 421 / valid special position and state 3 / entry 427 / invalid
position every roughly 6 ms, never producing state 0. The hook now reports `anchor -> destination`
and rate-limits only rapid 3/4 log changes; its fresh overlay sample still updates on every call.
The status overlay label `Teleport slice` now states what that optional value actually is. Region
and Bubble continue to come from the coherent client-reported world snapshot.

Release SHA-256 for the pending traversal test:
`8136bd6c5f33aa926bdd608b6673b180f98e3c8e127923759cd09cccb192cfbd`.

### Authored private bubbles and durable public-visit admission

The `8136bd6c...` traversal proves the visit-token repair worked: after walking back, region 24
received a new descriptor/join for token 4 and later reached native PUBLIC CURRENT. The same run
then isolated a different classification error. When the client entered region 416, its retail
world-controller name was `PRV416.4`. Sunrise nevertheless allocated group
`0xDD3F...`, host `0x9EAA300100200004`, published the citizen descriptor four times, and never
received a gameplay join. The overlay's absent/unjoined row was therefore accurate; the client did
not consider this private region a new public-host target.

The authored distinction was already decoded locally. `scenario_reader.cpp` reads the state byte at
offset 12 as `SliceState::isPublic`; its verified native interpretation is that the first state
marks the bubble PUB or PRV. `bubble_state_reader` previously discarded that field. The scenario
definition and cache now retain one `bubblePublicFlags` byte per bubble, and region classification
uses `region / 8`, the same authored slice-set factor used throughout the destination layout. Cache
format 36 forces old records without this field to rebuild. A missing layout or out-of-range bubble
defaults to public, preserving the former behavior instead of silently suppressing a descriptor.

Private-region handling is intentionally narrow. Root membership still publishes the coherent
client-reported region and roster, but it does not build a citizen advertisement, claim an
activity-host session, or reflect a public group for that region. The successful private
publication is remembered by `(region, transitionToken)` so the keepalive does not resend it every
slice. Public regions retain the existing descriptor, view, activity-host, and settlement gates.

The final part of the run showed why `session_admitted(group)` alone could not govern retries.
After several transitions, the two-row public-session table retired an older admitted record even
though that visit had already produced a valid join. The retry timer then saw no transient row and
republished the same region-408 descriptor, causing repeated PUBLIC TARGET teardown/rejoin cycles.
Each host-session row now has a monotonic admission generation. Descriptor commit snapshots the
generation for that exact visit; `publish_membership` increments it after accepting a gameplay
join. Any changed generation permanently suppresses retries for the visit even if the admitted row
is later released. Settlement clears the retry state normally.

The supplied retail captures independently support this model. `d2dumpPublic` has one long-lived
activity connection (flow 054, `+59.069s` to `+495.310s`) spanning the EDZ session and eight shorter
overlapping activity connections as public zones change. `d2dumpRaid` has one activity connection
for the complete encounter/wipe. Activity payload bodies in the bundle are not typed yet, so this
is lifecycle evidence rather than a byte-for-byte codec proof, but it rules against allocating a
new public host for every PRV region.

Release SHA-256 for this candidate:
`b83c295cb35d36328299470242b8a5b99b938fe727fe06461823ae0862ef5e57`.

### Private-region validation and split region/token ordering

The `b83c295...` traversal conclusively validates the authored PUB/PRV repair. Entries into
`PRV416` and `PRV424` changed the root player's reported region and roster, but created no public
group, no activity-host token, no citizen descriptor, and no gameplay join. Returning to `PUB24`
created a fresh public visit and joined the existing region-24 host. Across the run Sunrise
allocated only `.002` for public region 408 and `.003` for public region 24; the former `.004`
private-region host did not exist. Durable admission generations also prevented a settled visit
from being retried merely because the bounded admitted-session table later retired its row.

The remaining public loop is a different boundary. At `t=109959` the committed client-reported
region changed from 24 to 408 while the visit token remained 10. Sunrise therefore published and
settled `408/10`. Native code did not start the next `PUB408` citizen join until `t=123995`, after
token 11 arrived. That transition completed at `t=133894`, gracefully left the same host, and the
client immediately opened another `PUB408` transition using token 12. The server did not invent
those tokens, and the durable retry path remained disarmed; they originated in successive
client-authoritative/native transition events. It is not yet safe to suppress either event because
the old message-22 logs did not expose which sparse fields accompanied each update.

The passive follow-up reports one `stage=authoritative` line only when a parsed message-22 delta
changes stored authoritative state, region, or transition token. It includes region/hash and
presence, token and presence, region/token move flags, snapshot creation, and spawn/teleport
presence. This will distinguish a deliberate region-only crossing from a stale-token publication
and will show whether the immediate token-12 restart carries a region delta or only a fresh native
load token. No State, descriptor, role, active-manager, or native controller field is changed by
the diagnostic.

Release SHA-256 for the authoritative-order diagnostic:
`a4acaa0296a1070ed27fb38bf91da5b4ebeddea8b98a7c5e080531d8a847d021`.

### Traversal ordering and retail packet-layer assessment

The `a4acaa...` traversal establishes that the client has two independent notions that must not be
collapsed. Most authored load starts moved region and transition token together, for example
`24/2`, `416/3`, `448/6`, `280/11`, and `496/16`. Several physical crossings instead sent a region
with no token field at all: region 408 at `t=88887`, region 24 at `t=94652`, region 448 at
`t=102069`, and region 280 at `t=109669`. The stored token correctly remained unchanged. Token-only
updates then occurred without a region move, including region 24 tokens 7 and 8 and region 456
token 10. Therefore a region delta is authoritative player location, while a present changed token
is a native load/visit generation. Server publication may react to both, but it must never synthesize
one from the other.

The run exercised public regions 408, 24, 448, and 280 and rapid private crossings through 416,
424, 456, 96, 488, and 496. Only the four public groups routed gameplay views. Private publication
retained `group=0`, and no phantom private activity host appeared. There was no assertion hit,
network hitch, forced disconnect, or corrupt-channel failure; shutdown completed normally. No
synthetic `entity-create-out` occurred because every candidate window remained on an unproven
scheduler shape. This is the intended fail-closed result.

Three unusually large external scheduler tails (1114, 1124, and 1178 bits) were captured during
public session reconfiguration. None coincided with `sobject-create` or `sobject-update`, and the
first carried a signature update whose logical registered-view count was zero. They are scheduler
control/topology output plus trailing reserve/padding, not native enemy-creation exemplars.

The shared retail captures separate cleanly into three relevant layers:

- Port-30000 activity-host TCP is decrypted. Its service-8 requests and service-9 pushes contain
  join, authoritative activity, roster, membership, and related coordination. The inner activity
  bodies are not fully typed, but this channel is not the high-rate replicated-entity transport.
- Service-123 Investment objects are decompressed static resource/profile objects. They can supply
  definitions and content identifiers, but do not prove that one runtime enemy instance spawned.
- Port-3074 gameplay UDP contains the high-rate EDZ and lost-sector simulation stream where retail
  create/update/remove records necessarily travel. The bundle kept framing and raw datagrams, but
  their payload is high entropy and the gameplay cipher/session state is absent. Port 3075 is only
  short probe/handshake traffic in this capture.

The decrypted activity channel is nevertheless a concrete entity-authority oracle. All nine
`d2dumpPublic` activity-host sessions have valid service-8/service-9 envelopes with no declared-
length failures. Each session begins with client activity type 7 and server type 8, the current
retail build's join request/result pair, then receives exactly one server type-32 payload of 1,024
bytes within 100--178 ms of the join result. Every one of those payloads is an 8,192-bit mask with
exactly 125 set bits. The masks name different slot pools on different activity hosts; five hosts
received the same low pool, while the others received pools beginning near slots 2,016, 2,144, or
2,272.

The later authority traffic confirms that this is structured slot state rather than an arbitrary
1,024-byte body. Eight client type-42 payloads are exactly 1,028 bytes and contain a 1,024-byte mask
at offset four. Six repeat that session's original type-32 grant byte-for-byte, one adds three bits,
and one removes one bit. Client types 34, 36, and 41 and server types 35 and 37 form the same
1,024-byte-mask family with one-, two-, or five-byte prefixes during later handoffs. This evidence
can validate Sunrise's slot grant, acknowledgement, and authority lifecycle, and its 8,192-slot
width independently corroborates the target build's entity-slot codec.

The numeric activity ids cannot be copied into build 86657: the retail capture uses types 7/8 for
join where the target uses 3/4, and retail type 32 occupies the role target code assigns type 0.
More importantly, none of these activity messages exposes the native scheduler handler object.
They cannot directly reveal `self+9`, `self+0xA`, the four pending-state dwords, or whether
`FUN_141712CA0` returned true. Those remain in-process observations from `entity-collector-gate`.
The packet evidence can identify a missing authority transaction if that runtime byte never opens,
but it is not itself a plaintext entity-create exemplar.

The ten sustained UDP/3074 gameplay flows contain 11,753 datagrams. Each begins with the same
425-byte client/301-byte server exchange shape before protected traffic; those widths and the
subsequent counter progression are consistent with the association key-exchange and protected-
datagram layers, but the current retail clear header differs from build 86657's exact bit layout.
Across 5,412,192 bytes after the stable clear prefixes, byte entropy is 7.9999 bits per byte and all
256 byte values occur. Capturing both public exchange values does not recover the private SRP/ECDH
material needed to derive the traffic key. Packet lengths and timing remain usable, but attempting
to parse scheduler or entity lanes out of that ciphertext would create false structure.

A binary scan found zero literal occurrences of synthetic Vandal RSAT `0x815B204B` and zero of the
shared Vandal definition `0x80C187BD` across the bundled bodies, in either byte order. That negative
result is expected for encrypted, bit-packed gameplay data and also prevents falsely attributing an
Investment or activity message to the Vandal. Without a post-decryption gameplay buffer or the
ephemeral UDP cipher state, these captures cannot yield an exact enemy wire record. Continue at the
in-process plaintext scheduler and entity-codec boundaries; use the retail captures for connection
lifetimes, traffic timing, and later ciphertext validation.

### Outbound scheduler finalizer boundary

The four candidate encoders at `0x14171F650`, `0x14171F050`, `0x14171F200`, and `0x14171F7F0`
are not reliable lane boundaries. `FUN_1417B0D70` invokes them through vtable `+0x18` from one
globally prioritized candidate list, so their call order can repeat lanes and interleave views.
They are useful later for individual record semantics, but a strict four-call epoch around them
would reject normal traffic.

The deterministic boundary is the later vtable `+0x48` finalizer loop:

- `0x14171F020`: shared event/mask/fixed finalizer, ABI
  `void __fastcall(void* lane, int schedulerArgument, NativeWriter* writer)`, appends literal zero;
- `0x14171EFE0`: entity finalizer with the same ABI, appends literal one;
- order is view-major `event, mask, entity, fixed`, with writer objects separated by `0xD8`;
- the signature encoder runs afterward on the same thread, before those writers are copied into the
  outer packet writer.

The passive probe snapshots `NativeWriter.totalBits` before and after each finalizer, requires the
terminal-bit delta to equal one, and retains only copied scalar fields. It never dereferences the
scheduler-stack writer after the detour returns. Schema widths derive registered views exactly as
`(signatureBits - 130) / 72`, accepting 202, 274, and 346 bits for one through three views. A commit
requires exactly `views * 4` ordered finalizers and a same-thread signature within 500 ms. The
reported native total is `1 update gate + signature bits + sum(finalized lane bits)`. It must not
be compared directly with the server's old `scheduler-body` value: that diagnostic retained the
rest of the external trailer and therefore included data after the scheduler.

Logging is one compact, deduplicated `scheduler-outbound-shape` line per unique signature/width
shape, hard-capped at 32 per process. No hot candidate call logs are emitted. Release SHA-256:
`4dd1685f070b1d46d15b41b37b72fd05a92f1e44734a87b028c286e342349870`.

### Outbound entity candidate enrollment

The lane-split run captured five complete shapes across one- and two-view layouts. In every view,
event/mask/fixed widths varied but the finalized entity lane was exactly 11 bits. Because the
entity finalizer itself appends one bit, all observed native entity bodies were the same empty
10-bit prelude. The run contained no outbound kind-0 create/update call, decoded entity record,
promotion, type-2 job, native construction, or bind. Slot occupancy and dependency registration
therefore do not by themselves enroll an object for entity replication.

The next boundary is collector `FUN_14170C080` at `0x14170C080`, with ABI
`int32_t __fastcall(void* self, uint32_t view, uint32_t lane, void* context, int32_t capacity,
uint32_t* output)`. Its fixed 35-byte entry signature is unique in the current image. It walks the
manager's `+0xC920` candidate bitset and, for each internal object, applies these gates before
emitting a packed candidate word:

- handler enable bytes `self+9` and `self+0xA`;
- replicated-slot to internal-object mapping at manager `+0x114`, stride six;
- namespace support from metadata `+4` and owner compatibility;
- per-namespace dependency/eligibility flag `0x80` at metadata namespace state `+0x50`;
- object suppression bit `0x0200` at object `+0x50`;
- remaining output capacity.

The passive collector hook snapshots the watched plan before and after the native call, scans all
safely reported candidates (bounded to 1024), and records whether the target was active, eligible,
and emitted. It also captures a bounded sample of packed candidate words, whose low 13 bits are the
entity slot and whose high fields carry priority/lane/view. It changes no manager, bitset, object,
or output value.

### `203cce8...` collector test and circular watch gap

The deployed DLL exactly matched SHA-256
`203cce8b1121e33ee1ad39eea9dcefa470602f9dff8349343f71f34bfcf248f1`. In the resulting single
runtime session, `scheduler_zero_finalizer`, `scheduler_entity_finalizer`, and
`scheduler_entity_collector` all attached successfully at `t=3555`. The outbound boundary recorded
four new complete shapes and nine duplicate commits; no rejected or incomplete commit was logged.
Every captured view still finalized an 11-bit entity lane, so every native entity body remained
the empty 10-bit prelude before `FUN_14171EFE0` appended its terminal one.

No synthetic lifecycle began. The log contains zero `entity-create-out`, `entity-update-out`,
`entity-record`, entity-list decode, or `scheduler-entity-collector` records, and every server gate
report retained `attempts=0`. The earliest fully populated baseline candidate was
`0x9EAA300100200002`, namespace 1, slot 13, region/bubble/cell `408/51/145`. At `t=106580` its
capture had 13 occupied slots, 137 available slots, a valid free candidate, and spatial data, but
the first upstream failure was `scheduler-shape`: the stored scheduler described three views and
275 bits (`local=1/3`, `remote=1/0`) rather than the guarded one-view shape. Later the exact
one-view/203-bit form appeared for token `0x9EAA300100200007`, namespace 2, slot 13, and
`80/10/30`; at `t=138690`, `t=138952`, and `t=138968` that candidate advanced only as far as
`active-manager`. The final region-24 candidate again failed `scheduler-shape`. Across the run the
deduplicated gate transitions were 22 `view`, 12 `scheduler-shape`, and nine `active-manager`, with
none reaching `ready`.

The absence of collector records is not evidence that `FUN_14170C080` was never called. The first
version's `report` path suppresses `emitted <= 0` whenever no watched entity exists. Its watch is
currently supplied by `record_plan`, which is published only after the server successfully builds
an entity-create plan, or by an inbound decoder watch. This run reached neither source because the
server failed closed first. The result is circular silence: a zero-result collector call, no
collector invocation, and an unreadable inspection all remain observationally indistinguishable.
For the same reason the Entity Debug overlay correctly had no synthetic-plan lifecycle to render,
but its outbound collector row could not diagnose the intended candidate.

The observation-only correction now associates a collector call with the current native candidate
before plan creation. It reads the first valid free slot directly from the collector's live manager
and provider namespace; it intentionally does not infer a token from cached view captures because
the three native manager allocations are reused across public-session generations. The first-match
reader returns immediately and is SEH guarded. Pre-call inspection resolves the passive slot, and
post-call inspection reuses it only when the manager pointer and namespace are unchanged. A changed
manager resolves independently and normalizes the comparison fields, preventing a fabricated
`0 -> value` transition. Empty passive calls use the 16-state ambient budget, while every positive
collector result uses the 64-state useful-evidence budget. This preserves the meaning of
`record_plan`--the overlay will not claim that an entity was prepared or sent--while making the
following outcomes distinct on the next run:

- collector never entered;
- collector entered but the handler was disabled;
- candidate bit inactive or slot-to-internal mapping absent;
- namespace support or ownership mismatch;
- dependency flag `0x80` absent or object suppression `0x0200` present;
- eligible target omitted from output, or emitted with its packed priority/lane/view fields.

The direct-manager/empty-ambient correction is built as Release SHA-256
`de2ca0e736fcec207e4d2633ec738c3c2f43ca3da411d2357860341911fb0a91`. The completed prior run
was otherwise healthy: there was no assertion, `network-hitch`, forced disconnect,
corrupt-channel report, or packet loss, and shutdown completed normally at `t=161226`.

### `de2ca0e...` traversal and collector-dispatch leaf gate

The game-directory DLL and Release artifact both exactly matched SHA-256
`de2ca0e736fcec207e4d2633ec738c3c2f43ca3da411d2357860341911fb0a91` for this run. The client
completed the public `408 -> 24 -> 408` traversal without reproducing the descriptor-overcommit
hang. Three server admissions all completed; the client produced three matched retail site-86,
site-87, and site-88 join sequences. The only gameplay-time public leave request was associated
with activity host `0x9EAA300100200002`, marked on the exact admitted session, and acknowledged
six milliseconds later. Two later group leave requests were part of final teardown.

Transport health stayed clean. There was no `network-hitch`, assert hit, client error, corrupt
packet, or loss. The intermediate channel summary reported 32 delivered and zero lost; the final
summary reported 872 delivered, zero lost, and 114 incoming reads with zero unexpected discard or
corruption. The `_connection_failure_suicide` lines belong to deliberate old-activity-channel and
shutdown teardown, not an unsolicited gameplay disconnect. Client and core both ended with
`shutdown result=ok`.

The passive direct-manager correction did not produce a collector report. The hook attached, but
the run contained zero `scheduler-entity-collector`, `entity-create-out`, entity-list decode, or
`entity-record` lines, and server attempts remained zero. Three complete outbound shapes described
four view lanes. Every entity lane finalized at exactly 11 bits, proving that each contained only
the empty 10-bit native prelude followed by `FUN_14171EFE0`'s one-bit finalizer. Thus no kind-0
entity body reached the outbound scheduler in this run.

Ghidra identifies the immediate dispatch predicate above `FUN_14170C080` as leaf
`FUN_141712CA0`. Its exact 42-byte signature is unique in the current image at `0x141712CA0`. The
predicate reads only these handler fields:

- enable bytes `self+9` and `self+0xA` must both be nonzero;
- at least one pending-state dword at `self+0x28`, `+0x2C`, `+0x30`, or `+0x34` must be nonzero.

It returns false when either enable byte is clear or all four pending values are zero; otherwise it
returns true. The new `scheduler_entity_collector_gate` detour preserves that original bool-return
ABI, snapshots all six fields under SEH before the call, invokes the native predicate unchanged,
and emits one bounded, deduplicated `entity-collector-gate` line for each distinct state/result.
It writes no handler byte, pending value, manager bit, scheduler writer, candidate array, or other
game state. This removes the remaining ambiguity: a false leaf result proves the collector was
not dispatched, while a true leaf result with no collector evidence moves the search into the
caller/dispatch edge rather than object eligibility.

The gate-probe Release artifact is built and deployed. The Release and game DLLs match exactly at
SHA-256 `64b989bd6ede1fc1ed6c864daaf00c423671686ff4551c86fa482ec8a5f2e2e0`.

### Public descriptor overcommit hang

The last traversal's 20-second `network_update` hitch began after Sunrise published a third public
descriptor while two public views were still alive. The client force-disconnected the outgoing
target, opened the replacement, reached peer establishment, and queued join-complete at retail
site 86. The server had admitted the join; the expected site-87 send did not occur because the
managed pump stopped being entered afterward. This is a client lifecycle overcommit/reuse race,
not a missing server reply and not activity in either scheduler finalizer hook.

The bounded repair delays publication of a not-yet-admitted public descriptor while two admitted
public slots remain occupied, then waits 125 ms after a real release before republishing. Both
synchronous membership transactions and periodic keepalives apply the gate, so an authoritative
region delta cannot bypass it. Activity message 15 marks the exact leaving group through its bound
activity-host token. An ordinary gameplay leave still releases that row immediately; if the client
drops the stale target locally and sends no gameplay leave, a one-second fallback retires only the
marked row before the same 125-ms grace. This avoids the previous LRU/capacity deadlock while
preserving stable per-region group/activity-host identities. Reducing capacity or rejecting the
replacement after publication is unsafe because it allows the client to begin the same
forced-disconnect path first.

Release SHA-256 for the candidate-collector and public-slot lifecycle build:
`203cce8b1121e33ee1ad39eea9dcefa470602f9dff8349343f71f34bfcf248f1`.

For the deployed build, inspect:

```text
/home/zeex64/Games/Sunrise/bin/x64/Sunrise/logs/sunrise.log
```

Useful stages:

```text
membership-wire
membership-decode
view-membership
view-create
view-address
view-slots
view-lookup
view-state
view-codecs
view-readiness
membership-native-profile
account-soids
account-soid-publish
sobject-create
sobject-rsat-preload
sobject-update
sobject-native
sobject-promote
sobject-dirty-service
sobject-dirty-row
sobject-type2-job
sobject-bind-dispatch
entity-create
entity-create-out
entity-slots
entity-view
scheduler-tail
scheduler-signature
scheduler-native-signature
scheduler-outbound-shape
scheduler-outbound-commit
scheduler-entity-collector
scheduler-two-view-probe
scheduler-post-handoff-probe
scheduler-handler-trace
activity-host-decode
citizen-acceptance
citizen-join-status
z-leg-state
```

### Reflected-host retain-mask experiment

The collector gate run isolated the missing native lifecycle input. `FUN_141712CA0` saw initialized
entity handlers with pending create/update counters, but handler byte `+0xA` remained clear. Its
sole setter is reached from `FUN_141703910`; that lifecycle receives the per-peer bit returned by
`FUN_1404DF650`, which reads membership snapshot mask `+0x59250`. The existing membership decoder
diagnostic already labels that field `retain`, and every failing snapshot reported `retain=0` even
while `occupied` and `eligible` contained both local slot zero and reflected-host slot one.

The bounded wire experiment now makes the third top-level mask present only in a reflected-host
snapshot and writes `0x00000002`. Slot zero is deliberately excluded: it is the local client, while
slot one is the non-local gameplay host whose per-peer replication root must remain active. A
local-only snapshot still encodes the retain field absent, and the following two tail fields remain
absent in both forms. This adds exactly 32 meaningful bits only to reflected-host bodies: their
count changes from 31,335 to 31,367 bits and their padded byte size from 3,917 to 3,921; the
29,968-bit/3,746-byte local-only form is unchanged.

This does not patch or force the native handler byte. It supplies the decoded membership lifecycle
input and leaves `FUN_141703910` responsible for activating or tearing down the root.

### `3b49a5d4...` retain runtime result

The deployed DLL for the retain run matched SHA-256
`3b49a5d4defcfd90e741db4dc3e55fa34bbe9fb6251c21fd9113156d204caa7c`. At `t=60760` the client
consumed the entire 31,367-bit reflected-host membership body and decoded `tail2=2`; at `t=60794`
`view-membership` reported `retain=0x00000002` and peer-one `view-create` received retain state one.
The descriptor-plus-host body consumed all 32,391 bits. There was no malformed membership decode,
assertion hit, corrupt packet, or network hitch. This proves both the mask placement and the intended
slot-one bit ordering; the 29,968-bit local-only form remained unchanged.

The retain input opened the native lifecycle gate. All seven deduplicated
`entity-collector-gate` states reported `enabled=1/1`. The five states containing pending work
returned one, and the two all-zero states returned zero, matching `FUN_141712CA0` exactly. Natural
collector activity followed at `t=60927`, `t=67334`, `t=67401`, `t=67467`, `t=77160`, and
`t=92234`. Across the run the native outbound entity encoder produced 15 entity records.

Those 15 records are not evidence of 15 enemy spawns. They came from the client's natural
actor/world bootstrap, no record classified as an enemy, none used the synthetic Vandal target
RSAT, and there was no inbound synthetic `entity-record` or server `entity-create-out`. Server
arbitration never authorized a synthetic plan and attempts stayed zero. The result therefore proves
that `retain=0x2` restores native collection/encoding, while leaving target-manager and scheduler
selection as the next independent gate.

One natural record is especially useful as a precise native template. Entity `0x00100001`, flags
`0xC3`, contains nested sobject RSAT `0x80EF143E`; its complete outer entity body is 3,745 bits. The
pending instrumentation recognizes that exact nested RSAT while the entity encoder is active and
captures the bounded outer body, including the writer's partial prefix and suffix accumulator state.
It emits capped metadata and hex chunks rather than changing the writer. This instrumentation has
not yet produced a runtime capture, and `0x80EF143E` is described only as actor-like, not classified
as an enemy.

### Live collector namespace and strict target preseed

The retain-run collector log's `ns=0` was a diagnostic interpretation error, not proof that natural
records belonged to namespace zero. Handler byte `+0xC` is a selector, not the authoritative
namespace. The pending correction reads namespace directly from the live manager provider under
SEH and logs the `+0xC` byte separately as `raw_ns`. Manager-to-`ViewCapture` matches supply token,
slot, and generation context only when exactly one live capture matches; reused manager storage can
otherwise associate a current handler with a stale token. New passive arbitration telemetry also
records the primary group/world, active-manager age, scheduler and capture keys, peer-view tokens,
and their live namespaces without influencing arbitration.

Retain makes it reasonable to test a preseed before the target namespace becomes native-current:
prior promoted dirty state can survive until manager activation. It does not, however, guarantee
that the selected manager or cell is current, so the experiment remains narrow and fail-closed. An
inactive namespace-2 manager exposed slot zero as its first candidate, but natural bootstrap can
claim that slot later. The target preseed therefore uses exact slot 13 only. A passive exact-slot
inspection reads the manager's free and occupied bitsets plus the six-byte slot descriptor and
returns its handle, reserved, and object generations. Availability is checked when the plan is
prepared and repeated immediately before send; any disagreement cancels the attempt.

Preseed is further restricted to the fresh matched-but-not-ready target manager in the exact
two-view/275-bit scheduler shape, entity view one, with a unique live manager match and matching
z-leg transition. It does not write a manager, claim a slot, or relax the generic arbitration path.
The integrated Release build containing the actor-like capture, live namespace correction,
arbitration telemetry, and strict slot-13 target preseed is SHA-256
`b810e23321d5ad504fecfadbf2fb741d7e99c3deb4dd1f04d1ab6920647f4d2e`. The Release and deployed
game DLLs match exactly.
