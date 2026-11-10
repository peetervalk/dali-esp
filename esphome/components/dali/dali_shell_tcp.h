#pragma once

/*
 * dali_shell_tcp.h — network front ends for the diagnostic shell
 *
 * Presents the shell from components/dali/dali_shell.c on the network, so a
 * terminal in Home Assistant, or a browser, reaches the same commands, with the
 * same output, that the serial console has. Nothing about what a verb does
 * lives here: this moves bytes and owns a session's lifetime, exactly as
 * main/dali_diag.c does for UART0.
 *
 * Two bindings share one task and one session:
 *
 *   TCP   a raw line protocol on `port`, for `nc` and tools/dali-shell;
 *   web   optional, on `web: port:`. GET / serves one page from flash, and
 *         GET /ws upgrades to a WebSocket that carries the session.
 *
 * ── Why a raw line protocol ─────────────────────────────────────────────────
 *
 * The shell is a byte stream with no request/response framing: `discover`
 * prints for tens of seconds and `find switches` for minutes, both reporting as
 * they go. That is the shape a socket already has, and the shape an HTTP
 * request or a single Home Assistant text state does not. `nc dali.local 2323`
 * is then a working client, and a richer one is a client-side concern rather
 * than a firmware one. The WebSocket carries the same stream: each client text
 * frame is one line, and output goes out as text frames, the prompt in a frame
 * of its own so a page knows a command has finished.
 *
 * ── One task, one session ──────────────────────────────────────────────────
 *
 * The shell permits one session at a time (see dali_shell.h). One select()
 * waits on the listeners, the session's socket and the web connections not yet
 * answered. While a session is open the TCP listener is not watched, so a
 * second TCP client waits in the listen backlog, connected and unanswered,
 * until the session ends. The web listener is watched throughout: a page
 * request is served, and a second WebSocket upgrade is answered with a close
 * frame that says the shell is busy.
 *
 * All of that happens between verbs only. dali_shell_dispatch() holds this
 * task for the whole verb, so while a `discover` runs nothing is accepted and
 * a page load waits for the verb to end.
 *
 * A session whose peer is gone must not keep the shell, and mid-workflow it
 * must not keep the bus. A peer that stops reading is dropped once one send()
 * has taken nothing for SO_SNDTIMEO, which also ends the workflow at its next
 * step. One that vanished while idle is found by TCP keepalive in under a
 * minute. A session that is merely unused is reclaimed by the idle timeout.
 *
 * ── Access ─────────────────────────────────────────────────────────────────
 *
 * Both ports are unauthenticated. It is the same trust posture as OTA and the
 * ESPHome web server: whoever can reach the port can drive the bus. Because
 * that is a lower bar than physical access to the UART, commissioning verbs are
 * refused unless the YAML opts in with `allow_commissioning: true`, and that
 * one policy covers both bindings.
 *
 * A browser can reach the TCP port too, on behalf of any page it has open: a
 * cross-site form or no-cors fetch POSTs here without a preflight, and the
 * lines of its body would run as commands. So a connection that opens with an
 * HTTP request line is closed before any of its lines is dispatched
 * (dali_cli_peer_sniff()). That shuts the browser out; it does nothing about a
 * program on the network that speaks the shell.
 *
 * The web port is meant for a browser, so it checks instead that the browser
 * is on this device's own page: Host must be a name this device answers to,
 * and a WebSocket's Origin must be that same page. Both checks are
 * dali_web_route(); see dali_web.h.
 */

#include "esphome/core/component.h"

#include "dali_component.h"

extern "C" {
#include "../../../components/dali/dali_web.h"
}

#include <cstdint>

namespace esphome {
namespace dali {

class DaliShellServer : public Component {
 public:
  void set_dali_component(DaliComponent *parent) { parent_ = parent; }
  void set_port(uint16_t port) { port_ = port; }
  void set_idle_timeout(uint32_t seconds) { idle_timeout_s_ = seconds; }
  void set_allow_commissioning(bool allow) { allow_commissioning_ = allow; }

  // ── Web binding. Absent from the YAML, web_port_ stays 0 and nothing of it
  // runs. The page is the gzipped bytes codegen puts in flash.
  void set_web_port(uint16_t port) { web_port_ = port; }
  void add_web_allowed_host(const char *host);
  void set_web_page(const uint8_t *gzipped, uint32_t len)
  {
    web_page_ = gzipped;
    web_page_len_ = len;
  }

  // How many names `allowed_hosts` may list. __init__.py holds the same limit.
  static constexpr uint8_t WEB_ALLOWED_HOSTS_MAX = 8;
  // Web connections held open at once before they are answered or after.
  // Each is one lwIP socket; __init__.py registers the same number.
  static constexpr uint8_t WEB_SLOTS = 2;

  void setup() override;
  void dump_config() override;

  // Must come up after the bus it drives.
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 protected:
  enum class ClientKind : uint8_t { NONE = 0, RAW, WEBSOCKET };

  // Why a session is ending. Set where it is found, acted on once per pass of
  // the loop, so a read that finds the end does not close the socket under
  // the code still looking at it.
  enum class SessionEnd : uint8_t {
    NONE = 0,
    PEER_CLOSED,   // orderly TCP close, or a WebSocket close frame
    PEER_LOST,     // a send or receive failed
    IDLE,          // the idle timeout
    HTTP_PEER,     // the TCP port was sent an HTTP request
    WS_FAILED,     // the client broke the WebSocket protocol
  };

  // A web connection that is not the session: one being read up to the end of
  // its request head, or one already answered and being read to its close so
  // that closing it does not reset what was sent.
  struct WebSlot {
    enum class State : uint8_t { HEAD, DRAIN };
    int            fd{-1};
    State          state{State::HEAD};
    uint32_t       opened_ms{0};
    uint32_t       deadline_ms{0};
    DaliWebRequest req;
  };

  // The accept/select loop. Runs on its own task, because a verb may block for
  // tens of seconds. The task shares the ESPHome loop's core and priority; see
  // SHELL_TASK_PRIORITY in the .cpp.
  static void task_entry(void *arg);
  void        run();
  void        open_listeners();
  // An accepted socket blocks on send(), so that EAGAIN means SO_SNDTIMEO.
  static void set_blocking(int fd);
  // The peer's IPv4 address as text, or "unknown". `cap` of 16 fits any.
  static void peer_text(int fd, char *out, size_t cap);

  // ── Session (dali_shell_tcp.cpp)
  bool begin_session(int fd, ClientKind kind);
  void session_read();
  void session_input(const uint8_t *data, size_t len);
  void session_input_ws(uint8_t byte);
  void session_tick();
  void close_session();
  void end_session(SessionEnd why, DaliWebWsCloseCode code = DALI_WEB_WS_CLOSE_NORMAL,
                   const char *reason = nullptr);

  // Output is gathered and sent a line or more at a time; see the note above
  // write_cb() in the .cpp. A WebSocket session sends each flush as one text
  // frame, its header written into the room kept in front of the data.
  void  send_all(const char *data, size_t len);
  void  flush_output();
  void  write_ws(const char *text, size_t len);
  void  send_ws_control(DaliWebWsOpcode opcode, const uint8_t *payload, size_t len);
  void  send_ws_close(DaliWebWsCloseCode code, const char *reason);
  char *out_data() { return reinterpret_cast<char *>(out_frame_) + WS_HEAD_ROOM; }

  // ── Web connections (dali_shell_web.cpp)
  void web_accept();
  void web_slot_read(WebSlot &slot);
  void web_answer(WebSlot &slot, size_t leftover);
  void web_linger(WebSlot &slot);
  void web_linger_fd(int fd);
  void web_close_slot(WebSlot &slot);
  void web_expire_slots(uint32_t now);
  bool web_slots_busy() const;

  // ── Session bindings handed to dali_shell_attach() ──────────────────────
  static void write_cb(void *ctx, const char *text);
  static bool aborted_cb(void *ctx);
  static bool bus_claim_cb(void *ctx, const char *what);
  static void bus_release_cb(void *ctx);
  static void export_config_cb(void *ctx, const DaliCliOut *out,
                               const DaliDiscoveryInventory *inventory,
                               DaliShellInputLookupFn input_lookup);
  static void inventory_changed_cb(void *ctx,
                                   const DaliDiscoveryInventory *inventory);
  /* Address-backup persistence. The shell owns the snapshot and its codec;
   * these move the bytes to and from flash, which is the integration's to own
   * because the preferences API is. */
  static bool snapshot_save_cb(void *ctx, const uint8_t *buf, uint32_t len);
  static bool snapshot_load_cb(void *ctx, uint8_t *buf, uint32_t *len);
  static void config_applied_cb(void *ctx, DaliTarget target, DaliCommandId id,
                                uint8_t param);
  static void short_address_moved_cb(void *ctx, uint8_t from, uint8_t to);
  static void short_address_cleared_cb(void *ctx, uint8_t addr);
  static void instance_config_applied_cb(void *ctx, uint8_t addr, uint8_t instance);

  DaliComponent *parent_{nullptr};
  uint16_t       port_{2323};
  uint32_t       idle_timeout_s_{600};
  bool           allow_commissioning_{false};

  int        listen_fd_{-1};
  uint32_t   listen_retry_ms_{0};
  int        client_fd_{-1};
  ClientKind client_kind_{ClientKind::NONE};
  SessionEnd session_end_{SessionEnd::NONE};
  // What a WebSocket session's closing frame says.
  DaliWebWsCloseCode ws_close_code_{DALI_WEB_WS_CLOSE_NORMAL};
  const char        *ws_close_reason_{nullptr};
  uint32_t   last_activity_ms_{0};
  // Set when a send() fails or the peer closes; read by aborted_cb() so an
  // in-flight workflow stops instead of writing into a dead socket.
  bool peer_lost_{false};

  // Per-session input state: one or the other, by client_kind_.
  DaliCliPeerSniffer sniffer_{};
  DaliWebWsReceiver  ws_rx_{};

  // Output not yet sent. Touched only on the session task: output raised on
  // another task is deferred by the shell and pumped from session_tick().
  // A text frame header for up to 65,535 bytes is 4 bytes.
  static constexpr size_t WS_HEAD_ROOM = 4;
  static constexpr size_t OUT_BUF_SIZE = 512;
  uint8_t out_frame_[WS_HEAD_ROOM + OUT_BUF_SIZE];
  size_t  out_len_{0};

  // Web
  uint16_t       web_port_{0};
  int            web_listen_fd_{-1};
  uint32_t       web_listen_retry_ms_{0};
  const char    *web_allowed_hosts_[WEB_ALLOWED_HOSTS_MAX]{};
  uint8_t        web_allowed_host_count_{0};
  // <name> and <name>.local, from the node name, filled in at setup().
  char           web_name_[64]{};
  char           web_name_local_[72]{};
  const uint8_t *web_page_{nullptr};
  uint32_t       web_page_len_{0};
  WebSlot        web_slots_[WEB_SLOTS];
};

}  // namespace dali
}  // namespace esphome
