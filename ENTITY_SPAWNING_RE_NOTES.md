# Server Entity Spawning / Native View Reverse-Engineering Notes

Last updated: 2026-08-18

## Goal

Make a Sunrise-hosted Destiny 2 activity create the native per-peer replication view required for
server-authored enemies and other entities to appear on the client.

The immediate milestone is completing message 40's five-stage view handshake. Native view
creation and token lookup are now proven; the next layer is the replication scheduler and its
entity-create lane.

## Environment

- Repository: `/home/zeex64/Documents/Sunrise`
- Branch: `feat_entity_spawning`
- Ghidra program: `/destiny2_runtime_dump.exe`
- Runtime log: `/home/zeex64/Games/Sunrise/bin/x64/Sunrise/logs/sunrise.log`
- Build directory: `/tmp/sunrise-entity-probe-release-codex`
- Built DLL: `/home/zeex64/Documents/Sunrise/build/x64/Release/steam_api64.dll`
- Deployment remains manual; do not copy the DLL into the game directory automatically.

Latest diagnostic DLL at this checkpoint:

```text
SHA-256 8dd4ae90c238d8a233871fcb39d3ce7d20bec9ede5a23e49633140fd2d26e390
```

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
pump. Separately, Sunrise retains the exact MSB-first encoded signature-update bits received from
the client and replays them verbatim. A create is allowed only when the native logical count equals
the wire count and the bound token/key/tag has one unique logical lane. No lane-order guess or
server-side schema re-encoding remains in the path.

This run also proves the scheduler signature is dynamic. It first sent an empty 131-bit update
(the one-bit update gate plus a 130-bit zero-entry signature), then a valid 347-bit update with
three registered entries (one update bit plus a 346-bit signature). Any create writer must size
itself from the captured count and echo the complete captured signature; it must not assume the
earlier one-view 203-bit prefix.

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

The server's existing external-body probe now also records the scheduler body after its two
presence bits as `stage=scheduler-body`. It retains up to 256 bytes, preserves a final partial byte
as an MSB-aligned value, and reports the original/captured bit counts plus a truncation flag. This
replaces the old need to reconstruct a potentially 1500-bit native scheduler frame from only four
64-bit prefix words; it is read-only and is emitted only after the server considers the view
accepted and the client declares a scheduler body.

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
name-change lines plus roughly 1,400 full scheduler bodies were synchronously copied to the log in
157 seconds. Sunrise now suppresses that cosmetic retail line and retains the full scheduler-body
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
- Up to 2048 exact client scheduler-body bits after the gatekeeper/presence prefix.
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

1. Load EDZ free roam and confirm a regressed client stage 1 produces one inbound view report with
   `restart=1`, followed by ordinary stages 1 through 5. If local and remote both pause at stage 4,
   confirm Sunrise does not send a second stage 4 and the native initiator eventually publishes 5.
2. If `network_update` stalls again, read the one `stage=network-hitch` line. `observer=0 native=0`
   excludes both log paths; nonzero `native` means the reported entered site never returned.
3. Reproduce the PUB448-to-PUB96 or equivalent preempted handoff and confirm the join advances from
   `Queuing join-complete` to `sending initial join-complete` and then `received`.
4. Remain in the initial zone while namespace 2 finishes its native baseline population; movement
   must no longer be required.
5. Confirm the first `stage=entity-create-out` follows the post-baseline `stage=entity-view` update
   directly, without waiting for unrelated BAP or zone-transition traffic.
6. Confirm a create can now fire in the initial settled zone at any logical entry without movement,
   and that its `entity-view` local and remote layouts are identical immediately beforehand.
7. Capture `stage=entity-record` from the successful 78-bit create and identify the baseline update
   buffer's transform, parent, stream-source, and RSAT-defined regions.
8. Read `sobject-update` captures to identify which named components are present in an initial
   native update and separate their bit spans from the RSAT-defined suffix.
9. Decode the `transform`/`parent`/`stream-source` update body closely enough to place and move the
   successfully created enemy.
10. Determine whether the enemy additionally needs a kind-1 squad relationship after the minimal
    sobject is accepted; do not assume the squad codec can create the underlying native squad.

## Build and verification

```bash
cmake --build /tmp/sunrise-entity-probe-release-codex --config Release --parallel 8
sha256sum build/x64/Release/steam_api64.dll
git diff --check
```

After manual deployment, inspect:

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
sobject-create
sobject-update
sobject-native
entity-create
entity-create-out
entity-slots
entity-view
scheduler-body
scheduler-signature
scheduler-native-signature
activity-host-decode
```
