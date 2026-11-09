#include "dali_web.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Character classes
 * --------------------------------------------------------------------------*/

/* RFC 9110 tchar: what a method or a field name is made of. */
static bool web_is_tchar(uint8_t c)
{
    if ((c >= (uint8_t)'0' && c <= (uint8_t)'9') ||
        (c >= (uint8_t)'a' && c <= (uint8_t)'z') ||
        (c >= (uint8_t)'A' && c <= (uint8_t)'Z')) {
        return true;
    }
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
    case '+': case '-': case '.': case '^': case '_': case '`': case '|':
    case '~':
        return true;
    default:
        return false;
    }
}

static bool web_is_vchar(uint8_t c)
{
    return c >= 0x21u && c <= 0x7Eu;
}

static bool web_is_ows(uint8_t c)
{
    return c == (uint8_t)' ' || c == (uint8_t)'\t';
}

static bool web_is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static char web_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool web_equal_nocase_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0u; i < n; i++) {
        if (web_lower(a[i]) != web_lower(b[i])) {
            return false;
        }
    }
    return true;
}

static bool web_equal_nocase(const char *a, const char *b)
{
    size_t len = strlen(a);
    return len == strlen(b) && web_equal_nocase_n(a, b, len);
}

/* ---------------------------------------------------------------------------
 * Refusals
 * --------------------------------------------------------------------------*/

typedef struct {
    DaliWebStatus status;
    const char   *text;
} WebRefusalInfo;

static const WebRefusalInfo k_refusals[] = {
    [DALI_WEB_REFUSE_NONE]             = { DALI_WEB_STATUS_OK, "" },
    [DALI_WEB_REFUSE_MALFORMED]        = { DALI_WEB_STATUS_BAD_REQUEST,
                                           "malformed HTTP/1.1 request" },
    [DALI_WEB_REFUSE_DUPLICATE_HEADER] = { DALI_WEB_STATUS_BAD_REQUEST,
                                           "a header that may appear once appears twice" },
    [DALI_WEB_REFUSE_METHOD]           = { DALI_WEB_STATUS_METHOD_NOT_ALLOWED,
                                           "only GET is served" },
    [DALI_WEB_REFUSE_TARGET_TOO_LONG]  = { DALI_WEB_STATUS_URI_TOO_LONG,
                                           "request target too long" },
    [DALI_WEB_REFUSE_HEAD_TOO_LARGE]   = { DALI_WEB_STATUS_HEADERS_TOO_LARGE,
                                           "request headers too large" },
    [DALI_WEB_REFUSE_HTTP_VERSION]     = { DALI_WEB_STATUS_VERSION_NOT_SUPPORTED,
                                           "only HTTP/1.1 is served" },
    [DALI_WEB_REFUSE_NO_HOST]          = { DALI_WEB_STATUS_BAD_REQUEST,
                                           "no Host header" },
    [DALI_WEB_REFUSE_HOST]             = { DALI_WEB_STATUS_FORBIDDEN,
                                           "Host is not a name this device answers to" },
    [DALI_WEB_REFUSE_NOT_FOUND]        = { DALI_WEB_STATUS_NOT_FOUND,
                                           "not found" },
    [DALI_WEB_REFUSE_NOT_UPGRADE]      = { DALI_WEB_STATUS_UPGRADE_REQUIRED,
                                           "/ws is a WebSocket endpoint" },
    [DALI_WEB_REFUSE_WS_VERSION]       = { DALI_WEB_STATUS_UPGRADE_REQUIRED,
                                           "only WebSocket version 13 is served" },
    [DALI_WEB_REFUSE_WS_KEY]           = { DALI_WEB_STATUS_BAD_REQUEST,
                                           "missing or malformed Sec-WebSocket-Key" },
    [DALI_WEB_REFUSE_NO_ORIGIN]        = { DALI_WEB_STATUS_FORBIDDEN,
                                           "a WebSocket upgrade without Origin" },
    [DALI_WEB_REFUSE_ORIGIN]           = { DALI_WEB_STATUS_FORBIDDEN,
                                           "Origin is not this device's own page" },
    [DALI_WEB_REFUSE_UNAVAILABLE]      = { DALI_WEB_STATUS_SERVICE_UNAVAILABLE,
                                           "the device cannot take a connection now" },
};

#define WEB_REFUSAL_COUNT (sizeof(k_refusals) / sizeof(k_refusals[0]))

DaliWebStatus dali_web_refusal_status(DaliWebRefusal refusal)
{
    if ((size_t)refusal >= WEB_REFUSAL_COUNT) {
        return DALI_WEB_STATUS_BAD_REQUEST;
    }
    return k_refusals[refusal].status;
}

const char *dali_web_refusal_text(DaliWebRefusal refusal)
{
    if ((size_t)refusal >= WEB_REFUSAL_COUNT) {
        return k_refusals[DALI_WEB_REFUSE_MALFORMED].text;
    }
    return k_refusals[refusal].text;
}

static const char *web_reason_phrase(DaliWebStatus status)
{
    switch (status) {
    case DALI_WEB_STATUS_SWITCHING_PROTOCOLS:   return "Switching Protocols";
    case DALI_WEB_STATUS_OK:                    return "OK";
    case DALI_WEB_STATUS_BAD_REQUEST:           return "Bad Request";
    case DALI_WEB_STATUS_FORBIDDEN:             return "Forbidden";
    case DALI_WEB_STATUS_NOT_FOUND:             return "Not Found";
    case DALI_WEB_STATUS_METHOD_NOT_ALLOWED:    return "Method Not Allowed";
    case DALI_WEB_STATUS_URI_TOO_LONG:          return "URI Too Long";
    case DALI_WEB_STATUS_UPGRADE_REQUIRED:      return "Upgrade Required";
    case DALI_WEB_STATUS_HEADERS_TOO_LARGE:     return "Request Header Fields Too Large";
    case DALI_WEB_STATUS_SERVICE_UNAVAILABLE:   return "Service Unavailable";
    case DALI_WEB_STATUS_VERSION_NOT_SUPPORTED: return "HTTP Version Not Supported";
    }
    return "Bad Request";
}

/* ---------------------------------------------------------------------------
 * Request head
 * --------------------------------------------------------------------------*/

enum {
    REQ_PHASE_METHOD = 0,
    REQ_PHASE_TARGET,
    REQ_PHASE_VERSION,
    REQ_PHASE_LINE_START,  /* a header line, or the blank line that ends the head */
    REQ_PHASE_NAME,
    REQ_PHASE_VALUE,
    REQ_PHASE_SKIP,        /* a fault was recorded; looking for the blank line */
};

enum {
    REQ_FIELD_OTHER = 0,
    REQ_FIELD_HOST,
    REQ_FIELD_ORIGIN,
    REQ_FIELD_WS_KEY,
    REQ_FIELD_WS_VERSION,
    REQ_FIELD_UPGRADE,
    REQ_FIELD_CONNECTION,
};

enum {
    REQ_TOKEN_LEADING = 0,
    REQ_TOKEN_IN,
    REQ_TOKEN_TRAILING,
    REQ_TOKEN_BAD,
};

typedef struct {
    const char *name;      /* lowercase */
    uint8_t     field;
    uint16_t    once_flag; /* DALI_WEB_HAS_*, or 0 for a list that may repeat */
} WebKeptHeader;

/*
 * Host must appear once (RFC 9112 §3.2), and so must each Sec-WebSocket-* the
 * handshake reads (RFC 6455 §11.3). Origin is a single origin, never a list.
 * Upgrade and Connection are lists, so a repeat only extends them.
 */
static const WebKeptHeader k_kept_headers[] = {
    { "host",                  REQ_FIELD_HOST,       DALI_WEB_HAS_HOST },
    { "origin",                REQ_FIELD_ORIGIN,     DALI_WEB_HAS_ORIGIN },
    { "sec-websocket-key",     REQ_FIELD_WS_KEY,     DALI_WEB_HAS_WS_KEY },
    { "sec-websocket-version", REQ_FIELD_WS_VERSION, DALI_WEB_HAS_WS_VERSION },
    { "upgrade",               REQ_FIELD_UPGRADE,    0u },
    { "connection",            REQ_FIELD_CONNECTION, 0u },
};

void dali_web_request_init(DaliWebRequest *req)
{
    if (req != NULL) {
        memset(req, 0, sizeof(*req));
        req->state = DALI_WEB_REQUEST_MORE;
    }
}

/* Where a kept, stored field's value goes, and the flag that says it did not
 * fit. NULL for the list fields and for headers that are not kept. */
static char *req_field_buffer(DaliWebRequest *req, uint8_t field, size_t *cap,
                              uint16_t *overflow_flag)
{
    switch (field) {
    case REQ_FIELD_HOST:
        *cap = sizeof(req->host) - 1u;
        *overflow_flag = DALI_WEB_HOST_OVERFLOW;
        return req->host;
    case REQ_FIELD_ORIGIN:
        *cap = sizeof(req->origin) - 1u;
        *overflow_flag = DALI_WEB_ORIGIN_OVERFLOW;
        return req->origin;
    case REQ_FIELD_WS_KEY:
        *cap = sizeof(req->ws_key) - 1u;
        *overflow_flag = DALI_WEB_WS_KEY_OVERFLOW;
        return req->ws_key;
    case REQ_FIELD_WS_VERSION:
        *cap = sizeof(req->ws_version) - 1u;
        *overflow_flag = DALI_WEB_WS_VERSION_OVERFLOW;
        return req->ws_version;
    default:
        *cap = 0u;
        *overflow_flag = 0u;
        return NULL;
    }
}

/*
 * Record a fault and resynchronise on the blank line. `at_line_start` is true
 * when the byte that showed the fault was the LF that ended its line, so the
 * next byte opens a new one; a blank line straight after it ends the head.
 *
 * The skip phase examines nothing but line ends, so this is reached at most
 * once per head, and the fault it records is the first.
 */
static void req_fail(DaliWebRequest *req, DaliWebRefusal refusal, bool at_line_start)
{
    req->refusal = refusal;
    req->parse.phase = REQ_PHASE_SKIP;
    req->parse.skip_blank = at_line_start;
    req->parse.pending_cr = false;
}

static void req_token_end(DaliWebRequest *req)
{
    if (req->parse.token_state == REQ_TOKEN_IN ||
        req->parse.token_state == REQ_TOKEN_TRAILING) {
        req->parse.token[req->parse.token_len] = '\0';
        if (req->parse.field == REQ_FIELD_UPGRADE &&
            strcmp(req->parse.token, "websocket") == 0) {
            req->flags |= DALI_WEB_UPGRADE_WEBSOCKET;
        } else if (req->parse.field == REQ_FIELD_CONNECTION &&
                   strcmp(req->parse.token, "upgrade") == 0) {
            req->flags |= DALI_WEB_CONNECTION_UPGRADE;
        }
    }
    req->parse.token_len = 0u;
    req->parse.token_state = REQ_TOKEN_LEADING;
}

/* One byte of a comma-separated token list, such as "keep-alive, Upgrade".
 * Tokens compare case-insensitively; one with whitespace inside it, or longer
 * than any token looked for, matches nothing. */
static void req_token_byte(DaliWebRequest *req, uint8_t ch)
{
    if (ch == (uint8_t)',') {
        req_token_end(req);
        return;
    }
    if (web_is_ows(ch)) {
        if (req->parse.token_state == REQ_TOKEN_IN) {
            req->parse.token_state = REQ_TOKEN_TRAILING;
        }
        return;
    }
    if (req->parse.token_state == REQ_TOKEN_TRAILING ||
        req->parse.token_state == REQ_TOKEN_BAD) {
        req->parse.token_state = REQ_TOKEN_BAD;
        return;
    }
    if (req->parse.token_len >= DALI_WEB_TOKEN_MAX) {
        req->parse.token_state = REQ_TOKEN_BAD;
        return;
    }
    req->parse.token_state = REQ_TOKEN_IN;
    req->parse.token[req->parse.token_len++] = web_lower((char)ch);
}

static void req_begin_value(DaliWebRequest *req)
{
    req->parse.name[req->parse.len] = '\0';
    req->parse.field = REQ_FIELD_OTHER;

    if (!req->parse.name_overflow) {
        for (size_t i = 0u; i < sizeof(k_kept_headers) / sizeof(k_kept_headers[0]); i++) {
            const WebKeptHeader *kept = &k_kept_headers[i];
            if (strcmp(req->parse.name, kept->name) != 0) {
                continue;
            }
            if (kept->once_flag != 0u) {
                if ((req->flags & kept->once_flag) != 0u) {
                    req_fail(req, DALI_WEB_REFUSE_DUPLICATE_HEADER, false);
                    return;
                }
                req->flags |= kept->once_flag;
            }
            req->parse.field = kept->field;
            break;
        }
    }

    req->parse.len = 0u;
    req->parse.token_len = 0u;
    req->parse.token_state = REQ_TOKEN_LEADING;
    req->parse.phase = REQ_PHASE_VALUE;
}

static void req_value_byte(DaliWebRequest *req, uint8_t ch)
{
    switch (req->parse.field) {
    case REQ_FIELD_OTHER:
        return;
    case REQ_FIELD_UPGRADE:
    case REQ_FIELD_CONNECTION:
        req_token_byte(req, ch);
        return;
    default:
        break;
    }

    if (req->parse.len == 0u && web_is_ows(ch)) {
        return;  /* leading OWS */
    }
    size_t cap;
    uint16_t overflow_flag;
    char *buf = req_field_buffer(req, req->parse.field, &cap, &overflow_flag);
    if (buf == NULL) {
        return;
    }
    if (req->parse.len < cap) {
        buf[req->parse.len++] = (char)ch;
    } else {
        req->flags |= overflow_flag;
    }
}

static void req_finish_value(DaliWebRequest *req)
{
    switch (req->parse.field) {
    case REQ_FIELD_OTHER:
        return;
    case REQ_FIELD_UPGRADE:
    case REQ_FIELD_CONNECTION:
        req_token_end(req);
        return;
    default:
        break;
    }

    size_t cap;
    uint16_t overflow_flag;
    char *buf = req_field_buffer(req, req->parse.field, &cap, &overflow_flag);
    if (buf == NULL) {
        return;
    }
    uint8_t len = req->parse.len;
    while (len > 0u && web_is_ows((uint8_t)buf[len - 1u])) {
        len--;  /* trailing OWS */
    }
    buf[len] = '\0';
}

static void req_finish_version(DaliWebRequest *req)
{
    const char *v = req->parse.version;
    req->parse.version[req->parse.len] = '\0';

    if (strcmp(v, "HTTP/1.1") == 0) {
        req->parse.phase = REQ_PHASE_LINE_START;
        return;
    }
    /* Well-formed but another version: say so, rather than "malformed". */
    bool http_shaped = req->parse.len == 8u && memcmp(v, "HTTP/", 5u) == 0 &&
                       web_is_digit(v[5]) && v[6] == '.' && web_is_digit(v[7]);
    req_fail(req, http_shaped ? DALI_WEB_REFUSE_HTTP_VERSION : DALI_WEB_REFUSE_MALFORMED,
             true);
}

static void req_end_of_line(DaliWebRequest *req)
{
    switch (req->parse.phase) {
    case REQ_PHASE_VERSION:
        req_finish_version(req);
        return;
    case REQ_PHASE_LINE_START:
        req->state = DALI_WEB_REQUEST_DONE;
        return;
    case REQ_PHASE_VALUE:
        req_finish_value(req);
        req->parse.phase = REQ_PHASE_LINE_START;
        return;
    case REQ_PHASE_METHOD:  /* a request line with no target or no version */
    case REQ_PHASE_TARGET:
    case REQ_PHASE_NAME:    /* a header line with no colon */
    default:
        req_fail(req, DALI_WEB_REFUSE_MALFORMED, true);
        return;
    }
}

static void req_line_byte(DaliWebRequest *req, uint8_t ch)
{
    switch (req->parse.phase) {
    case REQ_PHASE_METHOD:
        if (ch == (uint8_t)' ' && req->parse.len > 0u) {
            req->parse.len = 0u;
            req->parse.phase = REQ_PHASE_TARGET;
            return;
        }
        if (!web_is_tchar(ch)) {
            req_fail(req, DALI_WEB_REFUSE_MALFORMED, false);
            return;
        }
        if (req->parse.len < DALI_WEB_METHOD_MAX) {
            req->method[req->parse.len++] = (char)ch;
        } else {
            req->flags |= DALI_WEB_METHOD_OVERFLOW;
        }
        return;

    case REQ_PHASE_TARGET:
        if (ch == (uint8_t)' ' && req->parse.len > 0u) {
            req->parse.len = 0u;
            req->parse.phase = REQ_PHASE_VERSION;
            return;
        }
        /* Origin-form only: the first byte is '/'. */
        if (!web_is_vchar(ch) || (req->parse.len == 0u && ch != (uint8_t)'/')) {
            req_fail(req, DALI_WEB_REFUSE_MALFORMED, false);
            return;
        }
        if (req->parse.len >= DALI_WEB_TARGET_MAX) {
            req_fail(req, DALI_WEB_REFUSE_TARGET_TOO_LONG, false);
            return;
        }
        req->target[req->parse.len++] = (char)ch;
        return;

    case REQ_PHASE_VERSION:
        if (!web_is_vchar(ch) || req->parse.len >= sizeof(req->parse.version) - 1u) {
            req_fail(req, DALI_WEB_REFUSE_MALFORMED, false);
            return;
        }
        req->parse.version[req->parse.len++] = (char)ch;
        return;

    case REQ_PHASE_LINE_START:
        /* Leading whitespace is obs-fold: a continuation of the previous
         * header that RFC 9112 §5.2 lets a server refuse. */
        if (!web_is_tchar(ch)) {
            req_fail(req, DALI_WEB_REFUSE_MALFORMED, false);
            return;
        }
        req->parse.len = 0u;
        req->parse.name_overflow = false;
        req->parse.phase = REQ_PHASE_NAME;
        req_line_byte(req, ch);  /* this byte is the first of the name */
        return;

    case REQ_PHASE_NAME:
        if (ch == (uint8_t)':' && req->parse.len > 0u) {
            req_begin_value(req);
            return;
        }
        /* Whitespace before the colon lands here too: RFC 9112 §5.1. */
        if (!web_is_tchar(ch)) {
            req_fail(req, DALI_WEB_REFUSE_MALFORMED, false);
            return;
        }
        if (req->parse.len < DALI_WEB_NAME_MAX) {
            req->parse.name[req->parse.len++] = web_lower((char)ch);
        } else {
            req->parse.name_overflow = true;
        }
        return;

    case REQ_PHASE_VALUE:
        /* Field values may carry HTAB, SP, VCHAR and obs-text; no other
         * control character, NUL included. */
        if ((ch < 0x20u && ch != (uint8_t)'\t') || ch == 0x7Fu) {
            req_fail(req, DALI_WEB_REFUSE_MALFORMED, false);
            return;
        }
        req_value_byte(req, ch);
        return;

    default:
        return;
    }
}

static void req_consume(DaliWebRequest *req, uint8_t ch)
{
    if (req->parse.phase == REQ_PHASE_SKIP) {
        /* Only the end of the head matters now. A CR changes nothing, so CRLF
         * and bare LF both end a line, and an empty line ends the head. */
        if (ch == (uint8_t)'\n') {
            if (req->parse.skip_blank) {
                req->state = DALI_WEB_REQUEST_INVALID;
                return;
            }
            req->parse.skip_blank = true;
        } else if (ch != (uint8_t)'\r') {
            req->parse.skip_blank = false;
        }
        return;
    }

    if (req->parse.pending_cr) {
        req->parse.pending_cr = false;
        if (ch != (uint8_t)'\n') {
            /* A bare CR: RFC 9112 §2.2 lets a recipient refuse it. */
            req_fail(req, DALI_WEB_REFUSE_MALFORMED, false);
            return;
        }
        req_end_of_line(req);
        return;
    }
    if (ch == (uint8_t)'\r') {
        req->parse.pending_cr = true;
        return;
    }
    if (ch == (uint8_t)'\n') {
        req_end_of_line(req);
        return;
    }
    req_line_byte(req, ch);
}

DaliWebRequestState dali_web_request_feed(DaliWebRequest *req, uint8_t ch)
{
    if (req == NULL) {
        return DALI_WEB_REQUEST_INVALID;
    }
    if (req->state != DALI_WEB_REQUEST_MORE) {
        return req->state;
    }

    /* A recorded fault always moves the parser to its skip phase, which ends
     * only in INVALID, so DONE never carries a refusal. */
    req->parse.head_len++;
    req_consume(req, ch);

    if (req->state == DALI_WEB_REQUEST_MORE && req->parse.head_len >= DALI_WEB_HEAD_MAX) {
        if (req->refusal == DALI_WEB_REFUSE_NONE) {
            req->refusal = DALI_WEB_REFUSE_HEAD_TOO_LARGE;
        }
        req->state = DALI_WEB_REQUEST_INVALID;
    }
    return req->state;
}

/* ---------------------------------------------------------------------------
 * Routing
 * --------------------------------------------------------------------------*/

bool dali_web_host_allowed(const DaliWebHostPolicy *policy, const char *host)
{
    if (policy == NULL || policy->names == NULL || host == NULL) {
        return false;
    }

    size_t name_len;
    const char *port_text = NULL;

    if (host[0] == '[') {
        /* An IPv6 literal keeps its brackets; only what follows the ']' can
         * be a port. */
        const char *close = strchr(host, ']');
        if (close == NULL) {
            return false;
        }
        name_len = (size_t)(close - host) + 1u;
        if (host[name_len] == ':') {
            port_text = host + name_len + 1u;
        } else if (host[name_len] != '\0') {
            return false;
        }
    } else {
        const char *colon = strchr(host, ':');
        name_len = colon != NULL ? (size_t)(colon - host) : strlen(host);
        if (colon != NULL) {
            port_text = colon + 1;
        }
    }
    if (name_len == 0u) {
        return false;
    }

    /* No port named means the scheme's default: a browser omits only that. */
    uint32_t port = 80u;
    if (port_text != NULL) {
        size_t digits = strlen(port_text);
        if (digits == 0u || digits > 5u) {
            return false;
        }
        port = 0u;
        for (size_t i = 0u; i < digits; i++) {
            if (!web_is_digit(port_text[i])) {
                return false;
            }
            port = port * 10u + (uint32_t)(port_text[i] - '0');
        }
    }
    if (port != policy->port) {
        return false;
    }

    for (uint8_t i = 0u; i < policy->name_count; i++) {
        const char *name = policy->names[i];
        if (name != NULL && strlen(name) == name_len &&
            web_equal_nocase_n(host, name, name_len)) {
            return true;
        }
    }
    return false;
}

/* A browser's Origin for a page it loaded from `host` is "http://" + host,
 * serialised by the same URL parser that wrote Host, so the two agree byte
 * for byte apart from case. */
static bool web_origin_matches(const char *origin, const char *host)
{
    static const char scheme[] = "http://";
    size_t scheme_len = sizeof(scheme) - 1u;
    return strlen(origin) > scheme_len &&
           web_equal_nocase_n(origin, scheme, scheme_len) &&
           web_equal_nocase(origin + scheme_len, host);
}

static DaliWebRoute web_refuse(DaliWebRefusal refusal)
{
    DaliWebRoute route = { DALI_WEB_ROUTE_REFUSE, refusal };
    return route;
}

static DaliWebRoute web_route_socket(const DaliWebRequest *req)
{
    const uint16_t upgrade = DALI_WEB_UPGRADE_WEBSOCKET | DALI_WEB_CONNECTION_UPGRADE;
    if ((req->flags & upgrade) != upgrade) {
        return web_refuse(DALI_WEB_REFUSE_NOT_UPGRADE);
    }
    if ((req->flags & DALI_WEB_HAS_ORIGIN) == 0u) {
        return web_refuse(DALI_WEB_REFUSE_NO_ORIGIN);
    }
    if ((req->flags & DALI_WEB_ORIGIN_OVERFLOW) != 0u ||
        !web_origin_matches(req->origin, req->host)) {
        return web_refuse(DALI_WEB_REFUSE_ORIGIN);
    }
    if ((req->flags & DALI_WEB_HAS_WS_VERSION) == 0u ||
        (req->flags & DALI_WEB_WS_VERSION_OVERFLOW) != 0u ||
        strcmp(req->ws_version, "13") != 0) {
        return web_refuse(DALI_WEB_REFUSE_WS_VERSION);
    }
    if ((req->flags & DALI_WEB_HAS_WS_KEY) == 0u ||
        (req->flags & DALI_WEB_WS_KEY_OVERFLOW) != 0u ||
        !dali_web_ws_key_valid(req->ws_key)) {
        return web_refuse(DALI_WEB_REFUSE_WS_KEY);
    }
    DaliWebRoute route = { DALI_WEB_ROUTE_SOCKET, DALI_WEB_REFUSE_NONE };
    return route;
}

DaliWebRoute dali_web_route(const DaliWebRequest *req, const DaliWebHostPolicy *policy)
{
    if (req == NULL || req->state == DALI_WEB_REQUEST_MORE) {
        return web_refuse(DALI_WEB_REFUSE_MALFORMED);
    }
    if (req->state == DALI_WEB_REQUEST_INVALID) {
        return web_refuse(req->refusal != DALI_WEB_REFUSE_NONE ? req->refusal
                                                               : DALI_WEB_REFUSE_MALFORMED);
    }

    if ((req->flags & DALI_WEB_HAS_HOST) == 0u) {
        return web_refuse(DALI_WEB_REFUSE_NO_HOST);
    }
    if ((req->flags & DALI_WEB_HOST_OVERFLOW) != 0u ||
        !dali_web_host_allowed(policy, req->host)) {
        return web_refuse(DALI_WEB_REFUSE_HOST);
    }
    if ((req->flags & DALI_WEB_METHOD_OVERFLOW) != 0u || strcmp(req->method, "GET") != 0) {
        return web_refuse(DALI_WEB_REFUSE_METHOD);
    }

    size_t path_len = strcspn(req->target, "?");
    if (path_len == 1u && req->target[0] == '/') {
        DaliWebRoute route = { DALI_WEB_ROUTE_PAGE, DALI_WEB_REFUSE_NONE };
        return route;
    }
    if (path_len == 3u && memcmp(req->target, "/ws", 3u) == 0) {
        return web_route_socket(req);
    }
    return web_refuse(DALI_WEB_REFUSE_NOT_FOUND);
}

/* ---------------------------------------------------------------------------
 * Response heads
 * --------------------------------------------------------------------------*/

/* snprintf's result as a length, or 0 with an empty buffer when it did not
 * fit: a truncated head would be a different, wrong response. */
static size_t web_finish(char *out, size_t cap, int written)
{
    if (written < 0 || (size_t)written >= cap) {
        out[0] = '\0';
        return 0u;
    }
    return (size_t)written;
}

size_t dali_web_format_handshake(const DaliWebRequest *req, char *out, size_t cap)
{
    if (out == NULL || cap == 0u) {
        return 0u;
    }
    if (req == NULL || !dali_web_ws_key_valid(req->ws_key)) {
        out[0] = '\0';
        return 0u;
    }
    char accept[DALI_WEB_WS_ACCEPT_LEN + 1u];
    dali_web_ws_accept(req->ws_key, accept);
    return web_finish(out, cap, snprintf(out, cap,
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n",
        accept));
}

size_t dali_web_format_page_head(uint32_t content_length, bool gzip,
                                 char *out, size_t cap)
{
    if (out == NULL || cap == 0u) {
        return 0u;
    }
    /* frame-ancestors and X-Frame-Options both forbid framing: the first for
     * browsers that read CSP, the second for those that do not. no-cache
     * revalidates, so a page from before an OTA is never reused. */
    return web_finish(out, cap, snprintf(out, cap,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "%s"
        "Content-Length: %lu\r\n"
        "Cache-Control: no-cache\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Content-Security-Policy: frame-ancestors 'none'\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "Connection: close\r\n"
        "\r\n",
        gzip ? "Content-Encoding: gzip\r\n" : "",
        (unsigned long)content_length));
}

size_t dali_web_format_refusal(DaliWebRefusal refusal, char *out, size_t cap)
{
    if (out == NULL || cap == 0u) {
        return 0u;
    }
    if (refusal == DALI_WEB_REFUSE_NONE || (size_t)refusal >= WEB_REFUSAL_COUNT) {
        out[0] = '\0';
        return 0u;
    }

    DaliWebStatus status = dali_web_refusal_status(refusal);
    const char *text = dali_web_refusal_text(refusal);

    /* RFC 9110 §15.5.6: a 405 lists what is allowed. §15.5.22 and §7.8: a 426
     * names the protocol to upgrade to, and Upgrade is a connection option.
     * RFC 6455 §4.4: a version refusal names the versions served. */
    const char *extra = "";
    const char *connection = "close";
    if (status == DALI_WEB_STATUS_METHOD_NOT_ALLOWED) {
        extra = "Allow: GET\r\n";
    } else if (status == DALI_WEB_STATUS_UPGRADE_REQUIRED) {
        extra = "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n";
        connection = "Upgrade, close";
    }

    return web_finish(out, cap, snprintf(out, cap,
        "HTTP/1.1 %u %s\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %u\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "%s"
        "Connection: %s\r\n"
        "\r\n"
        "%s\n",
        (unsigned)status, web_reason_phrase(status),
        (unsigned)(strlen(text) + 1u),
        extra, connection, text));
}

/* ---------------------------------------------------------------------------
 * WebSocket frames
 * --------------------------------------------------------------------------*/

enum {
    WS_PHASE_HEAD0 = 0,  /* FIN, RSV, opcode */
    WS_PHASE_HEAD1,      /* MASK, payload length */
    WS_PHASE_MASK,
    WS_PHASE_PAYLOAD,
    WS_PHASE_DONE,       /* closed or failed; final */
};

void dali_web_ws_receiver_init(DaliWebWsReceiver *rx)
{
    if (rx != NULL) {
        memset(rx, 0, sizeof(*rx));
    }
}

static DaliWebWsRxEvent ws_fail(DaliWebWsReceiver *rx, DaliWebWsFailure failure,
                                DaliWebWsCloseCode code)
{
    rx->failure = failure;
    rx->close_code = (uint16_t)code;
    rx->payload_len = 0u;
    rx->payload[0] = 0u;
    rx->rx.phase = WS_PHASE_DONE;
    rx->rx.terminal = DALI_WEB_WS_RX_FAIL;
    return DALI_WEB_WS_RX_FAIL;
}

static DaliWebWsRxEvent ws_complete(DaliWebWsReceiver *rx)
{
    rx->payload_len = rx->rx.len;
    rx->payload[rx->rx.len] = 0u;
    rx->rx.phase = WS_PHASE_HEAD0;

    switch (rx->rx.opcode) {
    case DALI_WEB_WS_OP_TEXT:
        return DALI_WEB_WS_RX_TEXT;
    case DALI_WEB_WS_OP_PING:
        return DALI_WEB_WS_RX_PING;
    case DALI_WEB_WS_OP_PONG:
        return DALI_WEB_WS_RX_PONG;
    case DALI_WEB_WS_OP_CLOSE:
    default:
        /* RFC 6455 §5.5.1: a close body is empty, or a 2-byte code first. */
        if (rx->rx.len == 1u) {
            return ws_fail(rx, DALI_WEB_WS_FAIL_CLOSE_PAYLOAD, DALI_WEB_WS_CLOSE_PROTOCOL);
        }
        rx->close_code = rx->rx.len >= 2u
                             ? (uint16_t)(((uint16_t)rx->payload[0] << 8) | rx->payload[1])
                             : (uint16_t)DALI_WEB_WS_CLOSE_NO_STATUS;
        rx->rx.phase = WS_PHASE_DONE;
        rx->rx.terminal = DALI_WEB_WS_RX_CLOSE;
        return DALI_WEB_WS_RX_CLOSE;
    }
}

DaliWebWsRxEvent dali_web_ws_receive(DaliWebWsReceiver *rx, uint8_t byte)
{
    if (rx == NULL) {
        return DALI_WEB_WS_RX_FAIL;
    }

    switch (rx->rx.phase) {
    case WS_PHASE_HEAD0: {
        bool fin = (byte & 0x80u) != 0u;
        uint8_t opcode = byte & 0x0Fu;
        if ((byte & 0x70u) != 0u) {
            return ws_fail(rx, DALI_WEB_WS_FAIL_RESERVED_BITS, DALI_WEB_WS_CLOSE_PROTOCOL);
        }
        switch (opcode) {
        case DALI_WEB_WS_OP_CONTINUATION:
            /* No fragmented message is ever started, so there is nothing for
             * a continuation to continue. */
            return ws_fail(rx, DALI_WEB_WS_FAIL_CONTINUATION, DALI_WEB_WS_CLOSE_PROTOCOL);
        case DALI_WEB_WS_OP_TEXT:
            /* A browser fragments only a message far longer than any line. */
            if (!fin) {
                return ws_fail(rx, DALI_WEB_WS_FAIL_FRAGMENTED, DALI_WEB_WS_CLOSE_TOO_BIG);
            }
            break;
        case DALI_WEB_WS_OP_BINARY:
            return ws_fail(rx, DALI_WEB_WS_FAIL_BINARY, DALI_WEB_WS_CLOSE_UNSUPPORTED);
        case DALI_WEB_WS_OP_CLOSE:
        case DALI_WEB_WS_OP_PING:
        case DALI_WEB_WS_OP_PONG:
            /* RFC 6455 §5.5: control frames are never fragmented. */
            if (!fin) {
                return ws_fail(rx, DALI_WEB_WS_FAIL_FRAGMENTED, DALI_WEB_WS_CLOSE_PROTOCOL);
            }
            break;
        default:
            return ws_fail(rx, DALI_WEB_WS_FAIL_RESERVED_OPCODE, DALI_WEB_WS_CLOSE_PROTOCOL);
        }
        rx->rx.opcode = opcode;
        rx->rx.phase = WS_PHASE_HEAD1;
        return DALI_WEB_WS_RX_MORE;
    }

    case WS_PHASE_HEAD1: {
        if ((byte & 0x80u) == 0u) {
            /* RFC 6455 §5.1: a server must fail an unmasked client frame. */
            return ws_fail(rx, DALI_WEB_WS_FAIL_UNMASKED, DALI_WEB_WS_CLOSE_PROTOCOL);
        }
        uint8_t len = byte & 0x7Fu;
        if (len > DALI_WEB_WS_PAYLOAD_MAX) {
            bool control = rx->rx.opcode >= DALI_WEB_WS_OP_CLOSE;
            return ws_fail(rx, DALI_WEB_WS_FAIL_TOO_LONG,
                           control ? DALI_WEB_WS_CLOSE_PROTOCOL : DALI_WEB_WS_CLOSE_TOO_BIG);
        }
        rx->rx.len = len;
        rx->rx.pos = 0u;
        rx->rx.phase = WS_PHASE_MASK;
        return DALI_WEB_WS_RX_MORE;
    }

    case WS_PHASE_MASK:
        rx->rx.mask[rx->rx.pos++] = byte;
        if (rx->rx.pos < 4u) {
            return DALI_WEB_WS_RX_MORE;
        }
        rx->rx.pos = 0u;
        if (rx->rx.len == 0u) {
            return ws_complete(rx);
        }
        rx->rx.phase = WS_PHASE_PAYLOAD;
        return DALI_WEB_WS_RX_MORE;

    case WS_PHASE_PAYLOAD: {
        uint8_t c = (uint8_t)(byte ^ rx->rx.mask[rx->rx.pos & 3u]);
        if (rx->rx.opcode == DALI_WEB_WS_OP_TEXT && (c < 0x20u || c > 0x7Eu)) {
            return ws_fail(rx, DALI_WEB_WS_FAIL_NOT_A_LINE, DALI_WEB_WS_CLOSE_INVALID_DATA);
        }
        rx->payload[rx->rx.pos++] = c;
        if (rx->rx.pos < rx->rx.len) {
            return DALI_WEB_WS_RX_MORE;
        }
        return ws_complete(rx);
    }

    case WS_PHASE_DONE:
    default:
        return rx->rx.terminal;
    }
}

size_t dali_web_ws_frame_header(DaliWebWsOpcode opcode, uint32_t payload_len,
                                uint8_t *out, size_t cap)
{
    bool control;
    switch (opcode) {
    case DALI_WEB_WS_OP_TEXT:
    case DALI_WEB_WS_OP_BINARY:
        control = false;
        break;
    case DALI_WEB_WS_OP_CLOSE:
    case DALI_WEB_WS_OP_PING:
    case DALI_WEB_WS_OP_PONG:
        control = true;
        break;
    default:
        return 0u;
    }
    if (out == NULL || (control && payload_len > DALI_WEB_WS_PAYLOAD_MAX)) {
        return 0u;
    }

    size_t need = payload_len <= DALI_WEB_WS_PAYLOAD_MAX ? 2u
                : payload_len <= 0xFFFFu                 ? 4u
                                                         : 10u;
    if (cap < need) {
        return 0u;
    }

    out[0] = (uint8_t)(0x80u | (uint8_t)opcode);  /* FIN, no RSV */
    if (need == 2u) {
        out[1] = (uint8_t)payload_len;              /* server frames are unmasked */
    } else if (need == 4u) {
        out[1] = 126u;
        out[2] = (uint8_t)(payload_len >> 8);
        out[3] = (uint8_t)payload_len;
    } else {
        out[1] = 127u;
        out[2] = 0u;
        out[3] = 0u;
        out[4] = 0u;
        out[5] = 0u;
        out[6] = (uint8_t)(payload_len >> 24);
        out[7] = (uint8_t)(payload_len >> 16);
        out[8] = (uint8_t)(payload_len >> 8);
        out[9] = (uint8_t)payload_len;
    }
    return need;
}

size_t dali_web_ws_close_frame(DaliWebWsCloseCode code, const char *reason,
                               uint8_t *out, size_t cap)
{
    /* 1005 reports a close that carried no code; RFC 6455 §7.4.1 forbids
     * sending it, so it sends exactly that: a close with no code. */
    bool has_code = code != DALI_WEB_WS_CLOSE_NONE && code != DALI_WEB_WS_CLOSE_NO_STATUS;
    size_t reason_len = 0u;
    if (has_code && reason != NULL) {
        reason_len = strlen(reason);
        if (reason_len > DALI_WEB_WS_PAYLOAD_MAX - 2u) {
            reason_len = DALI_WEB_WS_PAYLOAD_MAX - 2u;
        }
    }
    size_t payload_len = has_code ? 2u + reason_len : 0u;

    size_t head = dali_web_ws_frame_header(DALI_WEB_WS_OP_CLOSE, (uint32_t)payload_len,
                                           out, cap);
    if (head == 0u || cap < head + payload_len) {
        return 0u;
    }
    if (has_code) {
        out[head] = (uint8_t)((uint16_t)code >> 8);
        out[head + 1u] = (uint8_t)code;
        if (reason_len > 0u) {
            memcpy(out + head + 2u, reason, reason_len);
        }
    }
    return head + payload_len;
}

/* ---------------------------------------------------------------------------
 * Handshake primitives
 * --------------------------------------------------------------------------*/

static const char k_base64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int web_base64_index(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

size_t dali_web_base64_encode(const uint8_t *in, size_t len, char *out, size_t cap)
{
    if (out == NULL || cap == 0u) {
        return 0u;
    }
    size_t need = 4u * ((len + 2u) / 3u);
    if (need + 1u > cap || (len > 0u && in == NULL)) {
        out[0] = '\0';
        return 0u;
    }

    size_t o = 0u;
    for (size_t i = 0u; i < len; i += 3u) {
        uint32_t chunk = (uint32_t)in[i] << 16;
        if (i + 1u < len) {
            chunk |= (uint32_t)in[i + 1u] << 8;
        }
        if (i + 2u < len) {
            chunk |= (uint32_t)in[i + 2u];
        }
        out[o++] = k_base64[(chunk >> 18) & 0x3Fu];
        out[o++] = k_base64[(chunk >> 12) & 0x3Fu];
        out[o++] = i + 1u < len ? k_base64[(chunk >> 6) & 0x3Fu] : '=';
        out[o++] = i + 2u < len ? k_base64[chunk & 0x3Fu] : '=';
    }
    out[o] = '\0';
    return o;
}

bool dali_web_ws_key_valid(const char *key)
{
    if (key == NULL || strlen(key) != DALI_WEB_WS_KEY_LEN) {
        return false;
    }
    for (size_t i = 0u; i < 22u; i++) {
        if (web_base64_index(key[i]) < 0) {
            return false;
        }
    }
    if (key[22] != '=' || key[23] != '=') {
        return false;
    }
    /* 16 bytes are 128 bits: the 22nd character carries the last 2 and four
     * zero bits, so a canonical encoding ends in A, Q, g or w. */
    return (web_base64_index(key[21]) & 0x0F) == 0;
}

static uint32_t web_rol(uint32_t v, unsigned n)
{
    return (v << n) | (v >> (32u - n));
}

/* FIPS 180-4 §6.1.2, with the message schedule kept as a 16-word ring rather
 * than 80 words, so a call costs 64 bytes of stack instead of 320. */
static void web_sha1_block(uint32_t h[5], const uint8_t block[64])
{
    uint32_t w[16];
    for (unsigned i = 0u; i < 16u; i++) {
        w[i] = ((uint32_t)block[4u * i] << 24) | ((uint32_t)block[4u * i + 1u] << 16) |
               ((uint32_t)block[4u * i + 2u] << 8) | (uint32_t)block[4u * i + 3u];
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (unsigned i = 0u; i < 80u; i++) {
        if (i >= 16u) {
            /* W[t] = ROTL1(W[t-3] ^ W[t-8] ^ W[t-14] ^ W[t-16]) */
            w[i & 15u] = web_rol(w[(i + 13u) & 15u] ^ w[(i + 8u) & 15u] ^
                                 w[(i + 2u) & 15u] ^ w[i & 15u], 1u);
        }
        uint32_t f, k;
        if (i < 20u) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40u) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60u) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t t = web_rol(a, 5u) + f + e + k + w[i & 15u];
        e = d;
        d = c;
        c = web_rol(b, 30u);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

void dali_web_sha1(const uint8_t *data, size_t len, uint8_t digest[DALI_WEB_SHA1_LEN])
{
    uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };

    size_t full = len / 64u;
    for (size_t i = 0u; i < full; i++) {
        web_sha1_block(h, data + 64u * i);
    }

    /* The rest, the 0x80 marker, and the 64-bit bit count. A remainder of 56
     * bytes or more leaves no room for the count, so it spills a block. */
    uint8_t tail[128];
    memset(tail, 0, sizeof(tail));
    size_t rem = len - 64u * full;
    if (rem > 0u) {
        memcpy(tail, data + 64u * full, rem);
    }
    tail[rem] = 0x80u;
    size_t tail_len = rem < 56u ? 64u : 128u;
    uint64_t bits = (uint64_t)len * 8u;
    for (unsigned i = 0u; i < 8u; i++) {
        tail[tail_len - 1u - i] = (uint8_t)(bits >> (8u * i));
    }
    web_sha1_block(h, tail);
    if (tail_len == 128u) {
        web_sha1_block(h, tail + 64u);
    }

    for (unsigned i = 0u; i < 5u; i++) {
        digest[4u * i]      = (uint8_t)(h[i] >> 24);
        digest[4u * i + 1u] = (uint8_t)(h[i] >> 16);
        digest[4u * i + 2u] = (uint8_t)(h[i] >> 8);
        digest[4u * i + 3u] = (uint8_t)h[i];
    }
}

void dali_web_ws_accept(const char *key, char out[DALI_WEB_WS_ACCEPT_LEN + 1u])
{
    /* RFC 6455 §1.3 */
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t buf[DALI_WEB_WS_KEY_LEN + sizeof(guid) - 1u];
    uint8_t digest[DALI_WEB_SHA1_LEN];

    memcpy(buf, key, DALI_WEB_WS_KEY_LEN);
    memcpy(buf + DALI_WEB_WS_KEY_LEN, guid, sizeof(guid) - 1u);
    dali_web_sha1(buf, sizeof(buf), digest);
    (void)dali_web_base64_encode(digest, sizeof(digest), out, DALI_WEB_WS_ACCEPT_LEN + 1u);
}
