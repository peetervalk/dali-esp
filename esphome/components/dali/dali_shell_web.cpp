/*
 * dali_shell_web.cpp — the browser binding for the diagnostic shell
 *
 * The web connections of DaliShellServer: the ones that are not, or are not
 * yet, the session. See dali_shell_tcp.h for the whole front end.
 *
 * Each accepted connection waits in a slot while its request head arrives, a
 * byte at a time, into dali_web_request_feed(). dali_web_route() then decides
 * what it gets: the page, a refusal, or a WebSocket that becomes the session.
 * Nothing here parses HTTP or WebSocket bytes itself; dali_web does, and its
 * host vectors cover every decision.
 *
 * A slot is read without blocking from the same select() as the session, so a
 * browser that opens a connection and sends nothing -- Chrome's preconnect does
 * -- holds a slot until its deadline and never holds the task.
 */

#include "dali_shell_tcp.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

extern "C" {
#include "../../../components/dali/dali_shell.h"
}

#include <cerrno>
#include <cstring>

#include <esp_heap_caps.h>
#include <lwip/sockets.h>

namespace esphome {
namespace dali {

static const char *const TAG = "dali.shell";

/* How long a connection may take to send its request head. */
static constexpr uint32_t WEB_HEAD_TIMEOUT_MS = 10000u;
/* How long an answered connection is read for its close before it is closed
 * regardless. */
static constexpr uint32_t WEB_DRAIN_MS = 2000u;
/* Same as the session's: a browser that stops reading while the page streams
 * must not hold the task. */
static constexpr uint32_t WEB_SEND_TIMEOUT_MS = 10000u;

/*
 * Free internal heap below which the page and the WebSocket are refused with a
 * 503. Measured on 2k (project_log.md, 2026-11-10), a session costs about
 * 300 B once open. The page costs at most the send buffer while it streams,
 * about 5.7 kB. So this is a reserve for what else must still connect -- an OTA,
 * an API client -- rather than a budget for this binding. The figure is a
 * guard, not a measurement. Counted the way the `Heap free (internal)` sensor
 * in _local/dali-2k.yaml counts: byte-addressable internal RAM only.
 */
static constexpr size_t WEB_HEAP_FLOOR_BYTES = 16384u;

/* Every response head dali_web formats fits in 512 bytes; see dali_web.h. */
static constexpr size_t WEB_HEAD_OUT_MAX = 512u;

static const char WEB_BUSY_TEXT[] = "DALI shell busy: another session is connected.";

/* A blocking send of the whole buffer, bounded by SO_SNDTIMEO. */
static bool web_send(int fd, const void *data, size_t len)
{
    const uint8_t *cursor = static_cast<const uint8_t *>(data);
    while (len > 0u) {
        int sent = ::send(fd, cursor, len, 0);
        if (sent > 0) {
            cursor += sent;
            len -= (size_t) sent;
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool DaliShellServer::web_slots_busy() const
{
    for (const auto &slot : web_slots_) {
        if (slot.fd >= 0) {
            return true;
        }
    }
    return false;
}

void DaliShellServer::web_close_slot(WebSlot &slot)
{
    if (slot.fd >= 0) {
        ::close(slot.fd);
    }
    slot.fd = -1;
}

void DaliShellServer::web_accept()
{
    int fd = ::accept(web_listen_fd_, nullptr, nullptr);
    if (fd < 0) {
        return;
    }

    WebSlot *slot = nullptr;
    for (auto &candidate : web_slots_) {
        if (candidate.fd < 0) {
            slot = &candidate;
            break;
        }
    }
    if (slot == nullptr) {
        /* Full. The newest connection is the one a browser is waiting on, so
         * room is made: a connection already answered goes first, then the one
         * that has waited longest without sending a request. */
        for (auto &candidate : web_slots_) {
            if (candidate.state == WebSlot::State::DRAIN &&
                (slot == nullptr || (int32_t) (candidate.opened_ms - slot->opened_ms) < 0)) {
                slot = &candidate;
            }
        }
        if (slot == nullptr) {
            for (auto &candidate : web_slots_) {
                if (slot == nullptr || (int32_t) (candidate.opened_ms - slot->opened_ms) < 0) {
                    slot = &candidate;
                }
            }
        }
        ESP_LOGD(TAG, "Web connections full; closing the oldest");
        web_close_slot(*slot);
    }

    set_blocking(fd);
    struct timeval send_tv = {};
    send_tv.tv_sec = (time_t) (WEB_SEND_TIMEOUT_MS / 1000u);
    send_tv.tv_usec = (suseconds_t) ((WEB_SEND_TIMEOUT_MS % 1000u) * 1000u);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_tv, sizeof(send_tv));
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    uint32_t now = millis();
    slot->fd = fd;
    slot->state = WebSlot::State::HEAD;
    slot->opened_ms = now;
    slot->deadline_ms = now + WEB_HEAD_TIMEOUT_MS;
    dali_web_request_init(&slot->req);
}

void DaliShellServer::web_slot_read(WebSlot &slot)
{
    uint8_t buf[128];
    int received = ::recv(slot.fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (received < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            web_close_slot(slot);
        }
        return;
    }
    if (received == 0) {
        web_close_slot(slot);
        return;
    }
    if (slot.state == WebSlot::State::DRAIN) {
        return;  /* read only so that closing does not reset what was sent */
    }

    size_t used = 0u;
    DaliWebRequestState state = DALI_WEB_REQUEST_MORE;
    while (used < (size_t) received && state == DALI_WEB_REQUEST_MORE) {
        state = dali_web_request_feed(&slot.req, buf[used++]);
    }
    if (state != DALI_WEB_REQUEST_MORE) {
        web_answer(slot, (size_t) received - used);
    }
}

/*
 * Answer a head that has ended. `leftover` counts the bytes that followed it in
 * the same read: a page request's are drained with the rest, and an upgrade
 * must have none, because a client sends nothing until the handshake answers.
 */
void DaliShellServer::web_answer(WebSlot &slot, size_t leftover)
{
    char peer[16];
    peer_text(slot.fd, peer, sizeof(peer));

    /* The address this connection reached, which is the IP literal a browser
     * puts in Host. Read per connection, so a new DHCP lease or the AP's own
     * address needs no configuration. */
    char local_ip[16] = "";
    struct sockaddr_in local = {};
    socklen_t local_len = sizeof(local);
    if (::getsockname(slot.fd, (struct sockaddr *) &local, &local_len) == 0) {
        inet_ntoa_r(local.sin_addr, local_ip, sizeof(local_ip));
    }
    const char *names[3u + WEB_ALLOWED_HOSTS_MAX];
    uint8_t count = 0u;
    if (local_ip[0] != '\0') {
        names[count++] = local_ip;
    }
    names[count++] = web_name_;
    names[count++] = web_name_local_;
    for (uint8_t i = 0u; i < web_allowed_host_count_; i++) {
        names[count++] = web_allowed_hosts_[i];
    }
    DaliWebHostPolicy policy = { names, count, web_port_ };
    DaliWebRoute route = dali_web_route(&slot.req, &policy);

    DaliWebRefusal refusal = route.refusal;
    if (route.kind != DALI_WEB_ROUTE_REFUSE) {
        size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (free_heap < WEB_HEAP_FLOOR_BYTES) {
            refusal = DALI_WEB_REFUSE_UNAVAILABLE;
            ESP_LOGW(TAG, "Refused a web request from %s: %u B of heap free, floor %u",
                     peer, (unsigned) free_heap, (unsigned) WEB_HEAP_FLOOR_BYTES);
        } else if (route.kind == DALI_WEB_ROUTE_SOCKET && leftover > 0u) {
            refusal = DALI_WEB_REFUSE_MALFORMED;
        } else if (route.kind == DALI_WEB_ROUTE_PAGE &&
                   (web_page_ == nullptr || web_page_len_ == 0u)) {
            refusal = DALI_WEB_REFUSE_NOT_FOUND;
        }
    }

    char head[WEB_HEAD_OUT_MAX];

    if (refusal != DALI_WEB_REFUSE_NONE) {
        size_t len = dali_web_format_refusal(refusal, head, sizeof(head));
        if (len > 0u) {
            web_send(slot.fd, head, len);
        }
        /* Host and Origin refusals are what an attempt from another page looks
         * like, so they are the ones worth a warning. */
        bool security = refusal == DALI_WEB_REFUSE_HOST ||
                        refusal == DALI_WEB_REFUSE_NO_ORIGIN ||
                        refusal == DALI_WEB_REFUSE_ORIGIN;
        if (security) {
            ESP_LOGW(TAG, "Refused a web request from %s (Host '%s', Origin '%s'): %s",
                     peer, slot.req.host, slot.req.origin, dali_web_refusal_text(refusal));
        } else if (refusal != DALI_WEB_REFUSE_UNAVAILABLE) {
            ESP_LOGI(TAG, "Refused a web request from %s for '%s': %s", peer,
                     slot.req.target, dali_web_refusal_text(refusal));
        }
        web_linger(slot);
        return;
    }

    if (route.kind == DALI_WEB_ROUTE_PAGE) {
        size_t len = dali_web_format_page_head(web_page_len_, true, head, sizeof(head));
        if (len > 0u && web_send(slot.fd, head, len)) {
            web_send(slot.fd, web_page_, web_page_len_);
        }
        ESP_LOGD(TAG, "Served the shell page to %s", peer);
        web_linger(slot);
        return;
    }

    /* DALI_WEB_ROUTE_SOCKET */
    size_t len = dali_web_format_handshake(&slot.req, head, sizeof(head));
    if (len == 0u || !web_send(slot.fd, head, len)) {
        web_close_slot(slot);
        return;
    }
    if (dali_shell_is_attached()) {
        /* Answered rather than left waiting, unlike a second TCP client: the
         * page says why, and a person can close the other session. */
        uint8_t frame[2u + DALI_WEB_WS_PAYLOAD_MAX];
        size_t frame_len = dali_web_ws_close_frame(DALI_WEB_WS_CLOSE_TRY_AGAIN, WEB_BUSY_TEXT,
                                                   frame, sizeof(frame));
        if (frame_len > 0u) {
            web_send(slot.fd, frame, frame_len);
        }
        ESP_LOGW(TAG, "Refused a web session from %s: another session is open", peer);
        web_linger(slot);
        return;
    }

    /* The connection is the session from here. The slot lets go of it first,
     * so a session that ends at once can linger in it. */
    int fd = slot.fd;
    slot.fd = -1;
    begin_session(fd, ClientKind::WEBSOCKET);
}

void DaliShellServer::web_linger(WebSlot &slot)
{
    if (slot.fd < 0) {
        return;
    }
    ::shutdown(slot.fd, SHUT_WR);
    slot.state = WebSlot::State::DRAIN;
    slot.deadline_ms = millis() + WEB_DRAIN_MS;
}

/*
 * A socket that is in no slot -- a WebSocket session that has sent its close --
 * lingers in a free one. With none free it is closed at once; the worst that
 * costs is a reset the peer may see in place of our close frame.
 */
void DaliShellServer::web_linger_fd(int fd)
{
    for (auto &slot : web_slots_) {
        if (slot.fd < 0) {
            uint32_t now = millis();
            slot.fd = fd;
            slot.opened_ms = now;
            web_linger(slot);
            return;
        }
    }
    ::close(fd);
}

void DaliShellServer::web_expire_slots(uint32_t now)
{
    for (auto &slot : web_slots_) {
        if (slot.fd >= 0 && (int32_t) (now - slot.deadline_ms) >= 0) {
            if (slot.state == WebSlot::State::HEAD) {
                ESP_LOGD(TAG, "Closed a web connection that sent no request");
            }
            web_close_slot(slot);
        }
    }
}

}  // namespace dali
}  // namespace esphome
