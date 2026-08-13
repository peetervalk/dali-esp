#pragma once

/*
 * dali_shell_tcp.h — network front end for the diagnostic shell
 *
 * Presents the shell from components/dali/dali_shell.c on a TCP port, so a
 * terminal in Home Assistant reaches the same commands, with the same output,
 * that the serial console has. Nothing about what a verb does lives here: this
 * moves bytes and owns a session's lifetime, exactly as main/dali_diag.c does
 * for UART0.
 *
 * ── Why a raw line protocol ─────────────────────────────────────────────────
 *
 * The shell is a byte stream with no request/response framing: `discover`
 * prints for tens of seconds and `find switches` for minutes, both reporting as
 * they go. That is the shape a socket already has, and the shape an HTTP
 * request or a single Home Assistant text state does not. `nc dali.local 2323`
 * is then a working client, and a richer one is a client-side concern rather
 * than a firmware one.
 *
 * ── One session ────────────────────────────────────────────────────────────
 *
 * The shell permits one session at a time (see dali_shell.h), so a second
 * connection is accepted only far enough to be told why it is being closed —
 * leaving it unaccepted would look like a hung connect. An abandoned session is
 * reclaimed by the idle timeout.
 *
 * ── Access ─────────────────────────────────────────────────────────────────
 *
 * The port is unauthenticated. It is the same trust posture as OTA and the
 * ESPHome web server: whoever can reach the port can drive the bus. Because
 * that is a lower bar than physical access to the UART, commissioning verbs are
 * refused unless the YAML opts in with `allow_commissioning: true`.
 */

#include "esphome/core/component.h"

#include "dali_component.h"

#include <atomic>
#include <cstdint>

namespace esphome {
namespace dali {

class DaliShellServer : public Component {
 public:
  void set_dali_component(DaliComponent *parent) { parent_ = parent; }
  void set_port(uint16_t port) { port_ = port; }
  void set_idle_timeout(uint32_t seconds) { idle_timeout_s_ = seconds; }
  void set_allow_commissioning(bool allow) { allow_commissioning_ = allow; }

  void setup() override;
  void dump_config() override;

  // Must come up after the bus it drives.
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 protected:
  // Accept loop and session pump. Runs on its own task, off the ESPHome loop
  // core, because a verb may block for tens of seconds.
  static void task_entry(void *arg);
  void        run();
  // Serve one accepted connection until it closes or goes idle.
  void        serve(int client_fd);

  // ── Session bindings handed to dali_shell_attach() ──────────────────────
  static void write_cb(void *ctx, const char *text);
  static bool aborted_cb(void *ctx);
  static bool bus_claim_cb(void *ctx, const char *what);
  static void bus_release_cb(void *ctx);

  DaliComponent *parent_{nullptr};
  uint16_t       port_{2323};
  uint32_t       idle_timeout_s_{600};
  bool           allow_commissioning_{false};

  int  listen_fd_{-1};
  int  client_fd_{-1};
  // Set when a send() fails or the peer closes; read by aborted_cb() so an
  // in-flight workflow stops instead of writing into a dead socket.
  bool peer_lost_{false};
};

}  // namespace dali
}  // namespace esphome
