//
// Created by belgae on 25/11/26.
//

#ifndef NI_PROJECT_IP_IN_IP_H
#define NI_PROJECT_IP_IN_IP_H
#include <stdint.h>

/* IP-in-IP encapsulation (README, "Encapsulation"), ported from Atieh's test rig (scripts/tests_ring/e2e_main.c):
   the new Ethernet and outer IPv4 headers go over the 20 bytes of headroom and the old Ethernet header; the inner
   packet does not move. */
static uint16_t ip_checksum(const uint8_t *h);

static void encap(const lb_t *lb, pkt_t *pk, uint32_t dst);

static void put32(uint8_t *b, uint32_t x)
{
    b[0] = (uint8_t)(x >> 24); b[1] = (uint8_t)(x >> 16); b[2] = (uint8_t)(x >> 8); b[3] = (uint8_t)x;
}

#endif //NI_PROJECT_IP_IN_IP_H
