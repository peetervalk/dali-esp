# Commissioning a DALI Bus

How to go from an unknown bus to a configuration you can keep. Flash the starter
firmware, find out what is on the wire, then write your own config from what it
found.

**Last reviewed:** 2026-08-25

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

### Assigning short addresses

`commission unaddressed [first-addr] [max-devices]` runs the INITIALISE /
RANDOMISE / COMPARE / PROGRAM SHORT ADDRESS walk, sequenced and checked.

The run silences control devices for its duration. A broadcast Part 103 START
QUIESCENT MODE goes out before INITIALISE and a STOP follows TERMINATE, so an
occupancy sensor or wall switch cannot put an event frame into a COMPARE reply
window, where frame-like activity reads as YES and invents gear that is not
there. Only control devices are affected; lights keep working throughout.

Two consequences worth knowing. The release is unconditional, so a run also
releases a quiescence you started by hand with `quiescent on all`. And if the
release cannot be transmitted, the shell says so explicitly — control devices may
stay silent, and `quiescent off all` is the fix.

**It is still dependable only with a single unaddressed device on the bus.** The
reply-activity and cleanup changes described here are not a multi-gear HIL result.
Commission new gear one piece at a time until the equal-random-address and
mixed-device cases below have been implemented and exercised on a real bus.

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

The remaining commissioning work is explicit:

- The cross-part TERMINATE guard is not implemented: a control device that
  observes the Part 102 INITIALISE can still enter its own addressing state.
  START/STOP QUIESCENT bracketing now runs, which stops a control device from
  *transmitting* into the run, but a device that never received the broadcast is
  unaffected and none of it is HIL-validated.
- Two gear that generate the same 24-bit random address are not separated or
  recovered today; they can be programmed and withdrawn together.
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
closes an initialise window another tool opened.

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
real names, set `query_address` for group lights, and add the sensors you found.

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
  the interesting event is too fast to watch.
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
