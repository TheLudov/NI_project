/* Packet parser: the 5-tuple of an Ethernet/IPv4 TCP or UDP frame.
 * Responsible: Samuel (parse_flow, rd16, rd32), Ludovic (first version). */

#ifndef NI_PROJECT_PARSER_H
#define NI_PROJECT_PARSER_H
#include <stdint.h>
// Read 2 bytes and store them in CPU order (little-endian)
uint16_t rd16(const uint8_t *p);
// Read 4 bytes and store them in CPU order (little-endian)
uint32_t rd32(const uint8_t *p);

typedef struct 
{
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t protocol;
}flow_key_t;


int parse_flow(const uint8_t *f, uint16_t len, flow_key_t *key, uint8_t *tcp_flags);

#endif //NI_PROJECT_PARSER_H
