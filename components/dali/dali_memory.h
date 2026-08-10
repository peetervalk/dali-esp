#pragma once

/*
 * dali_memory.h — DALI memory bank access (IEC 62386-102/103)
 *
 * Memory read sequence for a single byte:
 *   DTR1 = bank     (special broadcast frame, no reply)
 *   DTR0 = offset   (special broadcast frame, no reply)
 *   READ_MEMORY_LOCATION (addressed 16-bit, expects 8-bit reply)
 *
 * Subsequent READ_MEMORY_LOCATION calls auto-increment DTR0, so a block read
 * needs one DTR1 + one DTR0 setup followed by N READ frames (§9.10.3).
 *
 * Control-device writes use a separate seven-step sequence builder so the
 * caller can enqueue the complete DTR/unlock/write workflow atomically.
 *
 * No hardware dependencies.
 */

#include <stdint.h>
#include "dali_protocol.h"
#include "dali_scheduler.h"

#define DALI_MEMORY_CONTROL_DEVICE_WRITE_STEPS 7u

/* A block read spends two steps on DTR1/DTR0 before the first READ frame. */
#define DALI_MEMORY_READ_SETUP_STEPS 2u
#define DALI_MEMORY_MAX_SEQUENCE_READ_BYTES \
    (DALI_SEQUENCE_MAX_STEPS - DALI_MEMORY_READ_SETUP_STEPS)

/* ---------------------------------------------------------------------------
 * Bank 0 — mandatory identity bank (IEC 62386-102:2022 §9.10.7)
 *
 * Location 0x01 is not implemented and must not be included in a contiguous
 * read. Location 0x02 reports the last accessible memory bank; the common
 * identity fields occupy 0x03..0x14.
 * --------------------------------------------------------------------------*/
#define DALI_MEMORY_BANK0                   0u

#define DALI_MEMORY_BANK0_OFFSET_LAST_ADDR  0x00u   /* last addressable location */
#define DALI_MEMORY_BANK0_OFFSET_LAST_BANK  0x02u   /* last accessible bank      */
#define DALI_MEMORY_BANK0_OFFSET_GTIN       0x03u   /* 6-byte GTIN, MSB first    */
#define DALI_MEMORY_BANK0_GTIN_LEN          6u
#define DALI_MEMORY_BANK0_OFFSET_FW_MAJOR   0x09u
#define DALI_MEMORY_BANK0_OFFSET_FW_MINOR   0x0Au
#define DALI_MEMORY_BANK0_OFFSET_IDENTIFICATION 0x0Bu
#define DALI_MEMORY_BANK0_IDENTIFICATION_LEN    8u
#define DALI_MEMORY_BANK0_OFFSET_HW_MAJOR   0x13u
#define DALI_MEMORY_BANK0_OFFSET_HW_MINOR   0x14u
#define DALI_MEMORY_BANK0_IDENTITY_FIRST    DALI_MEMORY_BANK0_OFFSET_GTIN
#define DALI_MEMORY_BANK0_IDENTITY_LAST     DALI_MEMORY_BANK0_OFFSET_HW_MINOR
#define DALI_MEMORY_BANK0_IDENTITY_SIZE     \
    (DALI_MEMORY_BANK0_IDENTITY_LAST - DALI_MEMORY_BANK0_IDENTITY_FIRST + 1u)

/* Backwards-compatible names for the standard 8-byte identification number. */
#define DALI_MEMORY_BANK0_OFFSET_SERIAL     DALI_MEMORY_BANK0_OFFSET_IDENTIFICATION
#define DALI_MEMORY_BANK0_SERIAL_LEN        DALI_MEMORY_BANK0_IDENTIFICATION_LEN

/* Bank 1 is optional OEM/part-specific data, not a second identity bank. */
#define DALI_MEMORY_BANK1                   1u

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
    /* Standard identification number, MSB first; retained name for compatibility. */
    uint8_t serial[DALI_MEMORY_BANK0_IDENTIFICATION_LEN];
    uint8_t hw_major;
    uint8_t hw_minor;
} DaliMemoryBank0Identity;

/* ---------------------------------------------------------------------------
 * Frame builders — pure, no side effects
 * --------------------------------------------------------------------------*/

/* Build a DTR1 = bank special broadcast frame. */
DaliFrame dali_memory_build_dtr1_bank(uint8_t bank);

/* Build a DTR0 = offset special broadcast frame. */
DaliFrame dali_memory_build_dtr0_offset(uint8_t offset);

/* Build READ_MEMORY_LOCATION for short_addr 0..63. Invalid addresses return a
 * zero-length frame; the transaction helpers reject them before bus traffic. */
DaliFrame dali_memory_build_read(uint8_t short_addr);

/*
 * Build the Part 103 control-device sequence that unlocks a writable bank and
 * writes one byte. Bank 0 is read-only; bank must be 1..255. The returned seven
 * logical steps expand to nine forward frames because each addressed ENABLE
 * WRITE MEMORY command is marked send-twice. The sequence has no callback.
 */
DaliError dali_memory_build_control_device_write_sequence(uint8_t       short_addr,
                                                          uint8_t       bank,
                                                          uint8_t       offset,
                                                          uint8_t       value,
                                                          DaliSequence *out);

/*
 * Build a control-gear (Part 102) block read as one sequence: DTR1 = bank,
 * DTR0 = offset, then count READ MEMORY LOCATION frames that each expect a
 * reply. Running the setup and the reads as one sequence stops other traffic
 * from redirecting DTR0/DTR1 midway through the read.
 *
 * count must be 1..DALI_MEMORY_MAX_SEQUENCE_READ_BYTES. Longer blocks must be
 * split into chunks that each re-issue their own DTR1/DTR0 setup.
 *
 * Read steps carry no retry budget on purpose: READ MEMORY LOCATION increments
 * DTR0 on the device, so re-sending it after a lost reply would return the byte
 * after the one requested. A caller that wants to retry must re-run the whole
 * sequence so the offset is re-established.
 */
DaliError dali_memory_build_read_sequence(uint8_t       short_addr,
                                          uint8_t       bank,
                                          uint8_t       offset,
                                          uint8_t       count,
                                          DaliSequence *out);

/*
 * Part 103 control-device equivalent of dali_memory_build_read_sequence(). Uses
 * the 24-bit control-device DTR frames and the addressed device READ MEMORY
 * LOCATION command rather than the 16-bit control-gear forms. Multi-byte reads
 * rely on the same DTR0 auto-increment and are not hardware-verified here.
 */
DaliError dali_memory_build_control_device_read_sequence(uint8_t       short_addr,
                                                         uint8_t       bank,
                                                         uint8_t       offset,
                                                         uint8_t       count,
                                                         DaliSequence *out);

/*
 * Copy the bytes returned by a sequence from either read builder into buf.
 * Every read step must have produced a backward frame, so a partially executed
 * sequence never yields a half-filled buffer. A failed sequence returns its own
 * error. buf must hold at least count bytes.
 */
DaliError dali_memory_read_from_sequence(const DaliSequenceResult *result,
                                         uint8_t                   count,
                                         uint8_t                  *buf);

/* ---------------------------------------------------------------------------
 * Read helpers — use the transport to issue frames and collect replies
 * --------------------------------------------------------------------------*/

/*
 * Read one byte from bank:offset on short_addr (0..63).
 * Issues DTR1, DTR0, then READ_MEMORY_LOCATION.
 */
DaliError dali_memory_read_byte(const DaliMemoryTransport *transport,
                                uint8_t                    short_addr,
                                uint8_t                    bank,
                                uint8_t                    offset,
                                uint8_t                   *out);

/*
 * Read count consecutive bytes starting at bank:offset into buf for short_addr
 * 0..63.
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
 * Read the standard Bank 0 identity block (18 bytes: offsets 0x03..0x14).
 * The reserved/not-implemented location 0x01 is deliberately not queried.
 */
DaliError dali_memory_read_bank0_identity(const DaliMemoryTransport *transport,
                                          uint8_t                    short_addr,
                                          DaliMemoryBank0Identity   *out);
