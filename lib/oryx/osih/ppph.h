#ifndef PPP_H_H
#define PPP_H_H

/** Point to Point Protocol RFC1331 - Supported tyes */
#define PPP_IP         0x0021       /* Internet Protocol */
#define PPP_IPv6       0x0057       /* Internet Protocol version 6 */
#define PPP_VJ_UCOMP   0x002f       /* VJ uncompressed TCP/IP */

/** Unsupported PPP types (libpcap source reference) */
#define PPP_IPX        0x002b       /* Novell IPX Protocol */
#define PPP_VJ_COMP    0x002d       /* VJ compressed TCP/IP */
#define PPP_IPX        0x002b       /* Novell IPX Protocol */
#define PPP_OSI        0x0023       /* OSI Network Layer */
#define PPP_NS         0x0025       /* Xerox NS IDP */
#define PPP_DECNET     0x0027       /* DECnet Phase IV */
#define PPP_APPLE      0x0029       /* Appletalk */
#define PPP_BRPDU      0x0031       /* Bridging PDU */
#define PPP_STII       0x0033       /* Stream Protocol (ST-II) */
#define PPP_VINES      0x0035       /* Banyan Vines */
#define PPP_HELLO      0x0201       /* 802.1d Hello Packets */
#define PPP_LUXCOM     0x0231       /* Luxcom */
#define PPP_SNS        0x0233       /* Sigma Network Systems */
#define PPP_MPLS_UCAST 0x0281       /* rfc 3032 */
#define PPP_MPLS_MCAST 0x0283       /* rfc 3022 */
#define PPP_IPCP       0x8021       /* IP Control Protocol */
#define PPP_OSICP      0x8023       /* OSI Network Layer Control Protocol */
#define PPP_NSCP       0x8025       /* Xerox NS IDP Control Protocol */
#define PPP_DECNETCP   0x8027       /* DECnet Control Protocol */
#define PPP_APPLECP    0x8029       /* Appletalk Control Protocol */
#define PPP_IPXCP      0x802b       /* Novell IPX Control Protocol */
#define PPP_STIICP     0x8033       /* Strean Protocol Control Protocol */
#define PPP_VINESCP    0x8035       /* Banyan Vines Control Protocol */
#define PPP_IPv6CP     0x8057       /* IPv6 Control Protocol */
#define PPP_MPLSCP     0x8281       /* rfc 3022 */
#define PPP_LCP        0xc021       /* Link Control Protocol */
#define PPP_PAP        0xc023       /* Password Authentication Protocol */
#define PPP_LQM        0xc025       /* Link Quality Monitoring */
#define PPP_CHAP       0xc223       /* Challenge Handshake Authentication Protocol */

/** PPP vlib_pkt_t header */
typedef struct PPPHdr_ {
    uint8_t address;
    uint8_t control;
    uint16_t protocol;
} __attribute__((__packed__)) PPPHdr;

/** PPP vlib_pkt_t header length */
#define PPP_HEADER_LEN 4

#endif
