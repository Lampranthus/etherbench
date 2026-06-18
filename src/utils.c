#include "utils.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int parse_unsigned_long_arg(
    const char *text,
    unsigned long max_value,
    unsigned long *value
)
{
    char *endptr;
    unsigned long parsed;

    if (text == NULL || value == NULL || text[0] == '\0') {
        return -1;
    }

    errno = 0;
    parsed = strtoul(text, &endptr, 0);

    if (errno != 0 || *endptr != '\0' || parsed > max_value) {
        return -1;
    }

    *value = parsed;
    return 0;
}

int parse_u8_arg(const char *text, uint8_t *value)
{
    unsigned long parsed;

    if (value == NULL ||
        parse_unsigned_long_arg(text, UINT8_MAX, &parsed) != 0) {
        return -1;
    }

    *value = (uint8_t)parsed;
    return 0;
}

int parse_u16_arg(const char *text, uint16_t *value)
{
    unsigned long parsed;

    if (value == NULL ||
        parse_unsigned_long_arg(text, UINT16_MAX, &parsed) != 0) {
        return -1;
    }

    *value = (uint16_t)parsed;
    return 0;
}

int parse_u32_arg(const char *text, uint32_t *value)
{
    unsigned long parsed;

    if (value == NULL ||
        parse_unsigned_long_arg(text, UINT32_MAX, &parsed) != 0) {
        return -1;
    }

    *value = (uint32_t)parsed;
    return 0;
}

int parse_int_arg(const char *text, int *value)
{
    char *endptr;
    long parsed;

    if (text == NULL || value == NULL || text[0] == '\0') {
        return -1;
    }

    errno = 0;
    parsed = strtol(text, &endptr, 0);

    if (errno != 0 ||
        *endptr != '\0' ||
        parsed < INT_MIN ||
        parsed > INT_MAX) {
        return -1;
    }

    *value = (int)parsed;
    return 0;
}

int parse_int_range_arg(
    const char *text,
    int min_value,
    int max_value,
    const char *name,
    int *value
)
{
    int parsed;

    if (parse_int_arg(text, &parsed) != 0 ||
        parsed < min_value ||
        parsed > max_value) {
        fprintf(
            stderr,
            "Invalid %s: %s (expected %d..%d)\n",
            name != NULL ? name : "argument",
            text != NULL ? text : "(null)",
            min_value,
            max_value
        );
        return -1;
    }

    *value = parsed;
    return 0;
}
