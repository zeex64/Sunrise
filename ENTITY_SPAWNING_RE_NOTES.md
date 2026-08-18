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
SHA-256 7e476d5ecaa4a80c2678bb2473d18a7985bd999d9a1aef7d72425e99bddb03f2
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

## Activity-membership schema discoveries

The relevant nested identity schema is referred to here as B2.

- B2 layout table: `0x1437E2000`.
- B2 has presence-coded fields 0 through 14.
- B2 field 0 is a six-bit scalar and controls the native gate byte.
- Writing field 0 as `0x10` produces `p1_gate=0x10` at the exact native predicate.
- B2 field 1 is type `0x80809EE1`, which wraps a 128-byte block. It is not the gate.
- B2 fields 10, 11, and 12 are type `0x80807C82` and are each `0x56` bytes.
- Their schema storage offsets appear as `0x112`, `0x168`, and `0x1BE`.
- Type `0x80807C82` schema data is at `0x143924878`.

Field 10 was encoded as an 86-byte raw NetAddr and the native decoder consumed the packet cleanly,
but the exact `member + 0x142` address passed to the creator remained zero. Therefore the current
mapping between the B2 schema storage offsets and the creator's member-relative address is still
incomplete. The field-10 payload is structurally valid, but it does not populate the creator's
requested address.

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

## Current blocker

The view creator now runs, but resolves the reflected host to channel slot 1:

```text
slot 0: lifecycle=4 state=5 family=0x0010 address_match=0
slot 1: lifecycle=2 state=1 family=0x00C0 address_match=1
```

Interpretation:

- Slot 0 is the healthy, established gameplay channel.
- Slot 1 is a pending duplicate created for the reflected activity member.
- Family bits `0x40` and `0x80` attach to slot 1, covering the active public-session families.
- Because slot 1 never reaches lifecycle 4 / state 5, `FUN_141703910` rejects it.
- The server's group-level peer-establish echo is not the missing transition; it operates on the
  already-established gameplay link and does not promote this duplicate.

Latest full native address dumps:

```text
requested/member + 0x142:
0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000

slot 0:
7F00000100790000000000000000000000000000000000000000000000007F00000100790000000001000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000

slot 1:
0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
```

The slot-0 blob decodes as expected:

- Local IPv4 at offset 0: `127.0.0.1`
- Local port at offset 4, little-endian: `0x7900` = `30976`
- Public IPv4 at offset 30: `127.0.0.1`
- Public port at offset 34: `30976`
- NAT type at offset 40: `1` (open)
- Direct method at the final byte: `0`

The requested address and slot-1 address being all zero explains the duplicate: activity-family
tracking associates the zero-address member with a separate pending slot, while the established
method-0 address remains in slot 0.

## Endpoint clarification

The `activity-host` gameplay parameter now publishes the gameplay endpoint (`30976`) instead of the
BAP listener (`30974`). This is correct because the parameter constructs a plain-UDP address.

The activity client's BAP service resolution still reports `127.0.0.1:30974`; that is a separate
service-transport path and is expected.

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

1. Scan the decoded reflected-member record for the known slot-0 byte sequence
   `7F0000010079` after the field-10 experiment.
2. Determine where the 86-byte payload actually landed and reconcile that offset with the B2
   storage metadata.
3. Probe the neighborhood around `member + 0x142` to identify the schema field or wrapper that
   owns the creator's requested address.
4. Encode the slot-0 NetAddr into that exact field.
5. Verify that the public-session family bits attach to slot 0, producing:

   ```text
   view-create channel=0 valid=1 lifecycle=4 state=5 established=1
   ```

6. Confirm `view-create result=ok`, a nonzero view count, and successful message-40 lookup.
7. Continue through the five message-40 stages and then validate server-authored entity output.

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
