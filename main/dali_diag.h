#pragma once

/*
 * dali_diag.h — serial front end for the diagnostic shell
 *
 * Transport : UART0, 115200 baud, text-based line interface
 * Task      : lower priority than DALI processing task
 *
 * The shell itself — every verb, the blocking transport, and the caches a
 * session accumulates — lives in `components/dali/dali_shell.c`. This file is
 * only the binding that reads bytes off UART0 and writes the shell's output
 * back to stdout, so that the same commands with the same output are available
 * over any other transport a front end cares to add.
 *
 * For the command set, run `help` at the prompt: the verb table in
 * `dali_cli.c` is the one authority on what exists and how each verb spells its
 * arguments, and it is shared with every other front end.
 */

#include "dali_frame.h"

/*
 * Initialize the serial front end and start its FreeRTOS task. Call after
 * dali_shell_init(); the session is attached for the life of the firmware,
 * because physical access to the UART is already physical access to the bus.
 */
DaliError dali_diag_init(void);
