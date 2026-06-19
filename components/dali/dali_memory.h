#pragma once

/*
 * dali_memory.h — DALI memory bank access (IEC 62386-102 §9.10)
 *
 * Memory read sequence for a single byte:
 *   DTR1 = bank     (special broadcast frame, no reply)
 *   DTR0 = offset   (special broadcast frame, no reply)
 *   READ_MEMORY_LOCATION (addressed 16-bit, expects 8-bit reply)
 *
 * Subsequent READ_MEMORY_LOCATION calls auto-increment DTR0, so a block read
 * needs one DTR1 + one DTR0 setup followed by N READ frames (§9.10.3).
 *
 * No hardware dependencies.
 */

#include <stdint.h>
#include "dali_protocol.h"

/* ---------------------------------------------------------------------------
 * Bank 0 — mandatory identity bank (IEC 62386-102 §9.10.6)
 * --------------------------------------------------------------------------*/
#define DALI_MEMORY_BANK0                   0u

#define DALI_MEMORY_BANK0_OFFSET_LAST_ADDR  0x00u   /* last addressable location */
#define DALI_MEMORY_BANK0_OFFSET_INDICATOR  0x01u   /* 0xFF when bank present    */
#define DALI_MEMORY_BANK0_OFFSET_GTIN       0x02u   /* 6-byte GTIN, MSB first    */
#define DALI_MEMORY_BANK0_GTIN_LEN          6u
#define DALI_MEMORY_BANK0_OFFSET_FW_MAJOR   0x08u
#define DALI_MEMORY_BANK0_OFFSET_FW_MINOR   0x09u
#define DALI_MEMORY_BANK0_OFFSET_SERIAL     0x0Au   /* 8-byte serial number      */
#define DALI_MEMORY_BANK0_SERIAL_LEN        8u
#define DALI_MEMORY_BANK0_IDENTITY_LAST     0x11u   /* last byte of standard block */
#define DALI_MEMORY_BANK0_IDENTITY_SIZE     18u     /* bytes from 0x00 to 0x11   */

/* ---------------------------------------------------------------------------
 * Bank 1 — extended identity (DALI-2 gear, IEC 62386-102 Annex A)
 * Superset of bank 0: adds hardware version at offsets 0x12-0x13.
 * --------------------------------------------------------------------------*/
#define DALI_MEMORY_BANK1                   1u

#define DALI_MEMORY_BANK1_OFFSET_INDICATOR  0x01u
#define DALI_MEMORY_BANK1_OFFSET_GTIN       0x02u
#define DALI_MEMORY_BANK1_GTIN_LEN          6u
#define DALI_MEMORY_BANK1_OFFSET_FW_MAJOR   0x08u
#define DALI_MEMORY_BANK1_OFFSET_FW_MINOR   0x09u
#define DALI_MEMORY_BANK1_OFFSET_SERIAL     0x0Au
#define DALI_MEMORY_BANK1_SERIAL_LEN        8u
#define DALI_MEMORY_BANK1_OFFSET_HW_MAJOR   0x12u
#define DALI_MEMORY_BANK1_OFFSET_HW_MINOR   0x13u
#define DALI_MEMORY_BANK1_IDENTITY_SIZE     20u   /* bytes 0x00 to 0x13 */

typedef struct {
    uint8_t gtin[DALI_MEMORY_BANK1_GTIN_LEN];
    uint8_t fw_major;
    uint8_t fw_minor;
    uint8_t serial[DALI_MEMORY_BANK1_SERIAL_LEN];
    uint8_t hw_major;
    uint8_t hw_minor;
} DaliMemoryBank1Identity;

#define DALI_MEMORY_BANK_IMPLEMENTED        0xFFu   /* indicator value when present */
#define DALI_MEMORY_QUERY_RETRIES           1u

/* ---------------------------------------------------------------------------
 * Transport abstraction — same call shape as DaliDiscoveryTransactionFn so
 * the same mock or real implementation can be cast and reused.
 * --------------------------------------------------------------------------*/
typedef DaliError (*DaliMemoryTransactionFn)(const DaliFrame *frame,
                                             bool             needs_reply,
                                             uint8_t          retries_left,
                                             bool             send_twice,
                                             DaliFrame       *reply_out,
                                             void            *ctx);

typedef struct {
    DaliMemoryTransactionFn transact;
    void                   *ctx;
} DaliMemoryTransport;

/* ---------------------------------------------------------------------------
 * Identity struct for Bank 0
 * --------------------------------------------------------------------------*/
typedef struct {
    uint8_t gtin[DALI_MEMORY_BANK0_GTIN_LEN];
    uint8_t fw_major;
    uint8_t fw_minor;
    uint8_t serial[DALI_MEMORY_BANK0_SERIAL_LEN];
} DaliMemoryBank0Identity;

/* ---------------------------------------------------------------------------
 * Frame builders — pure, no side effects
 * --------------------------------------------------------------------------*/

/* Build a DTR1 = bank special broadcast frame. */
DaliFrame dali_memory_build_dtr1_bank(uint8_t bank);

/* Build a DTR0 = offset special broadcast frame. */
DaliFrame dali_memory_build_dtr0_offset(uint8_t offset);

/* Build a READ_MEMORY_LOCATION frame addressed to short_addr. */
DaliFrame dali_memory_build_read(uint8_t short_addr);

/* ---------------------------------------------------------------------------
 * Read helpers — use the transport to issue frames and collect replies
 * --------------------------------------------------------------------------*/

/*
 * Read one byte from bank:offset on short_addr.
 * Issues DTR1, DTR0, then READ_MEMORY_LOCATION.
 */
DaliError dali_memory_read_byte(const DaliMemoryTransport *transport,
                                uint8_t                    short_addr,
                                uint8_t                    bank,
                                uint8_t                    offset,
                                uint8_t                   *out);

/*
 * Read count consecutive bytes starting at bank:offset into buf.
 * Issues DTR1 + DTR0 once, then count READ_MEMORY_LOCATION frames.
 * buf must be at least count bytes.
 */
DaliError dali_memory_read_bytes(const DaliMemoryTransport *transport,
                                 uint8_t                    short_addr,
                                 uint8_t                    bank,
                                 uint8_t                    offset,
                                 uint8_t                   *buf,
                                 uint8_t                    count);

/*
 * Read the standard Bank 0 identity block (18 bytes: offsets 0x00..0x11).
 * Returns DALI_ERR_INVALID if the indicator byte is not 0xFF.
 */
DaliError dali_memory_read_bank0_identity(const DaliMemoryTransport *transport,
                                          uint8_t                    short_addr,
                                          DaliMemoryBank0Identity   *out);

/*
 * Read the Bank 1 identity block (20 bytes: offsets 0x00..0x13).
 * Superset of bank 0 — adds hardware major/minor version at 0x12/0x13.
 * Returns DALI_ERR_INVALID if the indicator byte is not 0xFF.
 */
DaliError dali_memory_read_bank1_identity(const DaliMemoryTransport *transport,
                                          uint8_t                    short_addr,
                                          DaliMemoryBank1Identity   *out);
