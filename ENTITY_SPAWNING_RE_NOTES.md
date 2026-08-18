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
SHA-256 8700c45a9ab820f940655649f939abdae7fc211e2b2861cb65c10e888e1acf62
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

The final available log was captured with the pre-fix DLL. It repeatedly shows the exact receptor
failure the new state machine removes:

```text
client ... local=2 local_index=0 remote=1
server ... result=invalid-stage local=1 remote=0 got=2 index=-1 token=...002
```

It does not contain `view-codecs` or the expanded scheduler fields, so it is diagnostic evidence
for the fix rather than a runtime test of the new build.

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
- Once Sunrise marks the view bound, established packets already send the native simulation
  gatekeeper bit as enabled. The following replication-scheduler presence bit remains deliberately
  clear; encoding that scheduler frame and its entity-create body is the next protocol layer after
  the view is proven.

## Replication scheduler and entity lane

The scheduler is now mapped far enough to identify the exact entity record path, but not yet far
enough to safely emit a fabricated enemy. Sending a malformed scheduler body would desynchronize
the entire established packet bitstream, so Sunrise continues to publish `schedulerPresent=0`.

### Scheduler ownership and framing

- The scheduler descriptor is anchored by the `replication-scheduler` string at `0x141C9AD78`.
- `FUN_1416BB2B0` constructs the network view manager and its scheduler at manager `+0x5498`.
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

The empty-stream terminators are also recovered from the four inbound decoders:

- Event lane (`FUN_141718AE0`): a zero presence bit ends the stream.
- Mask/control lane (`FUN_1417183C0`): a zero presence bit ends the stream.
- Entity lane: `FUN_141718D90` first reads a one-bit auxiliary-entity count plus an optional 8-bit
  generation; `FUN_141718510` then uses a one bit as its no-more-records sentinel.
- Fixed control lane (`FUN_141718CB0`): a zero presence bit means no object.

Therefore the minimum known-signature, no-record scheduler body costs one signature-update bit plus
six bits per registered view. A view can additionally publish one auxiliary entity index and an
8-bit generation in the entity prelude even when it schedules no entity record. The remaining
prerequisite is knowing the client's registered-view count and current scheduler signature. The
`view-slots` probe now reads those directly from manager `+0x5498`, logging the scheduler view
count, local/remote 128-bit signatures, and signature flags.

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

`FUN_14171E240` is the core outbound object-body encoder and `FUN_141718080` is its inbound mirror:

- A create writes an 8-bit generation, a 2-bit replicated-object kind, and calls that kind's
  codec vtable slot `+0x58` for the creation payload.
- An update calls codec slot `+0x68`; the decoder uses the matching slot `+0x70`.
- The decoder resolves the 2-bit kind through `FUN_1416EAE90`, allocates the codec's declared
  create/update buffers, and injects a baseline when creation arrives without an update.
- `FUN_141714840` is the later apply/commit path that merges the decoded records into the local
  object table and clears lifecycle/dirty state.

The 2-bit codec registry is now resolved. An AI enemy's primary replicated object uses the
`sobject` codec (kind 0), but Sunrise still needs a valid enemy sobject RSAT identifier and the
matching baseline/update state before it can construct a valid first create record.

The create-body encoder is now tied directly to the scheduler's entity collector rather than a
separate transport:

- `FUN_14170B660` walks dirty replicated objects and builds the scheduler's create/update entries.
- A create-pending object calls `FUN_1417084B0`, which packages `FUN_1417003C0`'s kind, entity id,
  0x400-bit authority/presence mask, optional anchor, and codec `+0x58` payload.
- An update-pending object calls `FUN_141708C40` and the codec's update path.
- `FUN_1417085C0` and `FUN_1416FF790` are the matching create-body receive path. They validate the
  kind and entity id before calling codec `+0x60` and committing the object.
- The collector explicitly warns and schedules a baseline when creation is pending without an
  update, matching the baseline behavior already found in `FUN_14171E240`.

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
- `FUN_141723FD0` derives the remaining local creation data from the resolved RSAT definition.
  The id must already name valid loadable sobject data on the client.

The sobject update path also identifies the baseline state an enemy needs. `FUN_141725140` encodes
the named `transform`, `parent`, and `stream-source` components before its remaining sobject update
body; `FUN_141724FD0` decodes the same components. Squad updates use a named `squad` component, and
player-broadcast updates use a named `player` component. A squad relationship for AI is plausible
but not yet proven as a mandatory creation prerequisite.

The next diagnostic build follows `view + 0xB8 -> entity manager +0x10 -> global registry` without
calling native code. It logs the live count plus Ghidra-relative vtable/create/update RVAs as
`stage=view-codecs` and `stage=view-codec`. A correct runtime capture should report `count=4` and
match the four static registrations above.

## Instrumentation added

Client hooks now cover:

- View-message lookup and message-40 routing.
- Native message-40 error, local/remote stage, index, signature, compatibility, and open state.
- Live replicated-object codec count, vtable, create, and update entry points.
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

1. Run the receptor-state-machine build and confirm Sunrise accepts the client's stage 2, adopts
   index `0`, and echoes stages 2 through 5.
2. Confirm the server reports `stage=view result=bound` and the external probe reports
   `view=1 gate=1`.
3. Read the new `view-slots ... scheduler[...]` fields, including `lcount`/`rcount` and their
   key/tag entries, then separate the `1 + 6 * views` minimum frame from any auxiliary
   entity-index/generation prelude bits.
4. Confirm `view-codecs count=4` and that the four runtime RVAs match the static `sobject`, `squad`,
   `player_broadcast`, and `test_entity` registrations.
5. Resolve one valid enemy sobject RSAT id, then decode schema `0x80800014` and the
   `transform`/`parent`/`stream-source` update body closely enough to reproduce a baseline.
6. Determine whether an enemy additionally requires a kind-1 squad object or merely references an
   existing squad through its sobject update.
7. Add a scheduler writer only after an empty frame and one create record can be encoded without
   leaving unread or over-read bits on the native client.

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
activity-host-decode
```
