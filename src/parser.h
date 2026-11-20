//
// Created by belgae on 20/11/26.
//

#ifndef NI_PROJECT_PARSER_H
#define NI_PROJECT_PARSER_H
#include <stdint.h>
// Read 2 bytes and store them in CPU order (little-endian)
static uint16_t rd16(const uint8_t *p);
// Read 4 bytes and store them in CPU order (little-endian)
static uint16_t rd32(const uint8_t *p);

#endif //NI_PROJECT_PARSER_H
