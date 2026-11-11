#include "dali_shell_tcp.h"

#include "dali_core_affinity.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

extern "C" {
#include "../../../components/dali/dali_shell.h"
}

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <lwip/sockets.h>

namespace esphome {
namespace dali {

static const char *const TAG = "dali.shell";

/* Stack for the session task. The shell's workflows keep their working set in
 * module statics rather than on the stack, but discovery and commissioning
 * still nest several frames of protocol code, so this starts from what the scan
 * task already asks for. The extra over that 8192 is headroom: the scan task
 * runs one walk and nothing else, while a session task carries its own command
 * dispatch under everything a workflow nests. */
static constexpr uint32_t SHELL_TASK_STACK = 12288u;
/*
 * Level with ESPHome's loop task (1), on the same core, so FreeRTOS time-slices
 * the two and neither can hold the other off. Above the loop, a burst of output
 * can keep it from running: `schema` makes thousands of sends, and with lwIP
 * core locking each runs TCP, IP and the Wi-Fi transmit path on this task. The
 * loop missed the 5 s task watchdog when a session opened with one, at priority
 * 4. Still below the DALI task (10) and the scan task
 * (9): a shell session is interactive work and must never delay bus servicing.
 * Nothing here times the bus; the scheduler expands and times send-twice pairs
 * on the DALI task, and the shell only waits for the result.
 */
static constexpr UBaseType_t SHELL_TASK_PRIORITY = 1u;

/* How long select() waits while a session or a web connection is open, so the
 * loop can pump deferred trace output and check the idle and web deadlines. */
static constexpr uint32_t SHELL_POLL_MS = 250u;
/* How long it waits with neither: only the listeners, and a listener that
 * failed to open, need it to come round. */
static constexpr uint32_t SHELL_IDLE_POLL_MS = 5000u;
/* How long a listener that failed to open waits before the next attempt. */
static constexpr uint32_t SHELL_LISTEN_RETRY_MS = 5000u;

/* How long one send() may take nothing before the peer counts as gone. Without
 * it a client that stops reading without closing -- a laptop asleep with the
 * session open, a terminal suspended with Ctrl-Z -- parks this task inside
 * send() until lwIP abandons the connection, which takes minutes, and whatever
 * the running workflow holds stays held: the bus gate, quiescent mode, an open
 * INITIALISE window. Ten seconds rides out a few retransmission backoffs on a
 * poor Wi-Fi link; a reader that is merely slow drains the 5.7 KB send buffer
 * far sooner than that. */
static constexpr uint32_t SHELL_SEND_TIMEOUT_MS = 10000u;

/* TCP keepalive finds a peer that vanished while nothing was being sent: an
 * idle session would otherwise hold the shell until the idle timeout, or for
 * good when that is 0. Probes start after this much silence and the peer is
 * declared gone after the last unanswered one, about 45 s in all; recv() then
 * fails and the session closes. A quiet verb is covered too: its next write
 * fails at once instead of waiting out the send timeout. */
static constexpr int SHELL_KEEPALIVE_IDLE_S     = 30;
static constexpr int SHELL_KEEPALIVE_INTERVAL_S = 5;
static constexpr int SHELL_KEEPALIVE_COUNT      = 3;

static const char SHELL_BUSY_TEXT[] = "DALI shell busy: another session is connected.";

void DaliShellServer::add_web_allowed_host(const char *host)
{
    if (host != nullptr && web_allowed_host_count_ < WEB_ALLOWED_HOSTS_MAX) {
        web_allowed_hosts_[web_allowed_host_count_++] = host;
    }
}

void DaliShellServer::setup()
{
    if (parent_ == nullptr) {
        ESP_LOGE(TAG, "No DALI component; shell disabled");
        this->mark_failed();
        return;
    }

    /* Prepares the shell's module state and registers its scheduler
     * subscribers. The DALI component owns the primary event callback for its
     * dispatch table; the shell adds a second subscriber alongside it. */
    DaliError err = dali_shell_init();
    if (err != DALI_OK) {
        ESP_LOGE(TAG, "dali_shell_init failed: %d", (int) err);
        this->mark_failed();
        return;
    }

    if (web_port_ != 0u) {
        /* The names a browser may put in Host, besides the address it reached
         * the device on: the node name, which is also its mDNS name. Read
         * through size(), which both a std::string and a StringRef have. */
        const auto &name = App.get_name();
        snprintf(web_name_, sizeof(web_name_), "%.*s", (int) name.size(), name.c_str());
        snprintf(web_name_local_, sizeof(web_name_local_), "%s.local", web_name_);
    }

    BaseType_t created = xTaskCreatePinnedToCore(
        task_entry, "dali_shell", SHELL_TASK_STACK, this, SHELL_TASK_PRIORITY,
        nullptr, dali_worker_core());
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to start shell task");
        this->mark_failed();
        return;
    }

    ESP_LOGI(TAG, "DALI shell listening on port %u", (unsigned) port_);
    if (web_port_ != 0u) {
        ESP_LOGI(TAG, "DALI shell page on http://%s.local:%u/", web_name_,
                 (unsigned) web_port_);
    }
}

void DaliShellServer::dump_config()
{
    ESP_LOGCONFIG(TAG, "DALI Shell:");
    ESP_LOGCONFIG(TAG, "  Port: %u", (unsigned) port_);
    ESP_LOGCONFIG(TAG, "  Idle timeout: %u s", (unsigned) idle_timeout_s_);
    ESP_LOGCONFIG(TAG, "  Commissioning verbs: %s",
                  allow_commissioning_ ? "allowed" : "refused");
    if (web_port_ != 0u) {
        ESP_LOGCONFIG(TAG, "  Web port: %u (page %u bytes gzipped)",
                      (unsigned) web_port_, (unsigned) web_page_len_);
        ESP_LOGCONFIG(TAG, "  Web hosts: its address, %s, %s", web_name_, web_name_local_);
        for (uint8_t i = 0u; i < web_allowed_host_count_; i++) {
            ESP_LOGCONFIG(TAG, "    and %s", web_allowed_hosts_[i]);
        }
    }
}

/* ── Output ──────────────────────────────────────────────────────────────── */

void DaliShellServer::send_all(const char *data, size_t len)
{
    if (client_fd_ < 0 || peer_lost_) {
        return;
    }

    const char *cursor = data;
    size_t remaining = len;
    while (remaining > 0u) {
        int sent = ::send(client_fd_, cursor, remaining, 0);
        if (sent > 0) {
            cursor += sent;
            remaining -= (size_t) sent;
            continue;
        }
        /* A blocking socket returns EINTR without having sent anything; every
         * other error means this connection is finished. That includes EAGAIN,
         * which here means SO_SNDTIMEO expired with nothing taken: the peer
         * stopped reading. Marking the peer lost rather than retrying is what
         * stops a long workflow from spending its remaining seconds writing
         * into a connection nobody is reading. */
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        peer_lost_ = true;
        return;
    }
}

void DaliShellServer::flush_output()
{
    if (out_len_ == 0u) {
        return;
    }
    size_t len = out_len_;
    out_len_ = 0u;

    char *data = out_data();
    if (client_kind_ != ClientKind::WEBSOCKET) {
        send_all(data, len);
        return;
    }

    /* One text frame per flush, header and payload in one send, so a frame is
     * never a segment of its own. OUT_BUF_SIZE always fits a 4-byte header. */
    uint8_t head[WS_HEAD_ROOM];
    size_t head_len = dali_web_ws_frame_header(DALI_WEB_WS_OP_TEXT, (uint32_t) len,
                                               head, sizeof(head));
    if (head_len == 0u) {
        peer_lost_ = true;
        return;
    }
    memcpy(data - head_len, head, head_len);
    send_all(data - head_len, head_len + len);
}

/*
 * A WebSocket text frame must be valid UTF-8 on its own, or the browser fails
 * the connection. Shell output is ASCII, but a flush can fall anywhere, so a
 * byte above 0x7F, which only a device's own data could put there, is sent as
 * '?' rather than risk half a character at the end of a frame.
 */
void DaliShellServer::write_ws(const char *text, size_t len)
{
    while (len > 0u && !peer_lost_) {
        size_t room = OUT_BUF_SIZE - out_len_;
        if (room == 0u) {
            flush_output();
            continue;
        }
        size_t take = len < room ? len : room;
        char *dst = out_data() + out_len_;
        for (size_t i = 0u; i < take; i++) {
            uint8_t c = (uint8_t) text[i];
            dst[i] = c < 0x80u ? (char) c : '?';
        }
        out_len_ += take;
        text += take;
        len -= take;
    }
}

void DaliShellServer::send_ws_control(DaliWebWsOpcode opcode, const uint8_t *payload,
                                      size_t len)
{
    uint8_t frame[2u + DALI_WEB_WS_PAYLOAD_MAX];
    size_t head_len = dali_web_ws_frame_header(opcode, (uint32_t) len, frame, sizeof(frame));
    if (head_len == 0u || head_len + len > sizeof(frame)) {
        return;
    }
    if (len > 0u) {
        memcpy(frame + head_len, payload, len);
    }
    flush_output();
    send_all(reinterpret_cast<const char *>(frame), head_len + len);
}

void DaliShellServer::send_ws_close(DaliWebWsCloseCode code, const char *reason)
{
    uint8_t frame[2u + DALI_WEB_WS_PAYLOAD_MAX];
    size_t len = dali_web_ws_close_frame(code, reason, frame, sizeof(frame));
    if (len > 0u) {
        flush_output();
        send_all(reinterpret_cast<const char *>(frame), len);
    }
}

/* ── Session bindings ────────────────────────────────────────────────────── */

/*
 * The shell writes in pieces: a line or less per shell_printf(), a table row in
 * several calls. Sent as written, under TCP_NODELAY each piece is a segment of
 * its own, and ESP-IDF's lwIP gives every segment a full-MSS buffer until it is
 * acknowledged. A verb that wrote a character at a time once held about 28 kB
 * that way, and `help`, sent a line at a time, about 20 kB. So writes are
 * gathered and sent only when the shell is about to wait, or has finished:
 * - before every bus wait, from aborted_cb();
 * - before the bus claim that starts a walk, from bus_claim_cb(), because a
 *   claim can wait seconds for queued traffic to drain;
 * - after the prompt, once per pass of the session loop in session_tick();
 * - when the next write would not fit.
 *
 * A line end is not a reason to send. The lines printed between two waits are
 * produced microseconds apart, so sending them together costs the operator
 * nothing, and a verb that prints a hundred lines without waiting goes out in a
 * few full segments instead of a hundred short ones. A line printed just before
 * a wait still goes out at that wait. The only waits that pass through neither
 * hook are short: the walks' settle delays, 100 ms at most, and a PHY reset,
 * under 1 s.
 *
 * A WebSocket session gathers the same way, and each send is one text frame.
 */
void DaliShellServer::write_cb(void *ctx, const char *text)
{
    auto *self = static_cast<DaliShellServer *>(ctx);
    if (self == nullptr || self->client_fd_ < 0 || text == nullptr ||
        self->peer_lost_) {
        return;
    }

    size_t len = strlen(text);
    if (len == 0u) {
        return;
    }
    self->at_line_start_ = text[len - 1u] == '\n';
    if (self->client_kind_ == ClientKind::WEBSOCKET) {
        self->write_ws(text, len);
        return;
    }
    if (self->out_len_ + len > OUT_BUF_SIZE) {
        self->flush_output();
        if (len > OUT_BUF_SIZE) {
            self->send_all(text, len);
            return;
        }
    }
    memcpy(self->out_data() + self->out_len_, text, len);
    self->out_len_ += len;
}

/*
 * Trace lines raised on the DALI task, written into this session's output from
 * this one, with a note when the ring overflowed: a silent gap would read as a
 * quiet bus.
 */
void DaliShellServer::pump_trace()
{
    uint32_t dropped = dali_shell_pump_deferred_output();
    if (dropped > 0u) {
        char note[64];
        snprintf(note, sizeof(note), "[trace] %u line(s) dropped\r\n", (unsigned) dropped);
        write_cb(this, note);
    }
}

/*
 * The shell asks this before every wait on the bus, so it is where gathered
 * output goes out rather than being held through the wait. A send that fails
 * here also finds a dead peer before the wait instead of after it.
 *
 * It is also where trace reaches the operator during a verb. The session task
 * spends a verb inside dali_shell_dispatch(), so session_tick() cannot pump the
 * 16-line ring until the verb ends, and a walk under `trace on` once lost all
 * but 16 of its lines. Pumped here, each wait carries the lines the previous
 * one raised. Only at a line start, though: a verb can print part of a line,
 * wait on the bus, and finish it, and a trace line pumped between the two would
 * split it. Those lines wait for the next wait instead.
 */
bool DaliShellServer::aborted_cb(void *ctx)
{
    auto *self = static_cast<DaliShellServer *>(ctx);
    if (self == nullptr) {
        return true;
    }
    if (!self->peer_lost_ && self->at_line_start_) {
        self->pump_trace();
    }
    self->flush_output();
    return self->peer_lost_;
}

bool DaliShellServer::bus_claim_cb(void *ctx, const char *what)
{
    auto *self = static_cast<DaliShellServer *>(ctx);
    if (self == nullptr || self->parent_ == nullptr) {
        return false;
    }
    /* A claim waits up to 5 s for queued traffic to drain, so what the verb has
     * printed so far goes out first. */
    self->flush_output();
    return self->parent_->try_claim_bus(what);
}

void DaliShellServer::bus_release_cb(void *ctx)
{
    auto *self = static_cast<DaliShellServer *>(ctx);
    if (self != nullptr && self->parent_ != nullptr) {
        self->parent_->release_bus();
    }
}

bool DaliShellServer::snapshot_save_cb(void *ctx, const uint8_t *buf, uint32_t len)
{
    auto *self = static_cast<DaliShellServer *>(ctx);
    if (self == nullptr || self->parent_ == nullptr || buf == nullptr) {
        return false;
    }
    return self->parent_->save_address_backup(buf, len);
}

bool DaliShellServer::snapshot_load_cb(void *ctx, uint8_t *buf, uint32_t *len)
{
    auto *self = static_cast<DaliShellServer *>(ctx);
    if (self == nullptr || self->parent_ == nullptr || buf == nullptr || len == nullptr) {
        return false;
    }
    return self->parent_->load_address_backup(buf, len);
}

/*
 * `export config`. The `shell:` sub-block is this object's own configuration,
 * so it is filled in here rather than looked up: DaliComponent has no
 * back-reference to the front end driving it, and does not need one for this.
 */
void DaliShellServer::export_config_cb(void *ctx, const DaliCliOut *out,
                                       const DaliDiscoveryInventory *inventory,
                                       DaliShellInputLookupFn input_lookup)
{
    auto *self = static_cast<DaliShellServer *>(ctx);
    if (self == nullptr || self->parent_ == nullptr) {
        return;
    }

    DaliShellConfigInfo shell = {};
    shell.port                   = self->port_;
    shell.idle_timeout_s         = self->idle_timeout_s_;
    shell.allow_commissioning    = self->allow_commissioning_;
    shell.web_port               = self->web_port_;
    shell.web_allowed_hosts      = self->web_allowed_hosts_;
    shell.web_allowed_host_count = self->web_allowed_host_count_;

    self->parent_->export_config_yaml(out, &shell, inventory, input_lookup);
}

/*
 * A session finished a walk that says which addresses exist and what groups
 * they are in. Publishing it here is what makes a shell `discover` worth as
 * much to the component as the button scan: without it, a session could
 * commission new gear or watch a fixture change rooms and the group-membership
 * table would go on describing the bus as it was at the last button press.
 *
 * The component rejects an incomplete inventory itself, so nothing here decides
 * what counts as good enough.
 */
void DaliShellServer::inventory_changed_cb(void *ctx,
                                           const DaliDiscoveryInventory *inventory)
{
    auto *self = static_cast<DaliShellServer *>(ctx);
    if (self == nullptr || self->parent_ == nullptr) {
        return;
    }
    self->parent_->apply_inventory_snapshot(inventory);
}

/* One `config` verb went out. What it invalidated is the component's to decide. */
void DaliShellServer::config_applied_cb(void *ctx, DaliTarget target,
                                        DaliCommandId id, uint8_t param)
{
    auto *self = static_cast<DaliShellServer *>(ctx);
    if (self == nullptr || self->parent_ == nullptr) {
        return;
    }
    self->parent_->on_config_applied(target, id, param);
}

/* `address <from> set <to>` moved one gear, and both ends were confirmed on the
 * bus. Unlike config_applied_cb() this names where the gear went, so caches can
 * follow it rather than being dropped. */
void DaliShellServer::short_address_moved_cb(void *ctx, uint8_t from, uint8_t to)
{
    auto *self = static_cast<DaliShellServer *>(ctx);
    if (self == nullptr || self->parent_ == nullptr) {
        return;
    }
    self->parent_->on_short_address_moved(from, to);
}

/* `address <aN> clear` took the address away and `aN` was confirmed silent.
 * The mirror of short_address_moved_cb(): nothing follows the gear, because it
 * has no address to be followed to. */
void DaliShellServer::short_address_cleared_cb(void *ctx, uint8_t addr)
{
    auto *self = static_cast<DaliShellServer *>(ctx);
    if (self == nullptr || self->parent_ == nullptr) {
        return;
    }
    self->parent_->on_short_address_cleared(addr);
}

/* `iconfig` wrote to a control device's instances. The integration re-reads
 * what its sensors there say about themselves rather than trusting the write. */
void DaliShellServer::instance_config_applied_cb(void *ctx, uint8_t addr,
                                                 uint8_t instance)
{
    auto *self = static_cast<DaliShellServer *>(ctx);
    if (self == nullptr || self->parent_ == nullptr) {
        return;
    }
    self->parent_->on_instance_config_applied(addr, instance);
}

/* ── Listeners ───────────────────────────────────────────────────────────── */

/*
 * Non-blocking, so an accept() after select() cannot park the task on a
 * connection the peer reset in between. Each accepted socket is set back to
 * blocking: send_all() reads EAGAIN as SO_SNDTIMEO expiring.
 */
static int open_listener(uint16_t port, int backlog)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ESP_LOGW(TAG, "socket() failed: %d", errno);
        return -1;
    }

    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0 ||
        ::listen(fd, backlog) < 0) {
        ESP_LOGW(TAG, "bind/listen on port %u failed: %d", (unsigned) port, errno);
        ::close(fd);
        return -1;
    }
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, (flags < 0 ? 0 : flags) | O_NONBLOCK);
    return fd;
}

void DaliShellServer::open_listeners()
{
    uint32_t now = millis();
    /*
     * One session is served at a time, so the TCP backlog only decides what a
     * second client meets while the first is busy: with a depth of 1 the third
     * connect onward is dropped in the SYN queue and times out with no
     * diagnosis at all. Two lets one waiter sit queued and be served the moment
     * the session ends, which is the common case of an operator opening a
     * second terminal.
     *
     * The waiter gets no busy notice: the TCP listener is not watched while a
     * session is open, so a queued client sees an open socket and silence until
     * the session ends or its idle timeout fires.
     */
    if (listen_fd_ < 0 && (int32_t) (now - listen_retry_ms_) >= 0) {
        listen_fd_ = open_listener(port_, 2);
        listen_retry_ms_ = now + SHELL_LISTEN_RETRY_MS;
    }
    /* A page load opens the page and then the socket; three lets both queue
     * behind a verb with one to spare. */
    if (web_port_ != 0u && web_listen_fd_ < 0 &&
        (int32_t) (now - web_listen_retry_ms_) >= 0) {
        web_listen_fd_ = open_listener(web_port_, 3);
        web_listen_retry_ms_ = now + SHELL_LISTEN_RETRY_MS;
        if (web_listen_fd_ >= 0) {
            ESP_LOGD(TAG, "Web listener open on port %u", (unsigned) web_port_);
        }
    }
}

/* ── Accept loop ─────────────────────────────────────────────────────────── */

void DaliShellServer::task_entry(void *arg)
{
    static_cast<DaliShellServer *>(arg)->run();
}

void DaliShellServer::run()
{
    for (;;) {
        open_listeners();

        fd_set readable;
        FD_ZERO(&readable);
        int max_fd = -1;
        auto watch = [&](int fd) {
            if (fd >= 0) {
                FD_SET(fd, &readable);
                if (fd > max_fd) {
                    max_fd = fd;
                }
            }
        };
        if (client_kind_ == ClientKind::NONE) {
            watch(listen_fd_);
        } else {
            watch(client_fd_);
        }
        watch(web_listen_fd_);
        for (auto &slot : web_slots_) {
            watch(slot.fd);
        }

        bool active = client_kind_ != ClientKind::NONE || web_slots_busy();
        uint32_t wait_ms = active ? SHELL_POLL_MS : SHELL_IDLE_POLL_MS;
        if (max_fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
            continue;
        }

        struct timeval tv = {};
        tv.tv_sec = (time_t) (wait_ms / 1000u);
        tv.tv_usec = (suseconds_t) ((wait_ms % 1000u) * 1000u);
        int ready = ::select(max_fd + 1, &readable, nullptr, nullptr, &tv);
        if (ready < 0) {
            FD_ZERO(&readable);
            if (errno != EINTR) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }

        /* The session first: a line it carries may run a verb, and everything
         * found ready below is still ready after it. */
        if (client_kind_ != ClientKind::NONE && FD_ISSET(client_fd_, &readable)) {
            session_read();
        }
        if (client_kind_ == ClientKind::NONE && listen_fd_ >= 0 &&
            FD_ISSET(listen_fd_, &readable)) {
            int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd >= 0) {
                begin_session(fd, ClientKind::RAW);
            }
        }
        if (web_listen_fd_ >= 0 && FD_ISSET(web_listen_fd_, &readable)) {
            web_accept();
        }
        for (auto &slot : web_slots_) {
            if (slot.fd >= 0 && FD_ISSET(slot.fd, &readable)) {
                web_slot_read(slot);
            }
        }
        web_expire_slots(millis());
        if (client_kind_ != ClientKind::NONE) {
            session_tick();
        }
    }
}

/* ── Session ─────────────────────────────────────────────────────────────── */

void DaliShellServer::set_blocking(int fd)
{
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0 && (flags & O_NONBLOCK) != 0) {
        ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
}

void DaliShellServer::peer_text(int fd, char *out, size_t cap)
{
    snprintf(out, cap, "unknown");
    struct sockaddr_in peer = {};
    socklen_t peer_len = sizeof(peer);
    if (::getpeername(fd, (struct sockaddr *) &peer, &peer_len) == 0) {
        inet_ntoa_r(peer.sin_addr, out, cap);
    }
}

bool DaliShellServer::begin_session(int fd, ClientKind kind)
{
    set_blocking(fd);

    /* Set before anything is written, the busy notice included. */
    struct timeval send_tv = {};
    send_tv.tv_sec = (time_t) (SHELL_SEND_TIMEOUT_MS / 1000u);
    send_tv.tv_usec = (suseconds_t) ((SHELL_SEND_TIMEOUT_MS % 1000u) * 1000u);
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_tv, sizeof(send_tv)) != 0) {
        ESP_LOGW(TAG, "SO_SNDTIMEO failed (%d): a peer that stops reading can "
                      "hold a workflow until lwIP gives up", errno);
    }

    /* Nagle would hold back the prompt and the incremental progress lines that
     * make a long workflow readable. */
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    int keep_idle = SHELL_KEEPALIVE_IDLE_S;
    int keep_interval = SHELL_KEEPALIVE_INTERVAL_S;
    int keep_count = SHELL_KEEPALIVE_COUNT;
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keep_idle, sizeof(keep_idle));
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keep_interval, sizeof(keep_interval));
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keep_count, sizeof(keep_count));

    client_fd_ = fd;
    client_kind_ = kind;
    session_end_ = SessionEnd::NONE;
    ws_close_code_ = DALI_WEB_WS_CLOSE_NORMAL;
    ws_close_reason_ = nullptr;
    peer_lost_ = false;
    out_len_ = 0u;
    at_line_start_ = true;

    DaliShellSession session = {};
    session.out.write = write_cb;
    session.out.ctx = this;
    session.transport = *dali_shell_device_transport();
    session.hooks.bus_claim = bus_claim_cb;
    session.hooks.bus_release = bus_release_cb;
    session.hooks.inventory_changed = inventory_changed_cb;
    session.hooks.config_applied = config_applied_cb;
    session.hooks.short_address_moved = short_address_moved_cb;
    session.hooks.short_address_cleared = short_address_cleared_cb;
    session.hooks.instance_config_applied = instance_config_applied_cb;
    session.hooks.export_config = export_config_cb;
    session.hooks.snapshot_save = snapshot_save_cb;
    session.hooks.snapshot_load = snapshot_load_cb;
    session.hooks.ctx = this;
    session.policy = allow_commissioning_ ? DALI_SHELL_ALLOW_ALL : DALI_SHELL_ALLOW_RESET;
    session.aborted = aborted_cb;
    session.abort_ctx = this;
    /* `trace on` prints from the scheduler owner task; that output is queued
     * and flushed from session_tick() rather than sent from a task that must
     * not block. */
    session.defer_foreign_task_output = true;

    DaliError err = dali_shell_attach(&session);
    if (err != DALI_OK) {
        /* Accepted only to say why: an unaccepted connection looks like a hang.
         * Written directly because no session exists to write through. */
        if (kind == ClientKind::WEBSOCKET) {
            send_ws_close(DALI_WEB_WS_CLOSE_TRY_AGAIN, SHELL_BUSY_TEXT);
        } else {
            static const char busy[] = "DALI shell busy: another session is connected.\r\n";
            ::send(fd, busy, sizeof(busy) - 1u, 0);
        }
        client_fd_ = -1;
        client_kind_ = ClientKind::NONE;
        if (kind == ClientKind::WEBSOCKET) {
            web_linger_fd(fd);
        } else {
            ::close(fd);
        }
        ESP_LOGW(TAG, "Refused second session");
        return false;
    }

    char peer[16];
    peer_text(fd, peer, sizeof(peer));
    ESP_LOGI(TAG, "Shell session opened from %s%s", peer,
             kind == ClientKind::WEBSOCKET ? " (web)" : "");

    if (kind == ClientKind::WEBSOCKET) {
        dali_web_ws_receiver_init(&ws_rx_);
    } else {
        /* Is this a shell client at all? A browser can be made to send an HTTP
         * request here, and the lines of its body would run as commands; see
         * dali_cli_peer_sniff(). */
        dali_cli_peer_sniffer_init(&sniffer_);
    }

    dali_shell_write_banner();
    flush_output();
    dali_shell_write_prompt();
    flush_output();

    last_activity_ms_ = millis();
    return true;
}

void DaliShellServer::end_session(SessionEnd why, DaliWebWsCloseCode code,
                                  const char *reason)
{
    if (session_end_ == SessionEnd::NONE) {
        session_end_ = why;
        ws_close_code_ = code;
        ws_close_reason_ = reason;
    }
}

void DaliShellServer::session_read()
{
    uint8_t buf[64];
    int received = ::recv(client_fd_, buf, sizeof(buf), MSG_DONTWAIT);

    if (received > 0) {
        last_activity_ms_ = millis();
        session_input(buf, (size_t) received);
    } else if (received == 0) {
        end_session(SessionEnd::PEER_CLOSED);  /* orderly close */
    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        peer_lost_ = true;
        end_session(SessionEnd::PEER_LOST);
    }
}

void DaliShellServer::session_input(const uint8_t *data, size_t len)
{
    for (size_t i = 0u; i < len; i++) {
        if (peer_lost_ || session_end_ != SessionEnd::NONE) {
            return;
        }
        if (client_kind_ == ClientKind::WEBSOCKET) {
            session_input_ws(data[i]);
            continue;
        }

        /* Sniffed before it is fed. The verdict always arrives before a line
         * can end, so nothing an HTTP peer sends reaches dispatch: not its
         * request line, and not its body. What it fed before the verdict is a
         * partial line, which the next attach clears. */
        if (dali_cli_peer_sniff(&sniffer_, data[i]) == DALI_CLI_PEER_HTTP) {
            /* With the address: the browser that sent it is on this network,
             * and so is whoever can tell which page it had open. */
            char peer[16];
            peer_text(client_fd_, peer, sizeof(peer));
            ESP_LOGW(TAG, "Closed a connection from %s that opened with an HTTP "
                          "request; nothing it sent was run", peer);
            write_cb(this, "\r\nHTTP request refused: this port is the DALI shell, "
                           "not a web server.\r\n");
            end_session(SessionEnd::HTTP_PEER);
            return;
        }
        /* Telnet clients in line mode send a bare LF or a CR LF pair; the shell
         * treats either terminator as end of line, so the second byte of the
         * pair arrives as an empty line and is ignored by the resolver rather
         * than echoed as an error. */
        if (dali_shell_feed_byte(data[i])) {
            dali_shell_write_prompt();
        }
    }
}

/*
 * One byte of a WebSocket session. A text frame is exactly one line: the
 * receiver refuses CR, LF and every other byte outside 0x20-0x7E, so a frame
 * can never become two commands. The prompt goes out as a frame of its own,
 * which is how the page knows the line has finished.
 */
void DaliShellServer::session_input_ws(uint8_t byte)
{
    switch (dali_web_ws_receive(&ws_rx_, byte)) {
    case DALI_WEB_WS_RX_MORE:
    case DALI_WEB_WS_RX_PONG:
        return;

    case DALI_WEB_WS_RX_TEXT:
        for (uint8_t k = 0u; k < ws_rx_.payload_len; k++) {
            dali_shell_feed_byte(ws_rx_.payload[k]);
        }
        if (dali_shell_feed_byte('\n')) {
            flush_output();
            dali_shell_write_prompt();
            flush_output();
        }
        return;

    case DALI_WEB_WS_RX_PING:
        send_ws_control(DALI_WEB_WS_OP_PONG, ws_rx_.payload, ws_rx_.payload_len);
        return;

    case DALI_WEB_WS_RX_CLOSE:
        /* Answered with a close of our own. A code the peer sent is answered
         * with 1000; one that sent none is answered with none. */
        end_session(SessionEnd::PEER_CLOSED,
                    ws_rx_.close_code == DALI_WEB_WS_CLOSE_NO_STATUS
                        ? DALI_WEB_WS_CLOSE_NONE
                        : DALI_WEB_WS_CLOSE_NORMAL);
        return;

    case DALI_WEB_WS_RX_FAIL:
    default: {
        /* The reason goes back to the page, which shows it beside the code. */
        const char *reason;
        switch (ws_rx_.failure) {
        case DALI_WEB_WS_FAIL_NOT_A_LINE:
            reason = "a text frame must be one line of printable ASCII";
            break;
        case DALI_WEB_WS_FAIL_TOO_LONG:
        case DALI_WEB_WS_FAIL_FRAGMENTED:
            reason = "a frame must be unfragmented and at most 125 bytes";
            break;
        case DALI_WEB_WS_FAIL_BINARY:
            reason = "binary frames are not served";
            break;
        default:
            reason = "WebSocket protocol error";
            break;
        }
        char peer[16];
        peer_text(client_fd_, peer, sizeof(peer));
        ESP_LOGW(TAG, "Closing the web session from %s: %s (close code %u)", peer, reason,
                 (unsigned) ws_rx_.close_code);
        end_session(SessionEnd::WS_FAILED, (DaliWebWsCloseCode) ws_rx_.close_code, reason);
        return;
    }
    }
}

void DaliShellServer::session_tick()
{
    if (session_end_ == SessionEnd::NONE && !peer_lost_) {
        /* Between verbs, after the prompt; during one, aborted_cb() pumps. */
        pump_trace();

        if (idle_timeout_s_ > 0u &&
            (millis() - last_activity_ms_) >= idle_timeout_s_ * 1000u) {
            write_cb(this, "\r\nidle timeout; closing session\r\n");
            ESP_LOGI(TAG, "Shell session closed on idle timeout");
            end_session(SessionEnd::IDLE, DALI_WEB_WS_CLOSE_GOING_AWAY, "idle timeout");
        }

        /* Whatever is gathered, the prompt and pumped trace lines included,
         * goes out here before select() waits again. */
        flush_output();
    }

    if (peer_lost_ && session_end_ == SessionEnd::NONE) {
        end_session(SessionEnd::PEER_LOST);
    }
    if (session_end_ != SessionEnd::NONE) {
        close_session();
    }
}

void DaliShellServer::close_session()
{
    flush_output();
    ClientKind kind = client_kind_;
    if (kind == ClientKind::WEBSOCKET && !peer_lost_) {
        send_ws_close(ws_close_code_, ws_close_reason_);
    }
    dali_shell_detach();

    int fd = client_fd_;
    client_fd_ = -1;
    client_kind_ = ClientKind::NONE;
    session_end_ = SessionEnd::NONE;
    /* A WebSocket peer answers our close with its own and then closes; read
     * that rather than reset it. A raw TCP session closes at once, as it
     * always has. */
    if (kind == ClientKind::WEBSOCKET && !peer_lost_) {
        web_linger_fd(fd);
    } else {
        ::close(fd);
    }
    ESP_LOGI(TAG, "Shell session closed");
}

}  // namespace dali
}  // namespace esphome
