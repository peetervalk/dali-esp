# Commissioning a DALI Bus

How to go from an unknown bus to a configuration you can keep. Flash the starter
firmware, find out what is on the wire, then write your own config from what it
found. For a bus that already works and has to change — a fixture moving to
another room, an address that has to be freed, gear coming off the wall — skip
to [Change a bus that is already
commissioned](#5-change-a-bus-that-is-already-commissioned).

**Last reviewed:** 2026-08-26

`dali-starter.yaml` is mostly a shim: it brings up the bus and opens the
diagnostic shell on a TCP port. The shell is the tool. The buttons and the
command console in that file are a deliberate fallback — see
[From Home Assistant instead](#from-home-assistant-instead).

## 1. Flash the starter firmware

```sh
esphome run dali-starter.yaml
```

Wiring is TX → GPIO18, RX → GPIO19 on a MikroE DALI 2 Click. GPIO16 and GPIO17
are unavailable on WROVER-E — PSRAM uses them.

## 2. Connect to the shell

Run `tools/dali-shell` from any host that can reach the device. It is standard
library only, with nothing to install, and drops straight into the Home Assistant
"Advanced SSH & Web Terminal" add-on. Copying over Samba or the file editor drops
the execute bit, so run the interpreter:

```sh
python3 /config/dali-shell                 # interactive session
python3 /config/dali-shell --list          # nodes answering on this network
python3 /config/dali-shell discover        # one command, then exit
python3 /config/dali-shell export inventory > /config/dali_inventory.json
```

There is no default host. With no `--host` and no `DALI_SHELL_HOST` it browses
mDNS for ESPHome nodes and offers the ones whose shell answers, asking only if
more than one does — so the device name is something you pick out of a list
rather than something you have to type.

With no client to hand, `nc dali-starter.local 2323` is a complete if unfriendly
substitute: the shell is a plain line protocol. The client adds history, tab
completion, one-shot redirection, and finding the device. That last part matters
inside the add-on, whose resolver has no mDNS — `.local` names do not resolve
there, and bypassing the resolver is the gap the client closes.

Once connected, `help` lists every verb and `list <table>` prints a named command
table. This is the same shell, running the same code, as the serial console on a
native ESP-IDF build — not a subset written for ESPHome.

**One shell session at a time.** Long shell workflows also gate the controller's
other local producers, so a refresh cannot be inserted into a commissioning
probe. A second connection is accepted only far enough to be told why it is
closing, and a terminal closed without quitting is reclaimed by the
`idle_timeout` in the YAML. This is local serialization, not proof that the
physical DALI bus is single-master; another controller can still transmit.

## 3. Walk the bus

| Verb | What it does |
|---|---|
| `discover` | Full scan: short addresses, device types, groups |
| `inventory` | Reprint the last discover without touching the bus |
| `export inventory` | The same result as JSON — redirect it to a file |
| `export config` | This device's `dali:` block as YAML, entities and all |
| `identify <addr>` | Blink one fixture to confirm which address it is |
| `find switches 300` | Listen for input-device events and map switches |
| `instances <addr>` | What a control device actually offers |
| `sensor poll <addr>` | Read an input instance's current value |
| `commission unaddressed` | Assign short addresses to new gear |
| `max b`, `off b`, `level a3 128` | Drive gear directly while you work |
| `trace on` | Live per-frame trace |
| `capture start … export` | Rolling capture, dumped when something goes wrong |
| `bus check`, `stats` | RX level, scheduler state, fault counters |

A typical first pass is `discover`, then `identify <addr>` under each fixture to
map addresses to rooms, then `find switches 300` while someone presses every wall
switch.

A long verb prints as it goes and holds the bus for as long as it runs —
`find switches 300` is five minutes by design. Ctrl-C in the client drops the
connection. Once the device observes that loss, ordinary work stops at the next
transport boundary. Commissioning is the exception described below: it stops the
addressing walk, but still runs its safety TERMINATE cleanup.

Full verb syntax is in `dali_commands.md`.

Nothing in that table edits a group or moves a short address. Those are
`config` and `config-dtr0` commands rather than discovery verbs, and they have
their own section: [Change a bus that is already
commissioned](#5-change-a-bus-that-is-already-commissioned).

### Assigning short addresses

`commission unaddressed [first-addr] [max-devices]` runs the INITIALISE /
RANDOMISE / COMPARE / PROGRAM SHORT ADDRESS walk, sequenced and checked.

The run silences control devices for its duration. A broadcast Part 103 START
QUIESCENT MODE goes out before INITIALISE and a STOP follows TERMINATE, so an
occupancy sensor or wall switch cannot put an event frame into a COMPARE reply
window, where frame-like activity reads as YES and invents gear that is not
there. Only control devices are affected; lights keep working throughout.

A Part 103 `TERMINATE` goes out with it — before `INITIALISE`, again just after,
and once more on the way out. Quiescence stops a sensor talking; this stops one
that quietly entered its own addressing state when it saw the gear `INITIALISE`
go past, and would then answer `COMPARE` as a fixture that does not exist. It is
never acknowledged by anything, so the shell mentions it only if it could not be
sent.

Two consequences worth knowing. The release is unconditional, so a run also
releases a quiescence you started by hand with `quiescent on all`. And if the
release cannot be transmitted, the shell says so explicitly — control devices may
stay silent, and `quiescent off all` is the fix.

**It is still dependable only with a single unaddressed device on the bus.** The
reply-activity, cleanup, and equal-random-address handling described here are
host-tested, not a multi-gear HIL result: the collision classification they all
rest on has never met a real overlapping reply. Commission new gear one piece at
a time until that has been exercised on a real bus.

Before the walk starts, `commission` pre-scans the bus to learn which short
addresses are already taken. An address that answers that pre-scan with
undecodable activity — two pieces of gear sharing one short address is the
expected cause — is reported as `contested` and held out of the free pool. It
does not stop the pre-scan or the run: the addresses it can prove are free are
still free. Assigning a third device onto a contested address is the fault this
prevents.

```text
Scan complete: 3 device(s) found.
  note: 1 address(es) answered undecodably.
  Likely gear sharing a short address; reserved, not listed, not free.
    a7: contested
```

Resolving a contested address needs a hardware pass — pull one fixture, or
re-address the pair one at a time. Nothing on the bus can separate them remotely.

COMPARE now distinguishes silence from observed but undecodable traffic:

- Silence through the reply window is the only valid NO.
- A decoded backward reply is YES only when its byte is exactly `0xFF`. Any
  decoded non-`0xFF` byte is malformed traffic, not NO, and aborts the walk.
- A timestamped, frame-like malformed waveform wholly inside the backward-reply
  window is reported as qualified RX activity. COMPARE alone maps that condition
  to YES, which preserves the positive meaning of overlapping replies.
- Activity that does not qualify as a backward-frame-shaped collision remains
  ambiguous and aborts. RX overflow and an intervening decoded 16- or 24-bit
  forward frame also abort rather than being turned into either YES or NO.

This closes the previous path where an overlapping reply was dropped and then
mistaken for silence. It deliberately does not turn every pulse or decode error
into a device.

Once the opening TERMINATE / INITIALISE / RANDOMISE sequence is handed to the
atomic transport, every later exit converges on one Part 102 TERMINATE attempt:
normal completion, a bus error, front-end cancellation, and even a local wait
timeout whose sequence may finish later. The shell provides a cleanup transport
that ignores the disconnected/cancelled front end, while still reporting normal
scheduler, PHY, timing, and bus errors. Closing the terminal therefore does not
cancel the safety unwind itself.

For code using the shared workflow, `DaliCommissioningResult` records the two
outcomes separately:

- `last_error` remains the primary addressing error. If addressing otherwise
  succeeded, a failed TERMINATE becomes the primary error instead.
- `termination_required` says the opening sequence was handed to the transport;
  it is historical and remains true after successful cleanup.
- `termination_attempted` says the centralized unwind ran.
- `terminate_tx_succeeded` means the no-reply TERMINATE frame was reported fully
  transmitted. It does not prove that every gear accepted it in the presence of
  another bus master or a physical collision.
- `cleanup_error` preserves a cleanup failure without overwriting an earlier
  primary error. `initialisation_state_unknown` is then true because the
  fifteen-minute initialisation window may still be active.
- `quiesce_control_devices` in `DaliCommissioningOptions` turns the bracketing
  on. It is off in a zero-initialized struct, so an out-of-tree caller keeps the
  behaviour it had; the shell sets it.
- `quiescence_started` means START was transmitted, never that any device
  quiesced — nothing acknowledges it, so an empty bus reports the same. A failed
  START does not abort the run: quiescence is hardening, not a precondition.
- `quiescent_state_unknown` is the counterpart of `initialisation_state_unknown`
  and the more visible of the two: quiescence was started and the release could
  not be transmitted, so the installation's sensors may still be silent.

With a live connection, successful cleanup appears as `commission: terminate`.
A failed cleanup reports `cleanup terminate ERR ... initialisation state
unknown`; the firmware also logs the final primary and cleanup state, so a TCP
disconnect does not make that result disappear with the socket.

#### When two gear share a random address

RANDOMISE gives each piece of gear a 24-bit number. Two gear drawing the same
one is roughly a 1-in-100,000 event on a twenty-fixture bus — rare, but the walk
has no way to see it coming: the pair is selected together, programmed together,
and withdrawn together, and every command succeeds.

`VERIFY SHORT ADDRESS` is where it shows. Exactly one device is selected when
VERIFY runs, so one reply decodes and two overlap into something that does not.
The run reads that as "more than one device answered", takes the short address
back off both with PROGRAM SHORT ADDRESS `0xFF`, drops the pair out of the
search, and carries on:

```text
commission: found random=0x4C1E90 -> short 5
commission: random=0x4C1E90 answered from two gear; short 5 taken back, both left unaddressed
commission: found random=0x8823A1 -> short 5
commission: assigned short 5 (count=1)
```

Note that short 5 is reused rather than skipped — the pair gave it back, so the
next fixture found takes it.

At the end:

```text
commission: 1 random address(es) held by two gear
    random 0x4C1E90
  note: each pair was de-addressed and left out of this run.
  They are unaddressed gear now, not missing gear. Run 'commission unaddressed'
  again - they re-randomise, and colliding twice is a 1-in-16M event.
```

**Run it again.** That is the whole remedy — no hardware pass, nothing to
unplug. The pair is back to being unaddressed gear, which is the case
`commission unaddressed` exists for, and the second run draws them new random
numbers.

Two things this does not claim. The de-address is transmitted, not confirmed:
two devices answering QUERY SHORT ADDRESS with the same "no address" reply
collide exactly as they did before, so there is nothing clean to read back. And
the detection can fire on a single piece of gear with a marginal reply waveform.
That costs an extra search round and possibly a different short address than you
expected — it does not damage anything, which is why the run acts on it rather
than stopping to ask.

If the pair cannot be dropped out of the search — WITHDRAW refused, or gear that
ignores it — the run stops rather than searching the same address forever, and
says so. That is the one case where two gear may still be sharing a short
address; the post-scan below is what tells you.

#### Reading the post-scan

A run that assigned anything re-scans the bus afterwards and checks the result
against what it thinks it did:

```text
commission: verifying with post-scan
commission: post-scan found=18
commission: post-scan confirmed 3 of 3 assignment(s)
```

`confirmed` counts assigned addresses that answered QUERY STATUS as control
gear. Anything else is named:

```text
    a7: contested - two gear answered as one
commission: post-scan confirmed 2 of 3 assignment(s)
  note: 1 assigned address(es) answered undecodably.
  Two gear generated the same random address and were programmed together;
  both hold that short address now and neither can be reached alone.
  Separate them physically, then re-run 'commission unaddressed'.
```

An assigned address answering undecodably means two gear hold it. Since the run
itself now catches the equal-random-address case and hands the pair back
unaddressed, seeing it here means the in-run detection missed — twins whose
replies happened to decode cleanly — or the run reported that it could not drop
a pair out of the search. The post-scan is the backstop for the first and the
confirmation for the second. This one does need a hardware pass: pull one
fixture, commission it alone, put it back.

`assigned but silent in the post-scan` is the other failure. VERIFY confirmed the
write, so the gear took the address and then did not answer the scan — a reply
landing outside the attribution window, or gear that left the bus.

Addresses that became contested *without* being assigned are listed separately.
A run cannot program an address it never allocated, so that means the bus changed
underneath the walk: another master, or gear that was mid-boot during the
pre-scan. Contested addresses that were already there before the run are not
reported here at all — they were held out of the free pool and never touched.

A failed run returns before the post-scan, so none of this is available when it
is arguably most wanted. `discover` after a failure does the same walk.

The remaining commissioning work is explicit:

- The cross-part TERMINATE guard now runs, alongside the START/STOP QUIESCENT
  bracketing. The two cover different halves of the same problem: quiescence
  stops a control device *transmitting* into the run, and the Part 103 TERMINATE
  stops one sitting in its own addressing state and answering COMPARE as gear
  that is not there — which is a state the Part 102 INITIALISE itself can put it
  in. Neither reaches a device that never received the broadcast, and none of it
  is HIL-validated. The reverse guard, bracketing control-device commissioning
  with a Part 102 TERMINATE, does not exist because control-device commissioning
  does not.
- Two gear that generate the same 24-bit random address are detected during the
  run and sent back to being unaddressed, but they are not placed for you: a
  second run is what gives them addresses. See "When two gear share a random
  address" above. The detection has host vectors and no bus result.
- DALI-2 priority/backoff and complete multi-master intervention handling remain
  open. Local atomic sequences do not stop another physical master.
- Multi-gear, mixed Part 102/Part 103, cancellation-fault, and external-master
  scenarios still need hardware-in-loop runs before this warning can be relaxed.

The verb is refused unless the YAML says `allow_commissioning: true`. The shell
port is unauthenticated — the same posture as OTA and the web server, but a lower
bar than physical access to a UART — and one typed line can readdress a whole
bus. `dali-starter.yaml` enables it because commissioning is what that firmware
is for. Set it to `false` on anything left flashed on a shared network:
RANDOMISE cannot be undone.

The same rule refuses the nine commissioning primitives under `special`
(`initialise`, `randomise`, `search-h/m/l`, `program-short`, `withdraw`, and both
write-memory forms). `terminate` always stays available, because it is what
closes an initialise window another tool opened. What each primitive does on
its own, and the few jobs worth reaching for them by hand, is in
[The nine primitives, by hand](#the-nine-primitives-by-hand).

## 4. Turn the result into your config

```sh
python3 /config/dali-shell export config > /config/dali_block.yaml
```

`export config` prints the whole `dali:` block — pins, sensors, shell settings —
and every `light:` and `sensor:` entry naming it. It is reconstructed from the
running firmware rather than from a source file, since the YAML never reaches the
ESP32, so what it prints is what is actually in force. Run `discover` first and
gear the bus reported that no entity drives is appended commented out, ready to
uncomment and name.

Input instances are appended the same way, drafted rather than merely counted: a
scan reads each instance's type and resolution, so the export knows a value
instance's width and knows a push button has no value to poll at all. A button
gets a note pointing at `headless_dispatch` instead of a `sensor:` entry that
would poll forever and publish nothing. What no scan can supply — scale, offset,
unit — is left for you.

If any `headless_dispatch` rule matches `legacy_16bit` frames, the export says so
next to the buttons it lists. Those frames are forward frames and carry no source
address, so a rule may already be handling a button the export has just called
uncovered, and nothing on the bus can prove which device sent it.

The output is a `dali:` block, not a device backup. Nothing ESPHome owns appears
in it: not the node's own blocks, not an entity's unit, device class, id,
`internal`, filters or automations, and not the `button:`, `number:` or `text:`
platforms. Read it as a diff against your source.

From there, work from the example configuration in the README: give the entities
real names and add the sensors you found. Group lights need nothing extra — the
member each one polls for state is discovered, not configured.

## 5. Change a bus that is already commissioned

`commission unaddressed` is a first-pass verb. It finds control gear holding no
short address and gives it one, and it deliberately leaves everything already
addressed alone — which is what makes it safe to run on a live bus, and what
makes it the wrong verb for every change that comes afterwards. Moving a fixture
into another room's group, freeing an address someone else needs, and retiring a
fixture that came off the wall are all ordinary addressed commands against gear
that already answers. They are spread across `config`, `config-dtr0`, and
`special` rather than gathered under one verb, which is why they are easy to
miss.

| What changed on site | What to send |
|---|---|
| A fixture joins a room | `config a<N> add-group <G>` |
| A fixture leaves a room | `config a<N> remove-group <G>` |
| A whole room moves as one | `config g<S> add-group <D>`, then `config g<S> remove-group <S>` |
| A fixture needs a different short address | `config-dtr0 a<N> set-short-address-dtr0 <encoded>` |
| A fixture is gone for good | `config-dtr0 a<N> set-short-address-dtr0 255`, then `group forget <N>` |
| New gear on a bus that already works | `commission unaddressed` |
| The whole installation is being redone | Read [Starting over](#starting-over) before typing anything |

Groups and short addresses live in each gear's own non-volatile memory, not in
the ESP32. They survive a controller reflash, a controller swap, and being read
back by a different tool entirely. What does not survive — and what most of the
surprises in this section come from — is the *controller's cache* of that state.

### Group membership

Two verbs, both `config`, both taking the group number as their parameter:

```text
config <target> add-group <0-15>
config <target> remove-group <0-15>
```

`discover` prints what each address currently believes:

```text
05: present, LED, status=0x00, v2, level=254, groups=[1,3]
```

and a single address can be asked without walking the whole bus:

```text
query a5 groups-0-7      # bit N set means member of group N
query a5 groups-8-15     # bit N set means member of group N+8
```

Moving one fixture from the hallway (group 1) to the kitchen (group 3):

```text
query a5 groups-0-7      # before
config a5 add-group 3
config a5 remove-group 1
config a5 save-persistent
query a5 groups-0-7      # after
max g3                   # it should light with the kitchen
```

Add before removing. Order changes nothing on the bus, but a fixture that is
briefly in both groups answers both, while a fixture briefly in neither is dark
to its own room for as long as it takes you to type the second line.

`save-persistent` is not required by anything this firmware does. Whether a
given driver commits a group edit immediately or on its next save is not
something the bus reports, so the extra frame settles the question rather than
leaving it to be found after a power cut.

A group target moves a whole room at once, which is the difference between one
line and fourteen:

```text
config g1 add-group 3      # every current member of group 1 also joins 3
config g1 remove-group 1   # ...and then leaves 1
```

The second line is addressed to group 1 and tells its members to leave group 1;
the gear evaluates membership when the frame arrives, so the pair does what it
reads as. Run `discover` afterwards — a group-target edit is the one form where
the controller cannot compute the resulting membership from what it already
knows.

Broadcast is refused on both surfaces with `no broadcast group config`.
`config b add-group 3` would put every piece of control gear on the bus into
group 3, and the only command that undoes it — `config b remove-group 3` —
empties the group completely, including the members that belonged there before
you started. The runtime group-query cache cannot represent either operation,
which is why the refusal lives in the shared command layer rather than in one
front end. `raw2` still sends the frame if you genuinely mean it.

Group edits sit in the capability matrix's `Configuration commands (19 names)`
row, whose real-bus column reads `partial`. Verify the result with `discover` or
a group-level `max` / `off` rather than assuming the write landed.

#### What the controller caches, and when it needs a scan

A `light:` entity with `target_type: group` needs one member's short address to
poll for state, because QUERY ACTUAL LEVEL addressed to a group collides as soon
as the group has two members. Nothing has to tell it which: on a node with no
membership in flash the component asks the bus — QUERY GROUPS against each short
address until every group light has a representative — and seeds itself. A scan
then replaces that with verified membership, which is persisted to flash.

`query_address:` remains as an override, for pinning a particular member (a
plain lamp rather than one sharing an input device) and for a `broadcast`
entity, where "everyone" has no member to derive. It is not a required seed,
and on any node that has been scanned it has been inert since the first scan.

Every surface that can change it also updates it:

| What you did | Effect on the controller's group table |
|---|---|
| `add-group` / `remove-group`, either surface | Applied immediately, and a light refresh is started |
| **Scan DALI Bus** button | Rebuilt from the bus, replacing whatever was there |
| `discover` or `commission` in the shell | Rebuilt the same way, from the same walk |
| `set-short-address-dtr0` | Poll targets dropped, and a warning that only a scan can restore them |

What a config command invalidates is decided in one place, whichever surface
ran it, so an edit typed into the shell is not one the entity that polls the
group finds out about later.

Two cases still need a scan, because no amount of bookkeeping can derive them:

- A **group-target** edit whose *source* group has not been scan-verified. The
  controller cannot compute who was affected, so it logs a warning and either
  leaves the destination's cache partial (`add-group`) or clears it
  (`remove-group`). The bus is correct; only the poll target is unknown.
- A **short address that moved**. Which address it moved to is not knowable
  from the command: the `config-dtr0` form carries the new value out of band,
  and the plain `config` form consumes whatever DTR0 already held. Caches keyed
  by the old address are dropped and a warning is logged rather than a new
  address being guessed.

#### Gear that is physically gone

A scan keeps the memberships of gear it did not see, so that a merely offline
fixture is not dropped from its room. The cost is that removed gear stays in the
cache forever, still eligible as a group's poll target. `group forget <addr>` in
the command console is the explicit retirement — it sends nothing, because there
is nothing left to answer, and only edits the cache. See `dali_commands.md` for
its full behaviour. For gear that is still on the wall, `config <target>
remove-group <g>` is the right verb: it reconfigures the device.

### Short addresses on gear that already has one

Every command that carries a short address as *data* rather than as an address
byte carries it **encoded** as `(address << 1) | 1`, with `0xFF` meaning "no
short address". Nothing in the shell does this conversion for you: the DTR0
value for `set-short-address-dtr0`, and the parameters of `special program-short`
and `special verify-short`, are all raw bytes in the range `0-255`.

| Short address | Encoded byte |
|---:|---|
| `a0` | `0x01` (1) |
| `a1` | `0x03` (3) |
| `a5` | `0x0B` (11) |
| `a10` | `0x15` (21) |
| `a63` | `0x7F` (127) |
| none | `0xFF` (255) |

Typing the plain number is not rejected, because it is a perfectly valid frame:
`special program-short 5` programs short address **2**, not 5. An even value has
bit 0 clear and is not a valid short address at all. Read the table before you
type the command.

Re-addressing a fixture in place needs no INITIALISE window, no RANDOMISE, and
nothing to terminate afterwards — SET SHORT ADDRESS is an ordinary addressed
configuration command:

```text
scan                                       # which addresses are occupied
config-dtr0 a5 set-short-address-dtr0 27   # a5 becomes a13   ((13<<1)|1 = 27)
config a13 save-persistent
scan                                       # a5 gone, a13 present
identify 13                                # and it is the fixture you meant
```

Check the destination is free first, from `scan` or `discover`. Two pieces of
gear on one short address is the `contested` condition described above, and
nothing on the bus can separate them remotely.

`special verify-short` is **not** the free-address probe it looks like. It
answers about whatever PROGRAM SHORT ADDRESS last wrote to a selected device
inside an initialisation window, which is where `commission` uses it. Outside
that window use `scan`, or `status a13` and see whether anything replies.

Swapping two addresses needs a free third one to stage through, exactly as
swapping two variables does:

```text
config-dtr0 a5 set-short-address-dtr0 41    # a5 -> a20, an address nothing uses
config-dtr0 a8 set-short-address-dtr0 11    # a8 -> a5
config-dtr0 a20 set-short-address-dtr0 17   # a20 -> a8
```

To take a fixture out of the address space entirely — because it is being
removed, or because you want `commission unaddressed` to reassign it — write the
"no short address" value:

```text
config-dtr0 a5 set-short-address-dtr0 255
```

The gear stops answering `a5` and still answers broadcast, so it is not lost, and
`commission unaddressed` will pick it up on the next run. The controller drops
what it had cached against `a5` and logs that its group membership is stale, but
it cannot know where the fixture went: run **Scan DALI Bus**, and `group forget
5` in the command console if the fixture is gone for good rather than moving.

The DTR0-consuming configuration commands are host-tested and carry **no
real-bus result** in `dali_capability_matrix.md`. Re-address one fixture, verify
it with `scan` and `identify`, and only then do the rest.

`set-short-address-dtr0` is gated exactly as the `special` primitives are, on
both spellings (`config` and `config-dtr0`) and on every target. A shell
session refuses it with `refused by session policy` unless the YAML says
`allow_commissioning: true`; the **DALI Command** text entity refuses it
outright with `commissioning config; use the native CLI`, the same answer it
gives `special program-short`. Gating one spelling of re-addressing and not the
other would have refused the harder one and permitted the easier one.

### The nine primitives, by hand

`special` exposes the addressing primitives individually. They are refused
unless the session has `allow_commissioning: true`, and the Home Assistant
console refuses them regardless with `commissioning special; use the native CLI`.

| Name | What it does alone |
|---|---|
| `initialise <param>` | Opens a 15-minute initialisation window. `0` = all gear, `255` = only gear with no short address, encoded address = that one device |
| `randomise` | Every gear in the window draws a new 24-bit random address. **Cannot be undone** |
| `search-h/m/l <byte>` | Loads one byte of the 24-bit search address that the next `compare` tests against |
| `compare` | `yes` if any gear in the window has a random address at or below the search address |
| `program-short <encoded>` | Writes the encoded short address into the currently selected gear |
| `withdraw` | Takes the selected gear out of the search, so the walk can find the next one |
| `write-memory`, `write-memory-nr` | Memory-bank writes; grouped here because they are equally unverifiable from the bus |
| `terminate` | Closes the window. **Never gated**, on either surface |

Read as a workflow: `initialise` opens the window, `randomise` scatters the
addresses, then repeated `search-h/m/l` plus `compare` binary-searches down to
the lowest random address present, `program-short` names it, `withdraw` removes
it from the search, and the loop repeats until `compare` says no. `terminate`
closes the window. That is exactly what `commission unaddressed` does, sequenced
through an atomic transport so that no other locally scheduled frame can land
between a search triple and the `compare` that interprets it — which, typed by
hand one line at a time, is not achievable.

So reach for the primitives for the cases the verb does not cover, not to
re-implement it:

- **Closing a window another tool left open.** `special terminate`. This is why
  it is never gated. A Lunatone session that was interrupted, or a `commission`
  run whose cleanup failed, leaves gear in initialisation state for fifteen
  minutes; `terminate` ends it now.
- **Re-addressing exactly one known fixture** when `set-short-address-dtr0` is
  not an option — `initialise` with that fixture's encoded address opens the
  window for it alone.
- **Reading the state of a selection** — `special query-short` and
  `special compare` change nothing and are useful while diagnosing a walk.

Two rules that nothing enforces:

1. Anything that opens a window must be followed by `special terminate`, on
   every path including the ones where you gave up. `terminate` before an
   `initialise` is harmless, so an extra one costs nothing.
2. `randomise` is irreversible. It does not change short addresses, but the
   random addresses a subsequent walk depends on are gone for good.

### Starting over

There is no `decommission` verb, and no single line that returns the bus to
factory addressing. The nearest sequence is a broadcast de-address followed by a
fresh walk:

```text
config-dtr0 b set-short-address-dtr0 255   # every gear loses its short address
commission unaddressed
```

Both lines need `allow_commissioning: true`, and neither is reachable from the
**DALI Command** text entity at all — which is the point: this is the pair that
an unauthenticated port should not accept from one typed line.

**Do not run this on an installation you need working.** Commissioning is
dependable today only with a *single* unaddressed device on the bus (see
[Assigning short addresses](#assigning-short-addresses)), so the first line
reliably destroys the addressing of every fixture while the second cannot
reliably restore it. Until the multi-gear walk has a hardware result, a bus-wide
redo means de-addressing and re-commissioning one fixture at a time, physically
disconnecting the rest or working through gear that is powered down.

What is safe, and usually what "redo the configuration" actually means:

- Re-grouping. Groups are independent of short addresses, so a room can be
  rebuilt entirely with `add-group` and `remove-group`, with no addressing risk
  at all.
- Re-addressing individual fixtures with `set-short-address-dtr0`, one at a time,
  verified with `scan` and `identify`.
- Adding new gear with `commission unaddressed`, which never touches what is
  already addressed.

After any of these, re-export the config and diff it against your source:

```sh
python3 /config/dali-shell export config > /config/dali_block.yaml
```

Short-address changes move `target_address`, and group changes move which
entities exist and which `member_groups` they carry. `export config` reads the
running firmware, so what it prints is what is actually in force.

## From Home Assistant instead

Everything below the shell in `dali-starter.yaml` predates it and does a fraction
of what it does. It stays for three reasons:

- A phone in Home Assistant is not a terminal. Pressing **Identify** while
  standing under a fixture beats SSHing from a ladder.
- The shell allows one session. The buttons keep working while someone else holds
  it, and while nobody holds it at all.
- The button scan and `discover` are separate implementations of the same walk.
  When a result looks wrong, running both is a real diagnostic — if they
  disagree, the disagreement is the finding.

The button workflow:

1. **Find Couplers**, then activate every physical switch within 30 s. Detected
   groups appear in **Couplers Result**. (shell: `find switches`)
2. **Scan DALI Bus** — about 50 s for 16 devices. (shell: `discover`)
   **Group Map** holds a compact group→query summary, persisted in HA, and YAML
   lines stream to the log prefixed with `YAML|`. A scan publishes ready-to-paste
   light YAML only when group discovery is complete; otherwise it keeps the
   previous map and asks you to retry.
3. Set **Target Address** (0–63) and press **Identify** to blink that fixture.
   Verify control with On / Off / Max / Min. (shell: `identify <addr>`)
4. **Bus Monitor** shows the last unsolicited frame. (shell: `trace on`)

To turn a saved log session into a YAML file, when the shell is not an option:

```powershell
(Select-String "YAML| " "$env:USERPROFILE\Downloads\dali-starter-logs.txt") |
  ForEach-Object { $_.Line -replace '.*YAML\| ', '' } |
  Out-File -Encoding utf8 "$env:USERPROFILE\Downloads\dali_lights.yaml"
```

Then add `dali_id:` and real names. `export config` does this in one step and
without the log-line limits.

The **DALI Command** text entity is the third route: one typed line, one result
in **DALI Command Result**. It cannot stream, so it has no `discover`, `find`,
`capture`, or `commission` — the verbs whose value is in watching them work. What
it does have it spells identically to the shell, because both resolve through the
same tables.
Due to how HA works, if you need to send last command again, add a trailing space.
Queries are all lowercase and will fail when automatically capitalised.  

## When something looks wrong

- Run the walk both ways — button scan and `discover`. A disagreement is a
  finding, not noise.
- `bus check` and `stats` for RX level, scheduler state, and fault counters.
- `queue` if commands are being dropped: a non-zero `full` or `busy` is dropped
  work, not deferred work.
- `trace on` for a live per-frame view; `capture start` … `capture export` when
  the interesting event is too fast to watch. Capture around **one command**,
  not a whole `discover`: the ring holds 128 records and a scan generates about
  1900, so all a scan leaves you is its tail. `dali_commands.md` explains how to
  read the export — in particular that `since_tx_us` runs from the end of the
  forward frame to the end of the reply, so the settling time the standard
  talks about is that number minus about 7.5 ms.
- A device that answers one scan and not the next is intermittent, not absent.
  Run `discover` more than once before concluding gear is missing or a group is
  empty — two runs disagreeing is itself the finding.
- Gear that has been physically removed stays in the group cache on purpose, so
  that a merely offline device is not dropped. `group forget <addr>` in the
  command console is the explicit way to retire it.

## Safety notes

- `config <target> reset` resets control-gear variables — and on devices like the
  Steinel HF 360 II it also wipes their proprietary memory bank. Do not issue it
  while tuning.
- Memory writes are never verified by the firmware. Read the value back.
- Input-device configuration writes are experimental: `OK` means the sequence was
  queued, not that the device accepted it.
- Set `allow_commissioning: false` once the bus is commissioned.
