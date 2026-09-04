#include "dali_shell_tcp.h"

#include "dali_core_affinity.h"

#include "esphome/core/log.h"

extern "C" {
#include "../../../components/dali/dali_shell.h"
}

#include <cerrno>
#include <cstring>

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
/* Below the DALI task (10) and the scan task (9): a shell session is
 * interactive work and must never delay bus servicing. */
static constexpr UBaseType_t SHELL_TASK_PRIORITY = 4u;

/* How long recv() waits before returning so the loop can pump deferred trace
 * output and re-check the idle deadline. */
static constexpr uint32_t SHELL_RECV_TIMEOUT_MS = 250u;

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

    BaseType_t created = xTaskCreatePinnedToCore(
        task_entry, "dali_shell", SHELL_TASK_STACK, this, SHELL_TASK_PRIORITY,
        nullptr, dali_worker_core());
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to start shell task");
        this->mark_failed();
        return;
    }

    ESP_LOGI(TAG, "DALI shell listening on port %u", (unsigned) port_);
}

void DaliShellServer::dump_config()
{
    ESP_LOGCONFIG(TAG, "DALI Shell:");
    ESP_LOGCONFIG(TAG, "  Port: %u", (unsigned) port_);
    ESP_LOGCONFIG(TAG, "  Idle timeout: %u s", (unsigned) idle_timeout_s_);
    ESP_LOGCONFIG(TAG, "  Commissioning verbs: %s",
                  allow_commissioning_ ? "allowed" : "refused");
}

/* ── Session bindings ────────────────────────────────────────────────────── */

void DaliShellServer::write_cb(void *ctx, const char *text)
{
    auto *self = static_cast<DaliShellServer *>(ctx);
    if (self == nullptr || self->client_fd_ < 0 || text == nullptr ||
        self->peer_lost_) {
        return;
    }

    size_t remaining = strlen(text);
    const char *cursor = text;
    while (remaining > 0u) {
        int sent = ::send(self->client_fd_, cursor, remaining, 0);
        if (sent > 0) {
            cursor += sent;
            remaining -= (size_t) sent;
            continue;
        }
        /* A blocking socket returns EINTR without having sent anything; every
         * other error means this connection is finished. Marking the peer lost
         * rather than retrying is what stops a long workflow from spending its
         * remaining seconds writing into a closed descriptor. */
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        self->peer_lost_ = true;
        return;
    }
}

bool DaliShellServer::aborted_cb(void *ctx)
{
    auto *self = static_cast<DaliShellServer *>(ctx);
    return self == nullptr || self->peer_lost_;
}

bool DaliShellServer::bus_claim_cb(void *ctx, const char *what)
{
    auto *self = static_cast<DaliShellServer *>(ctx);
    if (self == nullptr || self->parent_ == nullptr) {
        return false;
    }
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
    shell.port                = self->port_;
    shell.idle_timeout_s      = self->idle_timeout_s_;
    shell.allow_commissioning = self->allow_commissioning_;

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

/* ── Accept loop ─────────────────────────────────────────────────────────── */

void DaliShellServer::task_entry(void *arg)
{
    static_cast<DaliShellServer *>(arg)->run();
}

void DaliShellServer::run()
{
    for (;;) {
        if (listen_fd_ < 0) {
            listen_fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (listen_fd_ < 0) {
                ESP_LOGW(TAG, "socket() failed: %d", errno);
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }

            int one = 1;
            ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

            struct sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            addr.sin_port = htons(port_);

            /*
             * One session is served at a time, so the backlog only decides what
             * a second client meets while the first is busy: with a depth of 1
             * the third connect onward is dropped in the SYN queue and times out
             * with no diagnosis at all. Two lets one waiter sit queued and be
             * served the moment the session ends, which is the common case of an
             * operator opening a second terminal.
             *
             * It does NOT deliver the busy notice below to that waiter: nothing
             * is accepted while serve() owns this loop, so a queued client sees
             * an open socket and silence until the session ends or its idle
             * timeout fires. Saying so promptly needs the accept to happen
             * during a session, not a deeper queue.
             */
            if (::bind(listen_fd_, (struct sockaddr *) &addr, sizeof(addr)) < 0 ||
                ::listen(listen_fd_, 2) < 0) {
                ESP_LOGW(TAG, "bind/listen on port %u failed: %d", (unsigned) port_, errno);
                ::close(listen_fd_);
                listen_fd_ = -1;
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
        }

        int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        serve(fd);
    }
}

void DaliShellServer::serve(int client_fd)
{
    /* Bounded recv so the loop still pumps deferred trace output and checks the
     * idle deadline while the operator is typing nothing. */
    struct timeval tv = {};
    tv.tv_sec = (time_t) (SHELL_RECV_TIMEOUT_MS / 1000u);
    tv.tv_usec = (suseconds_t) ((SHELL_RECV_TIMEOUT_MS % 1000u) * 1000u);
    ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Nagle would hold back the prompt and the incremental progress lines that
     * make a long workflow readable. */
    int one = 1;
    ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    client_fd_ = client_fd;
    peer_lost_ = false;

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
    session.hooks.export_config = export_config_cb;
    session.hooks.snapshot_save = snapshot_save_cb;
    session.hooks.snapshot_load = snapshot_load_cb;
    session.hooks.ctx = this;
    session.policy = allow_commissioning_ ? DALI_SHELL_ALLOW_ALL : DALI_SHELL_ALLOW_RESET;
    session.aborted = aborted_cb;
    session.abort_ctx = this;
    /* `trace on` prints from the scheduler owner task; that output is queued
     * and flushed below rather than sent from a task that must not block. */
    session.defer_foreign_task_output = true;

    DaliError err = dali_shell_attach(&session);
    if (err != DALI_OK) {
        /* Accepted only to say why: an unaccepted connection looks like a hang.
         * Written directly because no session exists to write through. */
        static const char busy[] =
            "DALI shell busy: another session is connected.\r\n";
        ::send(client_fd, busy, sizeof(busy) - 1u, 0);
        ::close(client_fd);
        client_fd_ = -1;
        ESP_LOGW(TAG, "Refused second session");
        return;
    }

    ESP_LOGI(TAG, "Shell session opened");
    dali_shell_write_banner();
    dali_shell_write_prompt();

    uint32_t last_activity_ms = millis();

    while (!peer_lost_) {
        uint8_t buf[64];
        int received = ::recv(client_fd, buf, sizeof(buf), 0);

        if (received > 0) {
            last_activity_ms = millis();
            for (int i = 0; i < received && !peer_lost_; i++) {
                /* Telnet clients in line mode send a bare LF or a CR LF pair;
                 * the shell treats either terminator as end of line, so the
                 * second byte of the pair arrives as an empty line and is
                 * ignored by the resolver rather than echoed as an error. */
                if (dali_shell_feed_byte(buf[i])) {
                    dali_shell_write_prompt();
                }
            }
        } else if (received == 0) {
            break;  /* orderly close */
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            break;
        }

        /* Trace lines raised on the DALI task, flushed from this one. */
        uint32_t dropped = dali_shell_pump_deferred_output();
        if (dropped > 0u) {
            /* Said out loud: a silent gap would read as a quiet bus. */
            char note[64];
            snprintf(note, sizeof(note),
                     "[trace] %u line(s) dropped\r\n", (unsigned) dropped);
            write_cb(this, note);
        }

        if (idle_timeout_s_ > 0u &&
            (millis() - last_activity_ms) >= idle_timeout_s_ * 1000u) {
            write_cb(this, "\r\nidle timeout; closing session\r\n");
            ESP_LOGI(TAG, "Shell session closed on idle timeout");
            break;
        }
    }

    dali_shell_detach();
    ::close(client_fd);
    client_fd_ = -1;
    ESP_LOGI(TAG, "Shell session closed");
}

}  // namespace dali
}  // namespace esphome
