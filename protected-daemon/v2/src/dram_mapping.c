#include "dram_mapping.h"

#include <string.h>

static size_t bounded_strlen(const char *s, size_t max_len)
{
    size_t n = 0U;

    if (s == NULL) {
        return 0U;
    }

    while (n < max_len && s[n] != '\0') {
        ++n;
    }

    return n;
}

static unsigned int parity64(uint64_t value)
{
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned int)__builtin_parityll(
        (unsigned long long)value);
#else
    /*
     * Portable fallback: fold to 4 bits and use a parity lookup constant.
     */
    value ^= value >> 32U;
    value ^= value >> 16U;
    value ^= value >> 8U;
    value ^= value >> 4U;

    return (unsigned int)((0x6996U >> (value & 0xFU)) & 1U);
#endif
}

void dram_mapping_reset(struct dram_mapping *mapping)
{
    if (mapping == NULL) {
        return;
    }

    memset(mapping, 0, sizeof(*mapping));
}

bool dram_mapping_is_ready(const struct dram_mapping *mapping)
{
    return mapping != NULL &&
           mapping->ready &&
           mapping->mask_count > 0U;
}

static int validate_calibration_result(
    const struct calibration_result *result)
{
    size_t id_len;
    size_t i;
    size_t j;

    if (result == NULL) {
        return DRAM_MAPPING_ERR_INVALID_ARG;
    }

    if (!result->status_success ||
        !result->acceptance_passed) {
        return DRAM_MAPPING_ERR_CALIBRATION_REJECTED;
    }

    if (result->bank_mask_count == 0U ||
        result->bank_mask_count > DRAM_MAPPING_MAX_MASKS) {
        return DRAM_MAPPING_ERR_MASK_COUNT;
    }

    id_len = bounded_strlen(result->mask_set_id,
                            DRAM_MAPPING_MASK_SET_ID_MAX);

    if (id_len == 0U ||
        id_len >= DRAM_MAPPING_MASK_SET_ID_MAX) {
        return DRAM_MAPPING_ERR_MASK_SET_ID;
    }

    for (i = 0U; i < result->bank_mask_count; ++i) {
        if (result->bank_masks[i] == 0U) {
            return DRAM_MAPPING_ERR_ZERO_MASK;
        }

        for (j = 0U; j < i; ++j) {
            if (result->bank_masks[i] ==
                result->bank_masks[j]) {
                return DRAM_MAPPING_ERR_DUPLICATE_MASK;
            }
        }
    }

    return DRAM_MAPPING_OK;
}

int dram_mapping_init_from_calibration(
    struct dram_mapping *mapping,
    const struct calibration_result *result)
{
    int rc;
    size_t id_len;

    if (mapping == NULL || result == NULL) {
        return DRAM_MAPPING_ERR_INVALID_ARG;
    }

    /*
     * Fail closed: a partially initialized mapping is never left ready.
     */
    dram_mapping_reset(mapping);

    rc = validate_calibration_result(result);
    if (rc != DRAM_MAPPING_OK) {
        return rc;
    }

    memcpy(mapping->masks,
           result->bank_masks,
           result->bank_mask_count * sizeof(mapping->masks[0]));

    mapping->mask_count = result->bank_mask_count;

    id_len = bounded_strlen(result->mask_set_id,
                            DRAM_MAPPING_MASK_SET_ID_MAX);

    memcpy(mapping->mask_set_id,
           result->mask_set_id,
           id_len);

    mapping->mask_set_id[id_len] = '\0';

    /*
     * Publish readiness only after every field has been initialized.
     */
    mapping->ready = true;

    return DRAM_MAPPING_OK;
}

uint64_t dram_mapping_bank_class_fast(
    const struct dram_mapping *mapping,
    uint64_t physical_address)
{
    uint64_t bank_class = 0U;
    size_t i;

    for (i = 0U; i < mapping->mask_count; ++i) {
        uint64_t bit = (uint64_t)parity64(
            physical_address & mapping->masks[i]);

        bank_class |= bit << i;
    }

    return bank_class;
}

int dram_mapping_bank_class(
    const struct dram_mapping *mapping,
    uint64_t physical_address,
    uint64_t *bank_class_out)
{
    if (mapping == NULL || bank_class_out == NULL) {
        return DRAM_MAPPING_ERR_INVALID_ARG;
    }

    if (!dram_mapping_is_ready(mapping)) {
        return DRAM_MAPPING_ERR_NOT_READY;
    }

    *bank_class_out = dram_mapping_bank_class_fast(
        mapping,
        physical_address);

    return DRAM_MAPPING_OK;
}

int dram_mapping_same_bank_class(
    const struct dram_mapping *mapping,
    uint64_t pa1,
    uint64_t pa2,
    bool *same_out)
{
    uint64_t diff;
    size_t i;

    if (mapping == NULL || same_out == NULL) {
        return DRAM_MAPPING_ERR_INVALID_ARG;
    }

    if (!dram_mapping_is_ready(mapping)) {
        return DRAM_MAPPING_ERR_NOT_READY;
    }

    diff = pa1 ^ pa2;

    for (i = 0U; i < mapping->mask_count; ++i) {
        if (parity64(diff & mapping->masks[i]) != 0U) {
            *same_out = false;
            return DRAM_MAPPING_OK;
        }
    }

    *same_out = true;
    return DRAM_MAPPING_OK;
}


int dram_mapping_page_class_deltas(
    const struct dram_mapping *mapping,
    size_t page_size,
    size_t cacheline_bytes,
    uint64_t *deltas_out,
    unsigned int *line_counts_out,
    size_t deltas_cap,
    size_t *delta_count_out)
{
    size_t lines_per_page;
    size_t delta_count = 0U;
    size_t i;

    if (mapping == NULL || deltas_out == NULL || delta_count_out == NULL ||
        !dram_mapping_is_ready(mapping) || page_size == 0U ||
        cacheline_bytes == 0U || (page_size % cacheline_bytes) != 0U ||
        deltas_cap == 0U) {
        return DRAM_MAPPING_ERR_INVALID_ARG;
    }

    lines_per_page = page_size / cacheline_bytes;

    for (i = 0U; i < lines_per_page; ++i) {
        uint64_t offset = (uint64_t)i * (uint64_t)cacheline_bytes;
        uint64_t delta = dram_mapping_bank_class_fast(mapping, offset);
        size_t j;
        bool found = false;

        for (j = 0U; j < delta_count; ++j) {
            if (deltas_out[j] == delta) {
                if (line_counts_out != NULL)
                    ++line_counts_out[j];
                found = true;
                break;
            }
        }

        if (found)
            continue;

        if (delta_count >= deltas_cap)
            return DRAM_MAPPING_ERR_INVALID_ARG;

        deltas_out[delta_count] = delta;
        if (line_counts_out != NULL)
            line_counts_out[delta_count] = 1U;
        ++delta_count;
    }

    *delta_count_out = delta_count;
    return DRAM_MAPPING_OK;
}

bool dram_mapping_page_deltas_form_xor_subgroup(
    const uint64_t *deltas,
    size_t delta_count)
{
    size_t i;
    size_t j;
    bool has_zero = false;

    if (deltas == NULL || delta_count == 0U)
        return false;

    for (i = 0U; i < delta_count; ++i) {
        if (deltas[i] == 0U) {
            has_zero = true;
            break;
        }
    }
    if (!has_zero)
        return false;

    for (i = 0U; i < delta_count; ++i) {
        for (j = 0U; j < delta_count; ++j) {
            uint64_t x = deltas[i] ^ deltas[j];
            size_t k;
            bool found = false;

            for (k = 0U; k < delta_count; ++k) {
                if (deltas[k] == x) {
                    found = true;
                    break;
                }
            }
            if (!found)
                return false;
        }
    }

    return true;
}

uint64_t dram_mapping_canonical_group_fast(
    uint64_t bank_class,
    const uint64_t *deltas,
    size_t delta_count)
{
    uint64_t representative = bank_class;
    size_t i;

    if (deltas == NULL || delta_count == 0U)
        return bank_class;

    representative = bank_class ^ deltas[0];
    for (i = 1U; i < delta_count; ++i) {
        uint64_t member = bank_class ^ deltas[i];
        if (member < representative)
            representative = member;
    }

    return representative;
}

const char *dram_mapping_strerror(int rc)
{
    switch (rc) {
    case DRAM_MAPPING_OK:
        return "success";
    case DRAM_MAPPING_ERR_INVALID_ARG:
        return "invalid argument";
    case DRAM_MAPPING_ERR_NOT_READY:
        return "DRAM mapping is not ready";
    case DRAM_MAPPING_ERR_CALIBRATION_REJECTED:
        return "calibration result was not accepted";
    case DRAM_MAPPING_ERR_MASK_COUNT:
        return "invalid bank-mask count";
    case DRAM_MAPPING_ERR_ZERO_MASK:
        return "zero bank mask is not allowed";
    case DRAM_MAPPING_ERR_DUPLICATE_MASK:
        return "duplicate bank mask";
    case DRAM_MAPPING_ERR_MASK_SET_ID:
        return "invalid mask-set id";
    default:
        return "unknown DRAM-mapping error";
    }
}
