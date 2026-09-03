#pragma once

/*
 * dali_shell.h — the diagnostic CLI as a reusable session
 *
 * dali_cli.c decides what a line *means*: tokenizing, the verb table, argument
 * validation, the named command tables, and response formatting. This module
 * decides what a line *does*: it owns the blocking transport, the caches a
 * workflow accumulates across commands, and the long-running walks (scan,
 * discover, commission, capture, find) that must block on a reply before they
 * know what to ask next.
 *
 * It is deliberately not a task and not a transport. A front end supplies both:
 *
 *   main/dali_diag.c            UART0, the serial console
 *   esphome/.../dali_shell_tcp  a listening socket, for a terminal in Home
 *                               Assistant
 *   host tests                  a scripted transport and a buffer sink
 *
 * That is what makes the Home Assistant surface identical to the serial one
 * rather than an approximation of it: there is one implementation of every
 * verb, and a front end changes only where the bytes come from and where they
 * go.
 *
 * ── One session at a time ───────────────────────────────────────────────────
 *
 * The caches below (inventory, capture ring, event queue, switch mappings) are
 * module state, not per-session state, and dali_shell_attach() refuses a second
 * caller while one holds the session. That is a deliberate limit rather than an
 * omission: the bus itself is single-tenant, so a second concurrent session
 * would serialize on the transport anyway while giving two operators an
 * inconsistent view of one capture ring. A front end that must survive an
 * abandoned session (a terminal window closed without a quit) is responsible
 * for its own idle timeout, and calls dali_shell_detach() when it fires.
 *
 * ── Threading ───────────────────────────────────────────────────────────────
 *
 * Everything except the two subscriber entry points runs on the task that calls
 * dali_shell_dispatch(). dali_shell_on_trace() and dali_shell_on_event() run on
 * the scheduler owner task and touch only the capture ring and event queue,
 * under the module's own critical section.
 */

#include "dali_cli.h"
#include "dali_discovery.h"
#include "dali_event.h"
#include "dali_frame.h"
#include "dali_input_device.h"
#include "dali_scheduler.h"
#include "dali_transport.h"

/* Session capacities. These keep the values they had as DIAG_* in dali_diag.c. */
#define DALI_SHELL_LINE_MAX            80u
#define DALI_SHELL_CAPTURE_MAX         64u
#define DALI_SHELL_SWITCH_MAPPING_MAX  16u
#define DALI_SHELL_INPUT_CACHE_MAX     16u
#define DALI_SHELL_SENSOR_CACHE_MAX    16u

/* ---------------------------------------------------------------------------
 * Policy
 *
 * The verb table is shared, so a front end that must refuse a verb cannot do it
 * by leaving the verb out — that would fork the spelling of commands across
 * surfaces, which is what dali_cli_resolve_in() exists to prevent. It declares
 * a policy instead, and the refusal is reported in one place with one wording.
 *
 * DALI_SHELL_ALLOW_COMMISSION is separate from everything else because it is
 * the one that can readdress a whole bus from a single typed line: the
 * `commission` verb, and the nine primitives dali_cli_special_is_commissioning()
 * marks. RANDOMISE among them cannot be undone. A serial console has always had
 * it, because physical access to the UART is already physical access to the
 * bus. A session reachable over the network must opt in.
 * --------------------------------------------------------------------------*/

#define DALI_SHELL_ALLOW_COMMISSION  0x01u  /* `commission`, commissioning specials */
#define DALI_SHELL_ALLOW_RESET       0x02u  /* `reset`: cancels in-flight work      */

#define DALI_SHELL_ALLOW_NONE        0x00u
#define DALI_SHELL_ALLOW_ALL         (DALI_SHELL_ALLOW_COMMISSION | \
                                      DALI_SHELL_ALLOW_RESET)

/* ---------------------------------------------------------------------------
 * Host hooks
 *
 * How a session tells the surrounding integration that it is about to take the
 * bus for something long. The native firmware has nothing to coordinate with
 * and leaves these NULL; the ESPHome binding uses them to raise the same gate a
 * button-initiated scan raises, so the light refresh pump does not interleave
 * queries into a commissioning walk.
 *
 * bus_claim() returning false aborts the verb before any traffic reaches the
 * bus, which is how a scan already running refuses a second one rather than
 * racing it. Every successful claim is matched by exactly one release.
 * --------------------------------------------------------------------------*/

/*
 * Per-instance detail for one input device, out of the session's caches.
 *
 * Whatever last read an address's instances fills this in — a `discover`
 * sweep or an explicit `instances <addr>`. Both store the instance type and
 * the reported resolution, which together are the difference between knowing
 * an address has three instances and knowing that instance 0 is a push button
 * and instance 1 an occupancy sensor reporting one byte.
 *
 * Returns false when nothing has read that address this session. That is not
 * the same answer as "no usable instances", and the two want different advice,
 * so a caller must not treat a false return as an empty device.
 */
typedef bool (*DaliShellInputLookupFn)(uint8_t addr,
                                       DaliDiscoveryInputDevice *out);

typedef struct {
    /* `what` names the workflow ("scan", "commission") for the integration's
     * own logging; it is never NULL. */
    bool (*bus_claim)(void *ctx, const char *what);
    void (*bus_release)(void *ctx);
    /* Called after a workflow that invalidates cached device state, so the
     * integration can re-read what it holds. NULL when nothing caches. */
    void (*inventory_changed)(void *ctx, const DaliDiscoveryInventory *inventory);
    /*
     * Called after one addressed configuration command was accepted for
     * transmission, naming what it was.
     *
     * A `config` verb is a single frame rather than a workflow, so it claims no
     * bus and reaches none of the hooks above — which meant a group edit typed
     * into a session left the integration's group-membership cache asserting
     * the previous membership, and a group light polling a fixture that had
     * left. The shell holds no such cache and cannot fix that itself; it can
     * only say what it just sent.
     *
     * `id` is the shared DaliCommandId, so the integration decides what a given
     * command invalidates rather than the shell deciding for it. Called on the
     * session's task, and only when the send reported success: a refused frame
     * changed nothing. NULL when nothing caches.
     */
    void (*config_applied)(void *ctx, DaliTarget target, DaliCommandId id,
                           uint8_t param);
    /*
     * Called after `address <from> set <to>` re-addressed one gear and both
     * ends were confirmed on the bus: `to` answers, `from` is silent.
     *
     * This is what config_applied() cannot say. SET SHORT ADDRESS carries its
     * destination in DTR0, so an integration told only "a config command went
     * to a5" knows the gear moved but not where, and its only safe response is
     * to drop everything keyed by a5 and ask for a rescan. The `address` verb
     * chose both ends and then verified them, so it can name the move and let
     * caches follow it instead.
     *
     * Only reached after verification succeeded: an unconfirmed move calls
     * nothing, because a cache moved to an address that turns out to be wrong
     * is worse than a cache dropped. Called on the session's task. NULL when
     * nothing caches.
     */
    void (*short_address_moved)(void *ctx, uint8_t from, uint8_t to);
    /*
     * Print the integration's own configuration as the YAML block that would
     * produce it — what `export config` emits.
     *
     * This is a hook rather than a shell verb implemented here because the
     * facts it prints are the integration's, not the bus's: which GPIOs the
     * PHY was handed, which entities exist and what each one targets. The
     * shell holds none of that, and the native firmware has none of it to
     * hold — it is not configured by YAML at all, so it leaves this NULL and
     * the verb says so rather than inventing a block nobody could flash.
     *
     * `inventory` is the session's last scan, or NULL when nothing has been
     * discovered yet; an integration merges what the bus reported into what it
     * was configured with, and says which is which.
     *
     * `input_lookup` reaches the same session's per-instance input detail, so
     * the integration can name what an uncovered instance actually is instead
     * of telling the operator to go and ask the device something it has
     * already been asked. NULL when the caller keeps no such cache.
     */
    void (*export_config)(void *ctx, const DaliCliOut *out,
                          const DaliDiscoveryInventory *inventory,
                          DaliShellInputLookupFn input_lookup);
    /*
     * Persist and reload the address backup across a reboot.
     *
     * These are hooks rather than shell code because durable storage belongs to
     * the integration: the ESPHome component has the preferences API and the
     * Core 0 affinity rule that comes with it, and the native firmware has
     * neither. The shell owns the snapshot, its codec and its verbs; the
     * integration owns only the bytes.
     *
     * `save` receives an encoded blob of `len` bytes, never more than
     * DALI_SNAPSHOT_BLOB_MAX, and returns true when it is stored. `load` fills
     * `buf` and writes the byte count to `*len`, returning false when nothing
     * is stored — which is a normal cold start, not a fault.
     *
     * Both NULL is legitimate and means backups live only in RAM and in
     * whatever the operator did with `backup export`. The shell says so rather
     * than implying a durability it does not have.
     */
    bool (*snapshot_save)(void *ctx, const uint8_t *buf, uint32_t len);
    bool (*snapshot_load)(void *ctx, uint8_t *buf, uint32_t *len);
    void  *ctx;
} DaliShellHooks;

/* ---------------------------------------------------------------------------
 * Session binding
 * --------------------------------------------------------------------------*/

typedef struct {
    /* Where output goes. Required. */
    DaliCliOut     out;
    /* How frames reach the bus. Required; must provide at least `transact`.
     * A transport without `transact_sequence` is accepted, but verbs that need
     * local atomicity refuse rather than fall back to split frames — on a live
     * bus the fallback is a different operation, not a slower one. */
    DaliTransport  transport;
    /* Optional; a zeroed struct means "nothing to coordinate with". */
    DaliShellHooks hooks;
    uint8_t        policy;

    /*
     * Polled by every long workflow between steps. A socket session sets it
     * when the peer disappears, so a discover that has forty addresses left to
     * walk stops working for a reader that is no longer there instead of
     * holding the bus for another thirty seconds. NULL means "never abort".
     */
    bool (*aborted)(void *ctx);
    void  *abort_ctx;

    /*
     * Where output produced on a task other than the one that attached goes.
     *
     * `trace on` prints from dali_shell_on_trace(), which runs on the scheduler
     * owner task — the task that drives RX decode and the reply window on a
     * 1 ms loop. Writing to a UART from there costs microseconds and has always
     * been acceptable. Writing to a socket does not: a send() that blocks for
     * tens of milliseconds delays reply handling and turns live tracing into
     * the cause of the timeouts it is being used to investigate.
     *
     * With this set, such output is queued into a fixed ring instead, and the
     * front end drains it from its own task with
     * dali_shell_pump_deferred_output(). Lines that do not fit are dropped and
     * counted, so a burst of bus traffic costs trace lines rather than bus
     * timing. Leave it false for a sink that is cheap to call from any task.
     */
    bool defer_foreign_task_output;
} DaliShellSession;

/*
 * Prepare module state and register the scheduler subscribers. Call once, from
 * the same place that starts the DALI task, before any front end attaches.
 *
 * The subscribers stay registered for the life of the firmware. A session that
 * comes and goes gates on its own attach state instead of registering and
 * unregistering, because a subscriber removed while the owner task is running
 * can still be entered once more (see dali_scheduler.h).
 */
DaliError dali_shell_init(void);

/*
 * Take the session. Returns DALI_ERR_BUSY when one is already attached, which
 * is how a listener refuses a second connection, and DALI_ERR_INVALID for a
 * session with no sink or an unusable transport.
 *
 * Attaching clears the per-session caches, so a new operator never inherits the
 * previous one's inventory or capture ring.
 */
DaliError dali_shell_attach(const DaliShellSession *session);

/* Release the session. Safe to call when nothing is attached. */
void dali_shell_detach(void);

bool dali_shell_is_attached(void);

/*
 * Execute one line. Echoes it, resolves it against the full native verb table,
 * reports a resolve failure through dali_cli_report_resolve(), applies the
 * session policy, and runs the handler.
 *
 * Blocks for as long as the verb takes — a discover on a populated bus is tens
 * of seconds — so the calling task must not be one with a deadline. In
 * particular this must never be called from the ESPHome loop task.
 */
void dali_shell_dispatch(const char *line);

/*
 * Optional line assembly for a byte-at-a-time front end: handles CR/LF,
 * backspace, and the length cap, and calls dali_shell_dispatch() on a complete
 * line. An over-long line is rejected whole rather than truncated, because a
 * clipped line can otherwise become a different, valid command.
 *
 * Returns true when a line was dispatched, so the caller knows when to write
 * its prompt.
 */
bool dali_shell_feed_byte(uint8_t ch);

/* Greeting and prompt, so every front end presents the same shell. */
void dali_shell_write_banner(void);
void dali_shell_write_prompt(void);

/*
 * Flush output that was produced on another task while
 * defer_foreign_task_output was set. Call from the session's own task,
 * regularly — a front end blocked in recv() should use a receive timeout and
 * pump on each expiry, so live trace still arrives while nothing is typed.
 *
 * Returns the number of lines dropped since the last call because the ring was
 * full, so a front end can tell the operator that the trace has gaps rather
 * than letting them read a partial capture as a complete one.
 */
uint32_t dali_shell_pump_deferred_output(void);

/* True while `trace on` is in effect. */
bool dali_shell_trace_enabled(void);

/* ---------------------------------------------------------------------------
 * Scheduler subscribers
 *
 * Registered by dali_shell_init() through dali_sched_add_*_subscriber(), so the
 * integration's own dispatch keeps its registration. Exposed because a host
 * test drives them directly.
 * --------------------------------------------------------------------------*/

void dali_shell_on_trace(const DaliSchedTraceEvent *event, void *ctx);
void dali_shell_on_event(const DaliFrame *frame, void *ctx);

/* ---------------------------------------------------------------------------
 * Device transport
 *
 * The blocking transport every device front end shares: it enqueues one
 * transaction or one sequence and parks the calling task on a FreeRTOS
 * notification until the scheduler's completion fires. Not available on host
 * builds, where a test supplies its own scripted transport instead.
 * --------------------------------------------------------------------------*/

#ifndef DALI_HOST_BUILD
const DaliTransport *dali_shell_device_transport(void);
#endif
