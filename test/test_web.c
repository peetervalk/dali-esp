#include "unity.h"
#include "dali_web.h"

#include <stdio.h>
#include <string.h>

/*
 * Vectors for dali_web: the request-head parser, routing with its Host and
 * Origin checks, the response heads, and the WebSocket frame codec.
 *
 * Where a standard publishes a vector, the vector is used as published rather
 * than re-derived from this implementation: FIPS 180 for SHA-1, RFC 4648 for
 * base64, RFC 6455 §1.3 for the accept value and §5.7 for frames. The SHA-1
 * padding-boundary digests and the second accept value come from Python's
 * hashlib, not from this code.
 */

void setUp(void) {}
void tearDown(void) {}

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------*/

static const char *const k_names[] = {
    "192.168.1.50", "dali-2k", "dali-2k.local", "dali.lan", "[fe80::1]",
};
static const DaliWebHostPolicy k_policy = { k_names, 5u, 8080u };

/* Feed `len` bytes until the parser decides. *consumed is how many it took. */
static DaliWebRequestState feed_bytes(DaliWebRequest *req, const char *text, size_t len,
                                      size_t *consumed)
{
    dali_web_request_init(req);
    DaliWebRequestState state = DALI_WEB_REQUEST_MORE;
    size_t i = 0u;
    while (i < len) {
        state = dali_web_request_feed(req, (uint8_t)text[i]);
        i++;
        if (state != DALI_WEB_REQUEST_MORE) {
            break;
        }
    }
    if (consumed != NULL) {
        *consumed = i;
    }
    return state;
}

static DaliWebRequestState feed(DaliWebRequest *req, const char *text)
{
    return feed_bytes(req, text, strlen(text), NULL);
}

static DaliWebRoute route_text(const char *text)
{
    static DaliWebRequest req;
    feed(&req, text);
    return dali_web_route(&req, &k_policy);
}

static void assert_refused(DaliWebRefusal expected, const char *text)
{
    DaliWebRoute route = route_text(text);
    TEST_ASSERT_EQUAL_MESSAGE(DALI_WEB_ROUTE_REFUSE, route.kind, text);
    TEST_ASSERT_EQUAL_MESSAGE(expected, route.refusal, text);
}

static void hex_digest(const uint8_t digest[DALI_WEB_SHA1_LEN], char out[41])
{
    for (unsigned i = 0u; i < DALI_WEB_SHA1_LEN; i++) {
        snprintf(out + 2u * i, 3u, "%02x", digest[i]);
    }
}

static void assert_sha1(const char *expected_hex, const uint8_t *data, size_t len)
{
    uint8_t digest[DALI_WEB_SHA1_LEN];
    char hex[41];
    dali_web_sha1(data, len, digest);
    hex_digest(digest, hex);
    TEST_ASSERT_EQUAL_STRING(expected_hex, hex);
}

/* A masked client frame, built the way a browser builds one. */
static size_t client_frame(uint8_t first, const char *payload, size_t len,
                           const uint8_t mask[4], uint8_t *out)
{
    out[0] = first;
    out[1] = (uint8_t)(0x80u | len);
    memcpy(out + 2, mask, 4u);
    for (size_t i = 0u; i < len; i++) {
        out[6u + i] = (uint8_t)((uint8_t)payload[i] ^ mask[i & 3u]);
    }
    return 6u + len;
}

/* Feed a whole buffer, returning the last event and how many were not MORE. */
static DaliWebWsRxEvent ws_feed(DaliWebWsReceiver *rx, const uint8_t *bytes, size_t len,
                                unsigned *events)
{
    DaliWebWsRxEvent last = DALI_WEB_WS_RX_MORE;
    unsigned count = 0u;
    for (size_t i = 0u; i < len; i++) {
        DaliWebWsRxEvent event = dali_web_ws_receive(rx, bytes[i]);
        if (event != DALI_WEB_WS_RX_MORE) {
            count++;
            last = event;
        }
    }
    if (events != NULL) {
        *events = count;
    }
    return last;
}

static const uint8_t k_mask[4] = { 0x37u, 0xFAu, 0x21u, 0x3Du };

/* Chrome's upgrade, as it sends it over plain http. */
#define CHROME_UPGRADE                                                              \
    "GET /ws HTTP/1.1\r\n"                                                          \
    "Host: 192.168.1.50:8080\r\n"                                                   \
    "Connection: Upgrade\r\n"                                                       \
    "Pragma: no-cache\r\n"                                                          \
    "Cache-Control: no-cache\r\n"                                                   \
    "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "     \
    "(KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36\r\n"                        \
    "Upgrade: websocket\r\n"                                                        \
    "Origin: http://192.168.1.50:8080\r\n"                                          \
    "Sec-WebSocket-Version: 13\r\n"                                                 \
    "Accept-Encoding: gzip, deflate\r\n"                                            \
    "Accept-Language: en-US,en;q=0.9,et;q=0.8\r\n"                                  \
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"                               \
    "Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\n"      \
    "\r\n"

/* Firefox lists Connection as "keep-alive, Upgrade" and adds Sec-Fetch-*. */
#define FIREFOX_UPGRADE                                                             \
    "GET /ws HTTP/1.1\r\n"                                                          \
    "Host: dali-2k.local:8080\r\n"                                                  \
    "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:144.0) "              \
    "Gecko/20100101 Firefox/144.0\r\n"                                              \
    "Accept: */*\r\n"                                                               \
    "Accept-Language: en-US,en;q=0.5\r\n"                                           \
    "Accept-Encoding: gzip, deflate\r\n"                                            \
    "Sec-WebSocket-Version: 13\r\n"                                                 \
    "Origin: http://dali-2k.local:8080\r\n"                                         \
    "Sec-WebSocket-Extensions: permessage-deflate\r\n"                              \
    "Sec-WebSocket-Key: x3JJHMbDL1EzLkh9GBhXDw==\r\n"                               \
    "Connection: keep-alive, Upgrade\r\n"                                           \
    "Sec-Fetch-Dest: empty\r\n"                                                     \
    "Sec-Fetch-Mode: websocket\r\n"                                                 \
    "Sec-Fetch-Site: same-origin\r\n"                                               \
    "Pragma: no-cache\r\n"                                                          \
    "Cache-Control: no-cache\r\n"                                                   \
    "Upgrade: websocket\r\n"                                                        \
    "\r\n"

/* Chrome's navigation to the page. Upgrade-Insecure-Requests is not Upgrade. */
#define CHROME_PAGE                                                                 \
    "GET / HTTP/1.1\r\n"                                                            \
    "Host: 192.168.1.50:8080\r\n"                                                   \
    "Connection: keep-alive\r\n"                                                    \
    "Upgrade-Insecure-Requests: 1\r\n"                                              \
    "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "     \
    "(KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36\r\n"                        \
    "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,"     \
    "image/webp,image/apng,*/*;q=0.8\r\n"                                           \
    "Accept-Encoding: gzip, deflate\r\n"                                            \
    "Accept-Language: en-US,en;q=0.9\r\n"                                           \
    "\r\n"

/* An upgrade with chosen Host and Origin lines; NULL leaves the line out. */
static const char *upgrade_with(const char *host_line, const char *origin_line)
{
    static char buf[512];
    snprintf(buf, sizeof(buf),
             "GET /ws HTTP/1.1\r\n"
             "%s"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "%s"
             "Sec-WebSocket-Version: 13\r\n"
             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
             "\r\n",
             host_line != NULL ? host_line : "",
             origin_line != NULL ? origin_line : "");
    return buf;
}

/* ---------------------------------------------------------------------------
 * SHA-1 and base64
 * --------------------------------------------------------------------------*/

static void test_sha1_fips_vectors(void)
{
    assert_sha1("a9993e364706816aba3e25717850c26c9cd0d89d", (const uint8_t *)"abc", 3u);
    assert_sha1("da39a3ee5e6b4b0d3255bfef95601890afd80709", NULL, 0u);
    const char *two_block = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    assert_sha1("84983e441c3bd26ebaae4aa1f95129e5e54670f1",
                (const uint8_t *)two_block, strlen(two_block));

    static uint8_t million[1000000];
    memset(million, 'a', sizeof(million));
    assert_sha1("34aa973cd4c4daa4f61eeb2bdbad27316534016f", million, sizeof(million));
}

/* 55 is the longest tail that leaves room for the length; 56 spills a block;
 * 64 is a whole block with an empty tail. */
static void test_sha1_padding_boundaries(void)
{
    static const struct { size_t len; const char *hex; } vectors[] = {
        {  55u, "c1c8bbdc22796e28c0e15163d20899b65621d65a" },
        {  56u, "c2db330f6083854c99d4b5bfb6e8f29f201be699" },
        {  63u, "03f09f5b158a7a8cdad920bddc29b81c18a551f5" },
        {  64u, "0098ba824b5c16427bd7a1122a5a442a25ec644d" },
        {  65u, "11655326c708d70319be2610e8a57d9a5b959d3b" },
        { 119u, "ee971065aaa017e0632a8ca6c77bb3bf8b1dfc56" },
        { 120u, "f34c1488385346a55709ba056ddd08280dd4c6d6" },
    };
    uint8_t data[120];
    memset(data, 'a', sizeof(data));
    for (size_t i = 0u; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        assert_sha1(vectors[i].hex, data, vectors[i].len);
    }
}

static void test_base64_rfc4648_vectors(void)
{
    static const struct { const char *in; const char *out; } vectors[] = {
        { "", "" }, { "f", "Zg==" }, { "fo", "Zm8=" }, { "foo", "Zm9v" },
        { "foob", "Zm9vYg==" }, { "fooba", "Zm9vYmE=" }, { "foobar", "Zm9vYmFy" },
    };
    for (size_t i = 0u; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        char out[16];
        size_t len = dali_web_base64_encode((const uint8_t *)vectors[i].in,
                                            strlen(vectors[i].in), out, sizeof(out));
        TEST_ASSERT_EQUAL_STRING(vectors[i].out, out);
        TEST_ASSERT_EQUAL(strlen(vectors[i].out), len);
    }

    /* The full alphabet, both ends of it. */
    static const uint8_t ends[] = { 0x00u, 0x10u, 0x83u, 0xFBu, 0xFFu, 0xBFu };
    char out[16];
    dali_web_base64_encode(ends, sizeof(ends), out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("ABCD+/+/", out);
}

static void test_base64_refuses_a_short_buffer(void)
{
    char out[5] = "xxxx";
    /* "foo" needs 4 characters and the NUL. */
    TEST_ASSERT_EQUAL(4u, dali_web_base64_encode((const uint8_t *)"foo", 3u, out, 5u));
    TEST_ASSERT_EQUAL(0u, dali_web_base64_encode((const uint8_t *)"foo", 3u, out, 4u));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL(0u, dali_web_base64_encode((const uint8_t *)"foo", 3u, NULL, 8u));
}

static void test_ws_accept_vectors(void)
{
    char accept[DALI_WEB_WS_ACCEPT_LEN + 1u];
    /* RFC 6455 §1.3 */
    dali_web_ws_accept("dGhlIHNhbXBsZSBub25jZQ==", accept);
    TEST_ASSERT_EQUAL_STRING("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", accept);
    dali_web_ws_accept("x3JJHMbDL1EzLkh9GBhXDw==", accept);
    TEST_ASSERT_EQUAL_STRING("HSmrc0sMlYUkAGmm5OPpG2HaGWk=", accept);
}

static void test_ws_key_validation(void)
{
    TEST_ASSERT_TRUE(dali_web_ws_key_valid("dGhlIHNhbXBsZSBub25jZQ=="));
    TEST_ASSERT_TRUE(dali_web_ws_key_valid("x3JJHMbDL1EzLkh9GBhXDw=="));
    TEST_ASSERT_TRUE(dali_web_ws_key_valid("AAAAAAAAAAAAAAAAAAAAAA=="));
    TEST_ASSERT_TRUE(dali_web_ws_key_valid("/////////////////////w=="));

    TEST_ASSERT_FALSE(dali_web_ws_key_valid(NULL));
    TEST_ASSERT_FALSE(dali_web_ws_key_valid(""));
    TEST_ASSERT_FALSE(dali_web_ws_key_valid("dGhlIHNhbXBsZSBub25jZQ="));    /* 23 */
    TEST_ASSERT_FALSE(dali_web_ws_key_valid("dGhlIHNhbXBsZSBub25jZQ==="));  /* 25 */
    TEST_ASSERT_FALSE(dali_web_ws_key_valid("dGhlIHNhbXBsZSBub25jZQAA"));   /* no padding */
    TEST_ASSERT_FALSE(dali_web_ws_key_valid("dGhlIHNhbXBsZSBub25jZQ=A"));
    TEST_ASSERT_FALSE(dali_web_ws_key_valid("dGhlIHNhbXBsZSBub25jZ*=="));   /* not base64 */
    TEST_ASSERT_FALSE(dali_web_ws_key_valid("dGhlIHNhbXBsZSBub25j Q=="));
    /* Non-canonical: R leaves a set bit in the padding. */
    TEST_ASSERT_FALSE(dali_web_ws_key_valid("dGhlIHNhbXBsZSBub25jZR=="));
    TEST_ASSERT_FALSE(dali_web_ws_key_valid("AAAAAAAAAAAAAAAAAAAAAB=="));
    TEST_ASSERT_FALSE(dali_web_ws_key_valid("AAAAAAAAAAAAAAAAAAAAAP=="));
    /* E and I set only the two padding bits a 2-bit check would miss. */
    TEST_ASSERT_FALSE(dali_web_ws_key_valid("AAAAAAAAAAAAAAAAAAAAAE=="));
    TEST_ASSERT_FALSE(dali_web_ws_key_valid("AAAAAAAAAAAAAAAAAAAAAI=="));
}

/* ---------------------------------------------------------------------------
 * Request head: what parses
 * --------------------------------------------------------------------------*/

static void test_request_parses_a_chrome_upgrade(void)
{
    DaliWebRequest req;
    size_t consumed = 0u;
    const char *text = CHROME_UPGRADE;
    TEST_ASSERT_EQUAL(DALI_WEB_REQUEST_DONE, feed_bytes(&req, text, strlen(text), &consumed));
    TEST_ASSERT_EQUAL(strlen(text), consumed);

    TEST_ASSERT_EQUAL_STRING("GET", req.method);
    TEST_ASSERT_EQUAL_STRING("/ws", req.target);
    TEST_ASSERT_EQUAL_STRING("192.168.1.50:8080", req.host);
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.50:8080", req.origin);
    TEST_ASSERT_EQUAL_STRING("dGhlIHNhbXBsZSBub25jZQ==", req.ws_key);
    TEST_ASSERT_EQUAL_STRING("13", req.ws_version);
    TEST_ASSERT_EQUAL_HEX16(DALI_WEB_HAS_HOST | DALI_WEB_HAS_ORIGIN | DALI_WEB_HAS_WS_KEY |
                            DALI_WEB_HAS_WS_VERSION | DALI_WEB_UPGRADE_WEBSOCKET |
                            DALI_WEB_CONNECTION_UPGRADE,
                            req.flags);
}

static void test_request_parses_a_firefox_upgrade(void)
{
    DaliWebRequest req;
    TEST_ASSERT_EQUAL(DALI_WEB_REQUEST_DONE, feed(&req, FIREFOX_UPGRADE));
    TEST_ASSERT_EQUAL_STRING("dali-2k.local:8080", req.host);
    TEST_ASSERT_TRUE((req.flags & DALI_WEB_CONNECTION_UPGRADE) != 0u);
    TEST_ASSERT_TRUE((req.flags & DALI_WEB_UPGRADE_WEBSOCKET) != 0u);
}

static void test_request_page_navigation_is_not_an_upgrade(void)
{
    DaliWebRequest req;
    TEST_ASSERT_EQUAL(DALI_WEB_REQUEST_DONE, feed(&req, CHROME_PAGE));
    TEST_ASSERT_EQUAL_STRING("/", req.target);
    TEST_ASSERT_EQUAL_HEX16(DALI_WEB_HAS_HOST, req.flags);
}

/* The head ends on its last LF; a frame sent straight behind it is not the
 * parser's to read. */
static void test_request_stops_at_the_blank_line(void)
{
    static const char text[] = "GET /ws HTTP/1.1\r\nHost: dali.lan:8080\r\n\r\n\x81\x85";
    DaliWebRequest req;
    size_t consumed = 0u;
    TEST_ASSERT_EQUAL(DALI_WEB_REQUEST_DONE,
                      feed_bytes(&req, text, sizeof(text) - 1u, &consumed));
    TEST_ASSERT_EQUAL(sizeof(text) - 3u, consumed);
    /* Nothing after the head is parsed, so nothing can turn DONE into a
     * refusal, however much it looks like the end of one. */
    static const char after[] = "\x81 x\r\n\r\n\n\n";
    for (size_t i = 0u; i < sizeof(after) - 1u; i++) {
        TEST_ASSERT_EQUAL(DALI_WEB_REQUEST_DONE, dali_web_request_feed(&req, (uint8_t)after[i]));
    }
    TEST_ASSERT_EQUAL(DALI_WEB_REFUSE_NONE, req.refusal);
    TEST_ASSERT_EQUAL_STRING("dali.lan:8080", req.host);
}

static void test_request_accepts_bare_lf(void)
{
    DaliWebRequest req;
    TEST_ASSERT_EQUAL(DALI_WEB_REQUEST_DONE,
                      feed(&req, "GET / HTTP/1.1\nHost: dali.lan:8080\n\n"));
    TEST_ASSERT_EQUAL_STRING("dali.lan:8080", req.host);
}

static void test_request_trims_value_whitespace(void)
{
    DaliWebRequest req;
    TEST_ASSERT_EQUAL(DALI_WEB_REQUEST_DONE,
                      feed(&req, "GET / HTTP/1.1\r\nHost: \t dali.lan:8080 \t\r\n"
                                 "Origin:http://dali.lan:8080\r\n\r\n"));
    TEST_ASSERT_EQUAL_STRING("dali.lan:8080", req.host);
    TEST_ASSERT_EQUAL_STRING("http://dali.lan:8080", req.origin);
}

/* Field names are case-insensitive, and so are the tokens looked for. */
static void test_request_header_names_and_tokens_ignore_case(void)
{
    DaliWebRequest req;
    TEST_ASSERT_EQUAL(DALI_WEB_REQUEST_DONE,
                      feed(&req, "GET /ws HTTP/1.1\r\nHOST: dali.lan:8080\r\n"
                                 "uPgRaDe: WebSocket\r\nCONNECTION: UPGRADE\r\n\r\n"));
    TEST_ASSERT_EQUAL_STRING("dali.lan:8080", req.host);
    TEST_ASSERT_TRUE((req.flags & DALI_WEB_UPGRADE_WEBSOCKET) != 0u);
    TEST_ASSERT_TRUE((req.flags & DALI_WEB_CONNECTION_UPGRADE) != 0u);
}

/* A list header may repeat, and a token matches only whole. */
static void test_request_token_lists(void)
{
    DaliWebRequest req;
    feed(&req, "GET /ws HTTP/1.1\r\nConnection: keep-alive\r\nConnection: Upgrade\r\n\r\n");
    TEST_ASSERT_TRUE((req.flags & DALI_WEB_CONNECTION_UPGRADE) != 0u);

    feed(&req, "GET /ws HTTP/1.1\r\nConnection: keep-alive,upgrade , close\r\n\r\n");
    TEST_ASSERT_TRUE((req.flags & DALI_WEB_CONNECTION_UPGRADE) != 0u);

    static const char *const near_misses[] = {
        "GET /ws HTTP/1.1\r\nConnection: upgrades\r\nUpgrade: websockets\r\n\r\n",
        "GET /ws HTTP/1.1\r\nConnection: up grade\r\nUpgrade: web socket\r\n\r\n",
        "GET /ws HTTP/1.1\r\nConnection: x-upgrade\r\nUpgrade: websocket/13\r\n\r\n",
        "GET /ws HTTP/1.1\r\nConnection: upgradeupgradeupgrade\r\n"
        "Upgrade: websocketwebsocketwebsocket\r\n\r\n",
        "GET /ws HTTP/1.1\r\nUpgrade-Insecure-Requests: 1\r\nX-Connection: upgrade\r\n\r\n",
    };
    for (size_t i = 0u; i < sizeof(near_misses) / sizeof(near_misses[0]); i++) {
        TEST_ASSERT_EQUAL(DALI_WEB_REQUEST_DONE, feed(&req, near_misses[i]));
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(0u, req.flags & (DALI_WEB_CONNECTION_UPGRADE |
                                                         DALI_WEB_UPGRADE_WEBSOCKET),
                                        near_misses[i]);
    }
}

/* ---------------------------------------------------------------------------
 * Request head: what is refused
 * --------------------------------------------------------------------------*/

static void assert_parse_refused(DaliWebRefusal expected, const char *text)
{
    DaliWebRequest req;
    TEST_ASSERT_EQUAL_MESSAGE(DALI_WEB_REQUEST_INVALID, feed(&req, text), text);
    TEST_ASSERT_EQUAL_MESSAGE(expected, req.refusal, text);
}

static void test_request_refuses_malformed_request_lines(void)
{
    static const char *const lines[] = {
        "\r\n\r\n",                                     /* no request line */
        " GET / HTTP/1.1\r\n\r\n",
        "GET  / HTTP/1.1\r\n\r\n",                      /* two spaces */
        "GET /  HTTP/1.1\r\n\r\n",
        "GET / HTTP/1.1 \r\n\r\n",
        "GET /\r\n\r\n",                                /* HTTP/0.9 */
        "GET\r\n\r\n",
        "GET http://192.168.1.50:8080/ HTTP/1.1\r\n\r\n", /* absolute-form */
        "OPTIONS * HTTP/1.1\r\n\r\n",                   /* asterisk-form */
        "GET ws HTTP/1.1\r\n\r\n",
        "GET /\x01 HTTP/1.1\r\n\r\n",
        "G(T / HTTP/1.1\r\n\r\n",
        "GET / HTTP/1.1x\r\n\r\n",
        "GET / HTTX/1.1\r\n\r\n",
        "GET / http/1.1\r\n\r\n",
        "GET / HTTP/1\r\n\r\n",
    };
    for (size_t i = 0u; i < sizeof(lines) / sizeof(lines[0]); i++) {
        assert_parse_refused(DALI_WEB_REFUSE_MALFORMED, lines[i]);
    }
}

static void test_request_refuses_other_http_versions(void)
{
    assert_parse_refused(DALI_WEB_REFUSE_HTTP_VERSION, "GET / HTTP/1.0\r\n\r\n");
    assert_parse_refused(DALI_WEB_REFUSE_HTTP_VERSION, "GET / HTTP/2.0\r\n\r\n");
    assert_parse_refused(DALI_WEB_REFUSE_HTTP_VERSION, "GET / HTTP/0.9\r\n\r\n");
}

/* The shapes RFC 9112 calls out as smuggling or splitting hazards. */
static void test_request_refuses_smuggling_shapes(void)
{
    static const char *const heads[] = {
        "GET / HTTP/1.1\r\nHost : dali.lan:8080\r\n\r\n",          /* space before colon */
        "GET / HTTP/1.1\r\nHost\t: dali.lan:8080\r\n\r\n",
        "GET / HTTP/1.1\r\nX-A: 1\r\n folded\r\n\r\n",               /* obs-fold */
        "GET / HTTP/1.1\r\nX-A: 1\r\n\tfolded\r\n\r\n",
        "GET / HTTP/1.1\rHost: dali.lan:8080\r\n\r\n",               /* bare CR */
        "GET / HTTP/1.1\r\nHost: dali.lan\r8080\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: dali.lan:8080\r\r\n\r\n",
        "GET / HTTP/1.1\r\nNo-colon-here\r\n\r\n",
        "GET / HTTP/1.1\r\n: no name\r\n\r\n",
        "GET / HTTP/1.1\r\nX(A): 1\r\n\r\n",
        "GET / HTTP/1.1\r\nX-A: 1\x01\r\n\r\n",                      /* control in value */
        "GET / HTTP/1.1\r\nX-A: 1\x7F\r\n\r\n",
    };
    for (size_t i = 0u; i < sizeof(heads) / sizeof(heads[0]); i++) {
        assert_parse_refused(DALI_WEB_REFUSE_MALFORMED, heads[i]);
    }

    /* A NUL inside a value, which strlen-based feeding cannot carry. */
    static const char nul[] = "GET / HTTP/1.1\r\nX-A: a\0b\r\n\r\n";
    DaliWebRequest req;
    TEST_ASSERT_EQUAL(DALI_WEB_REQUEST_INVALID, feed_bytes(&req, nul, sizeof(nul) - 1u, NULL));
    TEST_ASSERT_EQUAL(DALI_WEB_REFUSE_MALFORMED, req.refusal);
}

/* Field values may carry obs-text and HTAB; only the kept fields are stored. */
static void test_request_accepts_obs_text_in_values(void)
{
    DaliWebRequest req;
    TEST_ASSERT_EQUAL(DALI_WEB_REQUEST_DONE,
                      feed(&req, "GET / HTTP/1.1\r\nX-Name: K\xC3\xB6\xC3\xB6k\tsiin\r\n"
                                 "Host: dali.lan:8080\r\n\r\n"));
}

static void test_request_refuses_duplicate_headers(void)
{
    static const char *const heads[] = {
        "GET / HTTP/1.1\r\nHost: dali.lan:8080\r\nHost: evil.example:8080\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: dali.lan:8080\r\nhost: dali.lan:8080\r\n\r\n",
        "GET /ws HTTP/1.1\r\nOrigin: http://a\r\nOrigin: http://b\r\n\r\n",
        "GET /ws HTTP/1.1\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Key: x3JJHMbDL1EzLkh9GBhXDw==\r\n\r\n",
        "GET /ws HTTP/1.1\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Version: 8\r\n\r\n",
    };
    for (size_t i = 0u; i < sizeof(heads) / sizeof(heads[0]); i++) {
        assert_parse_refused(DALI_WEB_REFUSE_DUPLICATE_HEADER, heads[i]);
    }
}

static void test_request_target_limit(void)
{
    char head[256];
    char target[DALI_WEB_TARGET_MAX + 2u];
    memset(target, 'a', sizeof(target));
    target[0] = '/';

    target[DALI_WEB_TARGET_MAX] = '\0';
    snprintf(head, sizeof(head), "GET %s HTTP/1.1\r\n\r\n", target);
    DaliWebRequest req;
    TEST_ASSERT_EQUAL(DALI_WEB_REQUEST_DONE, feed(&req, head));
    TEST_ASSERT_EQUAL(DALI_WEB_TARGET_MAX, strlen(req.target));

    target[DALI_WEB_TARGET_MAX] = 'a';
    target[DALI_WEB_TARGET_MAX + 1u] = '\0';
    snprintf(head, sizeof(head), "GET %s HTTP/1.1\r\n\r\n", target);
    assert_parse_refused(DALI_WEB_REFUSE_TARGET_TOO_LONG, head);
}

/* Build a head of exactly `total` bytes, padded inside one header value. */
static void padded_head(char *buf, size_t total, bool ended)
{
    static const char start[] = "GET / HTTP/1.1\r\nX-Pad: ";
    const char *end = ended ? "\r\n\r\n" : "\r\n";
    size_t fixed = strlen(start) + strlen(end);
    memcpy(buf, start, strlen(start));
    memset(buf + strlen(start), 'p', total - fixed);
    memcpy(buf + total - strlen(end), end, strlen(end));
    buf[total] = '\0';
}

static void test_request_head_limit(void)
{
    static char head[DALI_WEB_HEAD_MAX + 64u];
    DaliWebRequest req;
    size_t consumed = 0u;

    /* Exactly the limit, ending on its last byte: served. */
    padded_head(head, DALI_WEB_HEAD_MAX, true);
    TEST_ASSERT_EQUAL(DALI_WEB_REQUEST_DONE, feed_bytes(&req, head, DALI_WEB_HEAD_MAX, &consumed));
    TEST_ASSERT_EQUAL(DALI_WEB_HEAD_MAX, consumed);

    /* One byte longer: refused on the limit's last byte, without waiting for
     * a byte the client may never send. */
    padded_head(head, DALI_WEB_HEAD_MAX + 1u, true);
    TEST_ASSERT_EQUAL(DALI_WEB_REQUEST_INVALID,
                      feed_bytes(&req, head, DALI_WEB_HEAD_MAX + 1u, &consumed));
    TEST_ASSERT_EQUAL(DALI_WEB_HEAD_MAX, consumed);
    TEST_ASSERT_EQUAL(DALI_WEB_REFUSE_HEAD_TOO_LARGE, req.refusal);

    /* A head that never ends. */
    padded_head(head, DALI_WEB_HEAD_MAX + 10u, false);
    TEST_ASSERT_EQUAL(DALI_WEB_REQUEST_INVALID,
                      feed_bytes(&req, head, strlen(head), &consumed));
    TEST_ASSERT_EQUAL(DALI_WEB_HEAD_MAX, consumed);
    TEST_ASSERT_EQUAL(DALI_WEB_REFUSE_HEAD_TOO_LARGE, req.refusal);

    /* A fault, then a head that never ends: the fault is still the answer. */
    padded_head(head, DALI_WEB_HEAD_MAX + 10u, false);
    memcpy(head, "GET / HTTP/1.0", 14u);
    TEST_ASSERT_EQUAL(DALI_WEB_REQUEST_INVALID,
                      feed_bytes(&req, head, strlen(head), &consumed));
    TEST_ASSERT_EQUAL(DALI_WEB_HEAD_MAX, consumed);
    TEST_ASSERT_EQUAL(DALI_WEB_REFUSE_HTTP_VERSION, req.refusal);
}

/* A refused head is read to its blank line, so the binding answers a client
 * that has finished sending. The first fault is the one reported. */
static void test_request_reads_a_refused_head_to_its_end(void)
{
    static const char *const heads[] = {
        "GET / HTTP/1.0\r\nHost: dali.lan:8080\r\nX-A: 1\r\n\r\n",
        "GET / HTTP/1.1\r\nHost : dali.lan:8080\r\nX-A: 1\r\n\r\n",
        "GET / HTTP/1.1\r\nX-A: 1\r\n folded\r\nX-B: 2\r\n\r\n",
        "GET  / HTTP/1.1\r\nX-A: 1\r\n\r\n",
        "GET / HTTP/1.1\rX\nX-A: 1\n\n",
    };
    for (size_t i = 0u; i < sizeof(heads) / sizeof(heads[0]); i++) {
        DaliWebRequest req;
        size_t consumed = 0u;
        TEST_ASSERT_EQUAL_MESSAGE(DALI_WEB_REQUEST_INVALID,
                                  feed_bytes(&req, heads[i], strlen(heads[i]), &consumed),
                                  heads[i]);
        TEST_ASSERT_EQUAL_MESSAGE(strlen(heads[i]), consumed, heads[i]);
    }

    DaliWebRequest req;
    feed(&req, "GET / HTTP/1.0\r\nHost : x\r\nHost: a\r\nHost: b\r\n\r\n");
    TEST_ASSERT_EQUAL(DALI_WEB_REFUSE_HTTP_VERSION, req.refusal);
}

static void test_request_marks_overflowing_values(void)
{
    char head[512];
    char long_host[DALI_WEB_HOST_MAX + 2u];
    memset(long_host, 'h', sizeof(long_host) - 1u);
    long_host[sizeof(long_host) - 1u] = '\0';
    snprintf(head, sizeof(head),
             "GET /ws HTTP/1.1\r\nHost: %s\r\nOrigin: http://%s\r\n"
             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==AA\r\n"
             "Sec-WebSocket-Version: 1300\r\n\r\n",
             long_host, long_host);
    DaliWebRequest req;
    TEST_ASSERT_EQUAL(DALI_WEB_REQUEST_DONE, feed(&req, head));
    const uint16_t overflow = DALI_WEB_HOST_OVERFLOW | DALI_WEB_ORIGIN_OVERFLOW |
                              DALI_WEB_WS_KEY_OVERFLOW | DALI_WEB_WS_VERSION_OVERFLOW;
    TEST_ASSERT_EQUAL_HEX16(overflow, req.flags & overflow);
    TEST_ASSERT_EQUAL(DALI_WEB_HOST_MAX, strlen(req.host));

    feed(&req, "PROPPATCH / HTTP/1.1\r\n\r\n");
    TEST_ASSERT_TRUE((req.flags & DALI_WEB_METHOD_OVERFLOW) != 0u);
    TEST_ASSERT_EQUAL_STRING("PROPPAT", req.method);
}

static void test_request_state_is_final(void)
{
    DaliWebRequest req;
    TEST_ASSERT_EQUAL(DALI_WEB_REQUEST_INVALID, feed(&req, "GET / HTTP/1.0\r\n\r\n"));
    TEST_ASSERT_EQUAL(DALI_WEB_REQUEST_INVALID, dali_web_request_feed(&req, (uint8_t)'G'));
    TEST_ASSERT_EQUAL(DALI_WEB_REFUSE_HTTP_VERSION, req.refusal);

    TEST_ASSERT_EQUAL(DALI_WEB_REQUEST_INVALID, dali_web_request_feed(NULL, (uint8_t)'G'));
    dali_web_request_init(NULL);
}

/* ---------------------------------------------------------------------------
 * Routing
 * --------------------------------------------------------------------------*/

static void test_route_serves_browser_requests(void)
{
    DaliWebRoute route = route_text(CHROME_PAGE);
    TEST_ASSERT_EQUAL(DALI_WEB_ROUTE_PAGE, route.kind);
    TEST_ASSERT_EQUAL(DALI_WEB_REFUSE_NONE, route.refusal);

    TEST_ASSERT_EQUAL(DALI_WEB_ROUTE_SOCKET, route_text(CHROME_UPGRADE).kind);
    TEST_ASSERT_EQUAL(DALI_WEB_ROUTE_SOCKET, route_text(FIREFOX_UPGRADE).kind);

    /* Names compare without case, and Origin follows Host's spelling. */
    TEST_ASSERT_EQUAL(DALI_WEB_ROUTE_SOCKET,
                      route_text(upgrade_with("Host: DALI-2K.LOCAL:8080\r\n",
                                              "Origin: http://dali-2k.local:8080\r\n")).kind);
    TEST_ASSERT_EQUAL(DALI_WEB_ROUTE_SOCKET,
                      route_text(upgrade_with("Host: dali-2k:8080\r\n",
                                              "Origin: HTTP://Dali-2k:8080\r\n")).kind);
    TEST_ASSERT_EQUAL(DALI_WEB_ROUTE_SOCKET,
                      route_text(upgrade_with("Host: [fe80::1]:8080\r\n",
                                              "Origin: http://[fe80::1]:8080\r\n")).kind);
}

/* DNS rebinding: the attacker's name resolves to the device, so Host and Origin
 * agree with each other and only Host can tell. Refused on every path. */
static void test_route_refuses_dns_rebinding(void)
{
    assert_refused(DALI_WEB_REFUSE_HOST,
                   upgrade_with("Host: evil.example:8080\r\n",
                                "Origin: http://evil.example:8080\r\n"));
    assert_refused(DALI_WEB_REFUSE_HOST,
                   "GET / HTTP/1.1\r\nHost: evil.example:8080\r\n\r\n");
    assert_refused(DALI_WEB_REFUSE_HOST,
                   "GET /favicon.ico HTTP/1.1\r\nHost: evil.example:8080\r\n\r\n");
    assert_refused(DALI_WEB_REFUSE_HOST,
                   "POST /ws HTTP/1.1\r\nHost: evil.example:8080\r\n\r\n");
}

/* Cross-site WebSocket hijacking: a page elsewhere opens ws://<device>/ws. The
 * browser sends the device's Host and the page's own Origin. */
static void test_route_refuses_cross_site_origins(void)
{
    static const char *const origins[] = {
        "Origin: http://evil.example\r\n",
        "Origin: null\r\n",
        "Origin: https://192.168.1.50:8080\r\n",
        "Origin: http://192.168.1.50:8080.evil.example\r\n",
        "Origin: http://192.168.1.50:8081\r\n",
        "Origin: http://192.168.1.50\r\n",
        "Origin: http://dali-2k.local:8080\r\n",   /* this device, but not this Host */
        "Origin: http://192.168.1.50:8080/\r\n",
        "Origin: http://\r\n",
        "Origin: \r\n",
        "Origin: 192.168.1.50:8080\r\n",
    };
    for (size_t i = 0u; i < sizeof(origins) / sizeof(origins[0]); i++) {
        assert_refused(DALI_WEB_REFUSE_ORIGIN,
                       upgrade_with("Host: 192.168.1.50:8080\r\n", origins[i]));
    }
    assert_refused(DALI_WEB_REFUSE_NO_ORIGIN,
                   upgrade_with("Host: 192.168.1.50:8080\r\n", NULL));

    char long_origin[DALI_WEB_ORIGIN_MAX + 32u];
    snprintf(long_origin, sizeof(long_origin), "Origin: http://192.168.1.50:8080%096d\r\n", 0);
    assert_refused(DALI_WEB_REFUSE_ORIGIN,
                   upgrade_with("Host: 192.168.1.50:8080\r\n", long_origin));
}

static void test_route_host_rules(void)
{
    static const char *const refused[] = {
        "192.168.1.50",                     /* no port means 80 */
        "192.168.1.50:80",
        "192.168.1.50:8081",
        "192.168.1.50:",
        "192.168.1.50:080800",
        "192.168.1.50:4294975376",          /* 2^32 + 8080: would wrap to 8080 */
        "192.168.1.50:008080",
        "192.168.1.50:80a0",
        "192.168.1.50:8080:8080",
        ":8080",
        "dali-2k.local.:8080",              /* the trailing dot is another name */
        "dali-2k.local.evil.example:8080",
        "evil.dali-2k.local:8080",
        "dali-2k.loca:8080",
        "user@dali-2k.local:8080",
        "192.168.1.5:8080",
        "192.168.1.500:8080",
        "[fe80::1]",
        "[fe80::1]x:8080",
        "[fe80::1:8080",
        "fe80::1:8080",
        "",
    };
    for (size_t i = 0u; i < sizeof(refused) / sizeof(refused[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(dali_web_host_allowed(&k_policy, refused[i]), refused[i]);
    }

    TEST_ASSERT_TRUE(dali_web_host_allowed(&k_policy, "192.168.1.50:8080"));
    TEST_ASSERT_TRUE(dali_web_host_allowed(&k_policy, "dali.lan:8080"));
    TEST_ASSERT_TRUE(dali_web_host_allowed(&k_policy, "Dali.LAN:8080"));
    TEST_ASSERT_TRUE(dali_web_host_allowed(&k_policy, "[FE80::1]:8080"));
    TEST_ASSERT_TRUE(dali_web_host_allowed(&k_policy, "dali.lan:08080"));

    /* On port 80 a browser names no port. */
    const DaliWebHostPolicy port80 = { k_names, 5u, 80u };
    TEST_ASSERT_TRUE(dali_web_host_allowed(&port80, "dali.lan"));
    TEST_ASSERT_TRUE(dali_web_host_allowed(&port80, "dali.lan:80"));
    TEST_ASSERT_TRUE(dali_web_host_allowed(&port80, "[fe80::1]"));
    TEST_ASSERT_FALSE(dali_web_host_allowed(&port80, "dali.lan:8080"));

    /* Only the names given, and only as many as name_count says. */
    const DaliWebHostPolicy first_only = { k_names, 1u, 8080u };
    TEST_ASSERT_TRUE(dali_web_host_allowed(&first_only, "192.168.1.50:8080"));
    TEST_ASSERT_FALSE(dali_web_host_allowed(&first_only, "dali-2k:8080"));
    const char *const with_null[] = { NULL, "dali.lan" };
    const DaliWebHostPolicy holes = { with_null, 2u, 8080u };
    TEST_ASSERT_TRUE(dali_web_host_allowed(&holes, "dali.lan:8080"));
    const DaliWebHostPolicy none = { NULL, 0u, 8080u };
    TEST_ASSERT_FALSE(dali_web_host_allowed(&none, "dali.lan:8080"));
    TEST_ASSERT_FALSE(dali_web_host_allowed(NULL, "dali.lan:8080"));
    TEST_ASSERT_FALSE(dali_web_host_allowed(&k_policy, NULL));

    assert_refused(DALI_WEB_REFUSE_NO_HOST, "GET / HTTP/1.1\r\nX-A: 1\r\n\r\n");
    assert_refused(DALI_WEB_REFUSE_HOST, "GET / HTTP/1.1\r\nHost:\r\n\r\n");

    char head[512];
    char long_host[DALI_WEB_HOST_MAX + 8u];
    memset(long_host, 'h', sizeof(long_host) - 1u);
    long_host[sizeof(long_host) - 1u] = '\0';
    snprintf(head, sizeof(head), "GET / HTTP/1.1\r\nHost: %s\r\n\r\n", long_host);
    assert_refused(DALI_WEB_REFUSE_HOST, head);
}

/* An overflowed Host must not be compared as a prefix: a listed name followed
 * by enough padding would otherwise match. */
static void test_route_does_not_match_a_truncated_host(void)
{
    static char longest[DALI_WEB_HOST_MAX + 1u];
    memset(longest, 'h', DALI_WEB_HOST_MAX);
    longest[DALI_WEB_HOST_MAX] = '\0';
    const char *const names[] = { longest };
    const DaliWebHostPolicy policy = { names, 1u, 80u };

    char head[512];
    snprintf(head, sizeof(head), "GET / HTTP/1.1\r\nHost: %sh\r\n\r\n", names[0]);
    DaliWebRequest req;
    TEST_ASSERT_EQUAL(DALI_WEB_REQUEST_DONE, feed(&req, head));
    TEST_ASSERT_EQUAL_STRING(names[0], req.host);
    DaliWebRoute route = dali_web_route(&req, &policy);
    TEST_ASSERT_EQUAL(DALI_WEB_REFUSE_HOST, route.refusal);

    snprintf(head, sizeof(head), "GET / HTTP/1.1\r\nHost: %s\r\n\r\n", names[0]);
    feed(&req, head);
    TEST_ASSERT_EQUAL(DALI_WEB_ROUTE_PAGE, dali_web_route(&req, &policy).kind);

    /* The same for Origin: with the longest Host, "http://" + Host fills the
     * buffer exactly, so a longer Origin would store as a perfect match. */
    snprintf(head, sizeof(head),
             "GET /ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
             "Origin: http://%s.evil.example\r\nSec-WebSocket-Version: 13\r\n"
             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
             names[0], names[0]);
    TEST_ASSERT_EQUAL(DALI_WEB_REQUEST_DONE, feed(&req, head));
    TEST_ASSERT_EQUAL_STRING(names[0], req.origin + 7);
    route = dali_web_route(&req, &policy);
    TEST_ASSERT_EQUAL(DALI_WEB_REFUSE_ORIGIN, route.refusal);
}

static void test_route_refuses_other_methods(void)
{
    static const char *const heads[] = {
        "POST / HTTP/1.1\r\nHost: dali.lan:8080\r\n\r\n",
        "HEAD / HTTP/1.1\r\nHost: dali.lan:8080\r\n\r\n",
        "OPTIONS / HTTP/1.1\r\nHost: dali.lan:8080\r\n\r\n",
        "get / HTTP/1.1\r\nHost: dali.lan:8080\r\n\r\n",
        "GETS / HTTP/1.1\r\nHost: dali.lan:8080\r\n\r\n",
        "GE / HTTP/1.1\r\nHost: dali.lan:8080\r\n\r\n",
        "GETGETGET / HTTP/1.1\r\nHost: dali.lan:8080\r\n\r\n",
        "POST /ws HTTP/1.1\r\nHost: dali.lan:8080\r\n\r\n",
    };
    for (size_t i = 0u; i < sizeof(heads) / sizeof(heads[0]); i++) {
        assert_refused(DALI_WEB_REFUSE_METHOD, heads[i]);
    }
}

static void test_route_paths(void)
{
    TEST_ASSERT_EQUAL(DALI_WEB_ROUTE_PAGE,
                      route_text("GET /?tab=map HTTP/1.1\r\nHost: dali.lan:8080\r\n\r\n").kind);
    TEST_ASSERT_EQUAL(DALI_WEB_ROUTE_PAGE,
                      route_text("GET /? HTTP/1.1\r\nHost: dali.lan:8080\r\n\r\n").kind);

    static const char *const missing[] = {
        "GET /favicon.ico HTTP/1.1\r\nHost: dali.lan:8080\r\n\r\n",
        "GET /ws/ HTTP/1.1\r\nHost: dali.lan:8080\r\n\r\n",
        "GET /WS HTTP/1.1\r\nHost: dali.lan:8080\r\n\r\n",
        "GET /wss HTTP/1.1\r\nHost: dali.lan:8080\r\n\r\n",
        "GET //ws HTTP/1.1\r\nHost: dali.lan:8080\r\n\r\n",
        "GET /w HTTP/1.1\r\nHost: dali.lan:8080\r\n\r\n",
        "GET /index.html HTTP/1.1\r\nHost: dali.lan:8080\r\n\r\n",
    };
    for (size_t i = 0u; i < sizeof(missing) / sizeof(missing[0]); i++) {
        assert_refused(DALI_WEB_REFUSE_NOT_FOUND, missing[i]);
    }
}

static void test_route_socket_with_a_query(void)
{
    static const char head[] =
        "GET /ws?session=1 HTTP/1.1\r\nHost: dali.lan:8080\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nOrigin: http://dali.lan:8080\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    TEST_ASSERT_EQUAL(DALI_WEB_ROUTE_SOCKET, route_text(head).kind);
}

static void test_route_socket_needs_an_upgrade(void)
{
    assert_refused(DALI_WEB_REFUSE_NOT_UPGRADE,
                   "GET /ws HTTP/1.1\r\nHost: dali.lan:8080\r\n\r\n");
    assert_refused(DALI_WEB_REFUSE_NOT_UPGRADE,
                   "GET /ws HTTP/1.1\r\nHost: dali.lan:8080\r\nUpgrade: websocket\r\n"
                   "Origin: http://dali.lan:8080\r\n\r\n");
    assert_refused(DALI_WEB_REFUSE_NOT_UPGRADE,
                   "GET /ws HTTP/1.1\r\nHost: dali.lan:8080\r\nConnection: Upgrade\r\n"
                   "Origin: http://dali.lan:8080\r\n\r\n");
    assert_refused(DALI_WEB_REFUSE_NOT_UPGRADE,
                   "GET /ws HTTP/1.1\r\nHost: dali.lan:8080\r\nUpgrade: h2c\r\n"
                   "Connection: Upgrade\r\nOrigin: http://dali.lan:8080\r\n\r\n");
    assert_refused(DALI_WEB_REFUSE_NOT_UPGRADE,
                   "GET /ws HTTP/1.1\r\nHost: dali.lan:8080\r\nUpgrade-Insecure-Requests: 1\r\n"
                   "Connection: Upgrade\r\nOrigin: http://dali.lan:8080\r\n\r\n");
}

static void test_route_socket_version_and_key(void)
{
    static const char base[] =
        "GET /ws HTTP/1.1\r\nHost: dali.lan:8080\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nOrigin: http://dali.lan:8080\r\n";
    char head[512];

    static const char *const versions[] = {
        "Sec-WebSocket-Version: 8\r\n", "Sec-WebSocket-Version: 13, 8\r\n",
        "Sec-WebSocket-Version: 130\r\n", "Sec-WebSocket-Version:\r\n", "",
        /* Stored as "13 " and trimmed to "13": only the overflow tells. */
        "Sec-WebSocket-Version: 13  8\r\n",
    };
    for (size_t i = 0u; i < sizeof(versions) / sizeof(versions[0]); i++) {
        snprintf(head, sizeof(head), "%s%sSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
                 base, versions[i]);
        assert_refused(DALI_WEB_REFUSE_WS_VERSION, head);
    }

    static const char *const keys[] = {
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZR==\r\n",
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ=\r\n",
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==A\r\n",
        "Sec-WebSocket-Key:\r\n",
        "",
    };
    for (size_t i = 0u; i < sizeof(keys) / sizeof(keys[0]); i++) {
        snprintf(head, sizeof(head), "%sSec-WebSocket-Version: 13\r\n%s\r\n", base, keys[i]);
        assert_refused(DALI_WEB_REFUSE_WS_KEY, head);
    }
}

static void test_route_invalid_and_unfinished_heads(void)
{
    assert_refused(DALI_WEB_REFUSE_HTTP_VERSION,
                   "GET / HTTP/1.0\r\nHost: dali.lan:8080\r\n\r\n");

    DaliWebRequest req;
    feed(&req, "GET / HTTP/1.1\r\nHost: dali.lan:8080\r\n");
    TEST_ASSERT_EQUAL(DALI_WEB_REQUEST_MORE, req.state);
    DaliWebRoute route = dali_web_route(&req, &k_policy);
    TEST_ASSERT_EQUAL(DALI_WEB_ROUTE_REFUSE, route.kind);
    TEST_ASSERT_EQUAL(DALI_WEB_REFUSE_MALFORMED, route.refusal);

    route = dali_web_route(NULL, &k_policy);
    TEST_ASSERT_EQUAL(DALI_WEB_ROUTE_REFUSE, route.kind);

    feed(&req, CHROME_PAGE);
    route = dali_web_route(&req, NULL);
    TEST_ASSERT_EQUAL(DALI_WEB_REFUSE_HOST, route.refusal);
}

/* ---------------------------------------------------------------------------
 * Response heads
 * --------------------------------------------------------------------------*/

static void test_format_handshake(void)
{
    DaliWebRequest req;
    feed(&req, CHROME_UPGRADE);
    char out[512];
    size_t len = dali_web_format_handshake(&req, out, sizeof(out));
    static const char expected[] =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
        "\r\n";
    TEST_ASSERT_EQUAL_STRING(expected, out);
    TEST_ASSERT_EQUAL(sizeof(expected) - 1u, len);
    /* No extension is agreed, so the client never sets RSV1. */
    TEST_ASSERT_NULL(strstr(out, "Extensions"));

    TEST_ASSERT_EQUAL(0u, dali_web_format_handshake(&req, out, sizeof(expected) - 1u));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL(sizeof(expected) - 1u, dali_web_format_handshake(&req, out, sizeof(expected)));

    feed(&req, "GET /ws HTTP/1.1\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZR==\r\n\r\n");
    TEST_ASSERT_EQUAL(0u, dali_web_format_handshake(&req, out, sizeof(out)));
    TEST_ASSERT_EQUAL(0u, dali_web_format_handshake(NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL(0u, dali_web_format_handshake(&req, NULL, sizeof(out)));
}

static void test_format_page_head(void)
{
    char out[512];
    size_t len = dali_web_format_page_head(41234u, true, out, sizeof(out));
    TEST_ASSERT_EQUAL(strlen(out), len);
    TEST_ASSERT_EQUAL(0, strncmp(out, "HTTP/1.1 200 OK\r\n", 17));
    TEST_ASSERT_NOT_NULL(strstr(out, "\r\nContent-Type: text/html; charset=utf-8\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\r\nContent-Encoding: gzip\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\r\nContent-Length: 41234\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\r\nX-Frame-Options: DENY\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\r\nContent-Security-Policy: frame-ancestors 'none'\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\r\nX-Content-Type-Options: nosniff\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\r\nConnection: close\r\n"));
    TEST_ASSERT_NULL(strstr(out, "Access-Control"));
    /* The only blank line is the one that ends the head. */
    TEST_ASSERT_EQUAL_PTR(out + len - 4u, strstr(out, "\r\n\r\n"));

    len = dali_web_format_page_head(0u, false, out, sizeof(out));
    TEST_ASSERT_NULL(strstr(out, "Content-Encoding"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\r\nContent-Length: 0\r\n"));

    TEST_ASSERT_EQUAL(0u, dali_web_format_page_head(1u, true, out, len));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL(0u, dali_web_format_page_head(1u, true, NULL, sizeof(out)));
}

static unsigned status_of(const char *response)
{
    unsigned status = 0u;
    sscanf(response, "HTTP/1.1 %u ", &status);
    return status;
}

static void test_format_every_refusal(void)
{
    for (int r = DALI_WEB_REFUSE_MALFORMED; r <= DALI_WEB_REFUSE_UNAVAILABLE; r++) {
        DaliWebRefusal refusal = (DaliWebRefusal)r;
        char out[512];
        size_t len = dali_web_format_refusal(refusal, out, sizeof(out));
        TEST_ASSERT_TRUE(len > 0u);
        TEST_ASSERT_EQUAL(strlen(out), len);
        TEST_ASSERT_EQUAL((unsigned)dali_web_refusal_status(refusal), status_of(out));

        /* The body is the sentence and a newline, and Content-Length says so. */
        const char *body = strstr(out, "\r\n\r\n");
        TEST_ASSERT_NOT_NULL(body);
        body += 4;
        const char *text = dali_web_refusal_text(refusal);
        TEST_ASSERT_TRUE(strlen(text) > 0u);
        TEST_ASSERT_EQUAL(0, strncmp(body, text, strlen(text)));
        TEST_ASSERT_EQUAL_STRING("\n", body + strlen(text));
        unsigned content_length = 0u;
        const char *cl = strstr(out, "\r\nContent-Length: ");
        TEST_ASSERT_NOT_NULL(cl);
        sscanf(cl, "\r\nContent-Length: %u", &content_length);
        TEST_ASSERT_EQUAL(strlen(body), content_length);
        TEST_ASSERT_NOT_NULL(strstr(out, "\r\nConnection: "));
        TEST_ASSERT_NULL(strstr(out, "Access-Control"));
    }

    char out[512];
    dali_web_format_refusal(DALI_WEB_REFUSE_METHOD, out, sizeof(out));
    TEST_ASSERT_EQUAL(0, strncmp(out, "HTTP/1.1 405 Method Not Allowed\r\n", 33));
    TEST_ASSERT_NOT_NULL(strstr(out, "\r\nAllow: GET\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\r\nConnection: close\r\n"));

    dali_web_format_refusal(DALI_WEB_REFUSE_WS_VERSION, out, sizeof(out));
    TEST_ASSERT_EQUAL(0, strncmp(out, "HTTP/1.1 426 Upgrade Required\r\n", 31));
    TEST_ASSERT_NOT_NULL(strstr(out, "\r\nUpgrade: websocket\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\r\nSec-WebSocket-Version: 13\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\r\nConnection: Upgrade, close\r\n"));

    dali_web_format_refusal(DALI_WEB_REFUSE_HOST, out, sizeof(out));
    TEST_ASSERT_EQUAL(0, strncmp(out, "HTTP/1.1 403 Forbidden\r\n", 24));
    TEST_ASSERT_NULL(strstr(out, "Allow:"));
    TEST_ASSERT_NULL(strstr(out, "Upgrade"));

    TEST_ASSERT_EQUAL(0u, dali_web_format_refusal(DALI_WEB_REFUSE_NONE, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL(0u, dali_web_format_refusal((DaliWebRefusal)99, out, sizeof(out)));
    TEST_ASSERT_EQUAL(0u, dali_web_format_refusal(DALI_WEB_REFUSE_HOST, out, 40u));
    TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_refusal_statuses(void)
{
    static const struct { DaliWebRefusal refusal; DaliWebStatus status; } map[] = {
        { DALI_WEB_REFUSE_NONE,             DALI_WEB_STATUS_OK },
        { DALI_WEB_REFUSE_MALFORMED,        DALI_WEB_STATUS_BAD_REQUEST },
        { DALI_WEB_REFUSE_DUPLICATE_HEADER, DALI_WEB_STATUS_BAD_REQUEST },
        { DALI_WEB_REFUSE_METHOD,           DALI_WEB_STATUS_METHOD_NOT_ALLOWED },
        { DALI_WEB_REFUSE_TARGET_TOO_LONG,  DALI_WEB_STATUS_URI_TOO_LONG },
        { DALI_WEB_REFUSE_HEAD_TOO_LARGE,   DALI_WEB_STATUS_HEADERS_TOO_LARGE },
        { DALI_WEB_REFUSE_HTTP_VERSION,     DALI_WEB_STATUS_VERSION_NOT_SUPPORTED },
        { DALI_WEB_REFUSE_NO_HOST,          DALI_WEB_STATUS_BAD_REQUEST },
        { DALI_WEB_REFUSE_HOST,             DALI_WEB_STATUS_FORBIDDEN },
        { DALI_WEB_REFUSE_NOT_FOUND,        DALI_WEB_STATUS_NOT_FOUND },
        { DALI_WEB_REFUSE_NOT_UPGRADE,      DALI_WEB_STATUS_UPGRADE_REQUIRED },
        { DALI_WEB_REFUSE_WS_VERSION,       DALI_WEB_STATUS_UPGRADE_REQUIRED },
        { DALI_WEB_REFUSE_WS_KEY,           DALI_WEB_STATUS_BAD_REQUEST },
        { DALI_WEB_REFUSE_NO_ORIGIN,        DALI_WEB_STATUS_FORBIDDEN },
        { DALI_WEB_REFUSE_ORIGIN,           DALI_WEB_STATUS_FORBIDDEN },
        { DALI_WEB_REFUSE_UNAVAILABLE,      DALI_WEB_STATUS_SERVICE_UNAVAILABLE },
    };
    for (size_t i = 0u; i < sizeof(map) / sizeof(map[0]); i++) {
        TEST_ASSERT_EQUAL(map[i].status, dali_web_refusal_status(map[i].refusal));
    }
    TEST_ASSERT_EQUAL(DALI_WEB_STATUS_BAD_REQUEST, dali_web_refusal_status((DaliWebRefusal)99));
    TEST_ASSERT_NOT_NULL(dali_web_refusal_text((DaliWebRefusal)99));
}

/* ---------------------------------------------------------------------------
 * WebSocket receiver
 * --------------------------------------------------------------------------*/

/* RFC 6455 §5.7: "A single-frame masked text message". */
static void test_ws_rx_rfc_masked_text(void)
{
    static const uint8_t frame[] = { 0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d,
                                     0x7f, 0x9f, 0x4d, 0x51, 0x58 };
    DaliWebWsReceiver rx;
    dali_web_ws_receiver_init(&rx);
    for (size_t i = 0u; i + 1u < sizeof(frame); i++) {
        TEST_ASSERT_EQUAL(DALI_WEB_WS_RX_MORE, dali_web_ws_receive(&rx, frame[i]));
    }
    TEST_ASSERT_EQUAL(DALI_WEB_WS_RX_TEXT, dali_web_ws_receive(&rx, frame[sizeof(frame) - 1u]));
    TEST_ASSERT_EQUAL_STRING("Hello", (const char *)rx.payload);
    TEST_ASSERT_EQUAL(5u, rx.payload_len);
}

/* RFC 6455 §5.7: "Unmasked Ping request and masked Ping response". */
static void test_ws_rx_rfc_masked_pong(void)
{
    static const uint8_t frame[] = { 0x8a, 0x85, 0x37, 0xfa, 0x21, 0x3d,
                                     0x7f, 0x9f, 0x4d, 0x51, 0x58 };
    DaliWebWsReceiver rx;
    dali_web_ws_receiver_init(&rx);
    TEST_ASSERT_EQUAL(DALI_WEB_WS_RX_PONG, ws_feed(&rx, frame, sizeof(frame), NULL));
    TEST_ASSERT_EQUAL(5u, rx.payload_len);
    TEST_ASSERT_EQUAL_MEMORY("Hello", rx.payload, 5u);
}

/* A ping's payload is binary and comes back as it was, every byte value. */
static void test_ws_rx_ping_carries_any_payload(void)
{
    uint8_t payload[DALI_WEB_WS_PAYLOAD_MAX];
    for (size_t i = 0u; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 37u + 1u);
    }
    uint8_t frame[6u + DALI_WEB_WS_PAYLOAD_MAX];
    size_t len = client_frame(0x89u, (const char *)payload, sizeof(payload), k_mask, frame);
    DaliWebWsReceiver rx;
    dali_web_ws_receiver_init(&rx);
    TEST_ASSERT_EQUAL(DALI_WEB_WS_RX_PING, ws_feed(&rx, frame, len, NULL));
    TEST_ASSERT_EQUAL(DALI_WEB_WS_PAYLOAD_MAX, rx.payload_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, rx.payload, sizeof(payload));
}

/* Back to back, as one recv() can deliver them. */
static void test_ws_rx_consecutive_frames(void)
{
    uint8_t bytes[128];
    size_t len = 0u;
    len += client_frame(0x81u, "discover", 8u, k_mask, bytes + len);
    len += client_frame(0x89u, "", 0u, k_mask, bytes + len);
    len += client_frame(0x81u, "", 0u, k_mask, bytes + len);
    len += client_frame(0x81u, "level a3 254", 12u, k_mask, bytes + len);

    DaliWebWsReceiver rx;
    dali_web_ws_receiver_init(&rx);
    DaliWebWsRxEvent events[4];
    char lines[4][DALI_WEB_WS_PAYLOAD_MAX + 1u];
    unsigned n = 0u;
    for (size_t i = 0u; i < len; i++) {
        DaliWebWsRxEvent event = dali_web_ws_receive(&rx, bytes[i]);
        if (event != DALI_WEB_WS_RX_MORE) {
            TEST_ASSERT_TRUE(n < 4u);
            events[n] = event;
            snprintf(lines[n], sizeof(lines[n]), "%s", (const char *)rx.payload);
            n++;
        }
    }
    TEST_ASSERT_EQUAL(4u, n);
    TEST_ASSERT_EQUAL(DALI_WEB_WS_RX_TEXT, events[0]);
    TEST_ASSERT_EQUAL_STRING("discover", lines[0]);
    TEST_ASSERT_EQUAL(DALI_WEB_WS_RX_PING, events[1]);
    TEST_ASSERT_EQUAL(DALI_WEB_WS_RX_TEXT, events[2]);
    TEST_ASSERT_EQUAL_STRING("", lines[2]);
    TEST_ASSERT_EQUAL(DALI_WEB_WS_RX_TEXT, events[3]);
    TEST_ASSERT_EQUAL_STRING("level a3 254", lines[3]);
}

static void test_ws_rx_longest_line(void)
{
    char line[DALI_WEB_WS_PAYLOAD_MAX + 1u];
    for (size_t i = 0u; i < DALI_WEB_WS_PAYLOAD_MAX; i++) {
        line[i] = (char)(0x20u + (i % 95u));  /* every printable byte */
    }
    line[DALI_WEB_WS_PAYLOAD_MAX] = '\0';
    uint8_t frame[6u + DALI_WEB_WS_PAYLOAD_MAX];
    size_t len = client_frame(0x81u, line, DALI_WEB_WS_PAYLOAD_MAX, k_mask, frame);
    DaliWebWsReceiver rx;
    dali_web_ws_receiver_init(&rx);
    TEST_ASSERT_EQUAL(DALI_WEB_WS_RX_TEXT, ws_feed(&rx, frame, len, NULL));
    TEST_ASSERT_EQUAL_STRING(line, (const char *)rx.payload);
}

static void assert_ws_fails(const uint8_t *bytes, size_t len, DaliWebWsFailure failure,
                            DaliWebWsCloseCode code)
{
    DaliWebWsReceiver rx;
    dali_web_ws_receiver_init(&rx);
    unsigned events = 0u;
    TEST_ASSERT_EQUAL(DALI_WEB_WS_RX_FAIL, ws_feed(&rx, bytes, len, &events));
    TEST_ASSERT_EQUAL(failure, rx.failure);
    TEST_ASSERT_EQUAL(code, rx.close_code);
    TEST_ASSERT_EQUAL(0u, rx.payload_len);
}

static void test_ws_rx_refuses_unmasked_frames(void)
{
    /* RFC 6455 §5.7: "A single-frame unmasked text message". */
    static const uint8_t frame[] = { 0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f };
    assert_ws_fails(frame, sizeof(frame), DALI_WEB_WS_FAIL_UNMASKED, DALI_WEB_WS_CLOSE_PROTOCOL);
    static const uint8_t ping[] = { 0x89, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f };
    assert_ws_fails(ping, sizeof(ping), DALI_WEB_WS_FAIL_UNMASKED, DALI_WEB_WS_CLOSE_PROTOCOL);
}

static void test_ws_rx_refuses_fragments(void)
{
    /* RFC 6455 §5.7's fragmented "Hel" + "lo", masked as a client sends it. */
    uint8_t bytes[32];
    size_t len = client_frame(0x01u, "Hel", 3u, k_mask, bytes);
    assert_ws_fails(bytes, len, DALI_WEB_WS_FAIL_FRAGMENTED, DALI_WEB_WS_CLOSE_TOO_BIG);

    len = client_frame(0x80u, "lo", 2u, k_mask, bytes);
    assert_ws_fails(bytes, len, DALI_WEB_WS_FAIL_CONTINUATION, DALI_WEB_WS_CLOSE_PROTOCOL);
    len = client_frame(0x00u, "lo", 2u, k_mask, bytes);
    assert_ws_fails(bytes, len, DALI_WEB_WS_FAIL_CONTINUATION, DALI_WEB_WS_CLOSE_PROTOCOL);

    static const uint8_t control_heads[] = { 0x08u, 0x09u, 0x0Au };
    for (size_t i = 0u; i < sizeof(control_heads); i++) {
        len = client_frame(control_heads[i], "", 0u, k_mask, bytes);
        assert_ws_fails(bytes, len, DALI_WEB_WS_FAIL_FRAGMENTED, DALI_WEB_WS_CLOSE_PROTOCOL);
    }
}

static void test_ws_rx_refuses_binary(void)
{
    uint8_t bytes[32];
    size_t len = client_frame(0x82u, "scan", 4u, k_mask, bytes);
    assert_ws_fails(bytes, len, DALI_WEB_WS_FAIL_BINARY, DALI_WEB_WS_CLOSE_UNSUPPORTED);
}

/* RSV1 is what permessage-deflate sets; nothing was agreed, so it is refused. */
static void test_ws_rx_refuses_reserved_bits(void)
{
    static const uint8_t heads[] = { 0xC1u, 0xA1u, 0x91u, 0xF1u, 0xC9u };
    for (size_t i = 0u; i < sizeof(heads); i++) {
        uint8_t bytes[32];
        size_t len = client_frame(heads[i], "help", 4u, k_mask, bytes);
        assert_ws_fails(bytes, len, DALI_WEB_WS_FAIL_RESERVED_BITS, DALI_WEB_WS_CLOSE_PROTOCOL);
    }
}

static void test_ws_rx_refuses_reserved_opcodes(void)
{
    static const uint8_t opcodes[] = { 0x3u, 0x4u, 0x5u, 0x6u, 0x7u, 0xBu, 0xCu, 0xDu, 0xEu, 0xFu };
    for (size_t i = 0u; i < sizeof(opcodes); i++) {
        uint8_t bytes[32];
        size_t len = client_frame((uint8_t)(0x80u | opcodes[i]), "", 0u, k_mask, bytes);
        assert_ws_fails(bytes, len, DALI_WEB_WS_FAIL_RESERVED_OPCODE, DALI_WEB_WS_CLOSE_PROTOCOL);
    }
}

static void test_ws_rx_refuses_extended_lengths(void)
{
    /* 126 and 127 announce 16- and 64-bit lengths. */
    static const uint8_t text126[] = { 0x81u, 0xFEu, 0x00u, 0x7Eu };
    assert_ws_fails(text126, sizeof(text126), DALI_WEB_WS_FAIL_TOO_LONG, DALI_WEB_WS_CLOSE_TOO_BIG);
    static const uint8_t text127[] = { 0x81u, 0xFFu };
    assert_ws_fails(text127, sizeof(text127), DALI_WEB_WS_FAIL_TOO_LONG, DALI_WEB_WS_CLOSE_TOO_BIG);
    /* A control frame that long is a protocol error, not a big message. */
    static const uint8_t ping126[] = { 0x89u, 0xFEu };
    assert_ws_fails(ping126, sizeof(ping126), DALI_WEB_WS_FAIL_TOO_LONG, DALI_WEB_WS_CLOSE_PROTOCOL);
    static const uint8_t close127[] = { 0x88u, 0xFFu };
    assert_ws_fails(close127, sizeof(close127), DALI_WEB_WS_FAIL_TOO_LONG, DALI_WEB_WS_CLOSE_PROTOCOL);
}

/* One text frame is one shell line: a line break inside it, or anything else
 * outside printable ASCII, fails the connection rather than becoming a second
 * command. */
static void test_ws_rx_refuses_what_is_not_one_line(void)
{
    static const char *const texts[] = {
        "scan\nreset", "scan\r", "\rscan", "a\tb", "\x7F", "K\xC3\xB6\xC3\xB6k", "\x01",
        "\x1F", "\x80", "\xFF",
    };
    for (size_t i = 0u; i < sizeof(texts) / sizeof(texts[0]); i++) {
        uint8_t bytes[32];
        size_t len = client_frame(0x81u, texts[i], strlen(texts[i]), k_mask, bytes);
        assert_ws_fails(bytes, len, DALI_WEB_WS_FAIL_NOT_A_LINE, DALI_WEB_WS_CLOSE_INVALID_DATA);
    }
    uint8_t bytes[32];
    static const char nul[] = { 'a', '\0', 'b' };
    size_t len = client_frame(0x81u, nul, sizeof(nul), k_mask, bytes);
    assert_ws_fails(bytes, len, DALI_WEB_WS_FAIL_NOT_A_LINE, DALI_WEB_WS_CLOSE_INVALID_DATA);
}

static void test_ws_rx_close(void)
{
    DaliWebWsReceiver rx;
    uint8_t bytes[64];

    static const char normal[] = { 0x03, (char)0xE8, 'b', 'y', 'e' };  /* 1000 "bye" */
    size_t len = client_frame(0x88u, normal, sizeof(normal), k_mask, bytes);
    dali_web_ws_receiver_init(&rx);
    TEST_ASSERT_EQUAL(DALI_WEB_WS_RX_CLOSE, ws_feed(&rx, bytes, len, NULL));
    TEST_ASSERT_EQUAL(1000u, rx.close_code);

    static const char away[] = { 0x03, (char)0xE9 };                    /* 1001 */
    len = client_frame(0x88u, away, sizeof(away), k_mask, bytes);
    dali_web_ws_receiver_init(&rx);
    TEST_ASSERT_EQUAL(DALI_WEB_WS_RX_CLOSE, ws_feed(&rx, bytes, len, NULL));
    TEST_ASSERT_EQUAL(1001u, rx.close_code);

    static const char app[] = { 0x0F, (char)0xA0 };                     /* 4000 */
    len = client_frame(0x88u, app, sizeof(app), k_mask, bytes);
    dali_web_ws_receiver_init(&rx);
    TEST_ASSERT_EQUAL(DALI_WEB_WS_RX_CLOSE, ws_feed(&rx, bytes, len, NULL));
    TEST_ASSERT_EQUAL(4000u, rx.close_code);

    len = client_frame(0x88u, "", 0u, k_mask, bytes);
    dali_web_ws_receiver_init(&rx);
    TEST_ASSERT_EQUAL(DALI_WEB_WS_RX_CLOSE, ws_feed(&rx, bytes, len, NULL));
    TEST_ASSERT_EQUAL(DALI_WEB_WS_CLOSE_NO_STATUS, rx.close_code);

    len = client_frame(0x88u, "\x03", 1u, k_mask, bytes);
    assert_ws_fails(bytes, len, DALI_WEB_WS_FAIL_CLOSE_PAYLOAD, DALI_WEB_WS_CLOSE_PROTOCOL);
}

/* Nothing after a close or a fault is delivered, however well-formed. */
static void test_ws_rx_terminal_events_are_final(void)
{
    uint8_t bytes[64];
    uint8_t text[32];
    size_t text_len = client_frame(0x81u, "commission", 10u, k_mask, text);

    DaliWebWsReceiver rx;
    dali_web_ws_receiver_init(&rx);
    size_t len = client_frame(0x88u, "", 0u, k_mask, bytes);
    TEST_ASSERT_EQUAL(DALI_WEB_WS_RX_CLOSE, ws_feed(&rx, bytes, len, NULL));
    for (size_t i = 0u; i < text_len; i++) {
        TEST_ASSERT_EQUAL(DALI_WEB_WS_RX_CLOSE, dali_web_ws_receive(&rx, text[i]));
    }

    dali_web_ws_receiver_init(&rx);
    len = client_frame(0x82u, "x", 1u, k_mask, bytes);
    TEST_ASSERT_EQUAL(DALI_WEB_WS_RX_FAIL, ws_feed(&rx, bytes, len, NULL));
    for (size_t i = 0u; i < text_len; i++) {
        TEST_ASSERT_EQUAL(DALI_WEB_WS_RX_FAIL, dali_web_ws_receive(&rx, text[i]));
    }
    TEST_ASSERT_EQUAL(DALI_WEB_WS_FAIL_BINARY, rx.failure);

    TEST_ASSERT_EQUAL(DALI_WEB_WS_RX_FAIL, dali_web_ws_receive(NULL, 0x81u));
    dali_web_ws_receiver_init(NULL);
}

/* The masking key cycles every four bytes from the payload's first byte. */
static void test_ws_rx_mask_cycles_by_four(void)
{
    static const uint8_t mask[4] = { 0x00u, 0xFFu, 0x0Fu, 0xF0u };
    uint8_t bytes[32];
    size_t len = client_frame(0x81u, "abcdefghij", 10u, mask, bytes);
    DaliWebWsReceiver rx;
    dali_web_ws_receiver_init(&rx);
    TEST_ASSERT_EQUAL(DALI_WEB_WS_RX_TEXT, ws_feed(&rx, bytes, len, NULL));
    TEST_ASSERT_EQUAL_STRING("abcdefghij", (const char *)rx.payload);

    /* A second frame with another key starts its cycle again. */
    len = client_frame(0x81u, "xyz", 3u, k_mask, bytes);
    TEST_ASSERT_EQUAL(DALI_WEB_WS_RX_TEXT, ws_feed(&rx, bytes, len, NULL));
    TEST_ASSERT_EQUAL_STRING("xyz", (const char *)rx.payload);
}

/* ---------------------------------------------------------------------------
 * WebSocket encoder
 * --------------------------------------------------------------------------*/

static void test_ws_frame_header_rfc_vectors(void)
{
    uint8_t out[10];
    /* RFC 6455 §5.7: unmasked "Hello" */
    TEST_ASSERT_EQUAL(2u, dali_web_ws_frame_header(DALI_WEB_WS_OP_TEXT, 5u, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8(0x81u, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x05u, out[1]);
    /* "Unmasked Ping request" */
    TEST_ASSERT_EQUAL(2u, dali_web_ws_frame_header(DALI_WEB_WS_OP_PING, 5u, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8(0x89u, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x05u, out[1]);
    /* "256 bytes binary message in a single unmasked frame" */
    static const uint8_t b256[] = { 0x82u, 0x7Eu, 0x01u, 0x00u };
    TEST_ASSERT_EQUAL(4u, dali_web_ws_frame_header(DALI_WEB_WS_OP_BINARY, 256u, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(b256, out, 4u);
    /* "64KiB binary message in a single unmasked frame" */
    static const uint8_t b64k[] = { 0x82u, 0x7Fu, 0x00u, 0x00u, 0x00u, 0x00u,
                                    0x00u, 0x01u, 0x00u, 0x00u };
    TEST_ASSERT_EQUAL(10u, dali_web_ws_frame_header(DALI_WEB_WS_OP_BINARY, 65536u, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(b64k, out, 10u);
}

static void test_ws_frame_header_length_boundaries(void)
{
    uint8_t out[10];
    TEST_ASSERT_EQUAL(2u, dali_web_ws_frame_header(DALI_WEB_WS_OP_TEXT, 0u, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8(0x00u, out[1]);
    TEST_ASSERT_EQUAL(2u, dali_web_ws_frame_header(DALI_WEB_WS_OP_TEXT, 125u, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8(125u, out[1]);

    static const uint8_t t126[] = { 0x81u, 0x7Eu, 0x00u, 0x7Eu };
    TEST_ASSERT_EQUAL(4u, dali_web_ws_frame_header(DALI_WEB_WS_OP_TEXT, 126u, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(t126, out, 4u);
    static const uint8_t t65535[] = { 0x81u, 0x7Eu, 0xFFu, 0xFFu };
    TEST_ASSERT_EQUAL(4u, dali_web_ws_frame_header(DALI_WEB_WS_OP_TEXT, 65535u, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(t65535, out, 4u);
    static const uint8_t tmax[] = { 0x81u, 0x7Fu, 0x00u, 0x00u, 0x00u, 0x00u,
                                    0xFFu, 0xFEu, 0xFDu, 0xFCu };
    TEST_ASSERT_EQUAL(10u, dali_web_ws_frame_header(DALI_WEB_WS_OP_TEXT, 0xFFFEFDFCu, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tmax, out, 10u);

    /* Too small, by exactly one byte, for each form. */
    TEST_ASSERT_EQUAL(0u, dali_web_ws_frame_header(DALI_WEB_WS_OP_TEXT, 5u, out, 1u));
    TEST_ASSERT_EQUAL(0u, dali_web_ws_frame_header(DALI_WEB_WS_OP_TEXT, 126u, out, 3u));
    TEST_ASSERT_EQUAL(0u, dali_web_ws_frame_header(DALI_WEB_WS_OP_TEXT, 65536u, out, 9u));
    TEST_ASSERT_EQUAL(0u, dali_web_ws_frame_header(DALI_WEB_WS_OP_TEXT, 5u, NULL, 10u));
}

static void test_ws_frame_header_refuses_what_is_never_sent(void)
{
    uint8_t out[10];
    TEST_ASSERT_EQUAL(2u, dali_web_ws_frame_header(DALI_WEB_WS_OP_PONG, 125u, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8(0x8Au, out[0]);
    TEST_ASSERT_EQUAL(0u, dali_web_ws_frame_header(DALI_WEB_WS_OP_PONG, 126u, out, sizeof(out)));
    TEST_ASSERT_EQUAL(0u, dali_web_ws_frame_header(DALI_WEB_WS_OP_PING, 126u, out, sizeof(out)));
    TEST_ASSERT_EQUAL(0u, dali_web_ws_frame_header(DALI_WEB_WS_OP_CLOSE, 126u, out, sizeof(out)));
    TEST_ASSERT_EQUAL(0u, dali_web_ws_frame_header(DALI_WEB_WS_OP_CONTINUATION, 5u, out, sizeof(out)));
    TEST_ASSERT_EQUAL(0u, dali_web_ws_frame_header((DaliWebWsOpcode)0x3, 5u, out, sizeof(out)));
    TEST_ASSERT_EQUAL(0u, dali_web_ws_frame_header((DaliWebWsOpcode)0xB, 5u, out, sizeof(out)));
}

static void test_ws_close_frame(void)
{
    uint8_t out[160];
    size_t len = dali_web_ws_close_frame(DALI_WEB_WS_CLOSE_TRY_AGAIN, "shell busy", out, sizeof(out));
    static const uint8_t busy[] = { 0x88u, 0x0Cu, 0x03u, 0xF5u, 's', 'h', 'e', 'l', 'l', ' ',
                                    'b', 'u', 's', 'y' };
    TEST_ASSERT_EQUAL(sizeof(busy), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(busy, out, sizeof(busy));

    len = dali_web_ws_close_frame(DALI_WEB_WS_CLOSE_NORMAL, NULL, out, sizeof(out));
    static const uint8_t normal[] = { 0x88u, 0x02u, 0x03u, 0xE8u };
    TEST_ASSERT_EQUAL(sizeof(normal), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(normal, out, sizeof(normal));

    /* No status, and 1005, which may only be received, both send an empty close. */
    static const uint8_t empty[] = { 0x88u, 0x00u };
    TEST_ASSERT_EQUAL(2u, dali_web_ws_close_frame(DALI_WEB_WS_CLOSE_NONE, "ignored", out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(empty, out, 2u);
    TEST_ASSERT_EQUAL(2u, dali_web_ws_close_frame(DALI_WEB_WS_CLOSE_NO_STATUS, "x", out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(empty, out, 2u);

    /* A reason longer than a control frame allows is cut to 123 bytes. */
    char reason[200];
    memset(reason, 'r', sizeof(reason) - 1u);
    reason[sizeof(reason) - 1u] = '\0';
    len = dali_web_ws_close_frame(DALI_WEB_WS_CLOSE_GOING_AWAY, reason, out, sizeof(out));
    TEST_ASSERT_EQUAL(2u + 125u, len);
    TEST_ASSERT_EQUAL_HEX8(125u, out[1]);
    TEST_ASSERT_EQUAL_HEX8('r', out[len - 1u]);

    /* Either side of the cut: 123 fits whole, 124 loses one byte. */
    reason[123] = '\0';
    TEST_ASSERT_EQUAL(2u + 125u,
                      dali_web_ws_close_frame(DALI_WEB_WS_CLOSE_GOING_AWAY, reason, out, sizeof(out)));
    reason[123] = 'r';
    reason[124] = '\0';
    TEST_ASSERT_EQUAL(2u + 125u,
                      dali_web_ws_close_frame(DALI_WEB_WS_CLOSE_GOING_AWAY, reason, out, sizeof(out)));
    reason[122] = '\0';
    TEST_ASSERT_EQUAL(2u + 124u,
                      dali_web_ws_close_frame(DALI_WEB_WS_CLOSE_GOING_AWAY, reason, out, sizeof(out)));

    TEST_ASSERT_EQUAL(0u, dali_web_ws_close_frame(DALI_WEB_WS_CLOSE_NORMAL, "bye", out, 6u));
    TEST_ASSERT_EQUAL(7u, dali_web_ws_close_frame(DALI_WEB_WS_CLOSE_NORMAL, "bye", out, 7u));
    TEST_ASSERT_EQUAL(0u, dali_web_ws_close_frame(DALI_WEB_WS_CLOSE_NORMAL, "bye", NULL, 64u));
}

/* What the encoder writes, the receiver reads back once it is masked: the
 * header carries no mask bit, and the length is in the same place. */
static void test_ws_encoder_and_receiver_agree(void)
{
    uint8_t frame[16];
    size_t head = dali_web_ws_frame_header(DALI_WEB_WS_OP_TEXT, 4u, frame, sizeof(frame));
    TEST_ASSERT_EQUAL(2u, head);
    TEST_ASSERT_EQUAL_HEX8(0x00u, frame[1] & 0x80u);

    DaliWebWsReceiver rx;
    dali_web_ws_receiver_init(&rx);
    TEST_ASSERT_EQUAL(DALI_WEB_WS_RX_MORE, dali_web_ws_receive(&rx, frame[0]));
    TEST_ASSERT_EQUAL(DALI_WEB_WS_RX_FAIL, dali_web_ws_receive(&rx, frame[1]));
    TEST_ASSERT_EQUAL(DALI_WEB_WS_FAIL_UNMASKED, rx.failure);
}

/* ---------------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------------*/

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sha1_fips_vectors);
    RUN_TEST(test_sha1_padding_boundaries);
    RUN_TEST(test_base64_rfc4648_vectors);
    RUN_TEST(test_base64_refuses_a_short_buffer);
    RUN_TEST(test_ws_accept_vectors);
    RUN_TEST(test_ws_key_validation);

    RUN_TEST(test_request_parses_a_chrome_upgrade);
    RUN_TEST(test_request_parses_a_firefox_upgrade);
    RUN_TEST(test_request_page_navigation_is_not_an_upgrade);
    RUN_TEST(test_request_stops_at_the_blank_line);
    RUN_TEST(test_request_accepts_bare_lf);
    RUN_TEST(test_request_trims_value_whitespace);
    RUN_TEST(test_request_header_names_and_tokens_ignore_case);
    RUN_TEST(test_request_token_lists);
    RUN_TEST(test_request_refuses_malformed_request_lines);
    RUN_TEST(test_request_refuses_other_http_versions);
    RUN_TEST(test_request_refuses_smuggling_shapes);
    RUN_TEST(test_request_accepts_obs_text_in_values);
    RUN_TEST(test_request_refuses_duplicate_headers);
    RUN_TEST(test_request_target_limit);
    RUN_TEST(test_request_head_limit);
    RUN_TEST(test_request_reads_a_refused_head_to_its_end);
    RUN_TEST(test_request_marks_overflowing_values);
    RUN_TEST(test_request_state_is_final);

    RUN_TEST(test_route_serves_browser_requests);
    RUN_TEST(test_route_refuses_dns_rebinding);
    RUN_TEST(test_route_refuses_cross_site_origins);
    RUN_TEST(test_route_host_rules);
    RUN_TEST(test_route_does_not_match_a_truncated_host);
    RUN_TEST(test_route_refuses_other_methods);
    RUN_TEST(test_route_paths);
    RUN_TEST(test_route_socket_with_a_query);
    RUN_TEST(test_route_socket_needs_an_upgrade);
    RUN_TEST(test_route_socket_version_and_key);
    RUN_TEST(test_route_invalid_and_unfinished_heads);

    RUN_TEST(test_format_handshake);
    RUN_TEST(test_format_page_head);
    RUN_TEST(test_format_every_refusal);
    RUN_TEST(test_refusal_statuses);

    RUN_TEST(test_ws_rx_rfc_masked_text);
    RUN_TEST(test_ws_rx_rfc_masked_pong);
    RUN_TEST(test_ws_rx_ping_carries_any_payload);
    RUN_TEST(test_ws_rx_consecutive_frames);
    RUN_TEST(test_ws_rx_longest_line);
    RUN_TEST(test_ws_rx_refuses_unmasked_frames);
    RUN_TEST(test_ws_rx_refuses_fragments);
    RUN_TEST(test_ws_rx_refuses_binary);
    RUN_TEST(test_ws_rx_refuses_reserved_bits);
    RUN_TEST(test_ws_rx_refuses_reserved_opcodes);
    RUN_TEST(test_ws_rx_refuses_extended_lengths);
    RUN_TEST(test_ws_rx_refuses_what_is_not_one_line);
    RUN_TEST(test_ws_rx_close);
    RUN_TEST(test_ws_rx_terminal_events_are_final);
    RUN_TEST(test_ws_rx_mask_cycles_by_four);

    RUN_TEST(test_ws_frame_header_rfc_vectors);
    RUN_TEST(test_ws_frame_header_length_boundaries);
    RUN_TEST(test_ws_frame_header_refuses_what_is_never_sent);
    RUN_TEST(test_ws_close_frame);
    RUN_TEST(test_ws_encoder_and_receiver_agree);
    return UNITY_END();
}
