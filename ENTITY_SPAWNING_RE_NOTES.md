# Server Entity Spawning / Native View Reverse-Engineering Notes

Last updated: 2026-08-18

## Goal

Make a Sunrise-hosted Destiny 2 activity create the native per-peer replication view required for
server-authored enemies and other entities to appear on the client.

The immediate milestone is a successful call to the game's membership-driven view creator. Entity
serialization cannot work until this view exists and message 40 can resolve it.

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
SHA-256 4cf30ae42e8678d6b981c9cd4e87755e90b6bf6aa38a4c6fb43d735c9084a973
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

## Current blocker at the last runtime test

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

The remaining blocker is message 40's stage-2 view index. The server reached local stage 2 while
the client repeatedly reported stage 1:

```text
server ... stage=view result=queued direction=out local=2 remote=1 index=0 ... list=15
server ... stage=view result=accepted direction=in local=2 remote=1 got=1 index=0 ... list=0
```

`FUN_1416ECF00` is the native local-stage/index selector called by `FUN_1416EB4D0`. On a receptor
view, incoming stage 2 requires a nonnegative index that differs from `view + 0x80`. The creator
`FUN_141703910` copies the client's local peer index from its manager into `view + 0x80`; that value
is `0` in this topology. Sunrise also sent index `0`, so native code rejected the transition before
the client could advance to stage 2. The server now uses remote host peer index `1`, matching the
client's `p1[...] create=1` membership/view entry.

The established slot-0 NetAddr remains:

```text
7F00000100790000000000000000000000000000000000000000000000007F00000100790000000001000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
```

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
- This matches Sunrise's existing five-stage server state machine and its captured stage-2
  signature. The stage-2 index is the remote host peer index, not the client's local peer index.
- Once Sunrise marks the view bound, established packets already send the native simulation
  gatekeeper bit as enabled. The following replication-scheduler presence bit remains deliberately
  clear; encoding that scheduler frame and its entity-create body is the next protocol layer after
  the view is proven.

## Instrumentation added

Client hooks now cover:

- View-message lookup and message-40 routing.
- View-slot manager state.
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

1. Run the index-1 build and confirm the client advances from stage 1 through stage 5 instead of
   retransmitting stage 1 after the server's stage-2 body.
2. Confirm the server reports `stage=view result=bound` and the external probe reports
   `view=1 gate=1`.
3. Use the first post-bind `external-probe` scheduler sample to map the replication frame before
   adding the first server-authored entity-create body.

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
