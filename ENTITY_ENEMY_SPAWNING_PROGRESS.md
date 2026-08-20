# Entity and Enemy Spawning Progress

Last updated: 2026-08-19

## Current outcome

Sunrise has proven the server-to-client native entity path, including one successful
server-authored object allocation. It has not yet produced a visible, functioning enemy.

The distinction matters: the hand-built entity is currently a wire-protocol probe. Normal enemies
should originate from the authored activity/director layer, which evaluates triggers, spawn rules,
squads, and encounters before publishing native objects through the replication path.

The latest broad-spawn control restored the configuration that reliably constructs an audible
Vandal. The new in-game entity overlay proves why it is not visible: the bound entity belongs to
Town replication namespace 1 at region 408 / bubble 51 / cell 145, while the player and renderer
are currently in Basin at region 24 / bubble 3. This is an explicit owner mismatch, not a failed
RSAT load, type-2 job, kind-0 construction, or native glue bind.

The next test removes that mismatch by promoting the simulation manager belonging to the player's
coherent current region. Ghidra shows that `FUN_1416EC250` is the sole non-initialization writer of
the active manager identity at runtime `+0x560E0`; it first marks the chosen manager container at
`+0x15C`. Sunrise now mirrors only those two manager-local writes. The override is fail-closed
until the client is in-world, the native slice equals the membership region, the region's group
view is bound, and the captured manager pointer exactly matches that namespace's fixed runtime
slot. A normal in-world z-leg is allowed to report the new region while the retiring native slice
still names the old one; that mismatch is diagnostic and is the transition this override repairs.
Initial loading remains blocked by the fresh `in_world` requirement. The atomic two-view create
then targets that same current host token, namespace, region, bubble, and map-global cell instead
of the outgoing Town view.

## Progress by layer

| Layer | Progress |
| --- | --- |
| Gameplay session and views | Working and substantially stabilized |
| Replication scheduler | One-view 203-bit and two-view 501-bit framing are runtime validated |
| Native entity creation | Shared Vandal RSAT `0x815B204B` reaches type-2, kind-0, native registration, and a completed glue bind |
| Entity placement and updates | Nearby transform and positional audio work; the current probe is bound to Town while the player is in Basin |
| Enemy AI and encounters | Not running; authored activity/director initialization remains missing |

## Current debug overlays

- `Entity Debug` is enabled by default and keeps the synthetic entity's server plan and observed
  client lifecycle in one snapshot. It shows identity/RSAT, namespace/view/token, region/bubble/
  cell, wire decode, promotion/type-2/apply state, native construction/binding, and the active
  simulation manager versus the current region's requested namespace.
- The latest screenshot shows `State bound`, RSAT `0x815B204B`, namespace 1/view 0, spatial owner
  408/51/145, and `Current world region 24 OWNER MISMATCH`. That is the present visual blocker.
- The player status overlay now separates the client-reported Region from Slice set. Bubble is
  derived from the scenario layout and the reported region; Slice set comes from the native world
  manager when the client-authored teleport field is absent. Closest-spawn caching is also keyed
  by destination so a previous map cannot linger for 250 ms.
- The session overlay no longer presents dormant host-session cache entries as active instances.
  Rows now survive only while current, admitted, or carried by a live link, and are labeled
  `current` or `overlap`. In the latest screenshot both 408 and 24 are genuinely connected/ready;
  408 is therefore an outgoing overlap, not merely a stale UI row.
- Current overlay/debug Release SHA-256:
  `5ac62efcbd6f0db1c880a32d6783355a17ac62fa478544b822b2d3115d0bf670`.
- Current-region manager promotion Release SHA-256:
  `54c55d9b2e6cdc40ac3634181d36506d23f3bc734d7632355bd0909d3c419edf`.

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
- Current-cell inheritance decoded successfully after the predicted 189 bits, but the active view
  context itself supplied `cell=0xFFFF`; the object remained invisible and no assert occurred.
- Parent and stream-source private probes encoded successfully in 49 and 18 bits. Marking the first
  old-RSAT field dirty raised a guarded `dirty bit inconsistency detected` assertion, while the
  second produced no payload beyond the five clean presence bits. This is not evidence that either
  field should be sent.
- Package decoding then linked the name-map definition `0x815B5420` (`Simulated Vandal`, class
  `0x80809C0F`) to `0x815B5422` at definition offset `+0x88`. The latter is class `0x80809BB6`,
  points back to `0x815B5420` at `+0x08`, and is therefore the exact create-wire RSAT.
- The Vandal RSAT carries 55 serialized component descriptors versus 2 in the old RSAT. The old
  112-bit transform update cannot frame that suffix safely. The next build sends one bounded
  Vandal create with no update and logs its native derived create profile.
- That create-only run was accepted on attempt two after exactly 77 bits. Its native profile is
  `22545B8101000000BC2200005E000000`: an 8,892-byte update scratch and derived value `0x5E`.
  Namespace 1 advanced from 13 to 14 occupied objects.
- The injected baseline let the private encoder recover the Vandal-specific wire safely. Its clean
  update is 25 bits; its transform-dirty update is 132 bits. The exact nearby-player X+3 wire is
  `C01440009A941F109724294A1F400000` followed by four zero bits.
- The next build carries that exact update with the Vandal create. The create-only missing-update
  assertions are no longer expected.
- The combined Simulated Vandal create/update was not accepted: both attempts returned result 2
  after the 19-bit handle boundary, no record was produced, occupancy remained 13, and the channel
  later timed out with 78 corrupt reads. Creation and update now need separate packets.
- The shared name-map definition `0x80C187BD` is a better real-Fallen target. Its definition
  `+0x88` links to class-`0x80809BB6` RSAT `0x815B204B`, which points back at `+0x08`. It backs
  plain `vandal` plus many actual Vandal variants and lives in the shared sandbox packages.
- Because shared RSAT `0x815B204B` has 53 serialized descriptors rather than the Simulated
  Vandal's 55, the next build profiles it with a create-only record before sending any update.
- That shared-Vandal create succeeded on attempt one after 77 bits. Its accepted baseline is
  `4B205B8101000000AC2200005C000000`, giving an 8,876-byte update scratch and profile `0x5C`.
- The native clean update is 23 bits and the transform-dirty update is 130 bits. At the captured
  player position, the nearby X+3 payload is
  `C01440009A641F107B842954A5C00000` plus two zero tail bits.
- Slot 13 became occupied, proving allocation. The same run later timed out with 65 corrupt reads,
  so the next build stops all create retries at that exact occupancy bit and sends only one staged
  update after a 500 ms settle period.
- The first two-stage run exposed an earlier timing failure instead: the create left two
  milliseconds after the world became fully enabled, while another public-region join began. Both
  attempts stopped after the 19-bit handle, slot 13 stayed clear, and the channel accumulated 63
  corrupt reads before recovery.
- The create gate had retained its 100 ms pristine-bootstrap timer after remote scheduler
  convergence. Bootstrap output now seeds only the empty scheduler. Convergence resets the timer,
  and an entity create requires a fresh 500 ms of the fully agreed one-view layout.
- That fix worked: create attempt one decoded after 77 bits, and slot 13 changed the low occupied
  word from `0x00001FFF` to `0x00003FFF`.
- The 500 ms update delay then missed the one-view window. The local scheduler expanded to two
  views before a busy control queue cleared; the server sent the cached one-view update afterward,
  but the client never entered the entity decoder and nothing rendered.
- The update delay was reduced to 100 ms, and transmission requires both live local and remote
  layouts to match the cached one-view signature. A changed layout suppresses the packet instead
  of emitting it into an incompatible scheduler context.
- The next run proved the overlap can be much shorter: the local layout grew to two views only
  18 ms after slot 13 became occupied. The 100 ms update was safely suppressed, so no update was
  decoded and nothing could render.
- The native payload is ready before occupancy publishes. The update now leaves on the first
  service tick that observes slot 13, with the same exact local/remote one-view guards.
- The first-tick run did not allocate: both creates returned result 2 after the direct handle and
  slot 13 remained free. Later traces returned the same result with capacity 256, so the initial
  capacity-255 observation was not causal.
- Ghidra now proves result 2 comes from the kind-0 create codec. `FUN_1417266B0` queues the decoded
  RSAT through `FUN_140A020E0`, checks it through `FUN_140A01C70`, and returns false while it is not
  resident. The generic record decoder maps that false return to result 2.
- The current build explicitly queues `0x815B204B` from the live game create decoder, polls its
  native readiness predicate on the game networking path, and holds the server create behind a new
  `rsat` gate. Create packets are no longer used as resource-preload probes.

The slot-14 atomic run then completed the current wire milestone:

- The slot-13 control left on token `0x9EAA300100200002` at `t=66530`, decoded one record after
  86 bits, and produced entity `0x0010000D`, cell `0x0091`, flags `0x0001`. Its two
  missing-update diagnostics are expected because this control deliberately uses create then
  update.
- Once slot 13 occupied, slot 14 left at `t=66597` with `update=inline`, `update_bits=130`, and
  `combined=1`. The client returned `result=0 count=1` after exactly 216 bits and produced entity
  `0x0010000E`, cell `0x0091`, flags `0x0003`, a transform-dirty mask, and the intended
  player-X-plus-three position. Slot 14 added no missing-update diagnostic.
- Namespace 1 advanced to 15 occupied objects (`occupied_low=0x00007FFF`), making slot 15 the next
  pristine candidate. This proves atomic create/update framing, resource residency, cell
  selection, transform decoding, and replicated-slot allocation. It does not by itself prove that
  the later native simulation-object glue bound the allocation to a renderable runtime object.
- Only one `sobject-native` line for `0x815B204B` appeared because that diagnostic intentionally
  recorded each RSAT only once per process. The missing second line is a logging blind spot, not
  evidence that slot 14 skipped construction. The passive probe now keeps the general one-per-RSAT
  catalog but records the first eight target-RSAT registrations with `target=1` and an occurrence
  number.
- Ghidra places the remaining boundary after the successful list decode. Network job type 2 runs
  `FUN_1417085C0`, re-decodes the stored creation through `FUN_1416FF790`, validates its entity and
  cell, then calls codec slot `+0xB0`. Kind 0 resolves to `FUN_1417242F0`; only after it obtains a
  native object index does it call `FUN_141704870` to associate that index with the replicated
  entity slot. The latest run produced target-RSAT occurrences one and two, followed by
  `sobject-bind` dispatches with valid native indices for slots 13 and 14. That proves both records
  reached native construction and the kind-0 glue dispatcher. The user heard positional Vandal
  audio immediately beside the player, supporting the nearby transform, but saw no model and the
  object neither reacted nor attacked. Native/audio/position works; render, active-world ownership,
  and AI do not.
- The decoded handles (`0x0010000D` and `0x0010000E`) are not the same full values later passed to
  the glue dispatcher (`0x7CFBA00D` and `0x2FFBA00E`); only their low 13-bit slots survive that
  lifecycle boundary. The revised passive probe therefore watches slots armed by successful
  `entity-record` decodes, not full handles or hard-coded slot numbers.
- Both experimental records used the older populated token `...0002`. The client had already
  started the `PUB24.24` transition and routed newer token `...0003`; that newer empty view reached
  replication-ready at `t=67198`, bound at `t=67465`, and became the settled current public group
  by `t=72536`. The selector retained `...0002` because it had 15 occupied slots while `...0003`
  had none. This old-token/current-token split is now a stronger visibility hypothesis than
  changing the already-accepted payload, but is not yet causal proof because the atomic send
  occurred while the transition was still in flight.
- The corrected overlay is visually confirmed and reports the physical EDZ state (slice 24,
  bubble 3). A later screenshot shows old region-408 token `...0002` ready and current region-24
  token `...0003` unjoined after that newer row had left. That is a later snapshot, not the
  injection-time state: during injection, `...0003` joined and bound only after the slot-14 native
  dispatch. This timing keeps active-world ownership as the leading visibility hypothesis.
- The newest run later hit one four-second receive timeout at `t=103241` and reported 84 corrupt
  reads, but the process rebuilt the connection and continued afterward. This transport defect is
  still real, although it occurred more than 33 seconds after both native constructions and does
  not invalidate their successful decode/dispatch evidence.
- The current test build fixes the leading ownership error. Replication-view selection now follows
  `primary_world.region` to that region's advertised group and exact bound activity-host token; it
  refuses an older populated view once a current-region group exists. The spatial cell is resolved
  from the selected token's stored region and destination layout. In EDZ this maps Basin region 24
  / bubble 3 to map-global cell 11, while Town region 408 / bubble 51 remains cell 145. The audible
  Vandal was sent to old Town token `...0002`, cell 145, during the move to Basin token `...0003`.
- The first fail-contained two-view validation sent the captured 275-bit signature plus two
  five-bit tails and no entity. The client directly acknowledged packet 127 after 66 ms, and Basin
  resolved correctly as token `...0003`, region 24, bubble 3, cell 11. The handler trace exposed a
  narrower framing error: view 0 consumed six bits and completed, borrowing view 1's first zero;
  view 1 consequently began one bit late, its prelude consumed 10 bits, and its entity lane returned
  result 2 after 19 bits without reaching the fixed lane. The run later reported only one corrupt
  read, but direct transport acknowledgement alone is not complete scheduler acceptance.
- The corrected 287-bit probe passed at `t=94777`: all ten handler calls ran in exact view-major
  order, both views consumed `1,1,2,1,1`, every native result was zero, and ordinal 9/fixed reported
  `status=complete`. The client acknowledged packet 135 after 66 ms. Basin remained token
  `...0003`, namespace 2, region 24, bubble 3, cell 11. The later summary reported 536 delivered,
  zero lost, and one aggregate corrupt read; there was no timeout or disconnect during the observed
  session. The framing boundary is now proven, though the next entity packet remains one-shot.
- The first Basin-create run exposed a scheduler-layout race rather than another rendering
  failure. The empty packet completed all ten two-view handlers at `t=71208`, and the client
  acknowledged it at `t=71235`. In between, root token `...0001` joined the local scheduler at
  `t=71229`, expanding it from two views to three. The server then sent the cached two-view create
  to the otherwise-correct Basin token `...0003`, namespace 2, view 1, slot 0, cell 11. The client
  never entered a second handler epoch or entity decoder, slot 0 remained empty, and no target
  native registration or bind occurred. The later health summary contained zero corrupt reads.
  The overlay independently confirmed Basin region 24/bubble 3 and both activity-host rows as
  connected and ready.
- The revised candidate uses the synchronous handler completion instead of waiting for the network
  ACK. On the first tick after the empty frame proves all ten calls, it may send the create only if
  the live local scheduler still exactly matches the cached two-view order. Once that create's own
  ten-call epoch yields the native 130-bit nearby transform, the next tick sends an update-only
  record to the same slot. Both packets recheck the live layout at send time and fail closed after
  root token `...0001` joins.
- That pre-ACK create succeeded at `t=80823`. Namespace 2 decoded entity `0x00200000`, cell 11,
  flags `0x0001` after exactly 86 bits, and slot 0 became occupied. The decoded baseline produced
  the exact 130-bit player-X+3 transform. The separate update left at `t=80856`, but the root
  membership expanded the live local scheduler from two views to three on that same tick. No third
  handler epoch or update record followed. The create-only slot also produced no target native
  registration or glue bind, so this run still had no renderable Vandal. The later health interval
  reported one corrupt read without packet loss.
- The new candidate removes the second packet. After RSAT residency and player-position readiness,
  it asks the game's native encoder to prebuild the transform from the exact accepted shared-Vandal
  profile (`4B205B8101000000AC2200005C000000`), identity baseline, and observed mask metadata
  `0x00004000`. The entity gate opens only if this produces exactly 130 bits. The first Basin create
  then carries that update atomically and suppresses the staged follow-up.
- The atomic Basin run passed every wire-side requirement. At `t=68853`, token `...0003`,
  namespace 2, view 1 decoded a 216-bit record as entity `0x00200000`, cell `0x000B`, flags
  `0x0003`, with the exact player-X+3 transform. Slot 0 became occupied six milliseconds later,
  there was no missing-update assertion, and the session later reported zero corrupt reads. It
  still produced no target-RSAT registration and no glue bind. The accepted record therefore did
  not reach native construction; invisibility in this run is expected and is not yet a render-only
  failure.
- Ghidra identifies the next exact boundary as the asynchronous type-2 apply job
  `FUN_1417085C0`. It re-decodes the retained creation and invokes codec slot `+0xB0`, whose kind-0
  implementation is `FUN_1417242F0`. The new diagnostic build passively logs both boundaries for
  decoded experiment slots as `sobject-apply` and `sobject-kind0`; it does not alter the accepted
  create/update body.
- The post-decode diagnostic run proved neither boundary executes. Both hooks attached, yet the
  current Basin record at `t=69242` produced no `sobject-apply`, `sobject-kind0`, target native
  registration, or bind before its view later left normally. Slot 0 remained occupied. The first
  missing boundary is now staged-record commit/job dispatch, before native construction.
- The timing is narrow and repeatable: the empty two-view proof left at `t=69208`, the atomic create
  followed at `t=69241`, and root membership expanded the local scheduler at `t=69274`, precisely
  when the older-view experiment had previously reached native construction. The next candidate
  uses the already-proven first two-view packet for the atomic create, gaining one full service
  interval before root expansion. A new passive `sobject-commit` probe distinguishes commit from
  later type-2 job dispatch if construction still does not start.
- The first-packet candidate logs `body_bits=501`: 275 signature bits, the outgoing view's complete
  six-bit empty tail, and the current view's 220-bit handler body (including the 216-bit entity
  list). Its Release DLL SHA-256 is
  `7a5ae97befc9be4050403c9342c57b5a6a8d6a324da9835d92e8c5439a826211`.
- The first-packet run passed. At `t=74442`, the client decoded entity `0x00200000` in namespace 2,
  view 1, Basin cell 11 with flags `0x0003` and the exact nearby transform. Slot 0 became occupied,
  packet 139 was directly acknowledged after 27 ms, and all ten native handlers completed. Root
  membership did not expand to three views until 68 ms after the create, yet no native apply,
  kind-0 construction, RSAT registration, or bind followed. Sending one service interval earlier
  therefore disproves the scheduler-window timing hypothesis.
- Ghidra corrected the next boundary. `FUN_141714840` is a rollback/merge path, not the normal
  accepted-record commit, so the absence of `sobject-commit` is expected. Normal decoding calls
  `FUN_141716010` immediately; it allocates the replicated row, sets its creation-pending flag, and
  sets the root dirty bit at manager `+0xCA20`. Per-frame `FUN_141717790` must then service that
  manager, pass the row through `FUN_14170B660`, and let `FUN_1417084B0` allocate a type-2 job before
  the existing `FUN_1417085C0` apply hook can run.
- The next passive build traces that exact chain as `sobject-promote`, `sobject-dirty-service`, and
  `sobject-type2-job`. It distinguishes an unserviced namespace-2 manager, a globally suppressed or
  inactive-cell dirty row, serialization failure, queue refusal, and a successfully allocated job
  without changing the accepted entity packet.
- The promotion/service run with Release SHA
  `f0467bca1b03b7023767a68f3225b9208ee4a0982a849058a0f2e18a4ebdc7c1` proved the
  immediate receive promotion succeeds. At `t=79733`, entity `0x00200000` entered namespace 2,
  Basin cell 11 with wire flags `0x0003`, internal flags `0x0023`, object generation 2, and manager
  occupancy changing `0 -> 1`. Packet 137 was directly acknowledged at `t=79799`, 67 ms after the
  send. The exact namespace-2 manager never produced `sobject-dirty-service`; consequently there
  was no `sobject-type2-job`, apply, kind-0 construction, target native registration, or bind for
  the rest of the run. The first missing boundary is therefore active-manager selection before the
  dirty scan, not packet framing, record validation, slot allocation, update decoding, or ACK.
- The bounded active-manager proof keeps the current Basin token `...0003`, scheduler view 1,
  region 24, bubble 3, and cell 11 as authority/spatial context, but places the atomic record in
  scheduler view 0's live namespace-1 manager. The first deployed version, SHA
  `13d441ff2878130ddd6e6c50a40b3a88a1a2df3ffa3512b76ccbe0e8d3c5b060`, sent
  nothing. The target `...0002` was client-live as namespace 1 with manager `0x471F040`, exactly 13
  occupied objects, and pristine next slot 13, but its server view stopped at replication-ready and
  never reached the server's bound state. The extra target-bound predicate rejected the plan at
  `t=79237` as `scheduler-shape`; no entity body or handler lane ran. Network health remained clean:
  794 delivered, zero lost, and zero corrupt reads.
- The correction treats a unique server view-token row as sufficient target presence while keeping
  the target fail-closed on a non-null client manager, namespace 1, exact scheduler entry and local
  layout, occupancy exactly 13, slot exactly 13, zero generations, and the proven 130-bit update.
  The current `...0003` authority still must be bound and owned by the advertised Basin group.
- The `f664c98f...` run sent packet 135 with `body_bits=501` and decoded the exact namespace-1
  Vandal record at slot 13/cell 11, but its old empty-tail order `000100` made view 1's prelude
  consume 10 bits and its entity lane fail. The earlier conclusion that view 0 stole a tail bit was
  wrong.
- The bit boundary is now exact. Sunrise stores and replays 275 signature bits, while the native
  signature decoder consumes 274. Stored signature bit 274 is therefore view 0's event bit. The
  existing 220-bit target body already supplies view 0's mask, two-bit entity prelude, 216-bit
  entity list, and fixed bit. Appending another target zero shifts the following view.
- The deployed `5262387a...` run proved that model. Packet 132 left at `t=68651` with
  `body_bits=502`; view 0 completed all five handlers and decoded entity `0x0010000D`, flags
  `0x0003`, RSAT `0x815B204B`, and the nearby transform. The extra appended zero then became view
  1's event bit. View 1 consumed event/mask/prelude as `1/1/2`, decoded an unintended entity count
  of 1, returned result 2 after 45 entity-list bits, and never reached fixed. The transaction rolled
  back: no promotion, type-2 job, apply, kind-0 construction, native registration, or bind occurred,
  and slot 13 remained `internal=-1 mapped=0 dirty=0`. No Vandal audio or model was expected.
- Network health remained clean for more than five minutes. One aggregate corrupt read appeared at
  120 seconds, later intervals returned zero, outgoing loss stayed zero, and there was no assert hit
  or network hitch. The final disconnect lines were a graceful user shutdown with
  `shutdown result=ok`.
- Ghidra `FUN_141718D90` and the handler traces support the final correction: keep the aligned
  empty view tail `000010`, remove the extra target zero, and return to `body_bits=501`. The final
  Release SHA is `28b14320728d4d2cabd0d0ba8384a4847449ea8f50b37b08e2112573b141bf03`;
  its runtime result is now confirmed below.
- The generic two-view writer is now index-aware as well: view 0 consumes the stored signature's
  carried event bit and writes only its five-bit remainder, while later views publish their own
  event bit. This does not alter the special view-0 Vandal packet, but prevents a later view-1
  create from reintroducing the same one-bit shift.
- The `28b14320...` run finally completed the entire scheduler transaction. Packet 148 left at
  `t=78255` with `body_bits=501`; view 0 consumed `1/1/2/216/1`, view 1 consumed
  `1/1/2/1/1`, all ten handler calls returned zero, and ordinal 9/fixed reported `complete`.
  Namespace 1 decoded the exact slot-13 Vandal at Basin cell 11 with flags `0x0003` and the
  player-X+3 transform. `sobject-promote` then reported manager `0x471F040`, internal flags
  `0x0023`, object generation 2, and occupancy changing `0 -> 1`. The server received a direct ACK
  after 65 ms.
- The first missing boundary is now inside the active manager's dirty service. Eight consecutive
  calls found slot 13 mapped at internal row 13 with its dirty bit still set `1 -> 1`; none reached
  `sobject-type2-job`. Ghidra shows `FUN_141717790` first tests
  `FUN_1416EC0F0(context)`, which is exactly signed `context+0x560E4 > 0`. A true result takes the
  suppression branch, skips `FUN_14170B660` and the type-2 builder, and retains the dirty row. This
  matches the log: the normal z-leg transition began at `t=76451`, never reported completion, and
  was stopped only during clean shutdown at `t=170369`.
- The run was network-clean: the 120-second summary reported 19 valid reads, 785 expected discards,
  and zero corrupt reads. There was no assert hit or hitch; the terminal connection-suicide lines
  belong to the graceful shutdown that ended with `shutdown result=ok`.
- The deployed passive predicate probe proved every watched namespace-1 service pass used
  `backend_count=0` and `backend_busy=0`. The exact 501-bit transaction completed, the slot-13
  record promoted with internal flags `0x0023`, and its dirty bit remained set through eight
  service passes, but `FUN_1417084B0` was never called. This rules out the unfinished public-target
  transition as the immediate construction blocker and moves the missing boundary inside
  `FUN_14170B660`, the per-row processor.
- The deployed row probe identifies active-cell rejection exactly. For cell 11, object state is
  otherwise eligible: kind 0, control `0x00`, create+update flags `0x0003`, defer sentinel `-1`,
  retry zero, and create suppression zero. Yet the batch stays `0 -> 0`, `state_out` becomes 1,
  and no type-2 call occurs. In `FUN_14170B660`, that combination is the branch where a cell below
  256 is absent from the active-cell bitset.
- The bounded Town-cell control conclusively completed the whole native construction chain.
  Namespace 1 accepted cell 145, allocated the type-2 job, registered target RSAT `0x815B204B`,
  returned kind-0 success, and bound native handle `0x58FC400C`. The Vandal produced clear
  positional audio but remained invisible in Basin. This proves the remaining visual failure is
  view/cell ownership, not create grammar, RSAT, transform, job dispatch, native construction, or
  glue binding.
- The launch itself exposes the ownership bug: the client requests EDZ Town and Sunrise grants
  initial slice 408, while broad spawn set `0x9617A6E7` has 54 points across seven cells and places
  this character on the Basin boundary. The client immediately starts a Town-to-Basin z-leg,
  leaving Town namespace 1 current and Basin namespace 2 only a target.
- Directly overriding the initial arrival to Basin was not a valid shortcut. The client completed
  slice 24 as PUBLIC CURRENT, but the public view contained only one kind-2 object instead of the
  normal 13-object Town baseline. Readiness remained pending and the player never instantiated,
  producing the reported black screen. Direct-Basin SHA
  `f6aeb6968e0251e32a66acb0eb250ed083016a82d4862e118667ea5a344a012e` is therefore rejected.
- Cache inspection found a coherent Town-only alternative. Spawn set `0xCB8903DF` contains three
  map-resident points around `(527,159,75)` and references only Town map cell 145. The current
  settings pair that spawn set with Town bubble 51 so the player, the active namespace, and the
  synthetic Vandal should all share the same owner and cell.
- The synthetic entity is scoped to its replication view and map cell; Sunrise does not migrate or
  remove it yet. The new control resolves the player's current region on every run and writes the
  entity into that same current view/cell only after its simulation manager is active.

## Immediate plan

### 1. Validate current-region manager promotion

- Start a fresh EDZ session and let the normal broad-spawn route run. Do not force an arrival
  region. Require `active-region-manager result=promoted` when the in-world overlay reports a newer
  current region than the retiring native slice/PUBLIC CURRENT manager.
- The Entity Debug Manager row must show the current region/slice and active namespace agreeing.
  Then require `entity-create-out` to use the same token/namespace and the overlay's exact
  region/bubble/cell, with no `OWNER MISMATCH`.
- Require type-2 result 0, kind-0 success, target native registration, and a completed bind. If the
  object is still audio-only with those ownership fields aligned, capture authored
  parent/stream-source state next.
- A type-2 job result of 4 means serialization failed; 1, 2, or 3 is an allocator/queue refusal;
  result 0 with a non-null job means dispatch should reach `sobject-apply`.
- Only after `sobject-apply`, `sobject-kind0 result=1`, target native registration, and
  `sobject-bind-dispatch status=bound` should the investigation move back to rendering.

### 2. Observe the nearby Vandal

- The decoded atomic record must retain the intended player-X+3 transform and add no
  missing-update assertion. There is no `entity-update-out` in this build.
- If target native registration and `sobject-bind-dispatch status=bound` occur but the object is
  still invisible, the remaining issue is downstream render/stream-source state rather than
  scheduler timing, cell ownership, update atomicity, or glue binding.

### 3. Resolve rendering, then AI

- If the current-token/cell object is audible and still invisible despite a completed glue-table
  bind, passively capture a real authored biped's parent, stream-source, and RSAT-specific suffix.
- Treat nonreactive AI as a separate authored-content problem. A standalone kind-0 sobject is not
  an encounter actor; EDZ aggression comes from spawn rules, squads, triggers, engagement sensors,
  and director state. Kind-1 replication binds an existing native squad and cannot create one.

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

- use the external entity-name map to recognize package definitions such as Red Legion Legionary
  `0x80C1A52D` and Dreg `0x80FDEBC6` while tracing their enclosing squad definitions;
- capture their create and initial-update order;
- identify the squad/member relationship and required component data;
- implement the equivalent server publications in Sunrise;
- verify visible enemies, placement, replication, AI activation, damage, and cleanup.

## Current checkpoint

- Branch: `feat_entity_spawning`
- Scheduler framing base: `01bbf118 fix: restore nested scheduler update framing`
- Stationary-create base: `b46dabce fix: retain entity settle age across control records`
- Previous atomic-create build: `2ce86a3d test atomic Vandal create update`.
- Current-view probe base: `c3332f69 test: validate current EDZ replication view`.
- Two-view tail proof: `83cf6b0c test: isolate two-view scheduler tails`.
- Previous bounded spawn test: `2abfb562 test: create Vandal in current Basin view`.
- Pre-ACK Basin test: `12968ec2 test: use proven two-view window for Vandal`.
- Atomic Basin test: `4a19acb test: atomically create current-view Vandal`.
- Current post-decode diagnostic: `diagnose: trace replicated object apply` (this checkpoint).
- The tested path preloads shared Vandal RSAT `0x815B204B`, sends the 86-bit slot-13 staged control,
  then sends the 216-bit slot-14 atomic create/update with map-global cell `0x91` and the exact
  native nearby-player transform. Both allocate; neither is visible.
- The manually deployed diagnostic DLL, SHA-256
  `bee5df9bdf30023ba09a2d59b543355233d46984415fddc177d65be94197c67c`, proved two target-RSAT
  registrations and valid glue-dispatch arguments for slots 13 and 14. Its old logger observed
  dispatcher entry only.
- The first two-view probe DLL, SHA-256
  `dfd0b4a16fad03e868433234752f43a2c45cf7b7e20501f50b2ddc1303374c54`, proved current-region
  selection and direct ACK coverage but exposed the five-versus-six-bit per-view boundary.
- The corrected six-bit-tail Release candidate has SHA-256
  `4cd97f077e3b241d64ac195843b96f7e58f0a88c4050b412c556e52750fa1029`.
- The current bounded Basin-spawn Release candidate has SHA-256
  `69ceeaee5d60b92091f97331ee0d21f2bd92d9185b5661e6ebcaf48de5fb096d`.
- The current pre-ACK Basin Release candidate has SHA-256
  `a9afb46a3bd8e273f7346f312c333fcef18d61f2985bec692e157e922db5d3d6`.
- The current atomic Basin Release candidate has SHA-256
  `cba76fa8b262b02ff4453560ae5f7456ad00715f09db318bbf524f10fd76a9c2`.
- The current post-decode diagnostic Release candidate has SHA-256
  `b295f21dcaaf17706a05766f75ce477dde50e606b24f3aad4d2e6886fefc40ca`.
- The current first-packet Basin Release candidate has SHA-256
  `7a5ae97befc9be4050403c9342c57b5a6a8d6a324da9835d92e8c5439a826211` and reports
  `body_bits=501`.
- The current promotion/service diagnostic Release candidate has SHA-256
  `f0467bca1b03b7023767a68f3225b9208ee4a0982a849058a0f2e18a4ebdc7c1`.
- The deployed active-manager proof is commit
  `ec3c4471 test: route Basin Vandal through active manager`, SHA-256
  `f664c98f1a22f338eb41bc3ec62e3bc2b4d11a7e1220779ebe0af94d9894a330`. Its 501-bit body used the
  wrong `000100` empty-tail order, so the transaction rolled back.
- The previously deployed 502-bit correction has SHA-256
  `5262387a228cf793c17aed6b1d2c54b4d3dcd618636bb02bad5db27ca9163d5b`. Its extra target zero
  shifted view 1 and again rolled back the transaction.
- The runtime-proven final 501-bit Release candidate has SHA-256
  `28b14320728d4d2cabd0d0ba8384a4847449ea8f50b37b08e2112573b141bf03`.
- The deployed passive backend-predicate Release candidate has SHA-256
  `c7eac83722020049a6dd9559241127efdc9c99a63d823a4d06ab6c7e7b040dae`. It proved
  `backend_count=0`, `backend_busy=0`, and a retained dirty row with no type-2 job.
- The deployed passive dirty-row Release candidate has SHA-256
  `39f8cba810a0f2272c527e44589e0aee9657c753b5e66301dd8b3e6deefbb140`. It proves namespace 1
  rejects Basin cell 11 as inactive.
- The successful Town-cell construction control has SHA-256
  `1f3c7939e4b84d9337fc0cdbde41696a7ec13018fb0da623402f37ce727da0ac`.
- The rejected direct-Basin-arrival build has SHA-256
  `f6aeb6968e0251e32a66acb0eb250ed083016a82d4862e118667ea5a344a012e`.
- The Town bubble 51 plus Town-only spawn-set `0xCB8903DF` Release candidate has SHA-256
  `f70cd002c75cf9e42d2345340f17d564b66b77ad51f6d00e241cafca3b68aa7f`.
- The current-region manager promotion candidate has SHA-256
  `54c55d9b2e6cdc40ac3634181d36506d23f3bc734d7632355bd0909d3c419edf`. It dynamically resolves
  the player's current group host/captured namespace, promotes only the matching fixed-stride
  manager after the player is in-world, and sends the Vandal into that same view and spatial cell.
  Its predecessor `8d72493f382fb5382e3a05570eba87892c9b504082ba54509c3741d38a49cfdd`
  sent nothing: at the 61-ms two-view window membership was region 24 while the retiring slice was
  still 408, so the strict slice-equality gate closed before the scheduler expanded to three views.
- The deployed `54c55d9b...` run proved that writing the active-manager identity is not equivalent
  to completing a public-session transition. The current Basin record decoded and promoted in
  namespace 2, then reached the dirty-row builder every frame with `dirty_flags=0x0003`, an idle
  backend, and no type-2 job. The native transition remained `PUBLIC TARGET` for more than three
  minutes, `Slice set` stayed unknown, and later region changes remained absent or preempted.
- The transition-safe candidate has SHA-256
  `af9fd044c9d4aada42844c89ea065bdb567a8909e51395a00bcee35c8bd3e487`. It no longer writes the
  active manager. Citizen-advertisement retirement and entity output both wait for the game's own
  native `PUBLIC CURRENT` manager selection, so cell residency is established by the normal world
  handoff rather than inferred from a bound replication view.
- The sessions overlay now labels the player-reported destination `target` until its captured
  namespace is the native active manager. `current` therefore means native current, while
  `overlap` names another still-live session; a merely reported region is no longer mislabeled.
- The deployed `af9fd044...` run proved the initial EDZ session did become native `PUBLIC CURRENT`
  at `t=60714`; the overlay still called it `target` only because its passive observer waited for
  the later server view-bound marker. The same run proved later citizen joins establish their group
  and activity-host links but can remain `PUBLIC TARGET` until preempted, producing real
  `absent/unjoined` rows and stuck loading zones. Keeping the citizen descriptor alive did not
  complete that handoff.
- The next passive transition diagnostic has SHA-256
  `120c0594ee2ab23d237c18f8e2f13e0fe1bd994fd3dd97678dc81154dd853d26`. It reads the native active
  namespace before requiring a bound semantic view, correcting the initial role label. A unique
  hook at `FUN_141788810` logs `citizen-acceptance` only when the target session's initialization
  count, selected peer, selected peer state, or lifecycle changes. Ghidra proves the world
  controller requires a nonzero count and selected-peer state `10` immediately before it logs
  `Citizen join ... almost complete, accepting join`.
- The `120c0594...` zone-swap run proved region 24 can reach native `PUBLIC CURRENT`, but a rapid
  return to region 408 exposed a second descriptor-retirement path. The transaction path marked
  reused group `0x8C0ACD3899A0C132` settled while the keepalive still reported `ready=0`. The client
  then started `PUB408` twice without issuing a citizen join and remained in normal-z-leg loading.
  Packet loss and corruption stayed zero.
- The visit-aware transaction-retirement candidate has SHA-256
  `b2202d68ebdb850298fb4f734bf7edfda19aa60a6bcfcaa14dbd81a55679cc04`. A historical settled-group
  entry now counts only when the scalar settled region matches this visit, and transaction refreshes
  retain the citizen descriptor until the exact activity-host token is native `PUBLIC CURRENT`.
- The `b2202d68...` traversal run validates that fix: revisits issued fresh citizen joins, region 24
  became native current at `t=83085`, region 408 became native current at `t=110315`, and the run
  ended with zero packet loss/corruption. No entity packet left. Native-current selection occurred
  after the proven two-view/275-bit transition window; the usable scheduler had become 3/275,
  2/203, or 3/203, so the old writer correctly failed closed as `scheduler-shape`.
- The post-handoff scheduler candidate has SHA-256
  `dabac3c2de0de14f7174c3beb32789ef822342f79f29316ce8c5f90f3b3332e8`. It sends one empty packet
  for an exact current-manager two- or three-view layout, traces all five native handler lanes per
  view, and requires a direct ACK. Only the identical still-current layout may then carry one
  atomic create. A failed decoder, changed token/signature, or missing ACK cannot emit an entity or
  repeat the same malformed layout.
- The deployed citizen-join-status diagnostic has SHA-256
  `ad324006a54b2f2732e07e44db3819afeb8b27e102bfcc226daf703d59498215`. Ghidra proves its hooked
  async query has one code caller: the world-controller citizen-join state machine. Both the
  successful initial region-408 join and the stuck region-24 handoff followed the normal
  `1 -> 2 -> 3 -> 0` status sequence. Region 24 also reached lifecycle 4, ready count 10, selected
  peer 1, and peer state 10, clearing the native join gates themselves.
- The log then exposed a server-side cycle: the region-24 descriptor was published for this visit,
  its group joined, its activity host was published, and its view bound, but descriptor retirement
  waited for native `PUBLIC CURRENT`. The client kept the region as `PUBLIC TARGET` while that
  descriptor remained. The visit-safe retirement candidate has SHA-256
  `a59f8c047eda82adaab6f7e82a95c3ecd533cdeb424ee013d302a94592b1acc0`. It retires only after this
  visit's descriptor publication plus accepted view and published activity host; historical
  readiness from a reused group cannot satisfy the visit marker.
- DLL: `/home/zeex64/Documents/Sunrise/build/x64/Release/steam_api64.dll`
- Previous committed entity DLL SHA-256:
  `dfd0b4a16fad03e868433234752f43a2c45cf7b7e20501f50b2ddc1303374c54`
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
