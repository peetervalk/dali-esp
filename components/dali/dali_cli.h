#pragma once

/*
 * dali_cli.h — portable core of the native diagnostic CLI
 *
 * Everything here is plain C with no ESP-IDF, FreeRTOS, or bus dependency, so
 * the parts of the CLI that decide *what* a line means can be exercised on the
 * host. The parts that decide *when a frame reaches the bus* stay in
 * dali_diag.c, which owns the scheduler slots and the FreeRTOS task.
 *
 * The split is:
 *
 *   dali_cli.c   tokenizing, the verb table, argument validation, the named
 *                command tables, and response formatting
 *   dali_diag.c  transports, blocking waits, caches, and the long-running
 *                workflows (scan, commissioning, capture, find switches)
 *
 * A verb is dispatched by looking it up in the one table below. A verb that is
 * not in the table cannot be reached, and a verb in the table that has no
 * handler is a compile error, because dali_diag.c switches over
 * DaliCliCommandId with no default case.
 *
 * Trailing tokens are rejected for every verb rather than ignored: the table
 * carries each verb's argument count bounds, and dali_cli_resolve() enforces
 * them before any handler sees the line.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dali_control.h"
#include "dali_frame.h"
#include "dali_gear_dt6.h"
#include "dali_gear_dt8.h"
#include "dali_input_config.h"
#include "dali_input_device.h"
#include "dali_lunatone.h"
#include "dali_protocol.h"

/* ---------------------------------------------------------------------------
 * Output sink
 *
 * Formatting is written against this rather than printf so the same code paths
 * that run on the device can be captured into a buffer and asserted on.
 * --------------------------------------------------------------------------*/

typedef void (*DaliCliWriteFn)(void *ctx, const char *text);

typedef struct {
    DaliCliWriteFn write;
    void          *ctx;
} DaliCliOut;

/* Longest single formatted chunk. Longer output is emitted in several calls. */
#define DALI_CLI_FORMAT_MAX 160u

void dali_cli_write(const DaliCliOut *out, const char *text);
void dali_cli_printf(const DaliCliOut *out, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/*
 * Fixed-capacity capture sink. `truncated` records that output was lost, so a
 * test never mistakes a clipped buffer for the whole response.
 */
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   truncated;
} DaliCliBufferSink;

void       dali_cli_buffer_sink_init(DaliCliBufferSink *sink, char *buf, size_t cap);
DaliCliOut dali_cli_buffer_out(DaliCliBufferSink *sink);

/* ---------------------------------------------------------------------------
 * Tokenizing
 * --------------------------------------------------------------------------*/

#define DALI_CLI_MAX_TOKENS     8u
#define DALI_CLI_MAX_TOKEN_LEN 32u  /* including the terminating NUL */

typedef struct {
    uint8_t count;
    char    tok[DALI_CLI_MAX_TOKENS][DALI_CLI_MAX_TOKEN_LEN];
} DaliCliTokens;

typedef enum {
    DALI_CLI_TOKENIZE_OK = 0,
    DALI_CLI_TOKENIZE_EMPTY,
    DALI_CLI_TOKENIZE_TOO_MANY,
    DALI_CLI_TOKENIZE_TOO_LONG,
} DaliCliTokenizeResult;

/*
 * Split a line on spaces and tabs, dropping a trailing CR/LF. An over-long
 * token or an over-long line is an error rather than a silent truncation: both
 * would otherwise turn a typo into a different, valid command.
 */
DaliCliTokenizeResult dali_cli_tokenize(const char *line, DaliCliTokens *out);

/* ---------------------------------------------------------------------------
 * Verb table
 * --------------------------------------------------------------------------*/

typedef enum {
    DALI_CLI_CMD_HELP = 0,
    DALI_CLI_CMD_STATS,
    DALI_CLI_CMD_QUEUE,
    DALI_CLI_CMD_BUS,
    DALI_CLI_CMD_CAPTURE,
    DALI_CLI_CMD_TRACE,
    DALI_CLI_CMD_READ,
    DALI_CLI_CMD_RXDEBUG,
    DALI_CLI_CMD_RESET,

    DALI_CLI_CMD_LIST,
    DALI_CLI_CMD_SCHEMA,
    DALI_CLI_CMD_QUERY_LIST,
    DALI_CLI_CMD_SPECIAL_LIST,
    DALI_CLI_CMD_CONFIG_LIST,

    DALI_CLI_CMD_RAW,
    DALI_CLI_CMD_RAW2,
    DALI_CLI_CMD_DTR,

    DALI_CLI_CMD_LEVEL,
    DALI_CLI_CMD_MASK,
    DALI_CLI_CMD_OFF,
    DALI_CLI_CMD_UP,
    DALI_CLI_CMD_DOWN,
    DALI_CLI_CMD_STEP_UP,
    DALI_CLI_CMD_STEP_DOWN,
    DALI_CLI_CMD_STEP_OFF,
    DALI_CLI_CMD_ON_STEP,
    DALI_CLI_CMD_CONT_UP,
    DALI_CLI_CMD_CONT_DOWN,
    DALI_CLI_CMD_DAPC_SEQ,
    DALI_CLI_CMD_LAST,
    DALI_CLI_CMD_SCENE,
    DALI_CLI_CMD_MAX,
    DALI_CLI_CMD_MIN,
    DALI_CLI_CMD_STATUS,

    DALI_CLI_CMD_QUERY,
    DALI_CLI_CMD_SPECIAL,
    DALI_CLI_CMD_CONFIG,
    DALI_CLI_CMD_CONFIG_DTR0,
    DALI_CLI_CMD_ADDRESS,

    DALI_CLI_CMD_MEMREAD,
    DALI_CLI_CMD_MEMINFO,
    DALI_CLI_CMD_DEVMEM,
    DALI_CLI_CMD_DTRCHECK,

    DALI_CLI_CMD_DT6,
    DALI_CLI_CMD_DT8,

    DALI_CLI_CMD_IQUERY,
    DALI_CLI_CMD_ICONFIG,
    DALI_CLI_CMD_VENDOR,

    DALI_CLI_CMD_SCAN,
    DALI_CLI_CMD_DISCOVER,
    DALI_CLI_CMD_INVENTORY,
    DALI_CLI_CMD_COMMISSION,
    DALI_CLI_CMD_INSTANCES,
    DALI_CLI_CMD_SENSOR,
    DALI_CLI_CMD_SMOKE,
    DALI_CLI_CMD_EVENTS,
    DALI_CLI_CMD_FIND,
    DALI_CLI_CMD_EXPORT,
    DALI_CLI_CMD_IDENTIFY,
    DALI_CLI_CMD_QUIESCENT,
    DALI_CLI_CMD_BACKUP,
    DALI_CLI_CMD_RESTORE,
    DALI_CLI_CMD_DEVINFO,

    /*
     * Also the marker a front end with its own table (see dali_cli_resolve_in)
     * puts on a verb that has no shared id, because it acts on state only that
     * front end has. Use it only for that case: a verb that merely happens to
     * be unimplemented on the other side belongs in the shared table, so both
     * surfaces keep one spelling for it.
     */
    DALI_CLI_CMD_COUNT,
} DaliCliCommandId;

/* How a target is spelled, so every usage line says it the same way. */
#define DALI_CLI_TARGET_ARG "<addr|aN|gN|b>"

typedef struct {
    DaliCliCommandId id;
    const char      *name;
    const char      *args;     /* usage suffix; "" when the verb takes none */
    const char      *summary;
    uint8_t          min_args; /* argument count bounds, excluding the verb   */
    uint8_t          max_args;
    /*
     * For a verb that takes a fixed keyword rather than a value in one of its
     * argument positions, the accepted keywords, space-separated. Handlers test
     * membership with dali_cli_has_subcommand() instead of comparing their own
     * literals, so the usage line and what the handler accepts cannot drift
     * apart. Usually the first argument (`commission unaddressed`), but not
     * always: `address` names its subject first and the keyword second
     * (`address a5 set a13`), and the membership test does not care which.
     * NULL when the verb takes no keyword.
     */
    const char      *subcommands;
} DaliCliCommandSpec;

/* True when word is one of spec->subcommands. False for a NULL list. */
bool dali_cli_has_subcommand(const DaliCliCommandSpec *spec, const char *word);

uint8_t                   dali_cli_command_count(void);
const DaliCliCommandSpec *dali_cli_command_at(uint8_t index);
const DaliCliCommandSpec *dali_cli_command_find(const char *name);
/* Table lookup by id; NULL only if the table and the enum disagree. */
const DaliCliCommandSpec *dali_cli_command_for_id(DaliCliCommandId id);

void dali_cli_print_help(const DaliCliOut *out);
void dali_cli_print_usage(const DaliCliOut *out, const DaliCliCommandSpec *spec);

typedef enum {
    DALI_CLI_RESOLVE_OK = 0,
    DALI_CLI_RESOLVE_EMPTY,      /* blank line; produce no output             */
    DALI_CLI_RESOLVE_UNKNOWN,    /* verb is not in the table                  */
    DALI_CLI_RESOLVE_ARITY,      /* verb found, wrong number of arguments     */
    DALI_CLI_RESOLVE_MALFORMED,  /* token or line longer than the CLI accepts */
} DaliCliResolveResult;

/*
 * Tokenize, look the verb up, and check the argument count. On DALI_CLI_RESOLVE_OK
 * both tokens and *spec_out are filled; on DALI_CLI_RESOLVE_ARITY tokens and
 * *spec_out are filled so the caller can print the verb's usage.
 */
DaliCliResolveResult dali_cli_resolve(const char                *line,
                                      DaliCliTokens             *tokens,
                                      const DaliCliCommandSpec **spec_out);

/*
 * The same resolution against a caller-supplied verb table.
 *
 * The ESPHome console reaches the same bus through the same protocol stack but
 * offers a deliberately smaller surface: it has no terminal to print a scan or
 * a capture into, and its result is a single Home Assistant text state. It
 * therefore brings its own table of the verbs it actually implements, and gets
 * this module's tokenizing, arity checking, argument parsing, and named command
 * tables for free — which is what keeps one spelling of a command name and one
 * definition of what counts as a well-formed line across both front ends.
 */
DaliCliResolveResult dali_cli_resolve_in(const DaliCliCommandSpec  *table,
                                         uint8_t                    count,
                                         const char                *line,
                                         DaliCliTokens             *tokens,
                                         const DaliCliCommandSpec **spec_out);

const DaliCliCommandSpec *dali_cli_command_find_in(const DaliCliCommandSpec *table,
                                                   uint8_t                   count,
                                                   const char               *name);

/* Print the standard message for a resolve outcome. DALI_CLI_RESOLVE_OK and
 * DALI_CLI_RESOLVE_EMPTY print nothing. */
void dali_cli_report_resolve(const DaliCliOut          *out,
                             DaliCliResolveResult       result,
                             const DaliCliTokens       *tokens,
                             const DaliCliCommandSpec  *spec);

/* ---------------------------------------------------------------------------
 * Argument parsing
 *
 * All of these reject trailing characters. An unsigned value may be written in
 * decimal or with a 0x prefix; a leading zero is never read as octal.
 * --------------------------------------------------------------------------*/

bool dali_cli_parse_u32(const char *text, uint32_t max, uint32_t *out);
bool dali_cli_parse_u8(const char *text, unsigned max, uint8_t *out);
bool dali_cli_parse_target(const char *text, DaliTarget *out);
bool dali_cli_parse_short_addr(const char *text, uint8_t *out);
bool dali_cli_parse_instance(const char *text, uint8_t *out);

typedef struct {
    uint8_t level;
    bool    is_mask;
} DaliCliLevel;

/* Accepts 0..254, or "mask"/"255" for the MASK arc power value. */
bool dali_cli_parse_level(const char *text, DaliCliLevel *out);

/* Parse the "len=<bits>" token used by raw/raw2. */
bool dali_cli_parse_len_token(const char *text, uint8_t *bits_out);

/*
 * Parse a raw frame from its hex and "len=<bits>" tokens. The value must fit in
 * the stated width, which is how a mistyped length is caught rather than
 * transmitted as a differently framed command.
 */
bool dali_cli_parse_raw_frame(const char *hex_text,
                              const char *len_text,
                              DaliFrame  *out);

/* ---------------------------------------------------------------------------
 * Named control-gear command tables (query / special / config)
 * --------------------------------------------------------------------------*/

typedef struct {
    const char   *name;
    DaliCommandId id;
    bool          needs_param;
    uint8_t       max_param;
    bool          uses_dtr0;   /* config table only */
} DaliCliGearCommand;

uint8_t                   dali_cli_query_count(void);
const DaliCliGearCommand *dali_cli_query_at(uint8_t index);
const DaliCliGearCommand *dali_cli_query_find(const char *name);

uint8_t                   dali_cli_special_count(void);
const DaliCliGearCommand *dali_cli_special_at(uint8_t index);
const DaliCliGearCommand *dali_cli_special_find(const char *name);

/*
 * True for the special commands a front end without a guarded commissioning
 * workflow must refuse.
 *
 * Two groups qualify. The addressing commands — INITIALISE, RANDOMISE, the
 * SEARCH ADDRESS registers, PROGRAM SHORT ADDRESS, WITHDRAW — are steps of a
 * commissioning walk: sent alone they are at best inert and at worst destroy
 * the addressing of every device on the bus, and RANDOMISE cannot be undone.
 * WRITE MEMORY LOCATION is the other: it writes wherever DTR1/DTR0 happen to
 * point, which is a different location for every device whose DTRs another
 * master has moved since.
 *
 * TERMINATE is deliberately not in this set. It is what ends an initialise
 * window a different tool opened, so refusing it would leave the operator
 * holding the problem and none of the remedy.
 */
bool dali_cli_special_is_commissioning(DaliCommandId id);

uint8_t                   dali_cli_config_count(void);
const DaliCliGearCommand *dali_cli_config_at(uint8_t index);
const DaliCliGearCommand *dali_cli_config_find(const char *name);

/*
 * True for the config commands that carry the same risk as the commissioning
 * specials, and must be gated the same way.
 *
 * SET SHORT ADDRESS (DTR0) is the whole set. It is spelled as an ordinary
 * addressed configuration command, but what it configures is which address the
 * gear answers to, and a broadcast target de-addresses an entire installation
 * from one line. Gating `special program-short` while leaving this open refuses
 * the harder spelling of the operation and permits the easier one.
 *
 * The other DTR0 names change how a device behaves at an address it keeps, so
 * they stay outside the set: a wrong fade time is visible and reversible, while
 * a lost short address takes a commissioning walk to recover.
 */
bool dali_cli_config_is_commissioning(DaliCommandId id);

/*
 * True for the config commands a broadcast target must not carry.
 *
 * ADD TO GROUP and REMOVE FROM GROUP qualify because the runtime group-query
 * cache cannot represent the result: an integration that tracks which short
 * address to poll for a group's state has no way to record "every device on the
 * bus" as a membership change, and the undo is not symmetric — a broadcast
 * REMOVE empties the group, including the members that were there first.
 *
 * Refused in the shared layer rather than per surface, so that the console and
 * the shell cannot disagree about what the verb does. `raw2` remains the way to
 * send the frame deliberately.
 */
bool dali_cli_config_rejects_broadcast(DaliCommandId id);

/* The one wording for that refusal, so both front ends report it identically. */
#define DALI_CLI_MSG_NO_BROADCAST_GROUP  "no broadcast group config"

/* ---------------------------------------------------------------------------
 * Device-type (DT6/DT8) command tables
 * --------------------------------------------------------------------------*/

typedef DaliFrame (*DaliCliDtFrameFn)(uint8_t addr);

typedef enum {
    DALI_CLI_DT_ACTION = 0,  /* one transmission, no reply                  */
    DALI_CLI_DT_CONFIG,      /* send twice, no reply                        */
    DALI_CLI_DT_QUERY,       /* one transmission, 8-bit reply               */
    DALI_CLI_DT_COLOUR16,    /* DT8 16-bit colour value read; takes a selector */
} DaliCliDtKind;

typedef struct {
    const char      *name;
    DaliCliDtFrameFn build;         /* NULL for DALI_CLI_DT_COLOUR16 */
    DaliCliDtKind    kind;
    uint8_t          dtr_count;     /* DTR bytes the caller must supply, 0..3 */
    const char      *dtr_help;      /* what those bytes mean; NULL when none  */
    DaliResponseKind response_kind; /* how to format the reply                */
} DaliCliDtCommand;

uint8_t                 dali_cli_dt6_count(void);
const DaliCliDtCommand *dali_cli_dt6_at(uint8_t index);
const DaliCliDtCommand *dali_cli_dt6_find(const char *name);

uint8_t                 dali_cli_dt8_count(void);
const DaliCliDtCommand *dali_cli_dt8_at(uint8_t index);
const DaliCliDtCommand *dali_cli_dt8_find(const char *name);

/* DT8 QueryColourValue selector names, for the COLOUR16 argument. */
typedef struct {
    const char                *name;
    DaliDt8ColourValueSelector selector;
} DaliCliDt8Selector;

uint8_t                   dali_cli_dt8_selector_count(void);
const DaliCliDt8Selector *dali_cli_dt8_selector_at(uint8_t index);
const DaliCliDt8Selector *dali_cli_dt8_selector_find(const char *name);

/* ---------------------------------------------------------------------------
 * Part 103 input-device instance tables
 * --------------------------------------------------------------------------*/

/*
 * Returns the query frame, or a zero-length frame for arguments the underlying
 * builder rejects. A caller must refuse to send a zero-length frame.
 */
typedef DaliFrame (*DaliCliInstanceQueryFn)(uint8_t addr, uint8_t instance);

typedef struct {
    const char            *name;
    DaliCliInstanceQueryFn build;
    /* true when DTR0 selects which value the query returns, so the CLI must
     * write DTR0 in the same sequence as the query itself. */
    bool                   needs_dtr0;
    const char            *dtr0_help;
    DaliResponseKind       response_kind;
} DaliCliInstanceQuery;

uint8_t                     dali_cli_iquery_count(void);
const DaliCliInstanceQuery *dali_cli_iquery_at(uint8_t index);
const DaliCliInstanceQuery *dali_cli_iquery_find(const char *name);

typedef DaliFrame (*DaliCliInstanceConfigFn)(uint8_t addr, uint8_t instance);

/*
 * Accepted values for one DTR byte, taken from the applicable Part 103/3xx
 * command definition. These are machine-checked rather than left to the prose
 * in dtr_help: an out-of-range configuration byte is accepted and stored by
 * some gear, so the mistake shows up later as a sensor that behaves oddly
 * rather than as a rejected command.
 *
 * Some commands accept a sentinel outside their ordinary range — 0 to disable a
 * timer, or 255 (MASK) to clear a group — which is what the flags express.
 */
#define DALI_CLI_DTR_ALLOW_ZERO 0x01u  /* 0 is valid even when below min */
#define DALI_CLI_DTR_ALLOW_MASK 0x02u  /* 255 (MASK) is valid even when above max */

typedef struct {
    uint8_t min;
    uint8_t max;
    uint8_t flags;
} DaliCliDtrRange;

bool dali_cli_dtr_value_valid(const DaliCliDtrRange *range, uint8_t value);

typedef struct {
    const char             *name;
    DaliCliInstanceConfigFn build;
    bool                    send_twice;
    uint8_t                 dtr_count;  /* DTR bytes the caller must supply */
    const char             *dtr_help;
    /* Instance type this command belongs to: 0 = generic Part 103, otherwise
     * the Part 301/303/304 instance type it is only valid for. */
    uint8_t                 instance_type;
    /* Accepted range per DTR byte. Only the first dtr_count entries are
     * meaningful; the rest are never read. */
    DaliCliDtrRange         dtr_range[DALI_INPUT_CONFIG_MAX_DTR_BYTES];
} DaliCliInstanceConfig;

uint8_t                      dali_cli_iconfig_count(void);
const DaliCliInstanceConfig *dali_cli_iconfig_at(uint8_t index);
const DaliCliInstanceConfig *dali_cli_iconfig_find(const char *name);

/* ---------------------------------------------------------------------------
 * Vendor tables
 * --------------------------------------------------------------------------*/

typedef struct {
    const char           *name;
    DaliLunatoneCommandId id;
} DaliCliLunatoneCommand;

uint8_t                       dali_cli_lunatone_count(void);
const DaliCliLunatoneCommand *dali_cli_lunatone_at(uint8_t index);
const DaliCliLunatoneCommand *dali_cli_lunatone_find(const char *name);

/* ---------------------------------------------------------------------------
 * Named-table listing
 * --------------------------------------------------------------------------*/

typedef enum {
    DALI_CLI_TABLE_QUERY = 0,
    DALI_CLI_TABLE_SPECIAL,
    DALI_CLI_TABLE_CONFIG,
    DALI_CLI_TABLE_DT6,
    DALI_CLI_TABLE_DT8,
    DALI_CLI_TABLE_SELECTORS,
    DALI_CLI_TABLE_IQUERY,
    DALI_CLI_TABLE_ICONFIG,
    DALI_CLI_TABLE_VENDOR,
    DALI_CLI_TABLE_COUNT,
} DaliCliTableId;

const char *dali_cli_table_name(DaliCliTableId table);
bool        dali_cli_table_find(const char *name, DaliCliTableId *out);
void        dali_cli_print_table(const DaliCliOut *out, DaliCliTableId table);
/* Print the accepted table names, for a bad `list` argument. */
void        dali_cli_print_table_names(const DaliCliOut *out);

/* ---------------------------------------------------------------------------
 * Response formatting
 * --------------------------------------------------------------------------*/

void dali_cli_print_frame(const DaliCliOut *out,
                          const char       *prefix,
                          const DaliFrame  *frame);

void dali_cli_print_status_fields(const DaliCliOut *out, uint8_t raw);

/* Longest line either formatter below produces, including the NUL. */
#define DALI_CLI_STATUS_LINE_MAX   96u
#define DALI_CLI_RESPONSE_LINE_MAX 128u

/*
 * Format a QUERY STATUS byte as the raw value followed by the flags that are
 * set: "0x06 lamp-fail,arc-on", or "0x00 none" when the gear reports nothing.
 *
 * This is the same decode dali_cli_print_status_fields() prints as a block,
 * for a caller whose whole answer has to be one line.
 *
 * Returns the length written, excluding the NUL; 0 when the byte does not
 * decode or the buffer is unusable, in which case buf is left empty.
 */
size_t dali_cli_format_status(char *buf, size_t cap, uint8_t raw);

/*
 * Format one command reply under `name` as a single line, with no newline.
 *
 * The ESPHome console's entire answer is one Home Assistant text state: it has
 * no terminal to page a block into, and no room for a second line. Both front
 * ends decode a reply here so that a yes/no answer cannot be "yes" on one
 * surface and "255" on the other — dali_cli_print_response() is this function
 * plus the newline, and plus the per-field block for a status byte.
 *
 * A frame that does not decode as the expected kind formats as
 * "<name>: malformed reply" rather than as a plausible number.
 *
 * Returns the length written, excluding the NUL. A buffer of
 * DALI_CLI_RESPONSE_LINE_MAX holds every kind without truncation.
 */
size_t dali_cli_format_response(char             *buf,
                                size_t            cap,
                                const char       *name,
                                DaliResponseKind  kind,
                                const DaliFrame  *reply);

/*
 * Print one command reply under `name`. A frame that does not decode as the
 * expected kind prints "malformed reply" rather than a plausible number.
 */
void dali_cli_print_response(const DaliCliOut *out,
                             const char       *name,
                             DaliResponseKind  kind,
                             const DaliFrame  *reply);

/*
 * Print the outcome of a command that produced no reply, and of one whose reply
 * never arrived. These keep "OK", "timeout", and "ERR n" spelled the same way
 * across every verb.
 */
void dali_cli_print_tx_result(const DaliCliOut *out, const char *name, DaliError err);
void dali_cli_print_error(const DaliCliOut *out, const char *name, DaliError err);

/* Print a byte block as "bank:offset  hex hex ...". */
void dali_cli_print_memory_block(const DaliCliOut *out,
                                 uint8_t           bank,
                                 uint8_t           offset,
                                 const uint8_t    *data,
                                 uint8_t           count);
