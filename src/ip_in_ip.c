//
// Created by belgae on 25/11/26.
//

#include "ip_in_ip.h"

#include <string.h>

#include "parser.h"

static uint16_t ip_checksum(const uint8_t *h)
{
    uint32_t sum = 0;
    for (int i = 0; i < 20; i += 2)
        sum += ((uint32_t)h[i] << 8) | h[i + 1];
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static void encap(const lb_t *lb, pkt_t *pk, uint32_t dst)
{
    uint8_t *f = pk->buf + pk->off - 20;                        /* the new frame starts in the headroom */
    uint16_t inner_len = rd16(f + 20 + 14 + 2);                 /* the inner IPv4 total length */
    memcpy(f, lb->cfg.zone_mac, 6);
    memcpy(f + 6, lb->cfg.int_mac, 6);
    f[12] = 0x08; f[13] = 0x00;
    uint8_t *ip = f + 14;
    uint16_t tot = (uint16_t)(inner_len + 20);
    ip[0] = 0x45; ip[1] = 0;
    ip[2] = (uint8_t)(tot >> 8); ip[3] = (uint8_t)tot;
    ip[4] = ip[5] = ip[6] = ip[7] = 0;                          /* ID, flags, fragment offset */
    ip[8] = 64; ip[9] = 4;                                      /* TTL, protocol IP-in-IP */
    ip[10] = ip[11] = 0;
    put32(ip + 12, lb->cfg.int_ipv4);
    put32(ip + 16, dst);
    uint16_t c = ip_checksum(ip);
    ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)c;
    pk->off = (uint16_t)(pk->off - 20);
    pk->len = (uint16_t)(pk->len + 20);
}