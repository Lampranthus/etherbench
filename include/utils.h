#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>

int parse_u8_arg(const char *text, uint8_t *value);
int parse_u16_arg(const char *text, uint16_t *value);
int parse_u32_arg(const char *text, uint32_t *value);
int parse_int_arg(const char *text, int *value);
int parse_int_range_arg(
    const char *text,
    int min_value,
    int max_value,
    const char *name,
    int *value
);

#endif
