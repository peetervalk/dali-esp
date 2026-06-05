#pragma once

/*
 * dali_mapping.h - static DALI endpoint mapping helpers
 *
 * This layer validates and looks up user-provided mappings. It intentionally
 * does not assign room names, entity names, or "lamp 1 means group 0" defaults.
 */

#include "dali_control.h"

#define DALI_MAPPING_INSTANCE_UNUSED 0xFFu

typedef enum {
    DALI_MAPPING_OUTPUT = 0,
    DALI_MAPPING_INPUT_INSTANCE = 1,
} DaliMappingKind;

typedef struct {
    uint8_t         id;            /* caller-defined stable id, not a room/name */
    DaliMappingKind kind;
    DaliTarget      target;
    uint8_t         instance;      /* INPUT_INSTANCE: 0..31; OUTPUT: 0xFF */
    uint8_t         instance_type; /* caller/profile supplied; OUTPUT: 0xFF */
} DaliMappingEntry;

typedef struct {
    const DaliMappingEntry *entries;
    uint8_t                 count;
} DaliMappingTable;

DaliError dali_mapping_validate_entry(const DaliMappingEntry *entry);
DaliError dali_mapping_validate_table(const DaliMappingTable *table);

const DaliMappingEntry *dali_mapping_find_by_id(const DaliMappingTable *table,
                                                uint8_t id);
