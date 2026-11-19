#ifndef DESC_H
#define DESC_H
#include <stdint.h>

extern const uint8_t desc_device[18];
extern const uint8_t desc_config[];
extern const uint16_t desc_config_len;
extern const uint8_t desc_bos[];
extern const uint16_t desc_bos_len;
extern const uint8_t desc_ms_os_20[];
extern const uint16_t desc_ms_os_20_len;

uint16_t desc_get_string(uint8_t index, uint8_t *buf, uint16_t bufsize);

#endif
