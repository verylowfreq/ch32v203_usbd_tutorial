#ifndef DESC_H
#define DESC_H
#include <stdint.h>

extern const uint8_t desc_device[18];
extern const uint8_t desc_config[];
extern const uint16_t desc_config_len;

/* Build the string descriptor into buf and return its length (0 = not found) */
uint16_t desc_get_string(uint8_t index, uint8_t *buf, uint16_t bufsize);

#endif
