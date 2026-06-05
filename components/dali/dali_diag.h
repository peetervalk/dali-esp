#pragma once

/*
 * dali_diag.h — diagnostic serial CLI
 *
 * Transport : UART0, 115200 baud, text-based line interface
 * Task      : lower priority than DALI processing task
 *
 * Supported commands:
 *   help             — print command summary
 *   stats            — print all diagnostic counters
 *   trace on|off     — enable/disable per-frame bus trace logging
 *   read             — print the last received raw frame
 *   reset            — reset PHY, scheduler, and diagnostic state
 *   raw <hex> len=<n> [wait] — transmit arbitrary frame, optionally wait for reply
 *   level/off/max/min/status helpers for addr/sN/gN/b targets
 *   scan/discover    — scan short addresses 0–63 and update inventory
 *   inventory        — print the last discovered inventory
 *   identify <addr>  — blink one short-addressed lamp candidate
 */

#include "dali_frame.h"

/* Initialise the diagnostic CLI and start its FreeRTOS task. */
DaliError dali_diag_init(void);

/* Returns true if per-frame trace logging is currently enabled. */
bool dali_diag_trace_enabled(void);
