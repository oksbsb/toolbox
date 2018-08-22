#ifndef GRE_H_H
#define GRE_H_H

#ifndef IPPROTO_GRE
#define IPPROTO_GRE 47
#endif

typedef struct GREHdr_
{
    uint8_t flags; /**< GRE packet flags */
    uint8_t version; /**< GRE version */
    uint16_t ether_type; /**< ether type of the encapsulated traffic */

} __attribute__((__packed__)) GREHdr;

/* Generic Routing Encapsulation Source Route Entries (SREs).
 * The header is followed by a variable amount of Routing Information.
 */
typedef struct GRESreHdr_
{
    uint16_t af; /**< Address family */
    uint8_t sre_offset;
    uint8_t sre_length;
} __attribute__((__packed__)) GRESreHdr;

#define GRE_VERSION_0           0x0000
#define GRE_VERSION_1           0x0001

#define GRE_HDR_LEN             4
#define GRE_CHKSUM_LEN          2
#define GRE_OFFSET_LEN          2
#define GRE_KEY_LEN             4
#define GRE_SEQ_LEN             4
#define GRE_SRE_HDR_LEN         4
#define GRE_PROTO_PPP           0x880b

#define GRE_FLAG_ISSET_CHKSUM(r)    (r->flags & 0x80)
#define GRE_FLAG_ISSET_ROUTE(r)     (r->flags & 0x40)
#define GRE_FLAG_ISSET_KY(r)        (r->flags & 0x20)
#define GRE_FLAG_ISSET_SQ(r)        (r->flags & 0x10)
#define GRE_FLAG_ISSET_SSR(r)       (r->flags & 0x08)
#define GRE_FLAG_ISSET_RECUR(r)     (r->flags & 0x07)
#define GRE_GET_VERSION(r)   (r->version & 0x07)
#define GRE_GET_FLAGS(r)     (r->version & 0xF8)
#define GRE_GET_PROTO(r)     ntohs(r->ether_type)

#define GREV1_HDR_LEN           8
#define GREV1_ACK_LEN           4
#define GREV1_FLAG_ISSET_FLAGS(r)  (r->version & 0x78)
#define GREV1_FLAG_ISSET_ACK(r)    (r->version & 0x80)
#endif

