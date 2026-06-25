#pragma once

/*
 * dali_diag.h - diagnostic serial CLI
 *
 * Transport : UART0, 115200 baud, text-based line interface
 * Task      : lower priority than DALI processing task
 *
 * Supported commands:
 *   help             - print command summary
 *   stats            - print all diagnostic counters
 *   trace on|off     - enable/disable per-frame bus trace logging
 *   read             - print the last received raw frame
 *   rxdebug          - print the last malformed RX timing snapshot
 *   reset            - reset PHY, scheduler, and diagnostic state
 *   raw <hex> len=<n> [wait] - transmit arbitrary frame, optionally wait for reply
 *   level/off/up/down/step-up/step-down/step-off/on-step/dapc-seq/last/scene
 *                    - output helpers for addr/sN/gN/b targets
 *   max/min/status   - query/control helpers for addr/sN/gN/b targets
 *   query <target> [query-name] [param] - generic addressed control-gear query
 *   query-list       - print supported generic query names
 *   config <target> <config-name> [param] - generic addressed configuration
 *   config-list      - print supported generic config names
 *   scan             - scan all short addresses; brief output (control gear only)
 *   discover         - scan all short addresses; full output including DALI-2
 *                      input device instances (use this to find sensors/couplers)
 *   inventory        - print the last discovered inventory
 *   commission unaddressed [first-addr] [max-devices]
 *                    - assign short addresses to currently unaddressed gear
 *   instances <addr> - query generic DALI-2 input-device instance types
 *   identify <addr>  - blink one short-addressed lamp candidate
 */

#include "dali_frame.h"

/* Initialise the diagnostic CLI and start its FreeRTOS task. */
DaliError dali_diag_init(void);

/* Returns true if per-frame trace logging is currently enabled. */
bool dali_diag_trace_enabled(void);
