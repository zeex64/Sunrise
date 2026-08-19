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
| Native entity creation | Shared Vandal RSAT `0x815B204B` is proven on both the staged control and an atomic create/update; slots 13 and 14 allocate |
| Entity placement and updates | Cell `0x91`, nearby transform, native construction, and positional audio work; render/current-view ownership does not |
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

## Immediate plan

### 1. Test the pre-ACK current-Basin create

- Load EDZ, transition from Town to Basin once, and wait. First require the already-proven 287-bit
  probe and complete ten-call handler trace. The create should now leave on the next service tick,
  before the probe's transport acknowledgement.
- The next `entity-create-out` must name token `...0003`, namespace 2, view 1, slot 0, region 24,
  bubble 3, and cell 11. Any old token, namespace 1, or cell 145 is a hard regression.
- Require the create packet's ten-call handler epoch, `entity-record cell=0x000B flags=0x0001`, a
  target-RSAT registration, `sobject-bind-dispatch status=bound`, and slot-0 occupancy.

### 2. Observe the immediate nearby transform

- If the baseline produces the 130-bit native nearby-player update before the live local layout
  changes, expect `entity-update-out` for the same slot 0 and `update_bits=130`.
- Require the update record on entity slot 0 with cell `0x000B`, the intended nearby transform,
  target registration, and a completed glue-table bind. There are no two-view retries if either
  bounded send fails.

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
- Current pre-ACK Basin test: `test: use proven two-view window for Vandal` (this checkpoint).
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
