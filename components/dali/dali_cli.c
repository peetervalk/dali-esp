#include "dali_cli.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Output sink
 * --------------------------------------------------------------------------*/

void dali_cli_write(const DaliCliOut *out, const char *text)
{
    if (out == NULL || out->write == NULL || text == NULL) {
        return;
    }
    out->write(out->ctx, text);
}

void dali_cli_printf(const DaliCliOut *out, const char *fmt, ...)
{
    if (out == NULL || out->write == NULL || fmt == NULL) {
        return;
    }

    char buf[DALI_CLI_FORMAT_MAX];
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (written < 0) {
        return;
    }
    out->write(out->ctx, buf);
}

static void dali_cli_buffer_write(void *ctx, const char *text)
{
    DaliCliBufferSink *sink = (DaliCliBufferSink *)ctx;
    if (sink == NULL || sink->buf == NULL || sink->cap == 0u || text == NULL) {
        return;
    }

    size_t room = sink->cap - 1u - sink->len;
    size_t len  = strlen(text);
    if (len > room) {
        len = room;
        sink->truncated = true;
    }
    memcpy(sink->buf + sink->len, text, len);
    sink->len += len;
    sink->buf[sink->len] = '\0';
}

void dali_cli_buffer_sink_init(DaliCliBufferSink *sink, char *buf, size_t cap)
{
    if (sink == NULL) {
        return;
    }
    sink->buf       = buf;
    sink->cap       = cap;
    sink->len       = 0u;
    sink->truncated = false;
    if (buf != NULL && cap > 0u) {
        buf[0] = '\0';
    }
}

DaliCliOut dali_cli_buffer_out(DaliCliBufferSink *sink)
{
    return (DaliCliOut){ .write = dali_cli_buffer_write, .ctx = sink };
}

/* ---------------------------------------------------------------------------
 * Tokenising
 * --------------------------------------------------------------------------*/

static bool cli_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

DaliCliTokenizeResult dali_cli_tokenize(const char *line, DaliCliTokens *out)
{
    if (out == NULL) {
        return DALI_CLI_TOKENIZE_EMPTY;
    }

    *out = (DaliCliTokens){0};
    if (line == NULL) {
        return DALI_CLI_TOKENIZE_EMPTY;
    }

    size_t pos = 0u;
    for (;;) {
        while (line[pos] != '\0' && cli_is_space(line[pos])) {
            pos++;
        }
        if (line[pos] == '\0') {
            break;
        }
        if (out->count >= DALI_CLI_MAX_TOKENS) {
            return DALI_CLI_TOKENIZE_TOO_MANY;
        }

        size_t start = pos;
        while (line[pos] != '\0' && !cli_is_space(line[pos])) {
            pos++;
        }
        size_t len = pos - start;
        if (len >= DALI_CLI_MAX_TOKEN_LEN) {
            return DALI_CLI_TOKENIZE_TOO_LONG;
        }

        memcpy(out->tok[out->count], line + start, len);
        out->tok[out->count][len] = '\0';
        out->count++;
    }

    return out->count == 0u ? DALI_CLI_TOKENIZE_EMPTY : DALI_CLI_TOKENIZE_OK;
}

/* ---------------------------------------------------------------------------
 * Verb table
 *
 * min_args/max_args count the arguments after the verb. A verb that dispatches
 * on a subcommand still declares the widest form here; the handler reports the
 * specific shape it needs.
 * --------------------------------------------------------------------------*/

#define TARGET_ARG DALI_CLI_TARGET_ARG

static const DaliCliCommandSpec s_commands[] = {
    { DALI_CLI_CMD_HELP, "help", "", "print this command summary", 0u, 0u, NULL },
    { DALI_CLI_CMD_STATS, "stats", "", "PHY, RX, and scheduler queue counters", 0u, 0u, NULL },
    { DALI_CLI_CMD_QUEUE, "queue", "[reset]", "scheduler queue admission diagnostics", 0u, 1u, "reset" },
    { DALI_CLI_CMD_BUS, "bus", "check", "RX level, scheduler state, and fault counters", 1u, 1u, "check" },
    { DALI_CLI_CMD_CAPTURE, "capture", "start|stop|clear|status|export", "rolling frame/event capture", 1u, 1u,
      "start stop clear status export" },
    { DALI_CLI_CMD_TRACE, "trace", "on|off", "per-frame trace logging", 1u, 1u, "on off" },
    { DALI_CLI_CMD_READ, "read", "", "print the last received frame", 0u, 0u, NULL },
    { DALI_CLI_CMD_RXDEBUG, "rxdebug", "", "last malformed RX timing snapshot", 0u, 0u, NULL },
    { DALI_CLI_CMD_RESET, "reset", "", "reset PHY, scheduler, and diagnostic state", 0u, 0u, NULL },

    { DALI_CLI_CMD_LIST, "list", "<table>", "print a named command table", 1u, 1u, NULL },
    { DALI_CLI_CMD_SCHEMA, "schema", "", "print every command table as JSON", 0u, 0u, NULL },
    { DALI_CLI_CMD_QUERY_LIST, "query-list", "", "alias for 'list query'", 0u, 0u, NULL },
    { DALI_CLI_CMD_SPECIAL_LIST, "special-list", "", "alias for 'list special'", 0u, 0u, NULL },
    { DALI_CLI_CMD_CONFIG_LIST, "config-list", "", "alias for 'list config'", 0u, 0u, NULL },

    { DALI_CLI_CMD_RAW, "raw", "<hex> len=<bits> [wait]", "send one arbitrary frame", 2u, 3u, NULL },
    { DALI_CLI_CMD_RAW2, "raw2", "<hex> len=<bits>", "send one frame twice within the 100 ms window", 2u, 2u, NULL },
    { DALI_CLI_CMD_DTR, "dtr", "<0|1|2> <0-255>", "load a control-gear DTR register", 2u, 2u, NULL },

    { DALI_CLI_CMD_LEVEL, "level", TARGET_ARG " <0-254|mask>", "direct arc power control", 2u, 2u, NULL },
    { DALI_CLI_CMD_MASK, "mask", TARGET_ARG, "arc power MASK: leave the level unchanged", 1u, 1u, NULL },
    { DALI_CLI_CMD_OFF, "off", TARGET_ARG, "OFF", 1u, 1u, NULL },
    { DALI_CLI_CMD_UP, "up", TARGET_ARG, "UP: one fade step up", 1u, 1u, NULL },
    { DALI_CLI_CMD_DOWN, "down", TARGET_ARG, "DOWN: one fade step down", 1u, 1u, NULL },
    { DALI_CLI_CMD_STEP_UP, "step-up", TARGET_ARG, "STEP UP", 1u, 1u, NULL },
    { DALI_CLI_CMD_STEP_DOWN, "step-down", TARGET_ARG, "STEP DOWN", 1u, 1u, NULL },
    { DALI_CLI_CMD_STEP_OFF, "step-off", TARGET_ARG, "STEP DOWN AND OFF", 1u, 1u, NULL },
    { DALI_CLI_CMD_ON_STEP, "on-step", TARGET_ARG, "ON AND STEP UP", 1u, 1u, NULL },
    { DALI_CLI_CMD_CONT_UP, "cont-up", TARGET_ARG, "CONTINUOUS UP: fade toward max", 1u, 1u, NULL },
    { DALI_CLI_CMD_CONT_DOWN, "cont-down", TARGET_ARG, "CONTINUOUS DOWN: fade toward min", 1u, 1u, NULL },
    { DALI_CLI_CMD_DAPC_SEQ, "dapc-seq", TARGET_ARG, "ENABLE DAPC SEQUENCE", 1u, 1u, NULL },
    { DALI_CLI_CMD_LAST, "last", TARGET_ARG, "GO TO LAST ACTIVE LEVEL", 1u, 1u, NULL },
    { DALI_CLI_CMD_SCENE, "scene", TARGET_ARG " <0-15>", "GO TO SCENE", 2u, 2u, NULL },
    { DALI_CLI_CMD_MAX, "max", TARGET_ARG, "RECALL MAX LEVEL", 1u, 1u, NULL },
    { DALI_CLI_CMD_MIN, "min", TARGET_ARG, "RECALL MIN LEVEL", 1u, 1u, NULL },
    { DALI_CLI_CMD_STATUS, "status", TARGET_ARG, "QUERY STATUS with decoded fields", 1u, 1u, NULL },

    { DALI_CLI_CMD_QUERY, "query", TARGET_ARG " [query-name] [param]", "addressed control-gear query", 1u, 3u, NULL },
    { DALI_CLI_CMD_SPECIAL, "special", "<name> [param]", "special/broadcast command", 1u, 2u, NULL },
    { DALI_CLI_CMD_CONFIG, "config", TARGET_ARG " <config-name> [param]", "addressed configuration command", 2u, 3u, NULL },
    { DALI_CLI_CMD_CONFIG_DTR0, "config-dtr0", TARGET_ARG " <config-name> <dtr0> [param]", "load DTR0 and configure atomically", 3u, 4u, NULL },

    { DALI_CLI_CMD_MEMREAD, "memread", "<addr> <bank> <offset> [count]", "control-gear memory read (Part 102)", 3u, 4u, NULL },
    { DALI_CLI_CMD_MEMINFO, "meminfo", "<addr>", "control-gear Bank 0 identity", 1u, 1u, NULL },
    { DALI_CLI_CMD_DEVMEM, "devmem", "read|write <addr> <bank> <offset> [count|value]", "control-device memory (Part 103)", 4u, 5u,
      "read write" },
    { DALI_CLI_CMD_DTRCHECK, "dtrcheck", "<addr> <0|1|2> <0-255>", "load a control-device DTR and read it back", 3u, 3u, NULL },

    { DALI_CLI_CMD_DT6, "dt6", "<addr> <name> [dtr0]", "device type 6 (LED) command", 2u, 3u, NULL },
    { DALI_CLI_CMD_DT8, "dt8", "<addr> <name> [v0] [v1] [v2]", "device type 8 (colour) command", 2u, 5u, NULL },

    { DALI_CLI_CMD_IQUERY, "iquery", "<addr> <instance> <name> [dtr0]", "Part 103 instance query", 3u, 4u, NULL },
    { DALI_CLI_CMD_ICONFIG, "iconfig", "<addr> <instance> <name> [v0] [v1] [v2]", "Part 103 instance configuration", 3u, 6u, NULL },
    { DALI_CLI_CMD_VENDOR, "vendor", "lunatone <addr> <instance> <name> | steinel <instance> <raw>", "vendor helpers", 3u, 4u,
      "lunatone steinel" },

    { DALI_CLI_CMD_SCAN, "scan", "", "scan short addresses, brief output", 0u, 0u, NULL },
    { DALI_CLI_CMD_DISCOVER, "discover", "", "scan short addresses, full output", 0u, 0u, NULL },
    { DALI_CLI_CMD_INVENTORY, "inventory", "", "print the last discovered inventory", 0u, 0u, NULL },
    { DALI_CLI_CMD_COMMISSION, "commission", "unaddressed [first-addr] [max-devices]", "assign short addresses", 1u, 3u,
      "unaddressed" },
    { DALI_CLI_CMD_INSTANCES, "instances", "<addr>", "input-device instance types", 1u, 1u, NULL },
    { DALI_CLI_CMD_SENSOR, "sensor", "poll <addr> [instance]", "read input values", 2u, 3u, "poll" },
    { DALI_CLI_CMD_SMOKE, "smoke", "<addr>", "short read/write/read-back check", 1u, 1u, NULL },
    { DALI_CLI_CMD_EVENTS, "events", "", "drain queued Part 103 events", 0u, 0u, NULL },
    { DALI_CLI_CMD_FIND, "find", "switches [seconds]", "map switches by listening for events", 1u, 2u, "switches" },
    { DALI_CLI_CMD_EXPORT, "export", "inventory|config", "inventory as JSON, or the dali: YAML block for this device", 1u, 1u,
      "inventory config" },
    { DALI_CLI_CMD_IDENTIFY, "identify", "<addr>", "blink one short-addressed lamp", 1u, 1u, NULL },
};

#define CLI_COMMAND_COUNT ((uint8_t)(sizeof(s_commands) / sizeof(s_commands[0])))

uint8_t dali_cli_command_count(void)
{
    return CLI_COMMAND_COUNT;
}

const DaliCliCommandSpec *dali_cli_command_at(uint8_t index)
{
    return index < CLI_COMMAND_COUNT ? &s_commands[index] : NULL;
}

const DaliCliCommandSpec *dali_cli_command_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (uint8_t i = 0u; i < CLI_COMMAND_COUNT; i++) {
        if (strcmp(name, s_commands[i].name) == 0) {
            return &s_commands[i];
        }
    }
    return NULL;
}

bool dali_cli_has_subcommand(const DaliCliCommandSpec *spec, const char *word)
{
    if (spec == NULL || spec->subcommands == NULL || word == NULL || word[0] == '\0') {
        return false;
    }

    size_t len = strlen(word);
    const char *pos = spec->subcommands;
    while (*pos != '\0') {
        while (*pos == ' ') {
            pos++;
        }
        const char *start = pos;
        while (*pos != '\0' && *pos != ' ') {
            pos++;
        }
        size_t entry_len = (size_t)(pos - start);
        if (entry_len == len && strncmp(start, word, len) == 0) {
            return true;
        }
    }
    return false;
}

const DaliCliCommandSpec *dali_cli_command_for_id(DaliCliCommandId id)
{
    for (uint8_t i = 0u; i < CLI_COMMAND_COUNT; i++) {
        if (s_commands[i].id == id) {
            return &s_commands[i];
        }
    }
    return NULL;
}

void dali_cli_print_usage(const DaliCliOut *out, const DaliCliCommandSpec *spec)
{
    if (spec == NULL) {
        return;
    }
    if (spec->args[0] == '\0') {
        dali_cli_printf(out, "usage: %s\r\n", spec->name);
    } else {
        dali_cli_printf(out, "usage: %s %s\r\n", spec->name, spec->args);
    }
}

void dali_cli_print_help(const DaliCliOut *out)
{
    dali_cli_write(out, "commands:\r\n");
    for (uint8_t i = 0u; i < CLI_COMMAND_COUNT; i++) {
        const DaliCliCommandSpec *spec = &s_commands[i];
        if (spec->args[0] == '\0') {
            dali_cli_printf(out, "  %-12s          - %s\r\n", spec->name, spec->summary);
        } else {
            dali_cli_printf(out, "  %s %s\r\n", spec->name, spec->args);
            dali_cli_printf(out, "      %s\r\n", spec->summary);
        }
    }
}

const DaliCliCommandSpec *dali_cli_command_find_in(const DaliCliCommandSpec *table,
                                                   uint8_t                   count,
                                                   const char               *name)
{
    if (table == NULL || name == NULL) {
        return NULL;
    }
    for (uint8_t i = 0u; i < count; i++) {
        if (strcmp(name, table[i].name) == 0) {
            return &table[i];
        }
    }
    return NULL;
}

DaliCliResolveResult dali_cli_resolve_in(const DaliCliCommandSpec  *table,
                                         uint8_t                    count,
                                         const char                *line,
                                         DaliCliTokens             *tokens,
                                         const DaliCliCommandSpec **spec_out)
{
    if (tokens == NULL || spec_out == NULL) {
        return DALI_CLI_RESOLVE_MALFORMED;
    }
    *spec_out = NULL;

    switch (dali_cli_tokenize(line, tokens)) {
        case DALI_CLI_TOKENIZE_EMPTY:
            return DALI_CLI_RESOLVE_EMPTY;
        case DALI_CLI_TOKENIZE_TOO_MANY:
        case DALI_CLI_TOKENIZE_TOO_LONG:
            return DALI_CLI_RESOLVE_MALFORMED;
        case DALI_CLI_TOKENIZE_OK:
            break;
    }

    const DaliCliCommandSpec *spec = dali_cli_command_find_in(table, count, tokens->tok[0]);
    if (spec == NULL) {
        return DALI_CLI_RESOLVE_UNKNOWN;
    }
    *spec_out = spec;

    /* Both bounds, so a trailing token is rejected rather than ignored. */
    uint8_t argc = (uint8_t)(tokens->count - 1u);
    if (argc < spec->min_args || argc > spec->max_args) {
        return DALI_CLI_RESOLVE_ARITY;
    }
    return DALI_CLI_RESOLVE_OK;
}

DaliCliResolveResult dali_cli_resolve(const char                *line,
                                      DaliCliTokens             *tokens,
                                      const DaliCliCommandSpec **spec_out)
{
    return dali_cli_resolve_in(s_commands, CLI_COMMAND_COUNT, line, tokens, spec_out);
}

void dali_cli_report_resolve(const DaliCliOut         *out,
                             DaliCliResolveResult      result,
                             const DaliCliTokens      *tokens,
                             const DaliCliCommandSpec *spec)
{
    switch (result) {
        case DALI_CLI_RESOLVE_OK:
        case DALI_CLI_RESOLVE_EMPTY:
            break;

        case DALI_CLI_RESOLVE_UNKNOWN:
            dali_cli_printf(out, "unknown command: %s\r\n",
                            tokens != NULL ? tokens->tok[0] : "");
            dali_cli_write(out, "type 'help' for commands\r\n");
            break;

        case DALI_CLI_RESOLVE_ARITY:
            dali_cli_print_usage(out, spec);
            break;

        case DALI_CLI_RESOLVE_MALFORMED:
            dali_cli_write(out, "command line too long\r\n");
            break;
    }
}

/* ---------------------------------------------------------------------------
 * Argument parsing
 * --------------------------------------------------------------------------*/

static bool cli_digit_value(char c, unsigned base, unsigned *out)
{
    unsigned value;
    if (c >= '0' && c <= '9') {
        value = (unsigned)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
        value = (unsigned)(c - 'a') + 10u;
    } else if (c >= 'A' && c <= 'F') {
        value = (unsigned)(c - 'A') + 10u;
    } else {
        return false;
    }
    if (value >= base) {
        return false;
    }
    *out = value;
    return true;
}

bool dali_cli_parse_u32(const char *text, uint32_t max, uint32_t *out)
{
    if (text == NULL || out == NULL) {
        return false;
    }

    unsigned base = 10u;
    /* Only an explicit 0x prefix selects hex. A leading zero stays decimal, so
     * "010" is ten rather than eight. */
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16u;
        text += 2;
    }
    if (text[0] == '\0') {
        return false;
    }

    uint32_t value = 0u;
    for (size_t i = 0u; text[i] != '\0'; i++) {
        unsigned digit;
        if (!cli_digit_value(text[i], base, &digit)) {
            return false;
        }
        if (value > (UINT32_MAX - digit) / base) {
            return false;
        }
        value = value * base + digit;
        if (value > max) {
            return false;
        }
    }

    *out = value;
    return true;
}

bool dali_cli_parse_u8(const char *text, unsigned max, uint8_t *out)
{
    if (out == NULL || max > 0xFFu) {
        return false;
    }
    uint32_t value;
    if (!dali_cli_parse_u32(text, max, &value)) {
        return false;
    }
    *out = (uint8_t)value;
    return true;
}

bool dali_cli_parse_target(const char *text, DaliTarget *out)
{
    if (text == NULL || out == NULL) {
        return false;
    }

    if (strcmp(text, "b") == 0 || strcmp(text, "broadcast") == 0) {
        *out = (DaliTarget){ .type = DALI_ADDR_BROADCAST, .address = 0u };
        return true;
    }

    if (text[0] == 'g') {
        uint8_t group;
        if (!dali_cli_parse_u8(text + 1, DALI_MAX_GROUP, &group)) {
            return false;
        }
        *out = (DaliTarget){ .type = DALI_ADDR_GROUP, .address = group };
        return true;
    }

    const char *addr_text = text[0] == 's' ? text + 1 : text;
    uint8_t addr;
    if (!dali_cli_parse_u8(addr_text, DALI_MAX_SHORT_ADDRESS, &addr)) {
        return false;
    }
    *out = (DaliTarget){ .type = DALI_ADDR_SHORT, .address = addr };
    return true;
}

bool dali_cli_parse_short_addr(const char *text, uint8_t *out)
{
    if (text == NULL) {
        return false;
    }
    /* Accept the sN spelling here too, so a target and an address argument are
     * never written differently for the same device. */
    return dali_cli_parse_u8(text[0] == 's' ? text + 1 : text,
                             DALI_MAX_SHORT_ADDRESS,
                             out);
}

bool dali_cli_parse_instance(const char *text, uint8_t *out)
{
    return dali_cli_parse_u8(text, DALI_MAX_INSTANCE, out);
}

bool dali_cli_parse_level(const char *text, DaliCliLevel *out)
{
    if (text == NULL || out == NULL) {
        return false;
    }

    if (strcmp(text, "mask") == 0) {
        *out = (DaliCliLevel){ .level = DALI_DAPC_MASK_LEVEL, .is_mask = true };
        return true;
    }

    uint8_t level;
    if (!dali_cli_parse_u8(text, DALI_DAPC_MASK_LEVEL, &level)) {
        return false;
    }
    *out = (DaliCliLevel){
        .level   = level,
        .is_mask = level == DALI_DAPC_MASK_LEVEL,
    };
    return true;
}

bool dali_cli_parse_len_token(const char *text, uint8_t *bits_out)
{
    if (text == NULL || bits_out == NULL) {
        return false;
    }
    if (strncmp(text, "len=", 4) != 0) {
        return false;
    }

    uint8_t bits;
    if (!dali_cli_parse_u8(text + 4, DALI_MAX_FRAME_BITS, &bits) || bits == 0u) {
        return false;
    }
    *bits_out = bits;
    return true;
}

bool dali_cli_parse_raw_frame(const char *hex_text,
                              const char *len_text,
                              DaliFrame  *out)
{
    if (out == NULL) {
        return false;
    }

    uint8_t bits;
    if (!dali_cli_parse_len_token(len_text, &bits)) {
        return false;
    }

    /* The hex value is read without a 0x prefix, so allow both spellings and
     * bound it by the stated width rather than by 32 bits. */
    const char *digits = hex_text;
    if (digits != NULL && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        digits += 2;
    }
    if (digits == NULL || digits[0] == '\0') {
        return false;
    }

    uint32_t value = 0u;
    for (size_t i = 0u; digits[i] != '\0'; i++) {
        unsigned digit;
        if (!cli_digit_value(digits[i], 16u, &digit)) {
            return false;
        }
        if (value > (UINT32_MAX - digit) / 16u) {
            return false;
        }
        value = value * 16u + digit;
    }

    uint32_t limit = bits >= 32u ? UINT32_MAX : ((1UL << bits) - 1UL);
    if (value > limit) {
        return false;
    }

    *out = (DaliFrame){ .data = value, .bit_length = bits };
    return true;
}

/* ---------------------------------------------------------------------------
 * Named control-gear command tables
 * --------------------------------------------------------------------------*/

static const DaliCliGearCommand s_query_commands[] = {
    { "status",            DALI_CMD_QUERY_STATUS,                     false, 0u, false },
    { "present",           DALI_CMD_QUERY_CONTROL_GEAR_PRESENT,       false, 0u, false },
    { "lamp-failure",      DALI_CMD_QUERY_LAMP_FAILURE,               false, 0u, false },
    { "lamp-on",           DALI_CMD_QUERY_LAMP_POWER_ON,              false, 0u, false },
    { "limit-error",       DALI_CMD_QUERY_LIMIT_ERROR,                false, 0u, false },
    { "reset-state",       DALI_CMD_QUERY_RESET_STATE,                false, 0u, false },
    { "missing-address",   DALI_CMD_QUERY_MISSING_SHORT_ADDRESS,      false, 0u, false },
    { "version",           DALI_CMD_QUERY_VERSION_NUMBER,             false, 0u, false },
    { "dtr0",              DALI_CMD_QUERY_CONTENT_DTR0,               false, 0u, false },
    { "device-type",       DALI_CMD_QUERY_DEVICE_TYPE,                false, 0u, false },
    { "physical-min",      DALI_CMD_QUERY_PHYSICAL_MINIMUM,           false, 0u, false },
    { "power-failure",     DALI_CMD_QUERY_POWER_FAILURE,              false, 0u, false },
    { "dtr1",              DALI_CMD_QUERY_CONTENT_DTR1,               false, 0u, false },
    { "dtr2",              DALI_CMD_QUERY_CONTENT_DTR2,               false, 0u, false },
    { "operating-mode",    DALI_CMD_QUERY_OPERATING_MODE,             false, 0u, false },
    { "light-source",      DALI_CMD_QUERY_LIGHT_SOURCE_TYPE,          false, 0u, false },
    { "actual",            DALI_CMD_QUERY_ACTUAL_LEVEL,               false, 0u, false },
    { "max-level",         DALI_CMD_QUERY_MAX_LEVEL,                  false, 0u, false },
    { "min-level",         DALI_CMD_QUERY_MIN_LEVEL,                  false, 0u, false },
    { "power-on",          DALI_CMD_QUERY_POWER_ON_LEVEL,             false, 0u, false },
    { "failure-level",     DALI_CMD_QUERY_SYSTEM_FAILURE_LEVEL,       false, 0u, false },
    { "fade",              DALI_CMD_QUERY_FADE_TIME_FADE_RATE,        false, 0u, false },
    { "manufacturer-mode", DALI_CMD_QUERY_MANUFACTURER_SPECIFIC_MODE, false, 0u, false },
    { "next-device-type",  DALI_CMD_QUERY_NEXT_DEVICE_TYPE,           false, 0u, false },
    { "extended-fade",     DALI_CMD_QUERY_EXTENDED_FADE_TIME,         false, 0u, false },
    { "gear-failure",      DALI_CMD_QUERY_CONTROL_GEAR_FAILURE,       false, 0u, false },
    { "scene-level",       DALI_CMD_QUERY_SCENE_LEVEL,                true,  DALI_MAX_SCENE, false },
    { "groups-0-7",        DALI_CMD_QUERY_GROUPS_0_7,                 false, 0u, false },
    { "groups-8-15",       DALI_CMD_QUERY_GROUPS_8_15,                false, 0u, false },
    { "random-h",          DALI_CMD_QUERY_RANDOM_ADDRESS_H,           false, 0u, false },
    { "random-m",          DALI_CMD_QUERY_RANDOM_ADDRESS_M,           false, 0u, false },
    { "random-l",          DALI_CMD_QUERY_RANDOM_ADDRESS_L,           false, 0u, false },
    { "memory",            DALI_CMD_READ_MEMORY_LOCATION,             false, 0u, false },
    { "extended-version",  DALI_CMD_QUERY_EXTENDED_VERSION_NUMBER,    false, 0u, false },
};

static const DaliCliGearCommand s_special_commands[] = {
    { "terminate",       DALI_CMD_TERMINATE,                      false, 0u,   false },
    { "dtr0",            DALI_CMD_DTR0_DATA,                      true,  255u, false },
    { "initialise",      DALI_CMD_INITIALISE,                     true,  255u, false },
    { "randomize",       DALI_CMD_RANDOMIZE,                      false, 0u,   false },
    { "compare",         DALI_CMD_COMPARE,                        false, 0u,   false },
    { "withdraw",        DALI_CMD_WITHDRAW,                       false, 0u,   false },
    { "ping",            DALI_CMD_PING,                           false, 0u,   false },
    { "search-h",        DALI_CMD_SEARCH_ADDRH,                   true,  255u, false },
    { "search-m",        DALI_CMD_SEARCH_ADDRM,                   true,  255u, false },
    { "search-l",        DALI_CMD_SEARCH_ADDRL,                   true,  255u, false },
    { "program-short",   DALI_CMD_PROGRAM_SHORT_ADDRESS,          true,  255u, false },
    { "verify-short",    DALI_CMD_VERIFY_SHORT_ADDRESS,           true,  255u, false },
    { "query-short",     DALI_CMD_QUERY_SHORT_ADDRESS,            false, 0u,   false },
    { "enable-type",     DALI_CMD_ENABLE_DEVICE_TYPE,             true,  255u, false },
    { "dtr1",            DALI_CMD_DTR1_DATA,                      true,  255u, false },
    { "dtr2",            DALI_CMD_DTR2_DATA,                      true,  255u, false },
    { "write-memory",    DALI_CMD_WRITE_MEMORY_LOCATION,          true,  255u, false },
    { "write-memory-nr", DALI_CMD_WRITE_MEMORY_LOCATION_NO_REPLY, true,  255u, false },
};

static const DaliCliGearCommand s_config_commands[] = {
    { "reset",                   DALI_CMD_RESET,                         false, 0u,             false },
    { "store-actual-dtr0",       DALI_CMD_STORE_ACTUAL_LEVEL_DTR0,       false, 0u,             false },
    { "save-persistent",         DALI_CMD_SAVE_PERSISTENT_VARIABLES,     false, 0u,             false },
    { "set-operating-mode-dtr0", DALI_CMD_SET_OPERATING_MODE_DTR0,       false, 0u,             true  },
    { "reset-memory-dtr0",       DALI_CMD_RESET_MEMORY_BANK_DTR0,        false, 0u,             true  },
    { "identify-device",         DALI_CMD_IDENTIFY_DEVICE,               false, 0u,             false },
    { "set-max-dtr0",            DALI_CMD_SET_MAX_LEVEL_DTR0,            false, 0u,             true  },
    { "set-min-dtr0",            DALI_CMD_SET_MIN_LEVEL_DTR0,            false, 0u,             true  },
    { "set-failure-dtr0",        DALI_CMD_SET_SYSTEM_FAILURE_LEVEL_DTR0, false, 0u,             true  },
    { "set-power-on-dtr0",       DALI_CMD_SET_POWER_ON_LEVEL_DTR0,       false, 0u,             true  },
    { "set-fade-time-dtr0",      DALI_CMD_SET_FADE_TIME_DTR0,            false, 0u,             true  },
    { "set-fade-rate-dtr0",      DALI_CMD_SET_FADE_RATE_DTR0,            false, 0u,             true  },
    { "set-extended-fade-dtr0",  DALI_CMD_SET_EXTENDED_FADE_TIME_DTR0,   false, 0u,             true  },
    { "set-scene",               DALI_CMD_SET_SCENE,                     true,  DALI_MAX_SCENE, true  },
    { "remove-scene",            DALI_CMD_REMOVE_FROM_SCENE,             true,  DALI_MAX_SCENE, false },
    { "add-group",               DALI_CMD_ADD_TO_GROUP,                  true,  DALI_MAX_GROUP, false },
    { "remove-group",            DALI_CMD_REMOVE_FROM_GROUP,             true,  DALI_MAX_GROUP, false },
    { "set-short-address-dtr0",  DALI_CMD_SET_SHORT_ADDRESS_DTR0,        false, 0u,             true  },
    { "enable-write-memory",     DALI_CMD_ENABLE_WRITE_MEMORY,           false, 0u,             false },
};

static const DaliCliGearCommand *gear_find(const DaliCliGearCommand *table,
                                           uint8_t                   count,
                                           const char               *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (uint8_t i = 0u; i < count; i++) {
        if (strcmp(name, table[i].name) == 0) {
            return &table[i];
        }
    }
    return NULL;
}

#define CLI_TABLE_ENTRIES(t) ((uint8_t)(sizeof(t) / sizeof((t)[0])))

uint8_t dali_cli_query_count(void) { return CLI_TABLE_ENTRIES(s_query_commands); }
const DaliCliGearCommand *dali_cli_query_at(uint8_t index)
{
    return index < dali_cli_query_count() ? &s_query_commands[index] : NULL;
}
const DaliCliGearCommand *dali_cli_query_find(const char *name)
{
    return gear_find(s_query_commands, dali_cli_query_count(), name);
}

uint8_t dali_cli_special_count(void) { return CLI_TABLE_ENTRIES(s_special_commands); }
const DaliCliGearCommand *dali_cli_special_at(uint8_t index)
{
    return index < dali_cli_special_count() ? &s_special_commands[index] : NULL;
}
const DaliCliGearCommand *dali_cli_special_find(const char *name)
{
    return gear_find(s_special_commands, dali_cli_special_count(), name);
}

bool dali_cli_special_is_commissioning(DaliCommandId id)
{
    switch (id) {
        case DALI_CMD_INITIALISE:
        case DALI_CMD_RANDOMIZE:
        case DALI_CMD_SEARCH_ADDRH:
        case DALI_CMD_SEARCH_ADDRM:
        case DALI_CMD_SEARCH_ADDRL:
        case DALI_CMD_PROGRAM_SHORT_ADDRESS:
        case DALI_CMD_WITHDRAW:
        case DALI_CMD_WRITE_MEMORY_LOCATION:
        case DALI_CMD_WRITE_MEMORY_LOCATION_NO_REPLY:
            return true;
        default:
            return false;
    }
}

uint8_t dali_cli_config_count(void) { return CLI_TABLE_ENTRIES(s_config_commands); }
const DaliCliGearCommand *dali_cli_config_at(uint8_t index)
{
    return index < dali_cli_config_count() ? &s_config_commands[index] : NULL;
}
const DaliCliGearCommand *dali_cli_config_find(const char *name)
{
    return gear_find(s_config_commands, dali_cli_config_count(), name);
}

/* ---------------------------------------------------------------------------
 * Device type 6 (IEC 62386-207)
 * --------------------------------------------------------------------------*/

static const DaliCliDtCommand s_dt6_commands[] = {
    { "ref-power",          dali_dt6_reference_system_power,       DALI_CLI_DT_CONFIG, 0u, NULL, DALI_RESP_NONE },
    { "enable-protector",   dali_dt6_enable_current_protector,     DALI_CLI_DT_CONFIG, 0u, NULL, DALI_RESP_NONE },
    { "disable-protector",  dali_dt6_disable_current_protector,    DALI_CLI_DT_CONFIG, 0u, NULL, DALI_RESP_NONE },
    { "select-curve",       dali_dt6_select_dimming_curve,         DALI_CLI_DT_CONFIG, 1u, "DTR0=0 standard, 1 linear", DALI_RESP_NONE },
    { "store-fast-fade",    dali_dt6_store_dtr_as_fast_fade_time,  DALI_CLI_DT_CONFIG, 1u, "DTR0=fast fade time", DALI_RESP_NONE },

    { "gear-type",          dali_dt6_query_gear_type,                    DALI_CLI_DT_QUERY, 0u, NULL, DALI_RESP_BITSET8 },
    { "dimming-curve",      dali_dt6_query_dimming_curve,                DALI_CLI_DT_QUERY, 0u, NULL, DALI_RESP_UINT8 },
    { "operating-modes",    dali_dt6_query_possible_operating_modes,     DALI_CLI_DT_QUERY, 0u, NULL, DALI_RESP_BITSET8 },
    { "features",           dali_dt6_query_features,                     DALI_CLI_DT_QUERY, 0u, NULL, DALI_RESP_BITSET8 },
    { "failure-status",     dali_dt6_query_failure_status,               DALI_CLI_DT_QUERY, 0u, NULL, DALI_RESP_BITSET8 },
    { "short-circuit",      dali_dt6_query_short_circuit,                DALI_CLI_DT_QUERY, 0u, NULL, DALI_RESP_YES_NO },
    { "open-circuit",       dali_dt6_query_open_circuit,                 DALI_CLI_DT_QUERY, 0u, NULL, DALI_RESP_YES_NO },
    { "load-decrease",      dali_dt6_query_load_decrease,                DALI_CLI_DT_QUERY, 0u, NULL, DALI_RESP_YES_NO },
    { "load-increase",      dali_dt6_query_load_increase,                DALI_CLI_DT_QUERY, 0u, NULL, DALI_RESP_YES_NO },
    { "protector-active",   dali_dt6_query_current_protector_active,     DALI_CLI_DT_QUERY, 0u, NULL, DALI_RESP_YES_NO },
    { "thermal-shutdown",   dali_dt6_query_thermal_shutdown,             DALI_CLI_DT_QUERY, 0u, NULL, DALI_RESP_YES_NO },
    { "thermal-overload",   dali_dt6_query_thermal_overload,             DALI_CLI_DT_QUERY, 0u, NULL, DALI_RESP_YES_NO },
    { "reference-running",  dali_dt6_query_reference_running,            DALI_CLI_DT_QUERY, 0u, NULL, DALI_RESP_YES_NO },
    { "reference-failed",   dali_dt6_query_reference_measurement_failed, DALI_CLI_DT_QUERY, 0u, NULL, DALI_RESP_YES_NO },
    { "protector-enabled",  dali_dt6_query_current_protector_enabled,    DALI_CLI_DT_QUERY, 0u, NULL, DALI_RESP_YES_NO },
    { "operating-mode",     dali_dt6_query_operating_mode,               DALI_CLI_DT_QUERY, 0u, NULL, DALI_RESP_UINT8 },
    { "fast-fade",          dali_dt6_query_fast_fade_time,               DALI_CLI_DT_QUERY, 0u, NULL, DALI_RESP_UINT8 },
    { "min-fast-fade",      dali_dt6_query_min_fast_fade_time,           DALI_CLI_DT_QUERY, 0u, NULL, DALI_RESP_UINT8 },
    { "version",            dali_dt6_query_extended_version_number,      DALI_CLI_DT_QUERY, 0u, NULL, DALI_RESP_UINT8 },
};

/* ---------------------------------------------------------------------------
 * Device type 8 (IEC 62386-209)
 * --------------------------------------------------------------------------*/

static const DaliCliDtCommand s_dt8_commands[] = {
    { "set-x",             dali_dt8_set_temporary_x_coordinate,      DALI_CLI_DT_ACTION, 2u, "DTR0=low, DTR1=high", DALI_RESP_NONE },
    { "set-y",             dali_dt8_set_temporary_y_coordinate,      DALI_CLI_DT_ACTION, 2u, "DTR0=low, DTR1=high", DALI_RESP_NONE },
    { "set-tc",            dali_dt8_set_temporary_colour_temperature, DALI_CLI_DT_ACTION, 2u, "DTR0=low, DTR1=high (mirek)", DALI_RESP_NONE },
    { "x-step-up",         dali_dt8_x_coordinate_step_up,            DALI_CLI_DT_ACTION, 0u, NULL, DALI_RESP_NONE },
    { "x-step-down",       dali_dt8_x_coordinate_step_down,          DALI_CLI_DT_ACTION, 0u, NULL, DALI_RESP_NONE },
    { "y-step-up",         dali_dt8_y_coordinate_step_up,            DALI_CLI_DT_ACTION, 0u, NULL, DALI_RESP_NONE },
    { "y-step-down",       dali_dt8_y_coordinate_step_down,          DALI_CLI_DT_ACTION, 0u, NULL, DALI_RESP_NONE },
    { "tc-cooler",         dali_dt8_colour_temperature_step_cooler,  DALI_CLI_DT_ACTION, 0u, NULL, DALI_RESP_NONE },
    { "tc-warmer",         dali_dt8_colour_temperature_step_warmer,  DALI_CLI_DT_ACTION, 0u, NULL, DALI_RESP_NONE },
    { "set-primary",       dali_dt8_set_temporary_primary_n_dim_level, DALI_CLI_DT_ACTION, 3u, "DTR0=level, DTR1=primary, DTR2=reserved", DALI_RESP_NONE },
    { "set-rgb",           dali_dt8_set_temporary_rgb_dim_level,     DALI_CLI_DT_ACTION, 3u, "DTR0=R, DTR1=G, DTR2=B", DALI_RESP_NONE },
    { "set-waf",           dali_dt8_set_temporary_waf_dim_level,     DALI_CLI_DT_ACTION, 3u, "DTR0=W, DTR1=A, DTR2=F", DALI_RESP_NONE },
    { "set-rgbwaf-control", dali_dt8_set_temporary_rgbwaf_control,   DALI_CLI_DT_ACTION, 1u, "DTR0=channel enable bitmap", DALI_RESP_NONE },
    { "activate",          dali_dt8_activate,                        DALI_CLI_DT_ACTION, 0u, NULL, DALI_RESP_NONE },
    { "copy-report",       dali_dt8_copy_report_to_temporary,        DALI_CLI_DT_ACTION, 0u, NULL, DALI_RESP_NONE },

    { "store-ty-primary",  dali_dt8_store_ty_primary_n,              DALI_CLI_DT_CONFIG, 3u, "DTR0=Ty, DTR1=primary, DTR2=reserved", DALI_RESP_NONE },
    { "store-xy-primary",  dali_dt8_store_xy_coordinate_primary_n,   DALI_CLI_DT_CONFIG, 3u, "DTR0=x low, DTR1=x high, DTR2=primary", DALI_RESP_NONE },
    { "store-tc-limit",    dali_dt8_store_colour_temperature_tc_limit, DALI_CLI_DT_CONFIG, 3u, "DTR0=low, DTR1=high, DTR2=selector", DALI_RESP_NONE },
    { "store-features",    dali_dt8_store_gear_features_status,      DALI_CLI_DT_CONFIG, 1u, "DTR0=features bitmap", DALI_RESP_NONE },
    { "assign-colour",     dali_dt8_assign_colour_to_linked_channel, DALI_CLI_DT_CONFIG, 1u, "DTR0=channel", DALI_RESP_NONE },
    { "auto-calibration",  dali_dt8_start_auto_calibration,          DALI_CLI_DT_CONFIG, 0u, NULL, DALI_RESP_NONE },

    { "features",          dali_dt8_query_gear_features_status,      DALI_CLI_DT_QUERY, 0u, NULL, DALI_RESP_BITSET8 },
    { "colour-status",     dali_dt8_query_colour_status,             DALI_CLI_DT_QUERY, 0u, NULL, DALI_RESP_BITSET8 },
    { "colour-type-features", dali_dt8_query_colour_type_features,   DALI_CLI_DT_QUERY, 0u, NULL, DALI_RESP_BITSET8 },
    { "rgbwaf-control",    dali_dt8_query_rgbwaf_control,            DALI_CLI_DT_QUERY, 0u, NULL, DALI_RESP_BITSET8 },
    { "assigned-colour",   dali_dt8_query_assigned_colour,           DALI_CLI_DT_QUERY, 1u, "DTR0=primary index", DALI_RESP_UINT8 },
    { "version",           dali_dt8_query_extended_version_number,   DALI_CLI_DT_QUERY, 0u, NULL, DALI_RESP_UINT8 },

    /* The only entry whose argument is a selector name rather than DTR bytes:
     * the 16-bit read has its own four-step sequence builder. */
    { "colour",            NULL,                                     DALI_CLI_DT_COLOUR16, 1u, "selector name; see 'list selectors'", DALI_RESP_UINT8 },
};

static const DaliCliDt8Selector s_dt8_selectors[] = {
    { "x",        DALI_DT8_VALUE_X_COORDINATE },
    { "y",        DALI_DT8_VALUE_Y_COORDINATE },
    { "tc",       DALI_DT8_VALUE_COLOUR_TEMP_TC },
    { "primary0", DALI_DT8_VALUE_PRIMARY_0 },
    { "primary1", DALI_DT8_VALUE_PRIMARY_1 },
    { "primary2", DALI_DT8_VALUE_PRIMARY_2 },
    { "primary3", DALI_DT8_VALUE_PRIMARY_3 },
    { "primary4", DALI_DT8_VALUE_PRIMARY_4 },
    { "primary5", DALI_DT8_VALUE_PRIMARY_5 },
    { "red",      DALI_DT8_VALUE_RED },
    { "green",    DALI_DT8_VALUE_GREEN },
    { "blue",     DALI_DT8_VALUE_BLUE },
    { "white",    DALI_DT8_VALUE_WHITE },
    { "amber",    DALI_DT8_VALUE_AMBER },
    { "free",     DALI_DT8_VALUE_FREE_COLOUR },
};

static const DaliCliDtCommand *dt_find(const DaliCliDtCommand *table,
                                       uint8_t                 count,
                                       const char             *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (uint8_t i = 0u; i < count; i++) {
        if (strcmp(name, table[i].name) == 0) {
            return &table[i];
        }
    }
    return NULL;
}

uint8_t dali_cli_dt6_count(void) { return CLI_TABLE_ENTRIES(s_dt6_commands); }
const DaliCliDtCommand *dali_cli_dt6_at(uint8_t index)
{
    return index < dali_cli_dt6_count() ? &s_dt6_commands[index] : NULL;
}
const DaliCliDtCommand *dali_cli_dt6_find(const char *name)
{
    return dt_find(s_dt6_commands, dali_cli_dt6_count(), name);
}

uint8_t dali_cli_dt8_count(void) { return CLI_TABLE_ENTRIES(s_dt8_commands); }
const DaliCliDtCommand *dali_cli_dt8_at(uint8_t index)
{
    return index < dali_cli_dt8_count() ? &s_dt8_commands[index] : NULL;
}
const DaliCliDtCommand *dali_cli_dt8_find(const char *name)
{
    return dt_find(s_dt8_commands, dali_cli_dt8_count(), name);
}

uint8_t dali_cli_dt8_selector_count(void) { return CLI_TABLE_ENTRIES(s_dt8_selectors); }
const DaliCliDt8Selector *dali_cli_dt8_selector_at(uint8_t index)
{
    return index < dali_cli_dt8_selector_count() ? &s_dt8_selectors[index] : NULL;
}
const DaliCliDt8Selector *dali_cli_dt8_selector_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (uint8_t i = 0u; i < dali_cli_dt8_selector_count(); i++) {
        if (strcmp(name, s_dt8_selectors[i].name) == 0) {
            return &s_dt8_selectors[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Part 103 instance queries
 *
 * The generic queries validate their arguments and return DaliError; the
 * type-specific ones return a frame directly. The table needs one shape, so the
 * generic ones are wrapped. A wrapper that fails yields a zero-length frame,
 * which the caller must refuse to send.
 * --------------------------------------------------------------------------*/

#define CLI_WRAP_INSTANCE_QUERY(fn_name, builder)                              \
    static DaliFrame fn_name(uint8_t addr, uint8_t instance)                   \
    {                                                                          \
        DaliFrame frame = {0u, 0u};                                            \
        (void)builder(addr, instance, &frame);                                 \
        return frame;                                                          \
    }

CLI_WRAP_INSTANCE_QUERY(cli_iq_type, dali_input_build_query_instance_type)
CLI_WRAP_INSTANCE_QUERY(cli_iq_resolution, dali_input_build_query_resolution)
CLI_WRAP_INSTANCE_QUERY(cli_iq_error, dali_input_build_query_instance_error)
CLI_WRAP_INSTANCE_QUERY(cli_iq_status, dali_input_build_query_instance_status)
CLI_WRAP_INSTANCE_QUERY(cli_iq_enabled, dali_input_build_query_instance_enabled)
CLI_WRAP_INSTANCE_QUERY(cli_iq_event_priority, dali_input_build_query_event_priority)
CLI_WRAP_INSTANCE_QUERY(cli_iq_primary_group, dali_input_build_query_primary_instance_group)
CLI_WRAP_INSTANCE_QUERY(cli_iq_group1, dali_input_build_query_instance_group1)
CLI_WRAP_INSTANCE_QUERY(cli_iq_group2, dali_input_build_query_instance_group2)
CLI_WRAP_INSTANCE_QUERY(cli_iq_event_scheme, dali_input_build_query_event_scheme)
CLI_WRAP_INSTANCE_QUERY(cli_iq_event_filter0, dali_input_build_query_event_filter_zero)
CLI_WRAP_INSTANCE_QUERY(cli_iq_event_filter1, dali_input_build_query_event_filter_one)
CLI_WRAP_INSTANCE_QUERY(cli_iq_event_filter2, dali_input_build_query_event_filter_two)
CLI_WRAP_INSTANCE_QUERY(cli_iq_instance_config, dali_input_build_query_instance_configuration)
CLI_WRAP_INSTANCE_QUERY(cli_iq_available_types, dali_input_build_query_available_instance_types)

#undef CLI_WRAP_INSTANCE_QUERY

/* QUERY INPUT VALUE / LATCH have no dali_input_build_* wrapper because they are
 * plain instance commands with no type-specific framing. */
static DaliFrame cli_iq_input_value(uint8_t addr, uint8_t instance)
{
    DaliFrame frame = {0u, 0u};
    (void)dali_build_instance_command(addr, instance, DALI_CMD_QUERY_INPUT_VALUE, &frame);
    return frame;
}

static DaliFrame cli_iq_input_value_latch(uint8_t addr, uint8_t instance)
{
    DaliFrame frame = {0u, 0u};
    (void)dali_build_instance_command(addr, instance, DALI_CMD_QUERY_INPUT_VALUE_LATCH,
                                      &frame);
    return frame;
}

static const DaliCliInstanceQuery s_iquery_commands[] = {
    { "type",            cli_iq_type,            false, NULL, DALI_RESP_UINT8 },
    { "resolution",      cli_iq_resolution,      false, NULL, DALI_RESP_UINT8 },
    { "error",           cli_iq_error,           false, NULL, DALI_RESP_UINT8 },
    { "status",          cli_iq_status,          false, NULL, DALI_RESP_BITSET8 },
    { "enabled",         cli_iq_enabled,         false, NULL, DALI_RESP_YES_NO },
    { "event-priority",  cli_iq_event_priority,  false, NULL, DALI_RESP_UINT8 },
    { "primary-group",   cli_iq_primary_group,   false, NULL, DALI_RESP_UINT8 },
    { "group1",          cli_iq_group1,          false, NULL, DALI_RESP_UINT8 },
    { "group2",          cli_iq_group2,          false, NULL, DALI_RESP_UINT8 },
    { "event-scheme",    cli_iq_event_scheme,    false, NULL, DALI_RESP_UINT8 },
    { "event-filter0",   cli_iq_event_filter0,   false, NULL, DALI_RESP_BITSET8 },
    { "event-filter1",   cli_iq_event_filter1,   false, NULL, DALI_RESP_BITSET8 },
    { "event-filter2",   cli_iq_event_filter2,   false, NULL, DALI_RESP_BITSET8 },
    { "instance-config", cli_iq_instance_config, true,  "DTR0=configuration index", DALI_RESP_UINT8 },
    { "available-types", cli_iq_available_types, false, NULL, DALI_RESP_BITSET8 },
    /* On-demand read of the instance's current value. The latch variant freezes
     * a multi-byte value so its bytes belong to one reading; the remaining
     * bytes come from the DTRs and need an atomic follow-up read. */
    { "input-value",       cli_iq_input_value,       false, NULL, DALI_RESP_INPUT_VALUE_MSB },
    { "input-value-latch", cli_iq_input_value_latch, false, NULL, DALI_RESP_INPUT_VALUE_LATCH },

    { "pb-short-timer",      dali_input_pb_build_query_short_timer,      false, NULL, DALI_RESP_UINT8 },
    { "pb-short-timer-min",  dali_input_pb_build_query_short_timer_min,  false, NULL, DALI_RESP_UINT8 },
    { "pb-double-timer",     dali_input_pb_build_query_double_timer,     false, NULL, DALI_RESP_UINT8 },
    { "pb-double-timer-min", dali_input_pb_build_query_double_timer_min, false, NULL, DALI_RESP_UINT8 },
    { "pb-repeat-timer",     dali_input_pb_build_query_repeat_timer,     false, NULL, DALI_RESP_UINT8 },
    { "pb-stuck-timer",      dali_input_pb_build_query_stuck_timer,      false, NULL, DALI_RESP_UINT8 },

    { "occ-capabilities",    dali_input_occ_build_query_capabilities,    false, NULL, DALI_RESP_BITSET8 },
    { "occ-detection-range", dali_input_occ_build_query_detection_range, false, NULL, DALI_RESP_UINT8 },
    { "occ-sensitivity",     dali_input_occ_build_query_sensitivity,     false, NULL, DALI_RESP_UINT8 },
    { "occ-deadtime",        dali_input_occ_build_query_deadtime,        false, NULL, DALI_RESP_UINT8 },
    { "occ-hold-timer",      dali_input_occ_build_query_hold_timer,      false, NULL, DALI_RESP_UINT8 },
    { "occ-report-timer",    dali_input_occ_build_query_report_timer,    false, NULL, DALI_RESP_UINT8 },
    { "occ-catching",        dali_input_occ_build_query_catching,        false, NULL, DALI_RESP_YES_NO },

    { "light-hysteresis-min", dali_input_light_build_query_hysteresis_min, false, NULL, DALI_RESP_UINT8 },
    { "light-deadtime",       dali_input_light_build_query_deadtime,       false, NULL, DALI_RESP_UINT8 },
    { "light-report-timer",   dali_input_light_build_query_report_timer,   false, NULL, DALI_RESP_UINT8 },
    { "light-hysteresis",     dali_input_light_build_query_hysteresis,     false, NULL, DALI_RESP_UINT8 },
};

uint8_t dali_cli_iquery_count(void) { return CLI_TABLE_ENTRIES(s_iquery_commands); }
const DaliCliInstanceQuery *dali_cli_iquery_at(uint8_t index)
{
    return index < dali_cli_iquery_count() ? &s_iquery_commands[index] : NULL;
}
const DaliCliInstanceQuery *dali_cli_iquery_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (uint8_t i = 0u; i < dali_cli_iquery_count(); i++) {
        if (strcmp(name, s_iquery_commands[i].name) == 0) {
            return &s_iquery_commands[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Part 103 instance configuration
 * --------------------------------------------------------------------------*/

/* Accepted range for one DTR byte. ANY is the 0-255 pass-through used for
 * opaque values such as event-filter bitmaps and instance-configuration data. */
#define DTR_ANY        { 0u, 255u, 0u }
#define DTR_R(lo, hi)  { (lo), (hi), 0u }
/* 0 disables the timer, otherwise the value must be in range. */
#define DTR_R_ZERO(lo, hi) { (lo), (hi), DALI_CLI_DTR_ALLOW_ZERO }
/* 255 is MASK — "no group" — rather than an ordinary out-of-range value. */
#define DTR_R_MASK(lo, hi) { (lo), (hi), DALI_CLI_DTR_ALLOW_MASK }

static const DaliCliInstanceConfig s_iconfig_commands[] = {
    { "set-event-priority",  dali_input_build_set_event_priority,       true, 1u, "DTR0=priority 2-5", 0u, { DTR_R(2u, 5u) } },
    { "enable",              dali_input_build_enable_instance,          true, 0u, NULL, 0u, { DTR_ANY } },
    { "disable",             dali_input_build_disable_instance,         true, 0u, NULL, 0u, { DTR_ANY } },
    { "set-primary-group",   dali_input_build_set_primary_group,        true, 1u, "DTR0=group 0-31 or 255 for MASK", 0u, { DTR_R_MASK(0u, 31u) } },
    { "set-group1",          dali_input_build_set_instance_group1,      true, 1u, "DTR0=group 0-31 or 255 for MASK", 0u, { DTR_R_MASK(0u, 31u) } },
    { "set-group2",          dali_input_build_set_instance_group2,      true, 1u, "DTR0=group 0-31 or 255 for MASK", 0u, { DTR_R_MASK(0u, 31u) } },
    { "set-event-scheme",    dali_input_build_set_event_scheme,         true, 1u, "DTR0=scheme 0-4", 0u, { DTR_R(0u, 4u) } },
    { "set-event-filter",    dali_input_build_set_event_filter,         true, 3u, "DTR0,DTR1,DTR2=eventFilter[7:0],[15:8],[23:16]", 0u, { DTR_ANY, DTR_ANY, DTR_ANY } },
    { "set-instance-type",   dali_input_build_set_instance_type,        true, 1u, "DTR0=instance type 0-31", 0u, { DTR_R(0u, 31u) } },
    { "set-instance-config", dali_input_build_set_instance_configuration, true, 3u, "DTR0=index, DTR1=value low, DTR2=value high", 0u, { DTR_ANY, DTR_ANY, DTR_ANY } },

    { "pb-set-short-timer",  dali_input_pb_build_set_short_timer,       true, 1u, "DTR0=tShort 10-255 in 20 ms units", 1u, { DTR_R(10u, 255u) } },
    { "pb-set-double-timer", dali_input_pb_build_set_double_timer,      true, 1u, "DTR0=tDouble 10-100 in 20 ms units, 0 disables", 1u, { DTR_R_ZERO(10u, 100u) } },
    { "pb-set-repeat-timer", dali_input_pb_build_set_repeat_timer,      true, 1u, "DTR0=tRepeat 5-100 in 20 ms units", 1u, { DTR_R(5u, 100u) } },
    { "pb-set-stuck-timer",  dali_input_pb_build_set_stuck_timer,       true, 1u, "DTR0=tStuck 5-255 seconds", 1u, { DTR_R(5u, 255u) } },

    { "occ-catch-movement",  dali_input_occ_build_catch_movement,       false, 0u, NULL, 3u, { DTR_ANY } },
    { "occ-cancel-hold",     dali_input_occ_build_cancel_hold_timer,    false, 0u, NULL, 3u, { DTR_ANY } },
    { "occ-set-hold-timer",  dali_input_occ_build_set_hold_timer,       true, 1u, "DTR0=hold 0-254 in 10 s units, 0 selects 1 s", 3u, { DTR_R(0u, 254u) } },
    { "occ-set-report-timer", dali_input_occ_build_set_report_timer,    true, 1u, "DTR0=report seconds, 0 disables", 3u, { DTR_R(0u, 255u) } },
    { "occ-set-deadtime",    dali_input_occ_build_set_deadtime,         true, 1u, "DTR0=deadtime in 50 ms units, 0 disables", 3u, { DTR_R(0u, 255u) } },
    { "occ-set-detection-range", dali_input_occ_build_set_detection_range, true, 1u, "DTR0=range 0-100", 3u, { DTR_R(0u, 100u) } },
    { "occ-set-sensitivity", dali_input_occ_build_set_sensitivity,      true, 1u, "DTR0=sensitivity 0-100", 3u, { DTR_R(0u, 100u) } },

    { "light-set-report-timer",   dali_input_light_build_set_report_timer,   true, 1u, "DTR0=report seconds, 0 disables", 4u, { DTR_R(0u, 255u) } },
    { "light-set-hysteresis",     dali_input_light_build_set_hysteresis,     true, 1u, "DTR0=percentage 0-25", 4u, { DTR_R(0u, 25u) } },
    { "light-set-deadtime",       dali_input_light_build_set_deadtime,       true, 1u, "DTR0=deadtime in 50 ms units, 0 disables", 4u, { DTR_R(0u, 255u) } },
    { "light-set-hysteresis-min", dali_input_light_build_set_hysteresis_min, true, 1u, "DTR0=absolute minimum 0-255", 4u, { DTR_R(0u, 255u) } },
};

#undef DTR_ANY
#undef DTR_R
#undef DTR_R_ZERO
#undef DTR_R_MASK

bool dali_cli_dtr_value_valid(const DaliCliDtrRange *range, uint8_t value)
{
    if (range == NULL) {
        return false;
    }
    if (value >= range->min && value <= range->max) {
        return true;
    }
    if ((range->flags & DALI_CLI_DTR_ALLOW_ZERO) != 0u && value == 0u) {
        return true;
    }
    if ((range->flags & DALI_CLI_DTR_ALLOW_MASK) != 0u && value == 255u) {
        return true;
    }
    return false;
}

uint8_t dali_cli_iconfig_count(void) { return CLI_TABLE_ENTRIES(s_iconfig_commands); }
const DaliCliInstanceConfig *dali_cli_iconfig_at(uint8_t index)
{
    return index < dali_cli_iconfig_count() ? &s_iconfig_commands[index] : NULL;
}
const DaliCliInstanceConfig *dali_cli_iconfig_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (uint8_t i = 0u; i < dali_cli_iconfig_count(); i++) {
        if (strcmp(name, s_iconfig_commands[i].name) == 0) {
            return &s_iconfig_commands[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Vendor
 * --------------------------------------------------------------------------*/

static const DaliCliLunatoneCommand s_lunatone_commands[] = {
    { "multiplicator", DALI_LUNATONE_QUERY_VALUE_MULTIPLICATOR },
    { "divisor",       DALI_LUNATONE_QUERY_VALUE_DIVISOR },
    { "offset-msb",    DALI_LUNATONE_QUERY_OFFSET_MSB },
    { "offset-lsb",    DALI_LUNATONE_QUERY_OFFSET_LSB },
    { "offset-mult",   DALI_LUNATONE_QUERY_OFFSET_MULTIPLICATOR },
    { "offset-div",    DALI_LUNATONE_QUERY_OFFSET_DIVISOR },
    { "unit",          DALI_LUNATONE_QUERY_UNIT },
};

uint8_t dali_cli_lunatone_count(void) { return CLI_TABLE_ENTRIES(s_lunatone_commands); }
const DaliCliLunatoneCommand *dali_cli_lunatone_at(uint8_t index)
{
    return index < dali_cli_lunatone_count() ? &s_lunatone_commands[index] : NULL;
}
const DaliCliLunatoneCommand *dali_cli_lunatone_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (uint8_t i = 0u; i < dali_cli_lunatone_count(); i++) {
        if (strcmp(name, s_lunatone_commands[i].name) == 0) {
            return &s_lunatone_commands[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Named-table listing
 * --------------------------------------------------------------------------*/

static const char *const s_table_names[DALI_CLI_TABLE_COUNT] = {
    "query", "special", "config", "dt6", "dt8", "selectors",
    "iquery", "iconfig", "vendor",
};

const char *dali_cli_table_name(DaliCliTableId table)
{
    return table < DALI_CLI_TABLE_COUNT ? s_table_names[table] : NULL;
}

bool dali_cli_table_find(const char *name, DaliCliTableId *out)
{
    if (name == NULL || out == NULL) {
        return false;
    }
    for (uint8_t i = 0u; i < (uint8_t)DALI_CLI_TABLE_COUNT; i++) {
        if (strcmp(name, s_table_names[i]) == 0) {
            *out = (DaliCliTableId)i;
            return true;
        }
    }
    return false;
}

void dali_cli_print_table_names(const DaliCliOut *out)
{
    dali_cli_write(out, "tables:");
    for (uint8_t i = 0u; i < (uint8_t)DALI_CLI_TABLE_COUNT; i++) {
        dali_cli_printf(out, " %s", s_table_names[i]);
    }
    dali_cli_write(out, "\r\n");
}

static void print_gear_table(const DaliCliOut         *out,
                             const DaliCliGearCommand *(*at)(uint8_t),
                             uint8_t                   count,
                             bool                      show_frame_notes)
{
    for (uint8_t i = 0u; i < count; i++) {
        const DaliCliGearCommand *spec = at(i);
        dali_cli_printf(out, "  %s", spec->name);
        if (spec->needs_param) {
            dali_cli_printf(out, " <0-%u>", (unsigned)spec->max_param);
        }
        if (spec->uses_dtr0) {
            dali_cli_write(out, " [uses DTR0]");
        }
        if (show_frame_notes) {
            const DaliCommandInfo *cmd = dali_command_lookup(spec->id);
            if (cmd != NULL && cmd->send_twice) {
                dali_cli_write(out, " [send twice]");
            }
            if (cmd != NULL && cmd->response_kind != DALI_RESP_NONE) {
                dali_cli_write(out, " [waits reply]");
            }
        }
        dali_cli_write(out, "\r\n");
    }
}

static void print_dt_table(const DaliCliOut       *out,
                           const DaliCliDtCommand *(*at)(uint8_t),
                           uint8_t                 count)
{
    for (uint8_t i = 0u; i < count; i++) {
        const DaliCliDtCommand *spec = at(i);
        dali_cli_printf(out, "  %s", spec->name);
        switch (spec->kind) {
            case DALI_CLI_DT_CONFIG:
                dali_cli_write(out, " [send twice]");
                break;
            case DALI_CLI_DT_QUERY:
                dali_cli_write(out, " [waits reply]");
                break;
            case DALI_CLI_DT_COLOUR16:
                dali_cli_write(out, " [16-bit read]");
                break;
            case DALI_CLI_DT_ACTION:
                break;
        }
        if (spec->dtr_help != NULL) {
            dali_cli_printf(out, " - %s", spec->dtr_help);
        }
        dali_cli_write(out, "\r\n");
    }
}

void dali_cli_print_table(const DaliCliOut *out, DaliCliTableId table)
{
    switch (table) {
        case DALI_CLI_TABLE_QUERY:
            dali_cli_write(out, "query names:\r\n");
            print_gear_table(out, dali_cli_query_at, dali_cli_query_count(), false);
            break;

        case DALI_CLI_TABLE_SPECIAL:
            dali_cli_write(out, "special names:\r\n");
            print_gear_table(out, dali_cli_special_at, dali_cli_special_count(), true);
            break;

        case DALI_CLI_TABLE_CONFIG:
            dali_cli_write(out, "config names:\r\n");
            print_gear_table(out, dali_cli_config_at, dali_cli_config_count(), false);
            break;

        case DALI_CLI_TABLE_DT6:
            dali_cli_write(out, "dt6 names:\r\n");
            print_dt_table(out, dali_cli_dt6_at, dali_cli_dt6_count());
            break;

        case DALI_CLI_TABLE_DT8:
            dali_cli_write(out, "dt8 names:\r\n");
            print_dt_table(out, dali_cli_dt8_at, dali_cli_dt8_count());
            break;

        case DALI_CLI_TABLE_SELECTORS:
            dali_cli_write(out, "dt8 colour value selectors:\r\n");
            for (uint8_t i = 0u; i < dali_cli_dt8_selector_count(); i++) {
                const DaliCliDt8Selector *sel = dali_cli_dt8_selector_at(i);
                dali_cli_printf(out, "  %s (%u)\r\n",
                                sel->name, (unsigned)sel->selector);
            }
            break;

        case DALI_CLI_TABLE_IQUERY:
            dali_cli_write(out, "iquery names:\r\n");
            for (uint8_t i = 0u; i < dali_cli_iquery_count(); i++) {
                const DaliCliInstanceQuery *spec = dali_cli_iquery_at(i);
                dali_cli_printf(out, "  %s", spec->name);
                if (spec->dtr0_help != NULL) {
                    dali_cli_printf(out, " - %s", spec->dtr0_help);
                }
                dali_cli_write(out, "\r\n");
            }
            break;

        case DALI_CLI_TABLE_ICONFIG:
            dali_cli_write(out, "iconfig names:\r\n");
            for (uint8_t i = 0u; i < dali_cli_iconfig_count(); i++) {
                const DaliCliInstanceConfig *spec = dali_cli_iconfig_at(i);
                dali_cli_printf(out, "  %s", spec->name);
                if (spec->instance_type != 0u) {
                    dali_cli_printf(out, " [instance type %u]",
                                    (unsigned)spec->instance_type);
                }
                if (spec->send_twice) {
                    dali_cli_write(out, " [send twice]");
                }
                if (spec->dtr_help != NULL) {
                    dali_cli_printf(out, " - %s", spec->dtr_help);
                }
                dali_cli_write(out, "\r\n");
            }
            break;

        case DALI_CLI_TABLE_VENDOR:
            dali_cli_write(out, "vendor lunatone <addr> <instance> <name>:\r\n");
            for (uint8_t i = 0u; i < dali_cli_lunatone_count(); i++) {
                const DaliCliLunatoneCommand *spec = dali_cli_lunatone_at(i);
                const DaliLunatoneCommandInfo *info =
                    dali_lunatone_command_lookup(spec->id);
                dali_cli_printf(out, "  %s (opcode 0x%02X)\r\n",
                                spec->name,
                                info != NULL ? (unsigned)info->opcode : 0u);
            }
            dali_cli_write(out, "vendor steinel <instance> <raw>:\r\n");
            dali_cli_write(out, "  decode an HF 360 II reading without bus traffic\r\n");
            break;

        case DALI_CLI_TABLE_COUNT:
        default:
            dali_cli_print_table_names(out);
            break;
    }
}

/* ---------------------------------------------------------------------------
 * Response formatting
 * --------------------------------------------------------------------------*/

void dali_cli_print_frame(const DaliCliOut *out,
                          const char       *prefix,
                          const DaliFrame  *frame)
{
    if (frame == NULL) {
        return;
    }
    dali_cli_printf(out, "%s0x%0*lX (%u-bit)\r\n",
                    prefix != NULL ? prefix : "",
                    (int)((frame->bit_length + 3u) / 4u),
                    (unsigned long)frame->data,
                    (unsigned)frame->bit_length);
}

/*
 * Append to a bounded buffer, returning the new length.
 *
 * Truncation is reported by the returned length reaching cap - 1u rather than
 * by a flag: every caller here builds one short line, and a line clipped at the
 * buffer end is still a correct prefix of the answer.
 */
static size_t cli_append(char *buf, size_t cap, size_t len, const char *fmt, ...)
{
    if (buf == NULL || cap == 0u || fmt == NULL || len + 1u >= cap) {
        return len;
    }

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buf + len, cap - len, fmt, args);
    va_end(args);

    if (written < 0) {
        buf[len] = '\0';
        return len;
    }
    if ((size_t)written >= cap - len) {
        return cap - 1u;  /* vsnprintf terminated at the boundary */
    }
    return len + (size_t)written;
}

size_t dali_cli_format_status(char *buf, size_t cap, uint8_t raw)
{
    if (buf == NULL || cap == 0u) {
        return 0u;
    }
    buf[0] = '\0';

    DaliStatus s;
    if (dali_parse_status(raw, &s) != DALI_OK) {
        return 0u;
    }

    const struct {
        const char *name;
        bool        set;
    } flags[] = {
        { "ballast-fail", s.ballast_failure       },
        { "lamp-fail",    s.lamp_failure          },
        { "arc-on",       s.lamp_arc_power_on     },
        { "limit-err",    s.limit_error           },
        { "fading",       s.fade_running          },
        { "reset",        s.reset_state           },
        { "no-addr",      s.missing_short_address },
        { "power-fail",   s.power_failure         },
    };

    size_t len = cli_append(buf, cap, 0u, "0x%02X", (unsigned)raw);
    bool   any = false;
    for (uint8_t i = 0u; i < (uint8_t)(sizeof(flags) / sizeof(flags[0])); i++) {
        if (!flags[i].set) {
            continue;
        }
        len = cli_append(buf, cap, len, "%c%s", any ? ',' : ' ', flags[i].name);
        any = true;
    }
    if (!any) {
        len = cli_append(buf, cap, len, " none");
    }
    return len;
}

size_t dali_cli_format_response(char             *buf,
                                size_t            cap,
                                const char       *name,
                                DaliResponseKind  kind,
                                const DaliFrame  *reply)
{
    if (buf == NULL || cap == 0u) {
        return 0u;
    }
    buf[0] = '\0';
    if (name == NULL) {
        return 0u;
    }

    DaliParsedResponse parsed;
    if (dali_parse_by_kind(kind, reply, &parsed) != DALI_OK) {
        return cli_append(buf, cap, 0u, "%s: malformed reply", name);
    }

    size_t len = cli_append(buf, cap, 0u, "%s: ", name);

    switch (parsed.kind) {
        case DALI_RESP_STATUS: {
            char decoded[DALI_CLI_STATUS_LINE_MAX];
            dali_cli_format_status(decoded, sizeof(decoded), parsed.raw);
            return cli_append(buf, cap, len, "%s", decoded);
        }

        case DALI_RESP_YES_NO:
            return cli_append(buf, cap, len, "%s (0x%02X)",
                              parsed.yes ? "yes" : "no", (unsigned)parsed.raw);

        case DALI_RESP_BITSET8:
            return cli_append(buf, cap, len, "0x%02X", (unsigned)parsed.bitset);

        case DALI_RESP_FADE_TIME_RATE:
            return cli_append(buf, cap, len, "fade_time=%u fade_rate=%u (0x%02X)",
                              (unsigned)parsed.fade.fade_time,
                              (unsigned)parsed.fade.fade_rate,
                              (unsigned)parsed.raw);

        case DALI_RESP_UINT8:
        case DALI_RESP_MEMORY_BYTE:
        case DALI_RESP_INPUT_VALUE_MSB:
        case DALI_RESP_INPUT_VALUE_LATCH:
            return cli_append(buf, cap, len, "%u (0x%02X)",
                              (unsigned)parsed.value, (unsigned)parsed.raw);

        case DALI_RESP_NONE:
        default:
            return cli_append(buf, cap, len, "0x%02X", (unsigned)parsed.raw);
    }
}

void dali_cli_print_status_fields(const DaliCliOut *out, uint8_t raw)
{
    DaliStatus s;
    if (dali_parse_status(raw, &s) != DALI_OK) {
        return;
    }

    dali_cli_printf(out, "  Ballast failure:       %s\r\n", s.ballast_failure       ? "YES" : "no");
    dali_cli_printf(out, "  Lamp failure:          %s\r\n", s.lamp_failure          ? "YES" : "no");
    dali_cli_printf(out, "  Lamp arc power on:     %s\r\n", s.lamp_arc_power_on     ? "YES" : "no");
    dali_cli_printf(out, "  Limit error:           %s\r\n", s.limit_error           ? "YES" : "no");
    dali_cli_printf(out, "  Fade running:          %s\r\n", s.fade_running          ? "YES" : "no");
    dali_cli_printf(out, "  Reset state:           %s\r\n", s.reset_state           ? "YES" : "no");
    dali_cli_printf(out, "  Missing short address: %s\r\n", s.missing_short_address ? "YES" : "no");
    dali_cli_printf(out, "  Power failure:         %s\r\n", s.power_failure         ? "YES" : "no");
}

void dali_cli_print_response(const DaliCliOut *out,
                             const char       *name,
                             DaliResponseKind  kind,
                             const DaliFrame  *reply)
{
    if (name == NULL) {
        return;
    }

    char line[DALI_CLI_RESPONSE_LINE_MAX];
    dali_cli_format_response(line, sizeof(line), name, kind, reply);
    dali_cli_printf(out, "%s\r\n", line);

    /* A status byte also gets its per-field block. The terminal has room for
     * it, and the line above is a summary of the same eight bits. */
    DaliParsedResponse parsed;
    if (dali_parse_by_kind(kind, reply, &parsed) == DALI_OK &&
        parsed.kind == DALI_RESP_STATUS) {
        dali_cli_print_status_fields(out, parsed.raw);
    }
}

void dali_cli_print_tx_result(const DaliCliOut *out, const char *name, DaliError err)
{
    if (err == DALI_OK) {
        dali_cli_printf(out, "%s: OK\r\n", name);
    } else {
        dali_cli_print_error(out, name, err);
    }
}

void dali_cli_print_error(const DaliCliOut *out, const char *name, DaliError err)
{
    if (err == DALI_ERR_TIMEOUT) {
        dali_cli_printf(out, "%s: timeout\r\n", name);
    } else if (err == DALI_ERR_QUEUE_FULL) {
        dali_cli_printf(out, "%s: queue full\r\n", name);
    } else {
        dali_cli_printf(out, "%s: ERR %d\r\n", name, (int)err);
    }
}

void dali_cli_print_memory_block(const DaliCliOut *out,
                                 uint8_t           bank,
                                 uint8_t           offset,
                                 const uint8_t    *data,
                                 uint8_t           count)
{
    if (data == NULL || count == 0u) {
        return;
    }

    dali_cli_printf(out, "  bank %u offset 0x%02X:", (unsigned)bank, (unsigned)offset);
    for (uint8_t i = 0u; i < count; i++) {
        dali_cli_printf(out, " %02X", (unsigned)data[i]);
    }
    dali_cli_write(out, "\r\n");
}
