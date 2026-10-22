# DALI-ESP Agent Guide

Read `current_status.md` first: what the code does now, and its *Known
Limitations*. Then read Part 2 of `todo.md`, *Develop*, for open work. Consult
the rest as needed:

- **`todo.md`, Part 1, *Verify*,** lists everything a real bus has not
  confirmed, with the procedure for each. It is long. Read it when planning a
  bus session, not to orient.
- **`CHANGELOG.md`, *Unreleased*,** says what `dev` has changed since the last
  tag.
- **`dali_capability_matrix.md`** records, per capability, whether it has run
  on a bus.
- **`project_log.md`** holds the dated evidence and past investigations. Read
  it only when you need the history.
- **`dali_commands.md`** has verb and argument detail, and **`dali_protocol.md`**
  has frames and opcodes.

## Project Aim

Two goals, in order:

1. **CLI completeness**: if it can be done using DALI commands, the CLI
   should be able to do it. The CLI is the reference tool for verifying real-bus
   behavior and for proving protocol features before they reach the controller
   layer.
2. **ESP32 DALI controller**: leverage the proven CLI/protocol stack to build a
   working DALI controller firmware for ESP32, with primary emphasis on ESPHome and
   Home Assistant integration.

## Non-Negotiable Architecture

Keep protocol logic independent of ESPHome:

```text
ESPHome / HA integration
DALI integration and entity mapping
DaliControl
DaliProtocol
DaliScheduler
DaliPhy
DALI-2 Click
DALI bus
```

`components/dali` is the reusable protocol, scheduler, discovery, dispatch, and
PHY stack, plus `dali_shell.c` — the diagnostic CLI as a session, owning every
verb, the blocking transport, and the caches a workflow accumulates.
`main/main.c` is the native ESP-IDF entry point and `main/dali_diag.c/.h` is now
only the UART0 binding for that shell. The active ESPHome component is
`esphome/components/dali`, whose `dali_shell_tcp.cpp` is the second binding.

A front end moves bytes and owns a session's lifetime. It must not implement,
gate, or reword a verb: that is what keeps the Home Assistant shell identical to
the serial one rather than an approximation of it. A surface that must refuse a
verb declares a `DALI_SHELL_ALLOW_*` policy instead of editing the verb table.

Do not put protocol, timing, sensor, or addressing logic into ESPHome entities.
The ESPHome layer should map configured entities to `DaliTarget` values and call
the control/protocol APIs.

### What Lives Where

| Module | Location | Reason |
|---|---|---|
| Protocol, PHY, scheduler, discovery, memory, DT6, DT8, input config | `components/dali` | Reusable, no app dependencies |
| `dali_mapping` | `components/dali` | Generic enough to stay; move to `main/` if the component is published separately |
| `dali_dispatch` | `components/dali` | Headless dispatch engine; pure C, no ESPHome dependency |
| `headless_dispatch` rules | YAML, per site | Installation-specific dispatch table; no source file. Schema and codegen in `esphome/components/dali/__init__.py`, loaded through `DaliComponent::add_dispatch_entry()`; active when the list is non-empty |
| `dali_shell` | `components/dali` | Every CLI verb, the blocking transport, and session caches; one implementation both front ends run |
| `dali_diag` | `main/` | UART0 binding for the shell; moves bytes only |
| `dali_shell_tcp` | `esphome/components/dali` | TCP binding for the shell: one session, idle timeout, policy from YAML. Refuses a peer that opens with an HTTP request |
| `main.c` | `main/` | App entry point |

### Device Type Coverage

| DT | Standard | Status |
|---:|---|---|
| DT6 | IEC 62386-207 | Implemented (`dali_gear_dt6`) |
| DT8 | IEC 62386-209 | Implemented (`dali_gear_dt8`) |
| DT1 | IEC 62386-202 | Not implemented; add when needed following the DT6/DT8 pattern |

Input devices use instance types, not control-gear DT numbers. IEC 62386-301
push buttons are instance type 1, Part 303 occupancy sensors are instance type 3,
and Part 304 light sensors are instance type 4. Their type-specific configuration
builders are in `dali_input_config`; common Part 103 queries are in
`dali_input_device`.

## Timing And ISR Rules

- GPTIMER tick: 104 us.
- DALI bit period: about 833.3 us; half-bit period: about 416.7 us.
- **Reply attribution** is measured from the end of the forward frame's last
  data bit, which the TX ISR stamps:
  - A decoded backward frame is accepted from 3.664 ms.
  - Undecodable activity counts from 5.5 ms.
  - The window closes at 28.664 ms.

  The 5.5 ms edge is the standard's minimum, and it is a safety edge. COMPARE
  reads undecodable activity as YES, so moving that edge earlier invents gear.
- The stop bits take 1.664 ms after the stamp. TX-to-RX settle suppression is
  2 ms, counted from TX complete, after the stop bits.
- Scheduler reply wait: 25 ms after TX handoff.
- Gap before our next forward frame: 22 Te, counted from our own transmissions
  only. Nothing yet spaces our frame from a received one; see `todo.md`,
  *Space forward frames from received frames*.
- Send-twice window: 100 ms.
- GPIO16 and GPIO17 are connected to WROVER-E PSRAM and must not be used on
  that module. The pin schema does not reject them — they are ordinary pins on
  WROOM and other variants — so this is a review check, not a build error.

ISR code must remain minimal:

- No logging.
- No allocation.
- No protocol parsing.
- No ESPHome calls.
- Use counters for ISR-visible errors.
- All ISR-called functions must be `IRAM_ATTR`.

RX must follow the buffer-first model:

```text
ISR -> fixed ring buffer -> task-context frame decoder -> scheduler/protocol
```

## Coding Guidance

- Preserve fixed-size/static allocation in PHY, scheduler, and protocol layers.
- Support both 16-bit and 24-bit DALI frames; do not assume 16-bit-only traffic.
- Use `DaliFrame`, `DaliError`, and explicit enums instead of bare integers.
- Use `uint8_t`, `uint16_t`, and `uint32_t` in protocol and PHY surfaces.
- Keep host tests in `test/` as the canonical portable test harness.
- Use Lunatone DALI USB / DALI Cockpit as the main external reference tool for
  real-bus behavior.

## Collaboration And Hardware Rules

- Documentation-only updates may be made directly.
- Suggest software-stack changes first and implement them only after explicit
  go-ahead.
- Do not commit, do not push, unless specifically asked to.
- Touch hardware/serial only after explicit go-ahead. The procedures in
  `todo.md` drive real buses, so running one needs that go-ahead too.
- Touch only the serial port named in *Local setup* below. If it is
  unavailable, stop and notify rather than scanning for another — the wrong
  port is someone else's device.

## Documentation Rules

- **`current_status.md` describes what the code does now,** as expected
  behaviour and known limitations. It carries no dates, session narratives or
  verification labels.
  - A statement with a date on it belongs in `project_log.md`.
  - What a release changed belongs in `CHANGELOG.md`.
- **`todo.md` holds everything open.**
  - Each expectation that a real bus has not confirmed is listed there, with
    the procedure that would confirm it.
  - Each defect or gap has its fix, decision or measurement there.
  - When an item is done, record the result in `project_log.md`, update the
    matrix row, and delete the item. Do not annotate it as done.
- **`project_log.md` is append-only.** Never edit an entry to reflect a later
  finding; add one that supersedes it.
- **Every operator-visible or API change** goes into `CHANGELOG.md` under
  *Unreleased* as it lands, with its verification label: hardware-verified,
  host-tested, or unverified. At tagging, re-check the labels and rename the
  heading to the version.
- **Detail lives in its own file:** verb and argument detail in
  `dali_commands.md`, frame and opcode detail in `dali_protocol.md`, and
  per-capability bus status in `dali_capability_matrix.md`.
- **Pinned configs describe the tag.** `dali-starter.yaml` and the README's YAML
  example pin a release, so their comments must stay true of that release.
  Prose docs describe `dev`. A `dev` change that makes a pinned comment wrong
  goes on the release checklist in `todo.md`.

## Build And Test

The paths and port below are the maintainer's Windows workstation, which is the
only environment these have been run in; substitute your own. CI is the
portable definition of a build — see *CI* below.

**Local setup:**

- serial port `COM6`
- ESP-IDF 6.0.1 at `C:\Espressif`
- MSYS2 UCRT64 toolchain at `C:\msys64`
- ESPHome installed as a Python module, so it runs as `python -m esphome`

Run ESPHome from PowerShell. Under Git Bash the IDF configure step fails while
the compile still reports success.

Native firmware:

```powershell
. "C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1"
idf.py build
idf.py -p COM<N> flash monitor
```

Host tests:

```powershell
cd test
C:\msys64\ucrt64\bin\mingw32-make.exe --directory build
C:\msys64\ucrt64\bin\mingw32-make.exe --directory build test CTEST_OUTPUT_ON_FAILURE=1
```

ESPHome:

```powershell
python -m esphome config  dali_test.yaml      # cheapest check; validates the in-repo component
python -m esphome compile dali_test.yaml      # the working tree, every platform
python -m esphome compile dali-starter.yaml   # whatever ref it pins, not the working tree
```

**Reach for `esphome config` first.** It validates the schema without touching
a build directory, so it cannot collide with a compile running in another
terminal. It does not run `to_code()`, so a codegen error still needs a compile
to surface.

**To prove that component changes build, compile `dali_test.yaml`.** It
resolves `type: local` against `esphome/components`, so it compiles the working
tree by construction. It also declares every platform and every optional block
on purpose. An option added to the schema but not to `dali_test.yaml` is an
option nothing compiles, so extend that file in the same change.

### CI

Four workflows in `.github/workflows/`:

| Workflow | Covers | Trigger |
|---|---|---|
| `host-tests.yml` | The host suites, cmake/ctest over `test/` | push main/dev, PR to main |
| `idf-build.yml` | Native firmware, `idf.py build` for esp32 on IDF 6.0.1 | push main/dev, PR to main |
| `esphome-build.yml` | Source/shim/`SRCS` agreement, config discovery, per-config schema validation, and a compile of every `type: local` config | push main/dev, PR to main |
| `release-packaging.yml` | `esphome compile` from a git tag in an empty directory | tag `v*`, manual |

- **`esphome-build.yml` names no configuration.** `discover` runs `git ls-files
  'dali*.yaml'` and sorts the results by whether `external_components` says
  `type: local`.
  - Those build the tree under test, and are compiled.
  - A config pinning `type: git` at a ref is only validated. ESPHome would
    fetch and compile that ref instead of the branch, so a 20-minute build
    would report on code the pull request never touched.
- **A failure on a pinned config does not mean the branch is broken.** The
  tracked config has drifted out of schema with the ref it names. Either the
  pin or the config needs updating before release.
- **Do not delete `dali_test.yaml`** without replacing what it covers. If no
  tracked config uses `type: local`, `compile` is skipped and the ESPHome C++
  layer gets no coverage at all. `discover` emits a warning, but the run can
  still go green.
- **`secrets.yaml` is gitignored,** so every ESPHome job writes its own dummy
  one with the values the tracked configs reference. `dali_test.yaml` uses 
  inline dummies, so it validates in a bare checkout, a fork's first CI run included.
- **`release-packaging.yml` never runs `actions/checkout` and caches nothing,**
  on purpose. Run inside a repo checkout, it would pass for the wrong reason.
  `workflow_dispatch` takes a ref, so a branch can be packaging-tested before
  it is tagged.
- The two builds compile the same `components/dali` C with different
  toolchains. ESPHome 2026.9 builds with IDF 5.5.5; the native build pins IDF
  6.0.1.
