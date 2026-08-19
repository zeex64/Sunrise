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
| Native entity creation | A complete 78-bit entity record has previously allocated an object and advanced occupancy |
| Entity placement and updates | Not decoded yet |
| Enemy AI and encounters | Not running; authored activity/director initialization remains missing |

## Confirmed progress

- Native view creation, token lookup, message-40 staging, and entity-slot discovery work.
- Account/SOID handling that caused the old stationary disconnect was fixed.
- The client has accepted a server-authored kind-0 sobject once, advancing its native object
  manager from 14 to 15 occupied objects.
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

The first run of commit `01bbf118` validated the scheduler-boundary correction:

- The native one-view schema measured 202 bits and the captured wire measured the expected 203
  bits.
- When the scheduler expanded to two views, the native schema measured 274 bits and the complete
  update measured 275 bits.
- The erroneous scheduler-signature log on almost every incoming packet disappeared.
- No `entity-create-out` was emitted in this run.
- Namespace 1 reached its required 13-object baseline at `t=77537`, with slot 13 available.
- The scheduler layouts matched with one view, but reliable control records repeatedly reset the
  settle window. A second scheduler view appeared before a create could leave, and the guarded
  one-view writer correctly suppressed the unsupported two-view body.
- The later increase from 13 to 14 occupied objects is not proof of a server-created entity because
  no matching `entity-create-out` or `entity-record` occurred.
- An intermediate channel report contained `32 ok`, `782 discard-expected`, and `4 corrupt`, but
  there was no four-second gameplay timeout. The process continued exchanging valid gameplay and
  BAP traffic through at least `t=210805`.
- Identity 1 decoded with `selector=0` and completed `route=local`. Identity 2 decoded with
  `selector=1` and completed `route=authored`. This directly confirms that the primary activity is
  missing its authored selector; the authored constructor itself can run for another identity.

## Immediate plan

### 1. Make the stationary create window reliable

- Preserve the settle age while an otherwise unchanged candidate waits behind reliable control
  records. A create remains forbidden until the queue is acknowledged and empty.
- Permit the guarded create as soon as the already-validated identity, baseline, candidate slot,
  generations, one-view scheduler, and matching remote layout are simultaneously valid.
- Do not relax the one-view restriction or send scheduler bodies during two-view transitions.

Expected proof:

- `scheduler-signature bits=203` appears only on genuine one-view updates.
- `entity-create-out` occurs in the initial EDZ zone without player movement.
- The first decode may return `result=2` while the RSAT loads.
- A bounded retry eventually produces `count=1` and `stage=entity-record`.
- Packet corruption does not accumulate and the gameplay channel remains connected.

### 2. Decode the native entity update body

After reliable object acceptance, use the existing `entity-record`, `sobject-create`, and
`sobject-update` captures to recover:

- world transform and position;
- parent relationship;
- stream-source relationship;
- RSAT-specific component data;
- the initial native update and dirty-component masks;
- whether a kind-1 squad relationship is additionally required.

This milestone should make the allocated entity visible and placeable. It may still be a passive
object rather than a functioning enemy.

### 3. Complete safe scheduler support

- Retain the proven one-view path while entity/update decoding continues.
- Reverse the remaining per-view handlers and final boundary for two-view scheduler packets.
- Enable multi-view output only after packets are accepted without partial mutation, corrupt reads,
  or four-second channel timeouts.

### 4. Start the authored enemy pipeline

- Capture the identity-1 `activity-route-record` and its route selector.
- Determine whether the client takes `route=local` or `route=authored`.
- If it incorrectly selects the local route, reconstruct the missing complete authored descriptor.
- Validate the selected activity-mode definition and confirm that the authored initializer and
  downstream director return successfully.
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
- Current code also retains a validated candidate's settle age behind reliable control records.
- DLL: `/home/zeex64/Documents/Sunrise/build/x64/Release/steam_api64.dll`
- DLL SHA-256: `0a38dbe9827f38700cf19237c9ab8b71ef7376076bce7fc89778a736b4bc9d35`
- Runtime log: `/home/zeex64/Games/Sunrise/bin/x64/Sunrise/logs/sunrise.log`
- Detailed reverse-engineering notes: `ENTITY_SPAWNING_RE_NOTES.md`
- Deployment remains manual.
