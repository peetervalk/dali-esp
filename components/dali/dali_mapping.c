#include "dali_mapping.h"

static bool mapping_input_instance_valid(uint8_t instance)
{
    return instance < DALI_INSTANCE_COUNT;
}

DaliError dali_mapping_validate_entry(const DaliMappingEntry *entry)
{
    if (entry == NULL) {
        return DALI_ERR_INVALID;
    }

    switch (entry->kind) {
        case DALI_MAPPING_OUTPUT:
            if (entry->instance != DALI_MAPPING_INSTANCE_UNUSED ||
                entry->instance_type != DALI_MAPPING_INSTANCE_UNUSED) {
                return DALI_ERR_INVALID;
            }
            return dali_control_validate_target(entry->target);

        case DALI_MAPPING_INPUT_INSTANCE:
            if (entry->target.type != DALI_ADDR_SHORT ||
                entry->target.address >= DALI_SHORT_ADDRESS_COUNT ||
                !mapping_input_instance_valid(entry->instance)) {
                return DALI_ERR_INVALID;
            }
            return DALI_OK;

        default:
            return DALI_ERR_INVALID;
    }
}

DaliError dali_mapping_validate_table(const DaliMappingTable *table)
{
    if (table == NULL || (table->count > 0u && table->entries == NULL)) {
        return DALI_ERR_INVALID;
    }

    for (uint8_t i = 0u; i < table->count; i++) {
        if (dali_mapping_validate_entry(&table->entries[i]) != DALI_OK) {
            return DALI_ERR_INVALID;
        }
        for (uint8_t j = (uint8_t)(i + 1u); j < table->count; j++) {
            if (table->entries[i].id == table->entries[j].id) {
                return DALI_ERR_INVALID;
            }
        }
    }

    return DALI_OK;
}

const DaliMappingEntry *dali_mapping_find_by_id(const DaliMappingTable *table,
                                                uint8_t id)
{
    if (dali_mapping_validate_table(table) != DALI_OK) {
        return NULL;
    }

    for (uint8_t i = 0u; i < table->count; i++) {
        if (table->entries[i].id == id) {
            return &table->entries[i];
        }
    }
    return NULL;
}
