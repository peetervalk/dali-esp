# Commissioning a DALI Bus

How to go from an unknown bus to a configuration you can keep. Flash the starter
firmware, find out what is on the wire, then write your own config from what it
found.

**Last reviewed:** 2026-08-14

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

**One session at a time.** The bus is single-tenant, so the shell is too. A
second connection is accepted only far enough to be told why it is closing. A
terminal closed without quitting is reclaimed by the `idle_timeout` in the YAML.

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
| `max b`, `off b`, `level s3 128` | Drive gear directly while you work |
| `trace on` | Live per-frame trace |
| `capture start … export` | Rolling capture, dumped when something goes wrong |
| `bus check`, `stats` | RX level, scheduler state, fault counters |

A typical first pass is `discover`, then `identify <addr>` under each fixture to
map addresses to rooms, then `find switches 300` while someone presses every wall
switch.

A long verb prints as it goes and holds the bus for as long as it runs —
`find switches 300` is five minutes by design. Ctrl-C in the client drops the
connection, which the device notices between bus steps and stops.

Full verb syntax is in `dali_commands.md`.

### Assigning short addresses

`commission unaddressed [first-addr] [max-devices]` runs the INITIALISE /
RANDOMISE / COMPARE / PROGRAM SHORT ADDRESS walk, sequenced and checked.

**It is dependable only with a single unaddressed device on the bus.** The
COMPARE collision inversion recorded in `current_status.md` is unfixed, so
commission new gear one piece at a time.

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
