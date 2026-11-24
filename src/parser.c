//
// Created by belgae on 20/11/26.
//

#include "parser.h"

uint16_t rd16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

/*Function made By sam*/
/*Param : 
 const uint8_t *f   : pointer to first octet of the Trame
 uint16_t len       : Number of available octetn of the trame
 flow_key_t *key    : Struc to fill in with the 5 tuple
 uint8_t *tcp_flags : Variable to fill in with TCP flags

 Return : Int => 1 if packet is accepted, 0 if not

*/
int parse_flow(const uint8_t *f, uint16_t len, flow_key_t *key, uint8_t *tcp_flags){
    // Ethernet header: 14 bytes
    if (len < 14 + 20)
        return 0;

    // EtherType must be IPv4
    if (rd16(f + 12) != 0x0800)
        return 0;

    // IPv4 version and header length
    uint8_t version = f[14] >> 4;
    uint8_t ihl = (f[14] & 0x0F) * 4;

    if (version != 4 || ihl < 20)
        return 0;

    if (len < 14 + ihl)
        return 0;

    // IPv4 total length
    uint16_t ip_len = rd16(f + 16);

    if (ip_len < ihl || (uint32_t)ip_len > (uint32_t)len - 14)
        return 0;

    // Reject fragmented IPv4 packets
    uint16_t frag = rd16(f + 20);
    if ((frag & 0x3FFF) != 0)
        return 0;

    uint8_t protocol = f[23];

    if (protocol != 6 && protocol != 17)
        return 0;

    // TCP/UDP ports start after the IPv4 header
    uint16_t transport_offset = 14 + ihl;

    // Minimum transport header length
    uint16_t min_transport = protocol == 6 ? 20 : 8;

    if (ip_len < ihl + min_transport)
        return 0;

    key->src_ip = rd32(f + 26);
    key->dst_ip = rd32(f + 30);
    key->src_port = rd16(f + transport_offset);
    key->dst_port = rd16(f + transport_offset + 2);
    key->protocol = protocol;

    *tcp_flags = 0;

    if (protocol == 6) {
        uint8_t tcp_hlen = (f[transport_offset + 12] >> 4) * 4;

        if (tcp_hlen < 20 || ip_len < ihl + tcp_hlen)
            return 0;

        *tcp_flags = f[transport_offset + 13];
    }

    return 1;
}