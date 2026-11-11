#pragma once

/*
 * dali_web.h — the portable half of a browser front end to the shell
 *
 * A web binding serves one page and upgrades one path to a WebSocket that
 * carries a shell session. Everything that decides what a byte from the
 * network means is here, as pure functions over caller-owned state, so the
 * host suite can drive it the way it drives dali_cli_peer_sniff():
 *
 *   - an HTTP/1.1 request-head parser, fed one byte at a time;
 *   - the routing decision, including the Host and Origin checks that keep a
 *     web page elsewhere from reaching the shell;
 *   - the response heads: the page, the handshake, and every refusal;
 *   - the WebSocket (RFC 6455) frame receiver and the server frame encoder;
 *   - SHA-1 and base64, which the handshake needs.
 *
 * The binding moves bytes and owns each connection's lifetime, as
 * dali_shell_tcp.cpp does. Nothing here allocates, logs, or touches a socket,
 * and nothing here knows about the shell: a text frame is a line of printable
 * ASCII, and what the line means is still the shell's to decide.
 *
 * ── Why a page elsewhere cannot reach the shell ─────────────────────────────
 *
 * Browsers apply no CORS to a WebSocket, and a page that opens one reads every
 * reply. Two checks close that, both decided in dali_web_route():
 *
 *   Host     must name this device: an entry in DaliWebHostPolicy, and the
 *            port the listener is on. This refuses DNS rebinding, where a
 *            page served from the attacker's name has that name resolve to
 *            the device, so Host and Origin both carry the attacker's name and
 *            agree with each other.
 *   Origin   must be present on an upgrade and be exactly "http://" + Host.
 *            This refuses cross-site WebSocket hijacking, where a page served
 *            from anywhere opens ws://<device>/ws. Every browser sends Origin
 *            on an upgrade, so a missing one is refused too.
 *
 * The page head also forbids framing. Without that, a page elsewhere could
 * show the device's own page in an invisible frame, where its socket passes
 * both checks, and steer the operator's clicks into it.
 *
 * Nothing here authenticates anyone. Whoever can reach the port on the LAN can
 * use the shell, as with the TCP shell and OTA.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Limits
 * --------------------------------------------------------------------------*/

/* The whole request head, request line and blank line included. A browser's
 * upgrade request is about 600 bytes; cookies on a LAN name can add some. */
#define DALI_WEB_HEAD_MAX        4096u

/* Longest method kept. Only GET is served, so a longer one is refused as an
 * unsupported method rather than as a malformed request. */
#define DALI_WEB_METHOD_MAX         7u

/* Longest request target served. Both paths are short; a longer one is 414. */
#define DALI_WEB_TARGET_MAX        63u

/* Longest Host kept, port included. A longer one cannot equal any allowed
 * name, so it is refused as not this device's name, not as malformed. */
#define DALI_WEB_HOST_MAX          95u

/* "http://" plus the longest Host. */
#define DALI_WEB_ORIGIN_MAX       (7u + DALI_WEB_HOST_MAX)

/* A Sec-WebSocket-Key is 16 random bytes in base64, and the accept value is a
 * SHA-1 digest in base64. */
#define DALI_WEB_WS_KEY_LEN        24u
#define DALI_WEB_WS_ACCEPT_LEN     28u

/* Longest payload of a frame from the client: the 7-bit length form. A shell
 * line is at most DALI_SHELL_LINE_MAX (80) characters, and a control frame is
 * never longer than this, so no client frame needs the extended forms. */
#define DALI_WEB_WS_PAYLOAD_MAX   125u

#define DALI_WEB_SHA1_LEN          20u

/* ---------------------------------------------------------------------------
 * HTTP statuses and refusals
 * --------------------------------------------------------------------------*/

typedef enum {
    DALI_WEB_STATUS_SWITCHING_PROTOCOLS   = 101,
    DALI_WEB_STATUS_OK                    = 200,
    DALI_WEB_STATUS_BAD_REQUEST           = 400,
    DALI_WEB_STATUS_FORBIDDEN             = 403,
    DALI_WEB_STATUS_NOT_FOUND             = 404,
    DALI_WEB_STATUS_METHOD_NOT_ALLOWED    = 405,
    DALI_WEB_STATUS_URI_TOO_LONG          = 414,
    DALI_WEB_STATUS_UPGRADE_REQUIRED      = 426,
    DALI_WEB_STATUS_HEADERS_TOO_LARGE     = 431,
    DALI_WEB_STATUS_SERVICE_UNAVAILABLE   = 503,
    DALI_WEB_STATUS_VERSION_NOT_SUPPORTED = 505,
} DaliWebStatus;

/*
 * Why a request is not served. Each maps to one status through
 * dali_web_refusal_status(), and to one sentence through
 * dali_web_refusal_text(): the response body, and what a binding logs.
 */
typedef enum {
    DALI_WEB_REFUSE_NONE = 0,
    DALI_WEB_REFUSE_MALFORMED,        /* 400: not a well-formed HTTP/1.1 request */
    DALI_WEB_REFUSE_DUPLICATE_HEADER, /* 400: Host, Origin or a Sec-WebSocket-* twice */
    DALI_WEB_REFUSE_METHOD,           /* 405: anything but GET */
    DALI_WEB_REFUSE_TARGET_TOO_LONG,  /* 414 */
    DALI_WEB_REFUSE_HEAD_TOO_LARGE,   /* 431: DALI_WEB_HEAD_MAX reached */
    DALI_WEB_REFUSE_HTTP_VERSION,     /* 505: an HTTP version other than 1.1 */
    DALI_WEB_REFUSE_NO_HOST,          /* 400 */
    DALI_WEB_REFUSE_HOST,             /* 403: Host is not this device */
    DALI_WEB_REFUSE_NOT_FOUND,        /* 404 */
    DALI_WEB_REFUSE_NOT_UPGRADE,      /* 426: /ws without a WebSocket upgrade */
    DALI_WEB_REFUSE_WS_VERSION,       /* 426: a WebSocket version other than 13 */
    DALI_WEB_REFUSE_WS_KEY,           /* 400: missing or malformed Sec-WebSocket-Key */
    DALI_WEB_REFUSE_NO_ORIGIN,        /* 403: an upgrade without Origin */
    DALI_WEB_REFUSE_ORIGIN,           /* 403: Origin is not "http://" + Host */
    DALI_WEB_REFUSE_UNAVAILABLE,      /* 503: the binding's own call, e.g. a heap floor */
} DaliWebRefusal;

DaliWebStatus dali_web_refusal_status(DaliWebRefusal refusal);
const char   *dali_web_refusal_text(DaliWebRefusal refusal);

/* ---------------------------------------------------------------------------
 * Request head
 *
 * Fed one byte at a time from the start of a connection. The parser keeps only
 * what routing needs; every other header is checked for shape, counted against
 * DALI_WEB_HEAD_MAX, and dropped.
 *
 * A request that is wrong in any way is still read to its blank line before the
 * parser reports it. The binding then answers and closes with the whole head
 * consumed: closing a socket with unread bytes makes lwIP send a reset, and the
 * peer can lose the answer to it. Only DALI_WEB_HEAD_MAX ends the read early.
 * The first fault found is the one reported.
 *
 * What it accepts is narrower than HTTP/1.1 allows, deliberately:
 *   - origin-form targets only ("/..."), so no absolute-form target can carry
 *     an authority that disagrees with Host;
 *   - single spaces in the request line;
 *   - no whitespace between a field name and its colon, and no obs-fold
 *     continuation lines: both are request-smuggling shapes RFC 9112 says to
 *     refuse;
 *   - CRLF or a bare LF ends a line; a CR followed by anything else is
 *     malformed.
 * --------------------------------------------------------------------------*/

typedef enum {
    DALI_WEB_REQUEST_MORE = 0,  /* the head has not ended */
    DALI_WEB_REQUEST_DONE,      /* the head ended and parsed; route it */
    DALI_WEB_REQUEST_INVALID,   /* answer with `refusal`, then close */
} DaliWebRequestState;

/* Which of the kept headers were present. */
#define DALI_WEB_HAS_HOST          0x0001u
#define DALI_WEB_HAS_ORIGIN        0x0002u
#define DALI_WEB_HAS_WS_KEY        0x0004u
#define DALI_WEB_HAS_WS_VERSION    0x0008u
/* Upgrade lists "websocket"; Connection lists "upgrade". */
#define DALI_WEB_UPGRADE_WEBSOCKET 0x0010u
#define DALI_WEB_CONNECTION_UPGRADE 0x0020u
/* The value was longer than its buffer. What is stored is a prefix and must
 * not be compared as though it were the value. */
#define DALI_WEB_HOST_OVERFLOW     0x0100u
#define DALI_WEB_ORIGIN_OVERFLOW   0x0200u
#define DALI_WEB_WS_KEY_OVERFLOW   0x0400u
#define DALI_WEB_WS_VERSION_OVERFLOW 0x0800u
#define DALI_WEB_METHOD_OVERFLOW   0x1000u

#define DALI_WEB_NAME_MAX          24u   /* "sec-websocket-version" is 21 */
#define DALI_WEB_TOKEN_MAX         16u   /* "websocket" is 9 */
#define DALI_WEB_WS_VERSION_MAX     3u

typedef struct {
    DaliWebRequestState state;
    DaliWebRefusal      refusal;   /* set when state is INVALID */
    uint16_t            flags;     /* DALI_WEB_HAS_*, DALI_WEB_*_OVERFLOW */

    /* Results. NUL-terminated; empty when absent. */
    char method[DALI_WEB_METHOD_MAX + 1u];
    char target[DALI_WEB_TARGET_MAX + 1u];
    char host[DALI_WEB_HOST_MAX + 1u];
    char origin[DALI_WEB_ORIGIN_MAX + 1u];
    char ws_key[DALI_WEB_WS_KEY_LEN + 1u];
    char ws_version[DALI_WEB_WS_VERSION_MAX + 1u];

    /* Parser state. Private. */
    struct {
        uint16_t head_len;
        uint8_t  phase;
        uint8_t  field;
        uint8_t  len;          /* of the line element being read */
        uint8_t  token_len;
        uint8_t  token_state;
        bool     pending_cr;
        bool     skip_blank;   /* resynchronising: the line so far is empty */
        bool     name_overflow;
        char     version[9];
        char     name[DALI_WEB_NAME_MAX + 1u];
        char     token[DALI_WEB_TOKEN_MAX + 1u];
    } parse;
} DaliWebRequest;

void dali_web_request_init(DaliWebRequest *req);

/*
 * Feed one byte. Returns the state, which is final once it is not MORE: later
 * bytes return it unchanged and are not examined. The byte that ends the head
 * is the last one the parser consumes, so whatever follows it on the
 * connection belongs to the next protocol, not to this head.
 */
DaliWebRequestState dali_web_request_feed(DaliWebRequest *req, uint8_t ch);

/* ---------------------------------------------------------------------------
 * Routing
 * --------------------------------------------------------------------------*/

/*
 * The names this device answers to, without a port: its IP literal, <name>,
 * <name>.local, and whatever the YAML lists. Compared ASCII case-insensitively
 * and exactly, so "dali.local." with its trailing dot is not "dali.local". An
 * IPv6 literal is listed with its brackets, as it appears in Host.
 *
 * `port` is the listener's. A Host that names a port must name this one, and a
 * Host without one means port 80, as a browser omits only the default.
 */
typedef struct {
    const char *const *names;
    uint8_t            name_count;
    uint16_t           port;
} DaliWebHostPolicy;

typedef enum {
    DALI_WEB_ROUTE_REFUSE = 0,  /* answer with dali_web_format_refusal(), close */
    DALI_WEB_ROUTE_PAGE,        /* GET /: dali_web_format_page_head(), the body, close */
    DALI_WEB_ROUTE_SOCKET,      /* GET /ws: dali_web_format_handshake(), then frames */
} DaliWebRouteKind;

typedef struct {
    DaliWebRouteKind kind;
    DaliWebRefusal   refusal;   /* NONE unless kind is REFUSE */
} DaliWebRoute;

/*
 * Decide what a parsed head gets. Host is checked before anything else, on
 * every path, so a name that is not this device's learns nothing from which
 * paths exist. A query string is ignored; "/" and "/ws" are the only paths.
 * An INVALID head routes to its own refusal.
 */
DaliWebRoute dali_web_route(const DaliWebRequest *req, const DaliWebHostPolicy *policy);

/* True when `host`, a Host header value, names this device. */
bool dali_web_host_allowed(const DaliWebHostPolicy *policy, const char *host);

/* ---------------------------------------------------------------------------
 * Response heads
 *
 * Each writes a complete status line and header block, ending in the blank
 * line, into `out`, and returns its length without the NUL. A buffer too small
 * for the whole head gets nothing usable and returns 0; 512 bytes fits every
 * one of them.
 * --------------------------------------------------------------------------*/

/* 101, with the accept value for the request's key. No extension and no
 * subprotocol is agreed, so every frame keeps its RSV bits clear. */
size_t dali_web_format_handshake(const DaliWebRequest *req, char *out, size_t cap);

/* 200 for the page, which the binding sends after it: `content_length` bytes,
 * gzip-encoded when `gzip` is set. Framing is forbidden, and the connection
 * closes after the body. */
size_t dali_web_format_page_head(uint32_t content_length, bool gzip,
                                 char *out, size_t cap);

/* The refusal's status, its sentence as a text/plain body, and the header a
 * 405 or 426 must carry. Unlike the other two, this writes the body too. */
size_t dali_web_format_refusal(DaliWebRefusal refusal, char *out, size_t cap);

/* ---------------------------------------------------------------------------
 * WebSocket frames
 *
 * What this server accepts from a client, all of it allowed by RFC 6455 as a
 * server's choice except where marked:
 *   - masked frames only (required by the RFC);
 *   - RSV bits clear (required: no extension is negotiated);
 *   - unfragmented: FIN set on every frame;
 *   - payloads of at most DALI_WEB_WS_PAYLOAD_MAX bytes;
 *   - text whose every byte is printable ASCII, 0x20-0x7E. That is valid
 *     UTF-8, which the RFC requires of text, and it means one text frame is
 *     exactly one shell line: no CR or LF inside one can make it two;
 *   - ping, pong and close. Binary and continuation frames are refused.
 *
 * Anything else fails the connection with the close code the RFC assigns.
 * --------------------------------------------------------------------------*/

typedef enum {
    DALI_WEB_WS_OP_CONTINUATION = 0x0,
    DALI_WEB_WS_OP_TEXT         = 0x1,
    DALI_WEB_WS_OP_BINARY       = 0x2,
    DALI_WEB_WS_OP_CLOSE        = 0x8,
    DALI_WEB_WS_OP_PING         = 0x9,
    DALI_WEB_WS_OP_PONG         = 0xA,
} DaliWebWsOpcode;

typedef enum {
    DALI_WEB_WS_CLOSE_NONE         = 0,     /* send a close frame with no status */
    DALI_WEB_WS_CLOSE_NORMAL       = 1000,
    DALI_WEB_WS_CLOSE_GOING_AWAY   = 1001,  /* idle timeout, shutdown */
    DALI_WEB_WS_CLOSE_PROTOCOL     = 1002,
    DALI_WEB_WS_CLOSE_UNSUPPORTED  = 1003,  /* binary */
    DALI_WEB_WS_CLOSE_NO_STATUS    = 1005,  /* received only: a close without a code */
    DALI_WEB_WS_CLOSE_INVALID_DATA = 1007,  /* text that is not a shell line */
    DALI_WEB_WS_CLOSE_TOO_BIG      = 1009,
    DALI_WEB_WS_CLOSE_TRY_AGAIN    = 1013,  /* the shell is busy */
} DaliWebWsCloseCode;

typedef enum {
    DALI_WEB_WS_RX_MORE = 0,  /* frame not complete */
    DALI_WEB_WS_RX_TEXT,      /* `payload` holds one line, NUL-terminated */
    DALI_WEB_WS_RX_PING,      /* answer with a pong carrying `payload` */
    DALI_WEB_WS_RX_PONG,      /* nothing to do */
    DALI_WEB_WS_RX_CLOSE,     /* the peer closed: answer with a close, end the session */
    DALI_WEB_WS_RX_FAIL,      /* send a close with `close_code`, end the session */
} DaliWebWsRxEvent;

typedef enum {
    DALI_WEB_WS_FAIL_NONE = 0,
    DALI_WEB_WS_FAIL_UNMASKED,
    DALI_WEB_WS_FAIL_RESERVED_BITS,
    DALI_WEB_WS_FAIL_RESERVED_OPCODE,
    DALI_WEB_WS_FAIL_CONTINUATION,
    DALI_WEB_WS_FAIL_FRAGMENTED,
    DALI_WEB_WS_FAIL_BINARY,
    DALI_WEB_WS_FAIL_TOO_LONG,
    DALI_WEB_WS_FAIL_NOT_A_LINE,       /* a text byte outside 0x20-0x7E */
    DALI_WEB_WS_FAIL_CLOSE_PAYLOAD,    /* a close payload of one byte */
} DaliWebWsFailure;

typedef struct {
    /* Results of the last event. */
    uint8_t            payload[DALI_WEB_WS_PAYLOAD_MAX + 1u];
    uint8_t            payload_len;
    DaliWebWsFailure   failure;     /* FAIL: what was wrong */
    uint16_t           close_code;  /* FAIL: the code to send. CLOSE: the peer's, or 1005 */

    /* Receiver state. Private. */
    struct {
        uint8_t          phase;
        uint8_t          opcode;
        uint8_t          len;
        uint8_t          pos;
        uint8_t          mask[4];
        DaliWebWsRxEvent terminal;
    } rx;
} DaliWebWsReceiver;

void dali_web_ws_receiver_init(DaliWebWsReceiver *rx);

/*
 * Feed one byte from the client. A complete frame returns its event, and the
 * next byte starts the next frame. CLOSE and FAIL are final: later bytes return
 * the same event and are not examined, so nothing that arrives after a close
 * or a fault is ever delivered as a line.
 */
DaliWebWsRxEvent dali_web_ws_receive(DaliWebWsReceiver *rx, uint8_t byte);

/*
 * The header of one unmasked server frame with FIN set, for a payload the
 * binding sends after it. Returns 2, 4 or 10, or 0 when `cap` is too small,
 * the opcode is not one of the five sent, or a control frame would exceed 125
 * bytes.
 */
size_t dali_web_ws_frame_header(DaliWebWsOpcode opcode, uint32_t payload_len,
                                uint8_t *out, size_t cap);

/*
 * A whole close frame: `code`, then `reason` cut to the 123 bytes a control
 * frame leaves. DALI_WEB_WS_CLOSE_NONE sends no status and no reason. Returns
 * the frame's length, or 0 when it does not fit.
 */
size_t dali_web_ws_close_frame(DaliWebWsCloseCode code, const char *reason,
                               uint8_t *out, size_t cap);

/* ---------------------------------------------------------------------------
 * Handshake primitives
 * --------------------------------------------------------------------------*/

/* True for a Sec-WebSocket-Key that is 16 bytes in canonical base64. */
bool dali_web_ws_key_valid(const char *key);

/* base64(SHA-1(key + RFC 6455 GUID)), NUL-terminated. `key` must be valid. */
void dali_web_ws_accept(const char *key, char out[DALI_WEB_WS_ACCEPT_LEN + 1u]);

void dali_web_sha1(const uint8_t *data, size_t len, uint8_t digest[DALI_WEB_SHA1_LEN]);

/* Standard alphabet with padding, NUL-terminated. Returns the length without
 * the NUL, or 0 when `cap` cannot hold it all (an empty input also returns 0
 * and writes an empty string). */
size_t dali_web_base64_encode(const uint8_t *in, size_t len, char *out, size_t cap);
