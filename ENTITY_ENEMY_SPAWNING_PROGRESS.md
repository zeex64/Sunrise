# Entity and Enemy Spawning Progress

Last updated: 2026-08-19

## Current outcome

Sunrise has proven the server-to-client native entity path, including one successful
server-authored object allocation. It has not yet produced a visible, functioning enemy.

The distinction matters: the hand-built entity is currently a wire-protocol probe. Normal enemies
should originate from the authored activity/director layer, which evaluates triggers, spawn rules,
squads, and encounters before publishing native objects through the replication path.

## Progress by layer

| Layer | Progress |
| --- | --- |
| Gameplay session and views | Working and substantially stabilized |
| Replication scheduler | One-view layout understood; the 203-bit framing correction is validated |
| Native entity creation | A stationary 78-bit create now allocates an object reliably after one resource-load retry |
| Entity placement and updates | Default transform wire recovered; transform-bearing create is ready for runtime validation |
| Enemy AI and encounters | Not running; authored activity/director initialization remains missing |

## Confirmed progress

- Native view creation, token lookup, message-40 staging, and entity-slot discovery work.
- Account/SOID handling that caused the old stationary disconnect was fixed.
- The latest stationary EDZ run accepted a server-authored kind-0 sobject and advanced namespace 1
  from 13 to 14 occupied objects. Player movement is no longer required to expose the create path.
- RSAT `0x80C4FEAD` reaches the client loader, and bounded retries work.
- Stage-four bootstrap and cached retries were added so stationary creation does not have to wait
  for an unrelated zone transition.
- Multi-view scheduler output is intentionally suppressed because its complete handler layout has
  not yet been reconstructed safely.
- Ordinary acknowledgements stop carrying scheduler data after the first create attempt. This
  prevents a pending resource decoder from being re-entered on every packet.
- The scheduler body has two distinct gates: an outer body-presence bit and an inner signature
  update bit. The supported one-view signature update is 203 bits: one update bit plus the 202-bit
  schema body.

## Latest runtime checkpoint

The run of commit `b46dabce` completed the stationary-create milestone:

- Namespace 1 reached its 13-object native baseline at `t=102579`; slot 13 was pristine.
- The one-view scheduler used the validated 203-bit wire and the create gate reached ready after
  retaining 133 ms of settle age while reliable control work drained.
- Attempt 1 left at `t=102943`. The client consumed 77 bits, queued unloaded RSAT `0x80C4FEAD`,
  and returned the expected retry result.
- Attempt 2 left at `t=104946`. The decoder returned `result=0 count=1` after 78 bits and emitted
  a complete `entity-record` for entity `0x0010000D`.
- The kind-0 create codec accepted `ADFEC480000000004900000006000000`, then namespace occupancy
  advanced from 13 to 14 at `t=104979`. This is direct proof that the object was allocated.
- The create buffer's trailing flag is zero. Ghidra confirms that this makes the update codec skip
  the named `transform`, `parent`, and `stream-source` components and decode only the RSAT-defined
  suffix. That explains why allocation did not produce a visible enemy.
- There was no four-second gameplay timeout. An intermediate report did count 75 corrupt reads,
  so packet hygiene remains under observation, but valid gameplay and BAP traffic continued.
- The route logs are now interpreted correctly: identity 1 is the locally created posse and is
  expected to use the local constructor; identity 2 is the public group join and successfully uses
  the authored/remote constructor. Forcing identity 1 to the other branch is not the next step.

The first private native re-encoding run then resolved the update-presence boundary:

- `plain-clean` encoded successfully in exactly 2 zero bits. These are the two RSAT-defined
  presence fields for `0x80C4FEAD`.
- `spatial-clean` encoded successfully in exactly 5 zero bits. The three additional bits are
  transform, parent, and stream-source, in that order.
- Both variants returned 1 with `fault=0`; no hidden alignment, length, or payload bits occur in an
  all-clean update.

The create-plus-clean-update run then completed exactly as predicted:

- Attempt 2 returned `result=0 count=1`, consumed 83 bits, and decoded flags `0x0003`.
- The decoded spatial scratch is 217 bytes and begins with the default transform
  `0000000000000000000000000000803F00000000000000000000000000000000`, i.e. an identity
  quaternion followed by zero translation/auxiliary values.
- The private `spatial-transform-default` encoder returned 1 without a fault and produced 112 bits.
  Its exact complete wire is `C010000000000000000000000000`: eight flushed bytes
  `C010000000000000` followed by 48 pending zero bits.
- The five-bit clean update caused the client to assert repeatedly that it was injecting an update
  with a blank mask. Traffic continued, but the assertion loop confirms that an empty update must
  not be published as the object's initial update.

The following transform-bearing run also matched its prediction:

- Attempt 2 returned `result=0 count=1` after exactly 190 bits.
- The record retained flags `0x0003` and its decoded mask began with `01`, proving transform dirty.
- Namespace 1 advanced from 13 to 14 occupied objects and the blank-update assertion disappeared.
- Quaternion decompression produced approximately `[0, 0, 0.0061, 0.99998]`; the second float4
  remained `[0, 0, 0, 0]` and is now the controlled position-probe target.
- The gameplay channel later reported 146 corrupt reads and a four-second receive timeout during a
  regional transition. The transform record itself decoded cleanly, but channel hygiene is not yet
  considered solved.

The controlled second-float4 probes then resolved all four lanes:

- X/Y/Z each preserve the 112-bit total and independently change their own coordinate field.
- W changes the transform branch and shortens the record to 111 bits; it is not a world-position
  lane and remains zero for the placement experiment.
- The complete native wires are reconstructed correctly from flushed and pending state. For
  example X=`1.0` produces `C013F80000000000000000000000`, while Y and Z move that value into
  their later fields.
- The same run continued for more than two minutes without the earlier four-second gameplay
  timeout, but its two-minute channel report still counted 76 corrupt reads. The later BAP failure
  lines coincide with normal shutdown, not the entity decode.
- The launch selected EDZ Town (`bubble=0xB8459D59`, bubble ordinal 51, map index 145) and spawn set
  `0x9617A6E7`. The extracted cache contains real points for that set, but the flat point bank no
  longer retains each point's individual bubble mask, so it is not used to guess Town coordinates.

The live player-position probe then succeeded:

- Player position was `(509.15094, 30.1296005, 74.3163147)`.
- The native player transform is `C0143FE935241F1096C4294A1F40` in 112 bits.
- The native transform for three units beside the player is
  `C01440009A941F1096C4294A1F40`, also in 112 bits.
- The player's position agrees with one extracted spawn-point cluster, independently validating
  the physics position and native transform coordinate basis.

The first live nearby-placement run was accepted but remained invisible:

- Attempt 2 decoded exactly one entity after 190 bits and namespace 1 occupancy advanced from 13
  to 14.
- Its update scratch decoded the intended second float4
  `[512.15094, 30.1296005, 74.3163147, 0]`; no blank-update assertion occurred.
- Ghidra then confirmed the record's spatial-cell grammar. The emitted `1,0` branch explicitly
  selects global/default cell `0xFFFF`, while a zero bit inherits the active view's current cell.
- The next build changes only that branch to current-cell inheritance and logs the decoded cell.

## Immediate plan

### 1. Validate the first nearby visible placement

The current build publishes the exact native `player X + 3` transform captured above and inherits
the active view's spatial cell. The predicted record is 189 bits with flags `0x0003`, a 217-byte
update scratch, and transform dirty. It still emits only one entity and retains the existing
bounded loader retry behavior.

### 2. Publish one bounded position

After validating the transform probe output, recover:

- world transform and position;
- parent relationship;
- stream-source relationship;
- RSAT-specific component data;
- the initial native update and dirty-component masks;
- whether a kind-1 squad relationship is additionally required.

Confirm whether RSAT `0x80C4FEAD` becomes visible at the captured location. If it allocates but does
not render, inspect the decoded transform and object lifecycle/component requirements before trying
another RSAT. Keep retries bounded and do not hand-author compressed transform fields.

### 3. Complete safe scheduler support

- Retain the proven one-view path while entity/update decoding continues.
- Reverse the remaining per-view handlers and final boundary for two-view scheduler packets.
- Enable multi-view output only after packets are accepted without partial mutation, corrupt reads,
  or four-second channel timeouts.

### 4. Start the authored enemy pipeline

- Keep the already-correct local-posse and remote-public session constructors unchanged.
- Trace which activity-host state publication starts the server-authored director after the client
  has selected EDZ definition `0x0109ED6B` and enabled activity type 6.
- Identify the missing server lifecycle/selection data between the working public group join and
  encounter evaluation; the generic network-session route selector is not that content switch.
- Use archived EDZ scenario `80B2F00A` and simple encounter `80B2F02A` as validation references,
  not as substitutes for live runtime RSAT values.

### 5. Capture and reproduce a real enemy sequence

Once the director evaluates an encounter and creates native squad/member objects:

- capture their create and initial-update order;
- identify the squad/member relationship and required component data;
- implement the equivalent server publications in Sunrise;
- verify visible enemies, placement, replication, AI activation, damage, and cleanup.

## Current checkpoint

- Branch: `feat_entity_spawning`
- Scheduler framing base: `01bbf118 fix: restore nested scheduler update framing`
- Stationary-create base: `b46dabce fix: retain entity settle age across control records`
- Current working code publishes the exact 112-bit native transform for the captured point three
  units beside the player and inherits the active view's streamed spatial cell.
- DLL: `/home/zeex64/Documents/Sunrise/build/x64/Release/steam_api64.dll`
- DLL SHA-256: `f57c0b7badd028c94f6637b61667561287b53c8ba3fcaa58eac11117d12045a1`
- Runtime log: `/home/zeex64/Games/Sunrise/bin/x64/Sunrise/logs/sunrise.log`
- Detailed reverse-engineering notes: `ENTITY_SPAWNING_RE_NOTES.md`
- Deployment remains manual.

## Assessment of upstream commit `b8ccfb9b`

The shared commit is useful as architecture, but it does not duplicate or replace this wire work:

- its generic external codec accurately documents the create/update/remove envelope and will be a
  useful refactoring target later;
- the header explicitly says the codec has no caller yet;
- its scriptless payload callbacks accept only kind-0 and emit zero payload bits;
- `gameplay_external_body` and `server_default_entity` are disabled by default;
- its start-activity parser stops at the unresolved nested selection record;
- its physics host models future simulation, authority, and replication planning, but it does not
  encode Destiny's RSAT, transform, parent, stream-source, squad, or enemy payloads.

Cherry-picking the commit wholesale would mix a large independent architecture change into a
validated runtime path without supplying the missing native payload. Reuse should be selective,
after the exact client codec is recovered.
