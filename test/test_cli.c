/*
 * test_cli.c — native CLI parsing, dispatch, table, and formatting vectors
 *
 * These cover the half of the diagnostic CLI that decides what a typed line
 * means: tokenising, the verb table, argument validation, the named command
 * tables, and how a reply is rendered. Execution — scheduler slots, transports,
 * and the long-running workflows — stays in dali_diag.c and is not reachable
 * from the host.
 *
 * The table assertions are the point of several of these: a name that resolves
 * to the wrong DaliCommandId, or a ranged opcode without a parameter, produces
 * a CLI verb that transmits a different command than the one the operator
 * asked for, and nothing else in the build would notice.
 */

#include "unity.h"
#include "dali_cli.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ---------------------------------------------------------------------------
 * Capture helper
 * --------------------------------------------------------------------------*/

static char              s_capture[4096];
static DaliCliBufferSink s_sink;

static DaliCliOut capture_begin(void)
{
    dali_cli_buffer_sink_init(&s_sink, s_capture, sizeof(s_capture));
    return dali_cli_buffer_out(&s_sink);
}

/* ---------------------------------------------------------------------------
 * Tokenising
 * --------------------------------------------------------------------------*/

static void test_tokenize_splits_on_spaces(void)
{
    DaliCliTokens t;
    TEST_ASSERT_EQUAL(DALI_CLI_TOKENIZE_OK, dali_cli_tokenize("level s3 128", &t));
    TEST_ASSERT_EQUAL_UINT8(3u, t.count);
    TEST_ASSERT_EQUAL_STRING("level", t.tok[0]);
    TEST_ASSERT_EQUAL_STRING("s3", t.tok[1]);
    TEST_ASSERT_EQUAL_STRING("128", t.tok[2]);
}

static void test_tokenize_collapses_runs_and_tabs(void)
{
    DaliCliTokens t;
    TEST_ASSERT_EQUAL(DALI_CLI_TOKENIZE_OK,
                      dali_cli_tokenize("  level \t\t s3   128  ", &t));
    TEST_ASSERT_EQUAL_UINT8(3u, t.count);
    TEST_ASSERT_EQUAL_STRING("128", t.tok[2]);
}

static void test_tokenize_strips_line_endings(void)
{
    DaliCliTokens t;
    TEST_ASSERT_EQUAL(DALI_CLI_TOKENIZE_OK, dali_cli_tokenize("stats\r\n", &t));
    TEST_ASSERT_EQUAL_UINT8(1u, t.count);
    TEST_ASSERT_EQUAL_STRING("stats", t.tok[0]);
}

static void test_tokenize_empty_and_blank_lines(void)
{
    DaliCliTokens t;
    TEST_ASSERT_EQUAL(DALI_CLI_TOKENIZE_EMPTY, dali_cli_tokenize("", &t));
    TEST_ASSERT_EQUAL(DALI_CLI_TOKENIZE_EMPTY, dali_cli_tokenize("   \t \r\n", &t));
    TEST_ASSERT_EQUAL(DALI_CLI_TOKENIZE_EMPTY, dali_cli_tokenize(NULL, &t));
}

static void test_tokenize_rejects_too_many_tokens(void)
{
    DaliCliTokens t;
    TEST_ASSERT_EQUAL(DALI_CLI_TOKENIZE_TOO_MANY,
                      dali_cli_tokenize("a b c d e f g h i", &t));
}

/* An over-long token must not be silently truncated: "set-fade-time-dtr0xxx"
 * clipped to a valid prefix would run a command nobody typed. */
static void test_tokenize_rejects_over_long_token(void)
{
    char line[DALI_CLI_MAX_TOKEN_LEN + 8u];
    memset(line, 'a', sizeof(line) - 1u);
    line[sizeof(line) - 1u] = '\0';

    DaliCliTokens t;
    TEST_ASSERT_EQUAL(DALI_CLI_TOKENIZE_TOO_LONG, dali_cli_tokenize(line, &t));
}

static void test_tokenize_accepts_longest_valid_token(void)
{
    char line[DALI_CLI_MAX_TOKEN_LEN];
    memset(line, 'a', sizeof(line) - 1u);
    line[sizeof(line) - 1u] = '\0';

    DaliCliTokens t;
    TEST_ASSERT_EQUAL(DALI_CLI_TOKENIZE_OK, dali_cli_tokenize(line, &t));
    TEST_ASSERT_EQUAL_UINT8(1u, t.count);
    TEST_ASSERT_EQUAL_size_t(DALI_CLI_MAX_TOKEN_LEN - 1u, strlen(t.tok[0]));
}

/* ---------------------------------------------------------------------------
 * Verb table and enum parity
 * --------------------------------------------------------------------------*/

static void test_command_table_covers_every_id(void)
{
    TEST_ASSERT_EQUAL_UINT8((uint8_t)DALI_CLI_CMD_COUNT, dali_cli_command_count());

    for (uint8_t i = 0u; i < (uint8_t)DALI_CLI_CMD_COUNT; i++) {
        const DaliCliCommandSpec *spec =
            dali_cli_command_for_id((DaliCliCommandId)i);
        TEST_ASSERT_NOT_NULL_MESSAGE(spec, "DaliCliCommandId has no table entry");
        TEST_ASSERT_EQUAL((DaliCliCommandId)i, spec->id);
    }
}

static void test_command_table_entries_are_well_formed(void)
{
    for (uint8_t i = 0u; i < dali_cli_command_count(); i++) {
        const DaliCliCommandSpec *spec = dali_cli_command_at(i);
        TEST_ASSERT_NOT_NULL(spec);
        TEST_ASSERT_NOT_NULL(spec->name);
        TEST_ASSERT_NOT_NULL(spec->args);
        TEST_ASSERT_NOT_NULL(spec->summary);
        TEST_ASSERT_TRUE(spec->name[0] != '\0');
        TEST_ASSERT_TRUE(spec->summary[0] != '\0');
        TEST_ASSERT_TRUE(spec->min_args <= spec->max_args);
        /* Every argument has to fit alongside the verb in one tokenised line. */
        TEST_ASSERT_TRUE(spec->max_args < DALI_CLI_MAX_TOKENS);
        /* A verb that takes arguments has to document them. */
        if (spec->max_args > 0u) {
            TEST_ASSERT_TRUE(spec->args[0] != '\0');
        }
    }
}

static void test_command_names_are_unique_and_findable(void)
{
    for (uint8_t i = 0u; i < dali_cli_command_count(); i++) {
        const DaliCliCommandSpec *spec = dali_cli_command_at(i);
        TEST_ASSERT_EQUAL_PTR(spec, dali_cli_command_find(spec->name));

        for (uint8_t j = (uint8_t)(i + 1u); j < dali_cli_command_count(); j++) {
            TEST_ASSERT_TRUE_MESSAGE(
                strcmp(spec->name, dali_cli_command_at(j)->name) != 0,
                "duplicate CLI verb name");
        }
    }
    TEST_ASSERT_NULL(dali_cli_command_find("no-such-verb"));
    TEST_ASSERT_NULL(dali_cli_command_find(NULL));
}

/*
 * A verb that dispatches on a fixed keyword declares those keywords in the
 * table, and the handler tests membership against that field. Both halves are
 * therefore one string. This test exists because the first version of the table
 * advertised "bus on|off" while the handler only accepted "bus check": help told
 * the operator to type a command that could not work, and nothing caught it.
 */
static void test_subcommands_appear_in_usage(void)
{
    for (uint8_t i = 0u; i < dali_cli_command_count(); i++) {
        const DaliCliCommandSpec *spec = dali_cli_command_at(i);
        if (spec->subcommands == NULL) {
            continue;
        }

        TEST_ASSERT_TRUE_MESSAGE(spec->max_args > 0u,
                                 "a verb with subcommands must take an argument");

        /* Every declared keyword must be visible in the usage line. */
        const char *pos = spec->subcommands;
        while (*pos != '\0') {
            while (*pos == ' ') { pos++; }
            const char *start = pos;
            while (*pos != '\0' && *pos != ' ') { pos++; }
            size_t len = (size_t)(pos - start);
            if (len == 0u) { break; }

            char word[DALI_CLI_MAX_TOKEN_LEN];
            TEST_ASSERT_TRUE(len < sizeof(word));
            memcpy(word, start, len);
            word[len] = '\0';

            TEST_ASSERT_TRUE_MESSAGE(dali_cli_has_subcommand(spec, word),
                                     "declared subcommand not recognised");
            TEST_ASSERT_NOT_NULL_MESSAGE(strstr(spec->args, word),
                                         "subcommand missing from usage line");
        }
    }
}

static void test_has_subcommand_rejects_non_members(void)
{
    const DaliCliCommandSpec *bus = dali_cli_command_for_id(DALI_CLI_CMD_BUS);
    TEST_ASSERT_TRUE(dali_cli_has_subcommand(bus, "check"));
    TEST_ASSERT_FALSE(dali_cli_has_subcommand(bus, "on"));
    TEST_ASSERT_FALSE(dali_cli_has_subcommand(bus, "chec"));
    TEST_ASSERT_FALSE(dali_cli_has_subcommand(bus, "checks"));
    TEST_ASSERT_FALSE(dali_cli_has_subcommand(bus, ""));
    TEST_ASSERT_FALSE(dali_cli_has_subcommand(bus, NULL));

    const DaliCliCommandSpec *capture = dali_cli_command_for_id(DALI_CLI_CMD_CAPTURE);
    TEST_ASSERT_TRUE(dali_cli_has_subcommand(capture, "start"));
    TEST_ASSERT_TRUE(dali_cli_has_subcommand(capture, "export"));
    TEST_ASSERT_FALSE(dali_cli_has_subcommand(capture, "stopp"));

    /* A verb whose first argument is a value declares no keywords at all. */
    TEST_ASSERT_FALSE(dali_cli_has_subcommand(
        dali_cli_command_for_id(DALI_CLI_CMD_LEVEL), "s3"));
    TEST_ASSERT_FALSE(dali_cli_has_subcommand(NULL, "check"));
}

/* Help is generated from the table, so this asserts the generator rather than a
 * hand-written list: a verb missing here means help skipped a real command. */
static void test_help_lists_every_verb(void)
{
    DaliCliOut out = capture_begin();
    dali_cli_print_help(&out);
    TEST_ASSERT_FALSE_MESSAGE(s_sink.truncated, "help output overflowed capture");

    for (uint8_t i = 0u; i < dali_cli_command_count(); i++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_capture, dali_cli_command_at(i)->name),
                                     "verb missing from help");
    }
}

static void test_usage_line_matches_table(void)
{
    DaliCliOut out = capture_begin();
    dali_cli_print_usage(&out, dali_cli_command_for_id(DALI_CLI_CMD_SCENE));
    TEST_ASSERT_EQUAL_STRING("usage: scene <addr|sN|gN|b> <0-15>\r\n", s_capture);

    out = capture_begin();
    dali_cli_print_usage(&out, dali_cli_command_for_id(DALI_CLI_CMD_STATS));
    TEST_ASSERT_EQUAL_STRING("usage: stats\r\n", s_capture);
}

/* ---------------------------------------------------------------------------
 * Resolve: verb lookup plus argument count
 * --------------------------------------------------------------------------*/

static void test_resolve_accepts_exact_arity(void)
{
    DaliCliTokens t;
    const DaliCliCommandSpec *spec = NULL;

    TEST_ASSERT_EQUAL(DALI_CLI_RESOLVE_OK, dali_cli_resolve("level s3 128", &t, &spec));
    TEST_ASSERT_EQUAL(DALI_CLI_CMD_LEVEL, spec->id);
    TEST_ASSERT_EQUAL_UINT8(3u, t.count);
}

static void test_resolve_reports_empty_line(void)
{
    DaliCliTokens t;
    const DaliCliCommandSpec *spec = NULL;
    TEST_ASSERT_EQUAL(DALI_CLI_RESOLVE_EMPTY, dali_cli_resolve("  \r\n", &t, &spec));
    TEST_ASSERT_NULL(spec);
}

static void test_resolve_reports_unknown_verb(void)
{
    DaliCliTokens t;
    const DaliCliCommandSpec *spec = NULL;
    TEST_ASSERT_EQUAL(DALI_CLI_RESOLVE_UNKNOWN, dali_cli_resolve("levl s3 1", &t, &spec));
    TEST_ASSERT_NULL(spec);
}

/*
 * The reason this layer exists: a handler that only checks it has enough
 * tokens accepts "level a1 100 junk" and acts on it. Extra tokens mean the
 * operator typed something other than what will be transmitted.
 */
static void test_resolve_rejects_trailing_tokens(void)
{
    DaliCliTokens t;
    const DaliCliCommandSpec *spec = NULL;

    TEST_ASSERT_EQUAL(DALI_CLI_RESOLVE_ARITY,
                      dali_cli_resolve("level a1 100 junk", &t, &spec));
    TEST_ASSERT_EQUAL(DALI_CLI_CMD_LEVEL, spec->id);

    TEST_ASSERT_EQUAL(DALI_CLI_RESOLVE_ARITY, dali_cli_resolve("stats now", &t, &spec));
    TEST_ASSERT_EQUAL(DALI_CLI_RESOLVE_ARITY, dali_cli_resolve("off s1 s2", &t, &spec));
    TEST_ASSERT_EQUAL(DALI_CLI_RESOLVE_ARITY,
                      dali_cli_resolve("raw2 FE00 len=16 wait", &t, &spec));
}

static void test_resolve_rejects_missing_arguments(void)
{
    DaliCliTokens t;
    const DaliCliCommandSpec *spec = NULL;

    TEST_ASSERT_EQUAL(DALI_CLI_RESOLVE_ARITY, dali_cli_resolve("level s3", &t, &spec));
    TEST_ASSERT_EQUAL(DALI_CLI_RESOLVE_ARITY, dali_cli_resolve("off", &t, &spec));
    TEST_ASSERT_EQUAL(DALI_CLI_RESOLVE_ARITY, dali_cli_resolve("devmem read 3 0", &t, &spec));
}

static void test_resolve_accepts_optional_arguments(void)
{
    DaliCliTokens t;
    const DaliCliCommandSpec *spec = NULL;

    TEST_ASSERT_EQUAL(DALI_CLI_RESOLVE_OK, dali_cli_resolve("queue", &t, &spec));
    TEST_ASSERT_EQUAL(DALI_CLI_RESOLVE_OK, dali_cli_resolve("queue reset", &t, &spec));
    TEST_ASSERT_EQUAL(DALI_CLI_RESOLVE_OK, dali_cli_resolve("raw FE00 len=16", &t, &spec));
    TEST_ASSERT_EQUAL(DALI_CLI_RESOLVE_OK,
                      dali_cli_resolve("raw FE00 len=16 wait", &t, &spec));
}

static void test_resolve_reports_malformed_line(void)
{
    DaliCliTokens t;
    const DaliCliCommandSpec *spec = NULL;
    TEST_ASSERT_EQUAL(DALI_CLI_RESOLVE_MALFORMED,
                      dali_cli_resolve("a b c d e f g h i", &t, &spec));
}

static void test_report_resolve_messages(void)
{
    DaliCliTokens t;
    const DaliCliCommandSpec *spec = NULL;

    DaliCliResolveResult r = dali_cli_resolve("levl s3", &t, &spec);
    DaliCliOut out = capture_begin();
    dali_cli_report_resolve(&out, r, &t, spec);
    TEST_ASSERT_EQUAL_STRING("unknown command: levl\r\n"
                             "type 'help' for commands\r\n", s_capture);

    r = dali_cli_resolve("scene s3", &t, &spec);
    out = capture_begin();
    dali_cli_report_resolve(&out, r, &t, spec);
    TEST_ASSERT_EQUAL_STRING("usage: scene <addr|sN|gN|b> <0-15>\r\n", s_capture);

    r = dali_cli_resolve("", &t, &spec);
    out = capture_begin();
    dali_cli_report_resolve(&out, r, &t, spec);
    TEST_ASSERT_EQUAL_STRING("", s_capture);
}

/* ---------------------------------------------------------------------------
 * Argument validation
 * --------------------------------------------------------------------------*/

static void test_parse_u32_decimal_and_hex(void)
{
    uint32_t v = 0u;
    TEST_ASSERT_TRUE(dali_cli_parse_u32("0", 255u, &v));
    TEST_ASSERT_EQUAL_UINT32(0u, v);
    TEST_ASSERT_TRUE(dali_cli_parse_u32("254", 255u, &v));
    TEST_ASSERT_EQUAL_UINT32(254u, v);
    TEST_ASSERT_TRUE(dali_cli_parse_u32("0xFE", 255u, &v));
    TEST_ASSERT_EQUAL_UINT32(254u, v);
    TEST_ASSERT_TRUE(dali_cli_parse_u32("0Xfe", 255u, &v));
    TEST_ASSERT_EQUAL_UINT32(254u, v);
}

/* A leading zero is decimal here. Reading "010" as octal would silently make a
 * DTR value or a memory offset eight instead of ten. */
static void test_parse_u32_leading_zero_is_decimal(void)
{
    uint32_t v = 0u;
    TEST_ASSERT_TRUE(dali_cli_parse_u32("010", 255u, &v));
    TEST_ASSERT_EQUAL_UINT32(10u, v);
}

static void test_parse_u32_rejects_junk_and_overflow(void)
{
    uint32_t v = 0u;
    TEST_ASSERT_FALSE(dali_cli_parse_u32("", 255u, &v));
    TEST_ASSERT_FALSE(dali_cli_parse_u32("0x", 255u, &v));
    TEST_ASSERT_FALSE(dali_cli_parse_u32("12x", 255u, &v));
    TEST_ASSERT_FALSE(dali_cli_parse_u32("-1", 255u, &v));
    TEST_ASSERT_FALSE(dali_cli_parse_u32(" 12", 255u, &v));
    TEST_ASSERT_FALSE(dali_cli_parse_u32("256", 255u, &v));
    TEST_ASSERT_FALSE(dali_cli_parse_u32("FE", 255u, &v));   /* hex needs 0x */
    TEST_ASSERT_FALSE(dali_cli_parse_u32("99999999999", UINT32_MAX, &v));
    TEST_ASSERT_FALSE(dali_cli_parse_u32(NULL, 255u, &v));
}

static void test_parse_u8_bounds(void)
{
    uint8_t v = 0u;
    TEST_ASSERT_TRUE(dali_cli_parse_u8("15", DALI_MAX_SCENE, &v));
    TEST_ASSERT_EQUAL_UINT8(15u, v);
    TEST_ASSERT_FALSE(dali_cli_parse_u8("16", DALI_MAX_SCENE, &v));
    TEST_ASSERT_TRUE(dali_cli_parse_u8("255", 255u, &v));
    TEST_ASSERT_EQUAL_UINT8(255u, v);
}

static void test_parse_target_forms(void)
{
    DaliTarget target;

    TEST_ASSERT_TRUE(dali_cli_parse_target("b", &target));
    TEST_ASSERT_EQUAL(DALI_ADDR_BROADCAST, target.type);
    TEST_ASSERT_TRUE(dali_cli_parse_target("broadcast", &target));
    TEST_ASSERT_EQUAL(DALI_ADDR_BROADCAST, target.type);

    TEST_ASSERT_TRUE(dali_cli_parse_target("g7", &target));
    TEST_ASSERT_EQUAL(DALI_ADDR_GROUP, target.type);
    TEST_ASSERT_EQUAL_UINT8(7u, target.address);

    TEST_ASSERT_TRUE(dali_cli_parse_target("s12", &target));
    TEST_ASSERT_EQUAL(DALI_ADDR_SHORT, target.type);
    TEST_ASSERT_EQUAL_UINT8(12u, target.address);

    TEST_ASSERT_TRUE(dali_cli_parse_target("12", &target));
    TEST_ASSERT_EQUAL(DALI_ADDR_SHORT, target.type);
    TEST_ASSERT_EQUAL_UINT8(12u, target.address);
}

static void test_parse_target_rejects_out_of_range(void)
{
    DaliTarget target;
    TEST_ASSERT_FALSE(dali_cli_parse_target("g16", &target));
    TEST_ASSERT_FALSE(dali_cli_parse_target("s64", &target));
    TEST_ASSERT_FALSE(dali_cli_parse_target("64", &target));
    TEST_ASSERT_FALSE(dali_cli_parse_target("g", &target));
    TEST_ASSERT_FALSE(dali_cli_parse_target("s", &target));
    TEST_ASSERT_FALSE(dali_cli_parse_target("x1", &target));
    TEST_ASSERT_FALSE(dali_cli_parse_target("", &target));
}

static void test_parse_short_addr_and_instance(void)
{
    uint8_t v = 0u;
    TEST_ASSERT_TRUE(dali_cli_parse_short_addr("3", &v));
    TEST_ASSERT_EQUAL_UINT8(3u, v);
    TEST_ASSERT_TRUE(dali_cli_parse_short_addr("s63", &v));
    TEST_ASSERT_EQUAL_UINT8(63u, v);
    TEST_ASSERT_FALSE(dali_cli_parse_short_addr("64", &v));

    TEST_ASSERT_TRUE(dali_cli_parse_instance("31", &v));
    TEST_ASSERT_EQUAL_UINT8(31u, v);
    TEST_ASSERT_FALSE(dali_cli_parse_instance("32", &v));
}

/*
 * 255 is MASK, not a level. Both spellings resolve to the same flagged value so
 * the handler picks the MASK builder rather than a DAPC level of 255, which the
 * frame builder refuses outright.
 */
static void test_parse_level_distinguishes_mask(void)
{
    DaliCliLevel level;

    TEST_ASSERT_TRUE(dali_cli_parse_level("0", &level));
    TEST_ASSERT_FALSE(level.is_mask);
    TEST_ASSERT_EQUAL_UINT8(0u, level.level);

    TEST_ASSERT_TRUE(dali_cli_parse_level("254", &level));
    TEST_ASSERT_FALSE(level.is_mask);
    TEST_ASSERT_EQUAL_UINT8(DALI_DAPC_MAX_LEVEL, level.level);

    TEST_ASSERT_TRUE(dali_cli_parse_level("255", &level));
    TEST_ASSERT_TRUE(level.is_mask);
    TEST_ASSERT_EQUAL_UINT8(DALI_DAPC_MASK_LEVEL, level.level);

    TEST_ASSERT_TRUE(dali_cli_parse_level("mask", &level));
    TEST_ASSERT_TRUE(level.is_mask);
    TEST_ASSERT_EQUAL_UINT8(DALI_DAPC_MASK_LEVEL, level.level);

    TEST_ASSERT_FALSE(dali_cli_parse_level("256", &level));
    TEST_ASSERT_FALSE(dali_cli_parse_level("MASK", &level));
}

static void test_parse_len_token(void)
{
    uint8_t bits = 0u;
    TEST_ASSERT_TRUE(dali_cli_parse_len_token("len=16", &bits));
    TEST_ASSERT_EQUAL_UINT8(16u, bits);
    TEST_ASSERT_TRUE(dali_cli_parse_len_token("len=24", &bits));
    TEST_ASSERT_EQUAL_UINT8(24u, bits);

    TEST_ASSERT_FALSE(dali_cli_parse_len_token("len=0", &bits));
    TEST_ASSERT_FALSE(dali_cli_parse_len_token("len=25", &bits));
    TEST_ASSERT_FALSE(dali_cli_parse_len_token("len=", &bits));
    TEST_ASSERT_FALSE(dali_cli_parse_len_token("16", &bits));
    TEST_ASSERT_FALSE(dali_cli_parse_len_token("length=16", &bits));
}

static void test_parse_raw_frame(void)
{
    DaliFrame frame;

    TEST_ASSERT_TRUE(dali_cli_parse_raw_frame("FE00", "len=16", &frame));
    TEST_ASSERT_EQUAL_HEX32(0xFE00u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_TRUE(dali_cli_parse_raw_frame("0xfe00", "len=16", &frame));
    TEST_ASSERT_EQUAL_HEX32(0xFE00u, frame.data);

    TEST_ASSERT_TRUE(dali_cli_parse_raw_frame("A3FF00", "len=24", &frame));
    TEST_ASSERT_EQUAL_HEX32(0xA3FF00u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(24u, frame.bit_length);
}

/* A value wider than the stated frame would be transmitted as a differently
 * framed command, so the width is a bound rather than a hint. */
static void test_parse_raw_frame_rejects_over_wide_value(void)
{
    DaliFrame frame;
    TEST_ASSERT_FALSE(dali_cli_parse_raw_frame("1FE00", "len=16", &frame));
    TEST_ASSERT_FALSE(dali_cli_parse_raw_frame("100", "len=8", &frame));
    TEST_ASSERT_TRUE(dali_cli_parse_raw_frame("FF", "len=8", &frame));
}

static void test_parse_raw_frame_rejects_malformed(void)
{
    DaliFrame frame;
    TEST_ASSERT_FALSE(dali_cli_parse_raw_frame("", "len=16", &frame));
    TEST_ASSERT_FALSE(dali_cli_parse_raw_frame("0x", "len=16", &frame));
    TEST_ASSERT_FALSE(dali_cli_parse_raw_frame("FE0G", "len=16", &frame));
    TEST_ASSERT_FALSE(dali_cli_parse_raw_frame("FE00", "16", &frame));
    TEST_ASSERT_FALSE(dali_cli_parse_raw_frame(NULL, "len=16", &frame));
}

/* ---------------------------------------------------------------------------
 * Named control-gear tables
 * --------------------------------------------------------------------------*/

static void test_query_table_entries_are_addressed_queries(void)
{
    TEST_ASSERT_TRUE(dali_cli_query_count() > 0u);

    for (uint8_t i = 0u; i < dali_cli_query_count(); i++) {
        const DaliCliGearCommand *spec = dali_cli_query_at(i);
        TEST_ASSERT_EQUAL_PTR(spec, dali_cli_query_find(spec->name));

        const DaliCommandInfo *cmd = dali_command_lookup(spec->id);
        TEST_ASSERT_NOT_NULL_MESSAGE(cmd, "query name maps to no command");
        TEST_ASSERT_EQUAL_MESSAGE(DALI_CMD_FRAME_16BIT, cmd->frame_kind,
                                  "query must be an addressed 16-bit command");
        TEST_ASSERT_NOT_EQUAL_MESSAGE(DALI_RESP_NONE, cmd->response_kind,
                                      "query must expect a reply");

        /* A ranged opcode selects its target with the parameter, so a table
         * entry without one would always address element zero. */
        bool ranged = cmd->opcode_first != cmd->opcode_last;
        TEST_ASSERT_EQUAL_MESSAGE(ranged, spec->needs_param,
                                  "ranged query needs a parameter");
        if (ranged) {
            TEST_ASSERT_EQUAL_UINT8((uint8_t)(cmd->opcode_last - cmd->opcode_first),
                                    spec->max_param);
        }
    }
}

static void test_config_table_entries_are_configuration_commands(void)
{
    for (uint8_t i = 0u; i < dali_cli_config_count(); i++) {
        const DaliCliGearCommand *spec = dali_cli_config_at(i);
        TEST_ASSERT_EQUAL_PTR(spec, dali_cli_config_find(spec->name));

        const DaliCommandInfo *cmd = dali_command_lookup(spec->id);
        TEST_ASSERT_NOT_NULL(cmd);
        TEST_ASSERT_EQUAL(DALI_CMD_FRAME_16BIT, cmd->frame_kind);
        TEST_ASSERT_EQUAL_MESSAGE(DALI_RESP_NONE, cmd->response_kind,
                                  "configuration command expects no reply");
        TEST_ASSERT_TRUE_MESSAGE(cmd->send_twice,
                                 "configuration command must be sent twice");

        bool ranged = cmd->opcode_first != cmd->opcode_last;
        TEST_ASSERT_EQUAL_MESSAGE(ranged, spec->needs_param,
                                  "ranged configuration needs a parameter");

        /* The uses_dtr0 column drives the config-dtr0 verb, so it has to agree
         * with the shared stack rather than be maintained separately. */
        TEST_ASSERT_EQUAL_MESSAGE(dali_control_config_uses_dtr0(spec->id),
                                  spec->uses_dtr0,
                                  "uses_dtr0 disagrees with dali_control");
    }
}

static void test_special_table_entries_are_special_frames(void)
{
    for (uint8_t i = 0u; i < dali_cli_special_count(); i++) {
        const DaliCliGearCommand *spec = dali_cli_special_at(i);
        TEST_ASSERT_EQUAL_PTR(spec, dali_cli_special_find(spec->name));

        const DaliCommandInfo *cmd = dali_command_lookup(spec->id);
        TEST_ASSERT_NOT_NULL(cmd);
        TEST_ASSERT_EQUAL_MESSAGE(DALI_CMD_FRAME_SPECIAL, cmd->frame_kind,
                                  "special name maps to a non-special frame");
    }
}

static void test_gear_table_names_are_unique(void)
{
    for (uint8_t i = 0u; i < dali_cli_query_count(); i++) {
        for (uint8_t j = (uint8_t)(i + 1u); j < dali_cli_query_count(); j++) {
            TEST_ASSERT_TRUE(strcmp(dali_cli_query_at(i)->name,
                                    dali_cli_query_at(j)->name) != 0);
        }
    }
    for (uint8_t i = 0u; i < dali_cli_config_count(); i++) {
        for (uint8_t j = (uint8_t)(i + 1u); j < dali_cli_config_count(); j++) {
            TEST_ASSERT_TRUE(strcmp(dali_cli_config_at(i)->name,
                                    dali_cli_config_at(j)->name) != 0);
        }
    }
    for (uint8_t i = 0u; i < dali_cli_special_count(); i++) {
        for (uint8_t j = (uint8_t)(i + 1u); j < dali_cli_special_count(); j++) {
            TEST_ASSERT_TRUE(strcmp(dali_cli_special_at(i)->name,
                                    dali_cli_special_at(j)->name) != 0);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Device-type tables
 * --------------------------------------------------------------------------*/

static void test_dt6_table_is_well_formed(void)
{
    TEST_ASSERT_TRUE(dali_cli_dt6_count() > 0u);

    for (uint8_t i = 0u; i < dali_cli_dt6_count(); i++) {
        const DaliCliDtCommand *spec = dali_cli_dt6_at(i);
        TEST_ASSERT_EQUAL_PTR(spec, dali_cli_dt6_find(spec->name));
        TEST_ASSERT_NOT_NULL_MESSAGE(spec->build, "DT6 entry needs a builder");
        TEST_ASSERT_TRUE(spec->dtr_count <= DALI_DT6_MAX_DTR_BYTES);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(DALI_CLI_DT_COLOUR16, spec->kind,
                                      "the 16-bit colour read is DT8-only");
        if (spec->dtr_count > 0u) {
            TEST_ASSERT_NOT_NULL_MESSAGE(spec->dtr_help,
                                         "a DTR-taking entry must say what they mean");
        }
        if (spec->kind == DALI_CLI_DT_QUERY) {
            TEST_ASSERT_NOT_EQUAL(DALI_RESP_NONE, spec->response_kind);
        } else {
            TEST_ASSERT_EQUAL(DALI_RESP_NONE, spec->response_kind);
        }
        /* Every command must fit in one sequence alongside its enable. */
        TEST_ASSERT_TRUE((uint8_t)(spec->dtr_count + 2u) <= DALI_SEQUENCE_MAX_STEPS);
    }
}

static void test_dt8_table_is_well_formed(void)
{
    TEST_ASSERT_TRUE(dali_cli_dt8_count() > 0u);

    for (uint8_t i = 0u; i < dali_cli_dt8_count(); i++) {
        const DaliCliDtCommand *spec = dali_cli_dt8_at(i);
        TEST_ASSERT_EQUAL_PTR(spec, dali_cli_dt8_find(spec->name));
        TEST_ASSERT_TRUE(spec->dtr_count <= DALI_DT8_MAX_DTR_BYTES);

        if (spec->kind == DALI_CLI_DT_COLOUR16) {
            /* This one is built by the four-step colour sequence, not by a
             * plain frame builder, and it takes a selector name. */
            TEST_ASSERT_NULL(spec->build);
            TEST_ASSERT_EQUAL_UINT8(1u, spec->dtr_count);
            continue;
        }

        TEST_ASSERT_NOT_NULL(spec->build);
        TEST_ASSERT_TRUE((uint8_t)(spec->dtr_count + 2u) <= DALI_SEQUENCE_MAX_STEPS);
        if (spec->kind == DALI_CLI_DT_QUERY) {
            TEST_ASSERT_NOT_EQUAL(DALI_RESP_NONE, spec->response_kind);
        } else {
            TEST_ASSERT_EQUAL(DALI_RESP_NONE, spec->response_kind);
        }
    }
}

static void test_dt6_builders_emit_addressed_frames(void)
{
    const DaliCliDtCommand *spec = dali_cli_dt6_find("select-curve");
    TEST_ASSERT_NOT_NULL(spec);
    TEST_ASSERT_EQUAL(DALI_CLI_DT_CONFIG, spec->kind);
    TEST_ASSERT_EQUAL_UINT8(1u, spec->dtr_count);

    /* Short address 3, command selector set: 0x07; SELECT DIMMING CURVE = 0xE3. */
    DaliFrame frame = spec->build(3u);
    TEST_ASSERT_EQUAL_UINT8(DALI_FORWARD_FRAME_BITS, frame.bit_length);
    TEST_ASSERT_EQUAL_HEX32(0x07E3u, frame.data);
}

static void test_dt8_selector_table(void)
{
    TEST_ASSERT_TRUE(dali_cli_dt8_selector_count() > 0u);

    const DaliCliDt8Selector *tc = dali_cli_dt8_selector_find("tc");
    TEST_ASSERT_NOT_NULL(tc);
    TEST_ASSERT_EQUAL(DALI_DT8_VALUE_COLOUR_TEMP_TC, tc->selector);

    TEST_ASSERT_NOT_NULL(dali_cli_dt8_selector_find("x"));
    TEST_ASSERT_NOT_NULL(dali_cli_dt8_selector_find("red"));
    TEST_ASSERT_NULL(dali_cli_dt8_selector_find("magenta"));

    for (uint8_t i = 0u; i < dali_cli_dt8_selector_count(); i++) {
        const DaliCliDt8Selector *sel = dali_cli_dt8_selector_at(i);
        TEST_ASSERT_EQUAL_PTR(sel, dali_cli_dt8_selector_find(sel->name));
    }
}

/* ---------------------------------------------------------------------------
 * Part 103 tables
 * --------------------------------------------------------------------------*/

static void test_iquery_table_is_well_formed(void)
{
    TEST_ASSERT_TRUE(dali_cli_iquery_count() > 0u);

    for (uint8_t i = 0u; i < dali_cli_iquery_count(); i++) {
        const DaliCliInstanceQuery *spec = dali_cli_iquery_at(i);
        TEST_ASSERT_EQUAL_PTR(spec, dali_cli_iquery_find(spec->name));
        TEST_ASSERT_NOT_NULL(spec->build);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(DALI_RESP_NONE, spec->response_kind,
                                      "a query must expect a reply");

        /* Every instance query is a 24-bit instance frame addressed to a real
         * short address and instance. */
        DaliFrame frame = spec->build(3u, 1u);
        TEST_ASSERT_EQUAL_UINT8(DALI_EXTENDED_FRAME_BITS, frame.bit_length);
        TEST_ASSERT_EQUAL_HEX32(0x07u, (frame.data >> 16u) & 0xFFu);
        TEST_ASSERT_EQUAL_HEX32(0x01u, (frame.data >> 8u) & 0xFFu);

        for (uint8_t j = (uint8_t)(i + 1u); j < dali_cli_iquery_count(); j++) {
            TEST_ASSERT_TRUE(strcmp(spec->name, dali_cli_iquery_at(j)->name) != 0);
        }
    }
}

/* An out-of-range address must not produce a sendable frame. */
static void test_iquery_builders_reject_bad_arguments(void)
{
    const DaliCliInstanceQuery *spec = dali_cli_iquery_find("type");
    TEST_ASSERT_NOT_NULL(spec);
    TEST_ASSERT_EQUAL_UINT8(0u, spec->build(64u, 1u).bit_length);
    TEST_ASSERT_EQUAL_UINT8(0u, spec->build(3u, 32u).bit_length);
}

static void test_iconfig_table_is_well_formed(void)
{
    TEST_ASSERT_TRUE(dali_cli_iconfig_count() > 0u);

    for (uint8_t i = 0u; i < dali_cli_iconfig_count(); i++) {
        const DaliCliInstanceConfig *spec = dali_cli_iconfig_at(i);
        TEST_ASSERT_EQUAL_PTR(spec, dali_cli_iconfig_find(spec->name));
        TEST_ASSERT_NOT_NULL(spec->build);
        TEST_ASSERT_TRUE(spec->dtr_count <= DALI_INPUT_CONFIG_MAX_DTR_BYTES);
        TEST_ASSERT_TRUE((uint8_t)(spec->dtr_count + 1u) <= DALI_SEQUENCE_MAX_STEPS);
        if (spec->dtr_count > 0u) {
            TEST_ASSERT_NOT_NULL(spec->dtr_help);
        }

        DaliFrame frame = spec->build(3u, 1u);
        TEST_ASSERT_EQUAL_UINT8(DALI_EXTENDED_FRAME_BITS, frame.bit_length);

        for (uint8_t j = (uint8_t)(i + 1u); j < dali_cli_iconfig_count(); j++) {
            TEST_ASSERT_TRUE(strcmp(spec->name, dali_cli_iconfig_at(j)->name) != 0);
        }
    }
}

/*
 * CATCH MOVEMENT and CANCEL HOLD TIMER are the only Part 303 instance commands
 * sent once; everything else in the table is a configuration write and must be
 * marked send-twice, or the device discards it.
 */
static void test_iconfig_send_twice_matches_standard(void)
{
    TEST_ASSERT_FALSE(dali_cli_iconfig_find("occ-catch-movement")->send_twice);
    TEST_ASSERT_FALSE(dali_cli_iconfig_find("occ-cancel-hold")->send_twice);

    for (uint8_t i = 0u; i < dali_cli_iconfig_count(); i++) {
        const DaliCliInstanceConfig *spec = dali_cli_iconfig_at(i);
        if (strcmp(spec->name, "occ-catch-movement") == 0 ||
            strcmp(spec->name, "occ-cancel-hold") == 0) {
            continue;
        }
        TEST_ASSERT_TRUE_MESSAGE(spec->send_twice,
                                 "instance configuration must be sent twice");
    }
}

static void test_lunatone_table(void)
{
    TEST_ASSERT_EQUAL_UINT8(dali_lunatone_command_count(), dali_cli_lunatone_count());

    for (uint8_t i = 0u; i < dali_cli_lunatone_count(); i++) {
        const DaliCliLunatoneCommand *spec = dali_cli_lunatone_at(i);
        TEST_ASSERT_EQUAL_PTR(spec, dali_cli_lunatone_find(spec->name));
        TEST_ASSERT_NOT_NULL(dali_lunatone_command_lookup(spec->id));
    }
    TEST_ASSERT_NULL(dali_cli_lunatone_find("nope"));
}

/* ---------------------------------------------------------------------------
 * Table listing
 * --------------------------------------------------------------------------*/

static void test_table_names_round_trip(void)
{
    for (uint8_t i = 0u; i < (uint8_t)DALI_CLI_TABLE_COUNT; i++) {
        const char *name = dali_cli_table_name((DaliCliTableId)i);
        TEST_ASSERT_NOT_NULL(name);

        DaliCliTableId found;
        TEST_ASSERT_TRUE(dali_cli_table_find(name, &found));
        TEST_ASSERT_EQUAL((DaliCliTableId)i, found);
    }

    DaliCliTableId found;
    TEST_ASSERT_FALSE(dali_cli_table_find("nope", &found));
    TEST_ASSERT_NULL(dali_cli_table_name(DALI_CLI_TABLE_COUNT));
}

static void test_print_table_lists_every_entry(void)
{
    DaliCliOut out = capture_begin();
    dali_cli_print_table(&out, DALI_CLI_TABLE_QUERY);
    TEST_ASSERT_FALSE(s_sink.truncated);
    for (uint8_t i = 0u; i < dali_cli_query_count(); i++) {
        TEST_ASSERT_NOT_NULL(strstr(s_capture, dali_cli_query_at(i)->name));
    }

    out = capture_begin();
    dali_cli_print_table(&out, DALI_CLI_TABLE_DT8);
    TEST_ASSERT_FALSE(s_sink.truncated);
    for (uint8_t i = 0u; i < dali_cli_dt8_count(); i++) {
        TEST_ASSERT_NOT_NULL(strstr(s_capture, dali_cli_dt8_at(i)->name));
    }

    out = capture_begin();
    dali_cli_print_table(&out, DALI_CLI_TABLE_ICONFIG);
    TEST_ASSERT_FALSE(s_sink.truncated);
    for (uint8_t i = 0u; i < dali_cli_iconfig_count(); i++) {
        TEST_ASSERT_NOT_NULL(strstr(s_capture, dali_cli_iconfig_at(i)->name));
    }
}

static void test_print_table_marks_send_twice_specials(void)
{
    DaliCliOut out = capture_begin();
    dali_cli_print_table(&out, DALI_CLI_TABLE_SPECIAL);
    TEST_ASSERT_NOT_NULL(strstr(s_capture, "initialise <0-255> [send twice]"));
    TEST_ASSERT_NOT_NULL(strstr(s_capture, "compare [waits reply]"));
}

/* ---------------------------------------------------------------------------
 * Response formatting
 * --------------------------------------------------------------------------*/

static DaliFrame backward(uint8_t raw)
{
    return (DaliFrame){ .data = raw, .bit_length = DALI_BACKWARD_FRAME_BITS };
}

static void test_print_response_status_decodes_fields(void)
{
    DaliFrame reply = backward(0x04u);  /* lamp arc power on */
    DaliCliOut out = capture_begin();
    dali_cli_print_response(&out, "status", DALI_RESP_STATUS, &reply);

    TEST_ASSERT_NOT_NULL(strstr(s_capture, "status: 0x04 arc-on\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(s_capture, "Lamp arc power on:     YES"));
    TEST_ASSERT_NOT_NULL(strstr(s_capture, "Ballast failure:       no"));
}

/* ---------------------------------------------------------------------------
 * Single-line response formatting
 *
 * The ESPHome console's whole answer is one Home Assistant text state, so it
 * formats replies through these rather than through the printing forms. The
 * values are written from the standard's response definitions, not read back
 * from the formatter.
 * --------------------------------------------------------------------------*/

static void test_format_status_names_only_the_set_flags(void)
{
    char buf[DALI_CLI_STATUS_LINE_MAX];

    TEST_ASSERT_EQUAL_UINT(strlen("0x00 none"),
                           dali_cli_format_status(buf, sizeof(buf), 0x00u));
    TEST_ASSERT_EQUAL_STRING("0x00 none", buf);

    /* bit 1 lamp failure, bit 2 lamp arc power on */
    dali_cli_format_status(buf, sizeof(buf), 0x06u);
    TEST_ASSERT_EQUAL_STRING("0x06 lamp-fail,arc-on", buf);

    /* bit 6 missing short address — the one a freshly reset gear reports */
    dali_cli_format_status(buf, sizeof(buf), 0x40u);
    TEST_ASSERT_EQUAL_STRING("0x40 no-addr", buf);
}

/*
 * The buffer has to hold the worst case, or a gear reporting every fault at
 * once would have its status silently clipped to the faults that fitted.
 */
static void test_format_status_fits_all_eight_flags(void)
{
    char buf[DALI_CLI_STATUS_LINE_MAX];
    size_t len = dali_cli_format_status(buf, sizeof(buf), 0xFFu);

    TEST_ASSERT_LESS_THAN_UINT(sizeof(buf) - 1u, len);
    TEST_ASSERT_EQUAL_STRING(
        "0xFF ballast-fail,lamp-fail,arc-on,limit-err,fading,reset,no-addr,power-fail",
        buf);
}

static void test_format_response_matches_the_printed_forms(void)
{
    char buf[DALI_CLI_RESPONSE_LINE_MAX];

    DaliFrame yes = backward(DALI_YES_RESPONSE);
    dali_cli_format_response(buf, sizeof(buf), "present", DALI_RESP_YES_NO, &yes);
    TEST_ASSERT_EQUAL_STRING("present: yes (0xFF)", buf);

    DaliFrame value = backward(0x2Au);
    dali_cli_format_response(buf, sizeof(buf), "actual", DALI_RESP_UINT8, &value);
    TEST_ASSERT_EQUAL_STRING("actual: 42 (0x2A)", buf);

    dali_cli_format_response(buf, sizeof(buf), "groups-0-7", DALI_RESP_BITSET8, &value);
    TEST_ASSERT_EQUAL_STRING("groups-0-7: 0x2A", buf);

    DaliFrame fade = backward(0x37u);
    dali_cli_format_response(buf, sizeof(buf), "fade", DALI_RESP_FADE_TIME_RATE, &fade);
    TEST_ASSERT_EQUAL_STRING("fade: fade_time=3 fade_rate=7 (0x37)", buf);

    DaliFrame status = backward(0x04u);
    dali_cli_format_response(buf, sizeof(buf), "status", DALI_RESP_STATUS, &status);
    TEST_ASSERT_EQUAL_STRING("status: 0x04 arc-on", buf);
}

/* A reply that never arrived must not be formatted as a plausible number. */
static void test_format_response_reports_malformed(void)
{
    char buf[DALI_CLI_RESPONSE_LINE_MAX];

    DaliFrame forward = { .data = 0xFF00u, .bit_length = DALI_FORWARD_FRAME_BITS };
    dali_cli_format_response(buf, sizeof(buf), "actual", DALI_RESP_UINT8, &forward);
    TEST_ASSERT_EQUAL_STRING("actual: malformed reply", buf);

    dali_cli_format_response(buf, sizeof(buf), "actual", DALI_RESP_UINT8, NULL);
    TEST_ASSERT_EQUAL_STRING("actual: malformed reply", buf);
}

/*
 * A short buffer truncates rather than overruns, and always stays terminated.
 * The console's result buffer is fixed, so this is the behaviour it relies on.
 */
static void test_format_response_truncates_within_bounds(void)
{
    char buf[8] = { 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x' };
    DaliFrame reply = backward(0x2Au);

    size_t len = dali_cli_format_response(buf, sizeof(buf), "actual",
                                          DALI_RESP_UINT8, &reply);
    TEST_ASSERT_EQUAL_UINT(sizeof(buf) - 1u, len);
    TEST_ASSERT_EQUAL_CHAR('\0', buf[sizeof(buf) - 1u]);
    TEST_ASSERT_EQUAL_STRING("actual:", buf);

    /* A zero-capacity buffer writes nothing at all. */
    TEST_ASSERT_EQUAL_UINT(0u, dali_cli_format_response(buf, 0u, "actual",
                                                        DALI_RESP_UINT8, &reply));
}

/*
 * Which specials a front end without a guarded commissioning workflow refuses.
 *
 * The list is asserted in both directions: a name wrongly marked would either
 * put a bus-wide readdressing behind a Home Assistant text box, or withhold
 * TERMINATE from the operator who needs it to undo one.
 */
static void test_special_commissioning_set(void)
{
    static const char *restricted[] = {
        "initialise", "randomize", "search-h", "search-m", "search-l",
        "program-short", "withdraw", "write-memory", "write-memory-nr",
    };
    static const char *allowed[] = {
        "terminate", "compare", "ping", "verify-short", "query-short",
        "enable-type", "dtr0", "dtr1", "dtr2",
    };

    for (size_t i = 0u; i < sizeof(restricted) / sizeof(restricted[0]); i++) {
        const DaliCliGearCommand *spec = dali_cli_special_find(restricted[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(spec, restricted[i]);
        TEST_ASSERT_TRUE_MESSAGE(dali_cli_special_is_commissioning(spec->id),
                                 restricted[i]);
    }

    for (size_t i = 0u; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        const DaliCliGearCommand *spec = dali_cli_special_find(allowed[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(spec, allowed[i]);
        TEST_ASSERT_FALSE_MESSAGE(dali_cli_special_is_commissioning(spec->id),
                                  allowed[i]);
    }

    /* Every name in the table is accounted for above. */
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(sizeof(restricted) / sizeof(restricted[0]) +
                                      sizeof(allowed) / sizeof(allowed[0])),
                            dali_cli_special_count());
}

static void test_print_response_yes_no(void)
{
    DaliFrame yes = backward(DALI_YES_RESPONSE);
    DaliCliOut out = capture_begin();
    dali_cli_print_response(&out, "present", DALI_RESP_YES_NO, &yes);
    TEST_ASSERT_EQUAL_STRING("present: yes (0xFF)\r\n", s_capture);

    DaliFrame no = backward(0x00u);
    out = capture_begin();
    dali_cli_print_response(&out, "present", DALI_RESP_YES_NO, &no);
    TEST_ASSERT_EQUAL_STRING("present: no (0x00)\r\n", s_capture);
}

static void test_print_response_uint8_and_bitset(void)
{
    DaliFrame reply = backward(0x2Au);

    DaliCliOut out = capture_begin();
    dali_cli_print_response(&out, "actual", DALI_RESP_UINT8, &reply);
    TEST_ASSERT_EQUAL_STRING("actual: 42 (0x2A)\r\n", s_capture);

    out = capture_begin();
    dali_cli_print_response(&out, "groups-0-7", DALI_RESP_BITSET8, &reply);
    TEST_ASSERT_EQUAL_STRING("groups-0-7: 0x2A\r\n", s_capture);
}

static void test_print_response_fade_splits_nibbles(void)
{
    DaliFrame reply = backward(0x37u);
    DaliCliOut out = capture_begin();
    dali_cli_print_response(&out, "fade", DALI_RESP_FADE_TIME_RATE, &reply);
    TEST_ASSERT_EQUAL_STRING("fade: fade_time=3 fade_rate=7 (0x37)\r\n", s_capture);
}

/*
 * A forward-length frame is not a backward frame. Printing it as a number would
 * hand the operator a plausible value for a reply that never arrived.
 */
static void test_print_response_rejects_non_backward_frame(void)
{
    DaliFrame wrong = { .data = 0xFF00u, .bit_length = DALI_FORWARD_FRAME_BITS };
    DaliCliOut out = capture_begin();
    dali_cli_print_response(&out, "actual", DALI_RESP_UINT8, &wrong);
    TEST_ASSERT_EQUAL_STRING("actual: malformed reply\r\n", s_capture);

    out = capture_begin();
    dali_cli_print_response(&out, "actual", DALI_RESP_UINT8, NULL);
    TEST_ASSERT_EQUAL_STRING("actual: malformed reply\r\n", s_capture);
}

static void test_print_tx_and_error_results(void)
{
    DaliCliOut out = capture_begin();
    dali_cli_print_tx_result(&out, "level", DALI_OK);
    TEST_ASSERT_EQUAL_STRING("level: OK\r\n", s_capture);

    out = capture_begin();
    dali_cli_print_tx_result(&out, "level", DALI_ERR_QUEUE_FULL);
    TEST_ASSERT_EQUAL_STRING("level: queue full\r\n", s_capture);

    out = capture_begin();
    dali_cli_print_error(&out, "status", DALI_ERR_TIMEOUT);
    TEST_ASSERT_EQUAL_STRING("status: timeout\r\n", s_capture);

    out = capture_begin();
    dali_cli_print_error(&out, "status", DALI_ERR_INTERVENED);
    TEST_ASSERT_EQUAL_STRING("status: ERR 10\r\n", s_capture);
}

static void test_print_frame_widths(void)
{
    DaliFrame f16 = { .data = 0x00FEu, .bit_length = DALI_FORWARD_FRAME_BITS };
    DaliCliOut out = capture_begin();
    dali_cli_print_frame(&out, "TX: ", &f16);
    TEST_ASSERT_EQUAL_STRING("TX: 0x00FE (16-bit)\r\n", s_capture);

    DaliFrame f24 = { .data = 0x0103A5u, .bit_length = DALI_EXTENDED_FRAME_BITS };
    out = capture_begin();
    dali_cli_print_frame(&out, "RX: ", &f24);
    TEST_ASSERT_EQUAL_STRING("RX: 0x0103A5 (24-bit)\r\n", s_capture);

    DaliFrame f8 = backward(0x0Fu);
    out = capture_begin();
    dali_cli_print_frame(&out, "", &f8);
    TEST_ASSERT_EQUAL_STRING("0x0F (8-bit)\r\n", s_capture);
}

static void test_print_memory_block(void)
{
    const uint8_t data[] = { 0x00u, 0x1Fu, 0xA5u };
    DaliCliOut out = capture_begin();
    dali_cli_print_memory_block(&out, 0u, 0x03u, data, 3u);
    TEST_ASSERT_EQUAL_STRING("  bank 0 offset 0x03: 00 1F A5\r\n", s_capture);

    out = capture_begin();
    dali_cli_print_memory_block(&out, 0u, 0x03u, data, 0u);
    TEST_ASSERT_EQUAL_STRING("", s_capture);
}

/* A sink that runs out of room says so rather than reporting a clipped line. */
static void test_buffer_sink_reports_truncation(void)
{
    char small[8];
    DaliCliBufferSink sink;
    dali_cli_buffer_sink_init(&sink, small, sizeof(small));
    DaliCliOut out = dali_cli_buffer_out(&sink);

    dali_cli_write(&out, "1234");
    TEST_ASSERT_FALSE(sink.truncated);
    dali_cli_write(&out, "5678901");
    TEST_ASSERT_TRUE(sink.truncated);
    TEST_ASSERT_EQUAL_STRING("1234567", small);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_tokenize_splits_on_spaces);
    RUN_TEST(test_tokenize_collapses_runs_and_tabs);
    RUN_TEST(test_tokenize_strips_line_endings);
    RUN_TEST(test_tokenize_empty_and_blank_lines);
    RUN_TEST(test_tokenize_rejects_too_many_tokens);
    RUN_TEST(test_tokenize_rejects_over_long_token);
    RUN_TEST(test_tokenize_accepts_longest_valid_token);

    RUN_TEST(test_command_table_covers_every_id);
    RUN_TEST(test_command_table_entries_are_well_formed);
    RUN_TEST(test_command_names_are_unique_and_findable);
    RUN_TEST(test_subcommands_appear_in_usage);
    RUN_TEST(test_has_subcommand_rejects_non_members);
    RUN_TEST(test_help_lists_every_verb);
    RUN_TEST(test_usage_line_matches_table);

    RUN_TEST(test_resolve_accepts_exact_arity);
    RUN_TEST(test_resolve_reports_empty_line);
    RUN_TEST(test_resolve_reports_unknown_verb);
    RUN_TEST(test_resolve_rejects_trailing_tokens);
    RUN_TEST(test_resolve_rejects_missing_arguments);
    RUN_TEST(test_resolve_accepts_optional_arguments);
    RUN_TEST(test_resolve_reports_malformed_line);
    RUN_TEST(test_report_resolve_messages);

    RUN_TEST(test_parse_u32_decimal_and_hex);
    RUN_TEST(test_parse_u32_leading_zero_is_decimal);
    RUN_TEST(test_parse_u32_rejects_junk_and_overflow);
    RUN_TEST(test_parse_u8_bounds);
    RUN_TEST(test_parse_target_forms);
    RUN_TEST(test_parse_target_rejects_out_of_range);
    RUN_TEST(test_parse_short_addr_and_instance);
    RUN_TEST(test_parse_level_distinguishes_mask);
    RUN_TEST(test_parse_len_token);
    RUN_TEST(test_parse_raw_frame);
    RUN_TEST(test_parse_raw_frame_rejects_over_wide_value);
    RUN_TEST(test_parse_raw_frame_rejects_malformed);

    RUN_TEST(test_query_table_entries_are_addressed_queries);
    RUN_TEST(test_config_table_entries_are_configuration_commands);
    RUN_TEST(test_special_table_entries_are_special_frames);
    RUN_TEST(test_gear_table_names_are_unique);

    RUN_TEST(test_dt6_table_is_well_formed);
    RUN_TEST(test_dt8_table_is_well_formed);
    RUN_TEST(test_dt6_builders_emit_addressed_frames);
    RUN_TEST(test_dt8_selector_table);

    RUN_TEST(test_iquery_table_is_well_formed);
    RUN_TEST(test_iquery_builders_reject_bad_arguments);
    RUN_TEST(test_iconfig_table_is_well_formed);
    RUN_TEST(test_iconfig_send_twice_matches_standard);
    RUN_TEST(test_lunatone_table);

    RUN_TEST(test_table_names_round_trip);
    RUN_TEST(test_print_table_lists_every_entry);
    RUN_TEST(test_print_table_marks_send_twice_specials);

    RUN_TEST(test_special_commissioning_set);

    RUN_TEST(test_format_status_names_only_the_set_flags);
    RUN_TEST(test_format_status_fits_all_eight_flags);
    RUN_TEST(test_format_response_matches_the_printed_forms);
    RUN_TEST(test_format_response_reports_malformed);
    RUN_TEST(test_format_response_truncates_within_bounds);

    RUN_TEST(test_print_response_status_decodes_fields);
    RUN_TEST(test_print_response_yes_no);
    RUN_TEST(test_print_response_uint8_and_bitset);
    RUN_TEST(test_print_response_fade_splits_nibbles);
    RUN_TEST(test_print_response_rejects_non_backward_frame);
    RUN_TEST(test_print_tx_and_error_results);
    RUN_TEST(test_print_frame_widths);
    RUN_TEST(test_print_memory_block);
    RUN_TEST(test_buffer_sink_reports_truncation);

    return UNITY_END();
}
