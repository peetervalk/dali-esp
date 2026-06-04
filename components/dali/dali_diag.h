#pragma once

/*
 * dali_diag.h — diagnostic serial CLI
 *
 * Transport : UART0, 115200 baud, text-based line interface
 * Task      : lower priority than DALI processing task
 *
 * Supported commands:
 *   stats            — print all diagnostic counters
 *   trace on|off     — enable/disable per-frame bus trace logging
 *   reset            — reset PHY and scheduler state machines
 *   raw <hex> len=<n>— transmit arbitrary frame (requires PHY init)
 *   scan             — scan short addresses 0–63 (requires scheduler)
 *   query <addr>     — query device status (requires scheduler)
 */

#include "dali_frame.h"

/* Initialise the diagnostic CLI and start its FreeRTOS task. */
DaliError dali_diag_init(void);

/* Returns true if per-frame trace logging is currently enabled. */
bool dali_diag_trace_enabled(void);
