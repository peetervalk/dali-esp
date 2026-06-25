/*
 * dali_headless.cpp — installation-specific button-to-light dispatch table.
 *
 * This file is inert unless the YAML has:
 *
 *   dali:
 *     headless_dispatch: true
 *
 * Edit this file to match the physical wiring for each bus installation, then
 * recompile and flash the explicit headless firmware.
 *
 * ── Current installation: Bus 1k, Tridonic DALI MC couplers in BF6 mode ──
 *
 * The couplers are autonomous DALI-1 controllers.  In BF6 mode each button
 * sends one of two 16-bit DALI command frames directly to a group address:
 *   ON  button  → RECALL MAX LEVEL (opcode 0x05) → group N
 *   OFF button  → OFF              (opcode 0x00) → group N
 *
 * The lights respond immediately (coupler is the master).  The ESP32 sees
 * these frames as unsolicited RX and observes them via
 * DALI_DISPATCH_ACTION_OBSERVE so HA/state sync stays current without the ESP32
 * re-issuing the same command onto the bus.
 *
 * ── Switching to phantom-address mode (Approach B from todo_pb_couplers.md) ─
 *
 * Reprogram couplers via Macro 8 / masterCONFIGURATOR to target unused short
 * addresses (16–22) instead of the group addresses.  Then change each entry:
 *
 *   .key = { DALI_EVENT_FRAME_LEGACY_16BIT,
 *             DALI_EVENT_ADDRESS_SHORT,
 *             <phantom_addr>,          ← e.g. 16 for zone that was group 6
 *             DALI_DISPATCH_OPCODE_ANY },
 *   .output  = { DALI_ADDR_GROUP, <real_group> },
 *   .action  = DALI_DISPATCH_ACTION_MIRROR,
 *
 * The rest of the dispatch logic is unchanged — MIRROR still translates
 * RECALL MAX / OFF from the frame into the corresponding group command.
 *
 * ── Adding DALI-2 push buttons ───────────────────────────────────────────
 *
 * For a DALI-2 input device at short address A, instance 0, firing short-press:
 *
 *   { { DALI_EVENT_FRAME_INPUT_24BIT,
 *        DALI_EVENT_ADDRESS_SHORT,
 *        A,
 *        0x02 },                       ← short-press event code
 *     { DALI_ADDR_GROUP, N },
 *     DALI_DISPATCH_ACTION_TOGGLE, 0 },
 */

#include "esphome/core/defines.h"

#ifdef USE_DALI_HEADLESS

extern "C" {
#include "../../../components/dali/dali_dispatch.h"
}

static const DaliDispatchEntry dispatch_table[] = {

    /* Group 0 — addresses 5, 6, 7, 14 */
    { { DALI_EVENT_FRAME_LEGACY_16BIT,
        DALI_EVENT_ADDRESS_GROUP, 0,
        DALI_DISPATCH_OPCODE_ANY },
      { DALI_ADDR_GROUP, 0 },
      DALI_DISPATCH_ACTION_OBSERVE, 0 },

    /* Group 2 — addresses 4, 8, 9, 12 */
    { { DALI_EVENT_FRAME_LEGACY_16BIT,
        DALI_EVENT_ADDRESS_GROUP, 2,
        DALI_DISPATCH_OPCODE_ANY },
      { DALI_ADDR_GROUP, 2 },
      DALI_DISPATCH_ACTION_OBSERVE, 0 },

    /* Group 3 — address 13 */
    { { DALI_EVENT_FRAME_LEGACY_16BIT,
        DALI_EVENT_ADDRESS_GROUP, 3,
        DALI_DISPATCH_OPCODE_ANY },
      { DALI_ADDR_GROUP, 3 },
      DALI_DISPATCH_ACTION_OBSERVE, 0 },

    /* Group 4 — address 1 */
    { { DALI_EVENT_FRAME_LEGACY_16BIT,
        DALI_EVENT_ADDRESS_GROUP, 4,
        DALI_DISPATCH_OPCODE_ANY },
      { DALI_ADDR_GROUP, 4 },
      DALI_DISPATCH_ACTION_OBSERVE, 0 },

    /* Group 5 — address 0 (DT6 LED driver) */
    { { DALI_EVENT_FRAME_LEGACY_16BIT,
        DALI_EVENT_ADDRESS_GROUP, 5,
        DALI_DISPATCH_OPCODE_ANY },
      { DALI_ADDR_GROUP, 5 },
      DALI_DISPATCH_ACTION_OBSERVE, 0 },

    /* Group 6 — addresses 2, 3, 10, 11 */
    { { DALI_EVENT_FRAME_LEGACY_16BIT,
        DALI_EVENT_ADDRESS_GROUP, 6,
        DALI_DISPATCH_OPCODE_ANY },
      { DALI_ADDR_GROUP, 6 },
      DALI_DISPATCH_ACTION_OBSERVE, 0 },

    /* Group 7 — address 15 */
    { { DALI_EVENT_FRAME_LEGACY_16BIT,
        DALI_EVENT_ADDRESS_GROUP, 7,
        DALI_DISPATCH_OPCODE_ANY },
      { DALI_ADDR_GROUP, 7 },
      DALI_DISPATCH_ACTION_OBSERVE, 0 },
};

extern "C"
const DaliDispatchEntry *dali_headless_get_table(uint8_t *out_count)
{
    *out_count = (uint8_t)(sizeof(dispatch_table) / sizeof(dispatch_table[0]));
    return dispatch_table;
}

#endif  /* USE_DALI_HEADLESS */
