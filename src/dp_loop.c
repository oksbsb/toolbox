#include "oryx.h"
#include "dp_decode.h"
#include "dp_flow.h"
#include "dp_decode_tcp.h"
#include "dp_decode_udp.h"
#include "dp_decode_sctp.h"
#include "dp_decode_gre.h"
#include "dp_decode_icmpv4.h"
#include "dp_decode_icmpv6.h"
#include "dp_decode_ipv4.h"
#include "dp_decode_ipv6.h"
#include "dp_decode_pppoe.h"
#include "dp_decode_mpls.h"
#include "dp_decode_arp.h"
#include "dp_decode_vlan.h"
#include "dp_decode_marvell_dsa.h"
#include "dp_decode_eth.h"

#include "iface_private.h"
#include "map_private.h"
#include "appl_private.h"

#include "dpdk_classify.h"
#include "util_map.h"

extern volatile bool force_quit;
extern ThreadVars g_tv[];
extern DecodeThreadVars g_dtv[];
extern PacketQueue g_pq[];

/* Configure how many packets ahead to prefetch, when reading packets */
#define PREFETCH_OFFSET	  3

static __oryx_always_inline__
void dsa_tag_strip(struct rte_mbuf *m)
{
	void *new_start;
	
	/** hold the source and destination MAC address. */
	struct ether_hdr *ethh
		 = rte_pktmbuf_mtod(m, struct ether_hdr *);

	/** remove len bytes at the beginning of an mbuf to get a new start. */
	new_start = rte_pktmbuf_adj(m, sizeof(struct vlan_hdr));
	
	/* refill with 2 MAC address. */
	memmove(new_start, ethh, 2 * ETHER_ADDR_LEN);
}

static __oryx_always_inline__
void act_packet_trim(struct rte_mbuf *m, uint16_t slice_size)
{
	if (slice_size > m->pkt_len)
		return;
	/* packet will always be 64 bytes when drop_size = m->pkt_len - 64. */
	rte_pktmbuf_trim(m, m->pkt_len - slice_size);
}

static __oryx_always_inline__
void act_packet_timestamp(struct rte_mbuf *m)
{
	m  = m;
}

static __oryx_always_inline__
int dsa_tag_insert(struct rte_mbuf **m, uint32_t new_cpu_dsa)
{
	MarvellDSAEthernetHdr *nh;
	struct ether_hdr *oh;

	/* Can't insert header if mbuf is shared */
	if (rte_mbuf_refcnt_read(*m) > 1) {
		struct rte_mbuf *copy;

		copy = rte_pktmbuf_clone(*m, (*m)->pool);
		if (unlikely(copy == NULL))
			return -ENOMEM;
		rte_pktmbuf_free(*m);
		*m = copy;
	}

	oh = rte_pktmbuf_mtod(*m, struct ether_hdr *);
	nh = (MarvellDSAEthernetHdr *)
		rte_pktmbuf_prepend(*m, sizeof(MarvellDSAHdr));
	if (nh == NULL)
		return -ENOSPC;

	memmove(nh, oh, 2 * ETHER_ADDR_LEN);
	nh->dsah.dsa = rte_cpu_to_be_32(new_cpu_dsa);
	
	return 0;
}


static __oryx_always_inline__
uint32_t dsa_tag_update(uint32_t cpu_tag, uint8_t tx_virtual_port_id)
{
	uint32_t new_cpu_dsa = 0; /** this packet received from a panel sw virtual port ,
	                           * and also send to a panel sw virtual port, here just update the dsa tag. */

	SET_DSA_CMD(new_cpu_dsa, DSA_CMD_EGRESS); 		/** which port this packet should be send out. */
	SET_DSA_PORT(new_cpu_dsa, tx_virtual_port_id);  /** update dsa.port */
	SET_DSA_CFI(new_cpu_dsa, DSA_CFI(cpu_tag));		/** update dsa.C dsa.R1, dsa.R2 */
	SET_DSA_R1(new_cpu_dsa, DSA_R1(cpu_tag));
	SET_DSA_PRI(new_cpu_dsa, DSA_PRI(cpu_tag));
	SET_DSA_R0(new_cpu_dsa, DSA_R0(cpu_tag));
	SET_DSA_VLAN(new_cpu_dsa, DSA_VLAN(cpu_tag));
#if defined(BUILD_DEBUG)
	PrintDSA("Tx", new_cpu_dsa, QUA_TX);
#endif
	return new_cpu_dsa;

}

/* Send burst of packets on an output interface */
static __oryx_always_inline__
int send_burst(ThreadVars *tv, struct lcore_conf *qconf, uint16_t n, uint8_t tx_port_id)
{
	struct rte_mbuf **m_table;
	int ret;
	uint16_t tx_queue_id, n_tx_pkts;
	struct iface_t *tx_iface = NULL;
	vlib_port_main_t *pm = &vlib_port_main;

	/** port ID -> iface */
	iface_lookup_id(pm, tx_port_id, &tx_iface);

	tx_queue_id = qconf->tx_queue_id[tx_port_id];
	m_table = (struct rte_mbuf **)qconf->tx_mbufs[tx_port_id].m_table;

	n_tx_pkts = ret = rte_eth_tx_burst(tx_port_id, tx_queue_id, m_table, n);
	if (unlikely(ret < n)) {
		do {
			rte_pktmbuf_free(m_table[ret]);
		} while (++ ret < n);
	}

#if defined(BUILD_DEBUG)
	oryx_logd("tx_port_id=%d tx_queue_id %d, tx_lcore_id %d tx_pkts_num %d", tx_port_id, tx_queue_id, tv->lcore, n_tx_pkts);
#endif

	iface_counters_add(tx_iface, tx_iface->if_counter_ctx->lcore_counter_pkts[QUA_TX][qconf->lcore_id], n_tx_pkts);

	return 0;
}

/* Enqueue a single packet, and send burst if queue is filled */
static __oryx_always_inline__
int send_single_packet(ThreadVars *tv, DecodeThreadVars *dtv,
		struct iface_t *rx_iface, struct lcore_conf *qconf, 
		struct rte_mbuf *m, uint8_t tx_port_id, uint8_t tx_panel_port_id)
{
	uint16_t len;
	Packet *p;
	uint32_t cpu_dsa = 0;
	uint32_t tx_panel_ge_id = 0;
	struct iface_t *tx_panel_iface = NULL;
	vlib_port_main_t *pm = &vlib_port_main;
	
	p = GET_MBUF_PRIVATE(Packet, m);

	if (iface_id(rx_iface) == SW_CPU_XAUI_PORT_ID) {
		if(tx_panel_port_id > SW_PORT_OFFSET) {
			/* GE ---> GE */
#if defined(BUILD_DEBUG)
			BUG_ON(tx_port_id != SW_CPU_XAUI_PORT_ID);
#endif
			tx_panel_ge_id = tx_panel_port_id - SW_PORT_OFFSET;
			cpu_dsa = dsa_tag_update(p->dsa, PANEL_GE_ID_TO_DSA(tx_panel_ge_id));
			MarvellDSAEthernetHdr *dsaeth = rte_pktmbuf_mtod(m, MarvellDSAEthernetHdr *);
			dsaeth->dsah.dsa = rte_cpu_to_be_32(cpu_dsa);
			goto flush2_buffer;
		}

		/* GE ---> XE */
#if defined(BUILD_DEBUG)
		BUG_ON(tx_port_id == SW_CPU_XAUI_PORT_ID);
		oryx_logn ("strip dsa tag %08x", p->dsa);
#endif
		dsa_tag_strip(m);
		goto flush2_buffer;

	} else {
		if (tx_panel_port_id > SW_PORT_OFFSET) {
			/* XE ---> GE */
#if defined(BUILD_DEBUG)
			BUG_ON(tx_port_id != SW_CPU_XAUI_PORT_ID);
#endif
			tx_panel_ge_id = tx_panel_port_id - SW_PORT_OFFSET;
			/* add a dsa tag */
			cpu_dsa = dsa_tag_update(0, PANEL_GE_ID_TO_DSA(tx_panel_ge_id));
			dsa_tag_insert(&m, cpu_dsa);
#if defined(BUILD_DEBUG)
			oryx_logn ("insert dsa tag %08x", cpu_dsa);
#endif
			goto flush2_buffer;
		}
		/** XE -> XE */
#if defined(BUILD_DEBUG)
		BUG_ON(tx_port_id == SW_CPU_XAUI_PORT_ID);
#endif
	}

flush2_buffer:

	if (tx_panel_port_id > SW_PORT_OFFSET) {
		iface_lookup_id(pm, tx_panel_port_id, &tx_panel_iface);
		iface_counter_update(tx_panel_iface, 1, m->pkt_len, QUA_TX, tv->lcore);
	}
	
	len = qconf->tx_mbufs[tx_port_id].len;
	qconf->tx_mbufs[tx_port_id].m_table[len] = m;
	len++;
	
#if defined(BUILD_DEBUG)
	oryx_logn ("		buffering pkt (%p, pkt_len=%d @lcore=%d) for tx_port_id %d",
				m, m->pkt_len, tv->lcore, tx_port_id);
#endif

	/* enough pkts to be sent */
	if (likely(len == DPDK_MAX_TX_BURST)) {
		send_burst(tv, qconf, DPDK_MAX_TX_BURST, tx_port_id);
		len = 0;
	}

	qconf->tx_mbufs[tx_port_id].len = len;
	return 0;
}

static __oryx_always_inline__
void free_packet(ThreadVars *tv, DecodeThreadVars *dtv,
	struct rte_mbuf *pkt){
	rte_pktmbuf_free(pkt);
	oryx_counter_inc(&tv->perf_private_ctx0, dtv->counter_drop);
}


static __oryx_always_inline__
uint32_t ipv4_hash_crc(const void *data, __rte_unused uint32_t data_len,
		uint32_t init_val)
{
	const union ipv4_5tuple_host *k;
	uint32_t t;
	const uint32_t *p;

	k = data;
	t = k->proto;
	p = (const uint32_t *)&k->port_src;

	init_val = rte_hash_crc_4byte(t, init_val);
	init_val = rte_hash_crc_4byte(k->ip_src, init_val);
	init_val = rte_hash_crc_4byte(k->ip_dst, init_val);
	init_val = rte_hash_crc_4byte(*p, init_val);

	return init_val;
}

static __oryx_always_inline__
uint32_t recalc_rss(struct rte_mbuf *m)
{
	union ipv4_5tuple_host key;
	void *ipv4h;

	ipv4h = rte_pktmbuf_mtod_offset(m, struct ipv4_hdr *, sizeof(MarvellDSAEthernetHdr));
	ipv4h = (uint8_t *)ipv4h + offsetof(struct ipv4_hdr, time_to_live);
	/*
	 * Get 5 tuple: dst port, src port, dst IP address,
	 * src IP address and protocol.
	 */
	key.xmm = em_mask_key(ipv4h, mask0.x);

	return ipv4_hash_crc(&key, sizeof(key), 0);
}

static __oryx_always_inline__
int decide_tx_port(struct map_t *map, struct iface_t *rx_iface,
		struct rte_mbuf *m, uint8_t *tx_port_id)
{
	if(map->nb_online_tx_panel_ports == 0) {
		return UINT32_MAX;
	}
	
	uint32_t tx_panel_port_id = 0;
	uint32_t rss = iface_id(rx_iface) == SW_CPU_XAUI_PORT_ID ? recalc_rss(m) : m->hash.rss;
	uint32_t index = rss % map->nb_online_tx_panel_ports;

#if defined(BUILD_DEBUG)
	oryx_logn("%20s%8u", "m->hash.rss: ", m->hash.rss);
	oryx_logn("%20s%8u", "rss: ", rss);
	oryx_logn("%20s%8u", "index: ", index);
#endif

	*tx_port_id = tx_panel_port_id = map->online_tx_panel_ports[index];
	if(tx_panel_port_id != UINT32_MAX &&
		tx_panel_port_id > SW_PORT_OFFSET)
		*tx_port_id = SW_CPU_XAUI_PORT_ID;

	return tx_panel_port_id;
}

#define tx_panel_port_is_online(tx_panel_port_id)\
	((tx_panel_port_id) != UINT32_MAX)
	
static __oryx_always_inline__
void acl_send_one_packet(ThreadVars *tv, DecodeThreadVars *dtv, struct iface_t *rx_iface,
		struct lcore_conf *qconf, struct rte_mbuf *m, uint32_t res)
{
	uint32_t mapid, appid;
	uint8_t tx_port_id = iface_id(rx_iface);
	uint32_t tx_panel_port_id = 0;
	vlib_map_main_t *mm = &vlib_map_main;
	vlib_appl_main_t *am = &vlib_appl_main;
	struct appl_t *appl = NULL;	
	struct map_t *map = NULL;
	int i;

	if (likely(res == 0)) {
#if defined(BUILD_DEBUG)
		oryx_logn("acl lookup error");
#endif
		goto finish;
	}

	appid = __UNPACK_USERDATA(res);
	if(appid > MAX_APPLICATIONS) {
#if defined(BUILD_DEBUG)
		oryx_logn("no such application %d", appid);
#endif
		goto finish;
	}
	
	appl_entry_lookup_id(am, appid, &appl);
	if(appl_is_invalid(appl)) {
#if defined(BUILD_DEBUG)
		oryx_logn("no such application %d", appid);
#endif
		goto finish;
	}

	if(appl->nb_maps == 0) {
#if defined(BUILD_DEBUG)
		oryx_logn("application(\"%s\") is not mapped.", appl_alias(appl));
#endif
		goto finish;
	}

#if defined(BUILD_DEBUG)
	oryx_logn ("%18s%15s(id=%08d)", "hit_appl: ", appl_alias(appl), appid);
#endif

	for (i = 0; i < (int)appl->nb_maps; i ++) {
		
		struct appl_priv_t *priv = &appl->priv[i];
		mapid = priv->ul_map_id;

		/** lookup map table with userdata(map_mask) */
		map_entry_lookup_id(mm, mapid, &map);
		if (map_is_invalid(map)) {
			/* drop it defaultly if no map for this application */
#if defined(BUILD_DEBUG)
			oryx_logn("Can not find an valid map by id %d for iface %s", mapid, iface_alias(rx_iface));
#endif
			continue;
		}
		
#if defined(BUILD_DEBUG)
		oryx_logn(" 	###### Map[%s] %d", map_alias(map), mapid);
		oryx_logn("		 rx_iface (%s)_%d is in map ? %s", iface_alias(rx_iface), iface_id(rx_iface),
					map_rx_has_iface(map, rx_iface) ? "yes" : "no");
		oryx_logn("		 tx_online_ports_num %d", map->nb_online_tx_panel_ports);
#endif

		/* drop it defaulty if this rx_iface is not mapped */
		if(!map_rx_has_iface(map, rx_iface)) {
			continue;
		} else {
			tx_panel_port_id = decide_tx_port(map, rx_iface, m, &tx_port_id);
#if defined(BUILD_DEBUG)
			oryx_logn(" 	send to tx_panel_port_id %d, tx_port_id %d", tx_panel_port_id, tx_port_id);
#endif
			if(tx_panel_port_is_online(tx_panel_port_id)) {
				
				if(priv->ul_flags & ACT_SLICE) {
					act_packet_trim(m, 64);
				}
				
				if(priv->ul_flags & ACT_TIMESTAMP) {
					act_packet_timestamp(m);
				}
				
				/* forward packets for this map. */
				send_single_packet(tv, dtv, rx_iface, qconf, m, tx_port_id, tx_panel_port_id);
				continue;
			}
		}
	}
	
finish:
	free_packet(tv, dtv, m);
	return;
}

static __oryx_always_inline__
void acl_send_packets(ThreadVars *tv, DecodeThreadVars *dtv,
		struct iface_t *rx_iface, struct lcore_conf *qconf, struct rte_mbuf **m, uint32_t *res, int num)
{
	int i;

	/* Prefetch first packets */
	for (i = 0; i < PREFETCH_OFFSET && i < num; i++) {
		rte_prefetch0(rte_pktmbuf_mtod(
				m[i], void *));
	}

	for (i = 0; i < (num - PREFETCH_OFFSET); i++) {
		rte_prefetch0(rte_pktmbuf_mtod(m[
				i + PREFETCH_OFFSET], void *));
		acl_send_one_packet(tv, dtv, rx_iface, qconf, m[i], res[i]);
	}

	/* Process left packets */
	for (; i < num; i++)
		acl_send_one_packet(tv, dtv, rx_iface, qconf, m[i], res[i]);
}


static __oryx_always_inline__
void acl_drop_all(ThreadVars *tv, DecodeThreadVars *dtv,
	struct rte_mbuf **pkts_in, int nb){
	int i;
	for (i = 0; i < nb; i ++) {
		struct rte_mbuf *pkt = pkts_in[i];
		free_packet(tv, dtv, pkt);
	}
}

static __oryx_always_inline__
void dump_packet_info(struct rte_mbuf *pkt, uint32_t ipv4h_offset)
{
	union ipv4_5tuple_host key;
	void *ipv4h;
	
	ipv4h = rte_pktmbuf_mtod_offset(pkt, struct ipv4_hdr *, ipv4h_offset);
	ipv4h = (uint8_t *)ipv4h + offsetof(struct ipv4_hdr, time_to_live);
	/*
	 * Get 5 tuple: dst port, src port, dst IP address,
	 * src IP address and protocol.
	 */
	key.xmm = em_mask_key(ipv4h, mask0.x);
	char s[16];
	oryx_logn ("%16s%08d", "packet_type: ", pkt->packet_type);
	oryx_logn ("%16s%s", "ip_src: ",		inet_ntop(AF_INET, &key.ip_src, s, 16));
	oryx_logn ("%16s%s", "ip_dst: ",		inet_ntop(AF_INET, &key.ip_dst, s, 16));
	oryx_logn ("%16s%04d", "port_src: ",	ntoh16(key.port_src));
	oryx_logn ("%16s%04d", "port_dst: ",	ntoh16(key.port_dst));
	oryx_logn ("%16s%08d", "protocol: ",	key.proto);
	oryx_logn ("%16s%08u", "RSS: ", 		pkt->hash.rss);

}
static __oryx_always_inline__
void acl_prepare_one_packet(ThreadVars *tv, DecodeThreadVars *dtv,
	struct iface_t *rx_iface, struct rte_mbuf **pkts_in, struct acl_search_t *acl, int index)
{
	struct rte_mbuf *pkt = pkts_in[index];
	Packet *p;
	struct iface_t *rx_panel_iface = NULL;
	vlib_port_main_t *pm = &vlib_port_main;
	
	p = GET_MBUF_PRIVATE(Packet, pkt);
	p->iphd_offset = 0;
	p->dsa = 0;

#if defined(BUILD_DEBUG)
	oryx_logn("Rules [IPv4=%d, IPv6=%d]", g_runtime_acl_config->nb_ipv4_rules, g_runtime_acl_config->nb_ipv6_rules);
#endif

	if (iface_support_marvell_dsa(rx_iface)) {
			uint16_t ether_type;
			uint8_t rx_panel_port_id;
			MarvellDSAEthernetHdr *dsaeth = rte_pktmbuf_mtod(pkt, MarvellDSAEthernetHdr *);
			
			ether_type = dsaeth->eth_type;
			p->dsa = rte_be_to_cpu_32(dsaeth->dsah.dsa);
			rx_panel_port_id = DSA_TO_GLOBAL_PORT_ID(p->dsa);

#if defined(BUILD_DEBUG)
			oryx_logn("%20s%4d", "rx_panel_port_id: ",	rx_panel_port_id);
			oryx_logn("%20s%4x", "eth_type: ",			rte_be_to_cpu_16(ether_type));
			oryx_logn("%20s%4ld", "iphd_offset: ", OFF_DSAETHHEAD);
			PrintDSA("RX", p->dsa, QUA_RX);
			dump_pkt((uint8_t *)rte_pktmbuf_mtod(pkt, void *), pkt->pkt_len);
#endif

			if (rx_panel_port_id > SW_PORT_OFFSET) {
				iface_lookup_id(pm, rx_panel_port_id, &rx_panel_iface);
				iface_counter_update(rx_panel_iface, 1, pkt->pkt_len, QUA_RX, tv->lcore);
			}

			if (!DSA_IS_INGRESS(p->dsa)) {
				/* Unknown type, drop the packet */
				free_packet(tv, dtv, pkt);
				return;
			}
			
			if (ether_type == rte_cpu_to_be_16(ETHER_TYPE_IPv4)) {
				/* Fill acl structure */
				acl->data_ipv4[acl->num_ipv4] = DSA_MBUF_IPV4_2PROTO(pkt);
				acl->m_ipv4[(acl->num_ipv4)++] = pkt;

#if defined(BUILD_DEBUG)
				dump_packet_info(pkt, sizeof(MarvellDSAEthernetHdr));
#endif
				if (!g_runtime_acl_config->nb_ipv4_rules) {
					free_packet(tv, dtv, pkt);
				}

			} else if (ether_type == rte_cpu_to_be_16(ETHER_TYPE_IPv6)) {
				/* Fill acl structure */
				acl->data_ipv6[acl->num_ipv6] = DSA_MBUF_IPV6_2PROTO(pkt);
				acl->m_ipv6[(acl->num_ipv6)++] = pkt;
				if (!g_runtime_acl_config->nb_ipv6_rules) {
					free_packet(tv, dtv, pkt);
				}
			} else {
			/* Unknown type, drop the packet */
			free_packet(tv, dtv, pkt);
			return;
		}

	} else {

		if (RTE_ETH_IS_IPV4_HDR(pkt->packet_type)) {
			/* Fill acl structure */
			acl->data_ipv4[acl->num_ipv4] = MBUF_IPV4_2PROTO(pkt);
			acl->m_ipv4[(acl->num_ipv4)++] = pkt;
		
#if defined(BUILD_DEBUG)
			dump_packet_info(pkt, sizeof(struct ether_hdr));
#endif
			if (!g_runtime_acl_config->nb_ipv4_rules) {
				free_packet(tv, dtv, pkt);
			}
		} 
		else if (RTE_ETH_IS_IPV6_HDR(pkt->packet_type)) {
			/* Fill acl structure */
			acl->data_ipv6[acl->num_ipv6] = MBUF_IPV6_2PROTO(pkt);
			acl->m_ipv6[(acl->num_ipv6)++] = pkt;
			if (!g_runtime_acl_config->nb_ipv6_rules) {
				free_packet(tv, dtv, pkt);
			}
		}
		else {
			/* Unknown type, drop the packet */
			free_packet(tv, dtv, pkt);
		}
	}
}

static __oryx_always_inline__
void acl_prepare_acl_parameter(ThreadVars *tv, DecodeThreadVars *dtv,
	struct iface_t *rx_iface, struct rte_mbuf **pkts_in, struct acl_search_t *acl, int nb_rx)
{
	int i;

	acl->num_ipv4 = 0;
	acl->num_ipv6 = 0;

	/* Prefetch first packets */
	for (i = 0; i < PREFETCH_OFFSET && i < nb_rx; i++) {
		rte_prefetch0(rte_pktmbuf_mtod(
				pkts_in[i], void *));
	}

	for (i = 0; i < (nb_rx - PREFETCH_OFFSET); i++) {
		rte_prefetch0(rte_pktmbuf_mtod(pkts_in[
				i + PREFETCH_OFFSET], void *));
		acl_prepare_one_packet(tv, dtv, rx_iface, pkts_in, acl, i);
	}

	/* Process left packets */
	for (; i < nb_rx; i++)
		acl_prepare_one_packet(tv, dtv, rx_iface, pkts_in, acl, i);
}

static __oryx_always_inline__
void simple_forward(ThreadVars *tv, DecodeThreadVars *dtv, Packet *p, uint16_t pkt_len,
		struct rte_mbuf *m, struct iface_t *rx_cpu_iface, struct lcore_conf *qconf)
{
	uint8_t tx_port_id = -1, tx_panel_port_id = -1;
	uint8_t rx_port_id = -1, rx_panel_port_id = -1;
	uint32_t mapid;
	struct map_t *map = NULL;
	struct ipv4_hdr *ipv4h;
	uint8_t dst_port;
	uint32_t tcp_or_udp;
	uint32_t l3_ptypes;
	vlib_port_main_t *pm = &vlib_port_main;
	vlib_map_main_t *mm = &vlib_map_main;
	struct iface_t *tx_panel_iface = NULL;
	
#if defined(BUILD_DEBUG)
	BUG_ON(rx_cpu_iface == NULL);
#endif

	/** its an invalid packet. */
	if (p->flags & PKT_IS_INVALID) {
		rte_pktmbuf_free(m);
		return;
	}

	rx_port_id = GET_PKT_RX_PORT(p);
	rx_panel_port_id = GET_PKT_RX_PANEL_PORT(p);
	
	tcp_or_udp = m->packet_type & (RTE_PTYPE_L4_TCP | RTE_PTYPE_L4_UDP);
	l3_ptypes = m->packet_type & RTE_PTYPE_L3_MASK;

	/** which port this packet should be send out. */
	if (tcp_or_udp && (l3_ptypes == RTE_PTYPE_L3_IPV4)) {
		/* Handle IPv4 headers.*/
		ipv4h = rte_pktmbuf_mtod_offset(m, struct ipv4_hdr *,
						   sizeof(struct ether_hdr));
		/** exact match */
 		mapid = em_get_ipv4_dst_port(ipv4h);
		if (mapid != EM_HASH_ENTRIES) {
			oryx_logn("#### mapid %d ####", mapid);
			struct prefix_t lp_id = {
				.cmd = LOOKUP_ID,
				.v = (void *)&mapid,
				.s = sizeof(mapid),
			};
			map_table_entry_lookup (&lp_id, &map);
		} else {
			/** match fail */
		}
		
		/** lpm */
		//tx_port_id = lpm_get_ipv4_dst_port(ipv4h, rx_cpu_iface, qconf->ipv4_lookup_struct);
	}

	if (!map) {
		oryx_logn("no map for rx_panel_port %d", rx_panel_port_id);
		rte_pktmbuf_free(m);
		return;
	}
	if (!(map->rx_panel_port_mask & (1 << rx_panel_port_id))){
		oryx_logn("rx_panel_port_id %d is not in this map", rx_panel_port_id);
		rte_pktmbuf_free(m);
		return;
	}

	tx_panel_port_id = 1;	/** send to xe */
	tx_panel_port_id = 6;   /** send to ge */
	if (DSA_IS_INGRESS(p->dsa)) {
		MarvellDSAEthernetHdr *dsaeth = rte_pktmbuf_mtod(m, MarvellDSAEthernetHdr *);
		if (tx_panel_port_id > SW_PORT_OFFSET) /** SW -> SW */ {
			uint32_t new_cpu_dsa = dsa_tag_update(p->dsa, PANEL_GE_ID_TO_DSA(tx_panel_port_id));		
			iface_lookup_id(pm, tx_panel_port_id, &tx_panel_iface);
		#if defined(BUILD_DEBUG)
			BUG_ON(tx_panel_iface == NULL);
		#endif
			iface_counters_inc(tx_panel_iface, tx_panel_iface->if_counter_ctx->lcore_counter_pkts[QUA_TX][tv->lcore]);
			iface_counters_add(tx_panel_iface, tx_panel_iface->if_counter_ctx->lcore_counter_bytes[QUA_TX][tv->lcore], pkt_len);
			dsaeth->dsah.dsa = rte_cpu_to_be_32(new_cpu_dsa);
			tx_port_id = 0;	/** tx_port_id 0 is a cpu interface connected with SW unit. */
			oryx_logn("m->port %d", m->port);
		} else {
			/* SW -> XE */
			dsa_tag_strip(m);
			tx_port_id = tx_panel_port_id;
		}
	} else {
		if (tx_panel_port_id > SW_PORT_OFFSET) /* XE -> SW */ {
			uint32_t tx_panel_ge_id = tx_panel_port_id - SW_PORT_OFFSET;
			/* add a dsa tag */
			uint32_t new_cpu_dsa = dsa_tag_update(0, PANEL_GE_ID_TO_DSA(tx_panel_ge_id));
			dsa_tag_insert(&m, new_cpu_dsa);
			iface_lookup_id(pm, tx_panel_port_id, &tx_panel_iface);
		#if defined(BUILD_DEBUG)
			BUG_ON(tx_panel_iface == NULL);
		#endif
			iface_counters_inc(tx_panel_iface, tx_panel_iface->if_counter_ctx->lcore_counter_pkts[QUA_TX][tv->lcore]);
			iface_counters_add(tx_panel_iface, tx_panel_iface->if_counter_ctx->lcore_counter_bytes[QUA_TX][tv->lcore], pkt_len);
			tx_port_id = m->port; /** tx_port_id 0 is a cpu interface connected with SW unit. */
			PrintDSA("TX", new_cpu_dsa, QUA_TX);
			dump_pkt((uint8_t *)rte_pktmbuf_mtod(m, void *), m->pkt_len);
			
		} else {
			/* XE -> XE */
			tx_port_id = tx_panel_port_id;
		}
	}

//#if defined(BUILD_DEBUG)
	oryx_logn("%20s%4d", "tx_port_id: ", tx_port_id);
	oryx_logn("%20s%4d", "tx_panel_port_id: ", tx_panel_port_id);
	oryx_logn("%20s%4d%s", "Map: ", mapid, map?map_alias(map):"unknown");
//#endif

	send_single_packet(tv, dtv, rx_cpu_iface, qconf, m, tx_port_id, tx_panel_port_id);
}

static __oryx_always_inline__
void standard_eth_frame(ThreadVars *tv, DecodeThreadVars *dtv, Packet *p,
				  uint8_t *pkt, uint16_t pkt_len, PacketQueue *pq, struct rte_mbuf *m, struct iface_t *rx_cpu_iface)
{
	oryx_logd("Standard ethernet ...");
	struct ether_hdr *ethh;
	struct ipv4_hdr *ipv4h;
	struct ipv6_hdr *ipv6h;
	uint32_t packet_type = RTE_PTYPE_UNKNOWN;
	uint16_t ether_type;
	void *l3;
	int hdr_len;
	uint8_t rx_port_id = -1, rx_panel_port_id = -1;
	vlib_port_main_t *pm = &vlib_port_main;

	oryx_counter_add(&tv->perf_private_ctx0, dtv->counter_bytes, pkt_len);
	iface_counters_add(rx_cpu_iface, rx_cpu_iface->if_counter_ctx->lcore_counter_bytes[QUA_RX][tv->lcore], pkt_len);

	ethh = rte_pktmbuf_mtod(m, struct ether_hdr *);
	ether_type = ethh->ether_type;
	rx_panel_port_id = rx_port_id = GET_PKT_RX_PORT(p);
	SET_PKT_RX_PANEL_PORT(p, rx_panel_port_id);


//#if defined(BUILD_DEBUG)
	oryx_logn("%20s%4d", "m->port: ", m->port);
	oryx_logn("%20s%4d", "rx_port_id: ", rx_port_id);
	oryx_logn("%20s%4d", "rx_panel_port_id: ", rx_panel_port_id);
	oryx_logn("%20s%4x", "eth_type: ", rte_be_to_cpu_16(ether_type));
	dump_pkt((uint8_t *)rte_pktmbuf_mtod(m, void *), m->pkt_len);
//#endif

	l3 = (uint8_t *)ethh + sizeof(struct ether_hdr);
	if (ether_type == rte_cpu_to_be_16(ETHER_TYPE_IPv4)) {
		ipv4h = (struct ipv4_hdr *)l3;
		hdr_len = (ipv4h->version_ihl & IPV4_HDR_IHL_MASK) *
			  IPV4_IHL_MULTIPLIER;
		oryx_counter_inc(&tv->perf_private_ctx0, dtv->counter_ipv4);
		if (hdr_len == sizeof(struct ipv4_hdr)) {
			packet_type |= RTE_PTYPE_L3_IPV4;
			if (ipv4h->next_proto_id == IPPROTO_TCP){
				oryx_counter_inc(&tv->perf_private_ctx0, dtv->counter_tcp);
				packet_type |= RTE_PTYPE_L4_TCP;
			}
			else if (ipv4h->next_proto_id == IPPROTO_UDP){
				oryx_counter_inc(&tv->perf_private_ctx0, dtv->counter_udp);
				packet_type |= RTE_PTYPE_L4_UDP;
			}
			else if (ipv4h->next_proto_id == IPPROTO_SCTP){
				oryx_counter_inc(&tv->perf_private_ctx0, dtv->counter_sctp);
				packet_type |= RTE_PTYPE_L4_SCTP;
			}
		} else
			packet_type |= RTE_PTYPE_L3_IPV4_EXT;
	} else if (ether_type == rte_cpu_to_be_16(ETHER_TYPE_IPv6)) {
		ipv6h = (struct ipv6_hdr *)l3;
		if (ipv6h->proto == IPPROTO_TCP)
			packet_type |= RTE_PTYPE_L3_IPV6 | RTE_PTYPE_L4_TCP;
		else if (ipv6h->proto == IPPROTO_UDP)
			packet_type |= RTE_PTYPE_L3_IPV6 | RTE_PTYPE_L4_UDP;
		else if (ipv6h->proto == IPPROTO_SCTP)
			packet_type |= RTE_PTYPE_L3_IPV6 | RTE_PTYPE_L4_SCTP;
		else
			packet_type |= RTE_PTYPE_L3_IPV6_EXT_UNKNOWN;
	}
	m->packet_type = packet_type;

}
 
static __oryx_always_inline__
void marvell_dsa_frame(ThreadVars *tv, DecodeThreadVars *dtv, Packet *p,
				  uint8_t *pkt, uint16_t pkt_len, PacketQueue *pq, struct rte_mbuf *m, struct iface_t *rx_cpu_iface)
{
	oryx_logd("Marvell DSA ...");

	struct ether_hdr *ethh;
	struct ipv4_hdr *ipv4h;
	struct ipv6_hdr *ipv6h;
	uint32_t packet_type = RTE_PTYPE_UNKNOWN;
	uint16_t ether_type;
	void *l3;
	int hdr_len;
	vlib_port_main_t *pm = &vlib_port_main;
	MarvellDSAEthernetHdr *dsaeth;
	struct iface_t *rx_panel_iface = NULL;
	uint8_t rx_panel_port_id = -1;
	
	oryx_counter_add(&tv->perf_private_ctx0, dtv->counter_bytes, pkt_len);
	iface_counters_add(rx_cpu_iface, rx_cpu_iface->if_counter_ctx->lcore_counter_bytes[QUA_RX][tv->lcore], pkt_len);

	dsaeth = rte_pktmbuf_mtod(m, MarvellDSAEthernetHdr *);
	ether_type = dsaeth->eth_type;
	p->dsa = rte_be_to_cpu_32(dsaeth->dsah.dsa);
	rx_panel_port_id = DSA_TO_GLOBAL_PORT_ID(p->dsa);
	
//#if defined(BUILD_DEBUG)
	oryx_logn("%20s%4d", "mbuf->port: ", m->port);
	oryx_logn("%20s%4d", "rx_port_id: ", GET_PKT_RX_PORT(p));
	oryx_logn("%20s%4d", "rx_panel_port_id: ", rx_panel_port_id);
	oryx_logn("%20s%4x", "eth_type: ", rte_be_to_cpu_16(ether_type));
	PrintDSA("RX", p->dsa, QUA_RX);	
	dump_pkt((uint8_t *)rte_pktmbuf_mtod(m, void *), m->pkt_len);
//#endif

	if (!DSA_IS_INGRESS(p->dsa)) {
		SET_PKT_FLAGS(p, PKT_IS_INVALID | PKT_DSA_NOT_INGRESS);
		return;
	}

	SET_PKT_RX_PANEL_PORT(p, rx_panel_port_id);
	iface_lookup_id(pm, rx_panel_port_id, &rx_panel_iface);
#if defined(BUILD_DEBUG)
	BUG_ON(rx_panel_iface == NULL);
#endif
	iface_counters_inc(rx_panel_iface, rx_panel_iface->if_counter_ctx->lcore_counter_pkts[QUA_RX][tv->lcore]);
	iface_counters_add(rx_panel_iface, rx_panel_iface->if_counter_ctx->lcore_counter_bytes[QUA_RX][tv->lcore], pkt_len);

	l3 = (uint8_t *)dsaeth + sizeof(MarvellDSAEthernetHdr);
	if (ether_type == rte_cpu_to_be_16(ETHER_TYPE_IPv4)) {	
		ipv4h = (struct ipv4_hdr *)l3;
		hdr_len = (ipv4h->version_ihl & IPV4_HDR_IHL_MASK) *
			  IPV4_IHL_MULTIPLIER;
		oryx_counter_inc(&tv->perf_private_ctx0, dtv->counter_ipv4);
		if (hdr_len == sizeof(struct ipv4_hdr)) {
			packet_type |= RTE_PTYPE_L3_IPV4;
			if (ipv4h->next_proto_id == IPPROTO_TCP){
				oryx_counter_inc(&tv->perf_private_ctx0, dtv->counter_tcp);
				packet_type |= RTE_PTYPE_L4_TCP;
			}
			else if (ipv4h->next_proto_id == IPPROTO_UDP){
				oryx_counter_inc(&tv->perf_private_ctx0, dtv->counter_udp);
				packet_type |= RTE_PTYPE_L4_UDP;
			}
			else if (ipv4h->next_proto_id == IPPROTO_SCTP){
				oryx_counter_inc(&tv->perf_private_ctx0, dtv->counter_sctp);
				packet_type |= RTE_PTYPE_L4_SCTP;
			}
		} else
			packet_type |= RTE_PTYPE_L3_IPV4_EXT;
	} else if (ether_type == rte_cpu_to_be_16(ETHER_TYPE_IPv6)) {
		ipv6h = (struct ipv6_hdr *)l3;
		if (ipv6h->proto == IPPROTO_TCP)
			packet_type |= RTE_PTYPE_L3_IPV6 | RTE_PTYPE_L4_TCP;
		else if (ipv6h->proto == IPPROTO_UDP)
			packet_type |= RTE_PTYPE_L3_IPV6 | RTE_PTYPE_L4_UDP;
		else if (ipv6h->proto == IPPROTO_SCTP)
			packet_type |= RTE_PTYPE_L3_IPV6 | RTE_PTYPE_L4_SCTP;
		else
			packet_type |= RTE_PTYPE_L3_IPV6_EXT_UNKNOWN;
	} else if (ether_type == rte_cpu_to_be_16(ETHER_TYPE_ARP)) {
		oryx_counter_inc(&tv->perf_private_ctx0, dtv->counter_arp);
	}

	m->packet_type = packet_type;

}
				  

/* main processing loop */
int
main_loop0(__attribute__((unused)) void *ptr_data)
{
	vlib_main_t *vm = (vlib_main_t *)ptr_data;
	struct rte_mbuf *pkts_burst[DPDK_MAX_RX_BURST], *m;
	unsigned lcore_id;
	uint64_t prev_tsc, diff_tsc, cur_tsc, timer_tsc;
	int i, j, nb_rx;
	uint8_t rx_port_id, tx_port_id, rx_queue_id, tx_queue_id;
	struct lcore_conf *qconf;
	ThreadVars *tv;
	DecodeThreadVars *dtv;
	struct iface_t *rx_cpu_iface = NULL;
	vlib_port_main_t *pm = &vlib_port_main;
	int qua = QUA_RX;
	uint16_t pkt_len;
	uint8_t *pkt;
	Packet *p;
	struct ether_hdr *ethh;
	struct ipv4_hdr *ipv4h;
	struct ipv6_hdr *ipv6h;
	uint32_t packet_type = RTE_PTYPE_UNKNOWN;
	uint16_t ether_type;
	void *l3;
	int hdr_len;

	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) /
		US_PER_S * BURST_TX_DRAIN_US;
	
	prev_tsc = 0;

	lcore_id = rte_lcore_id();
	tv = &g_tv[lcore_id];
	dtv = &g_dtv[lcore_id];
	qconf = &lcore_conf[lcore_id];
	tv->lcore = lcore_id;

	if (qconf->n_rx_queue == 0) {
		oryx_logn("lcore %u has nothing to do\n", lcore_id);
		return 0;
	}

	while (!(vm->ul_flags & VLIB_DP_INITIALIZED)) {
		printf("lcore %d wait for dataplane ...\n", tv->lcore);
		sleep(1);
	}

	oryx_logn("entering main loop on lcore %u\n", lcore_id);

	for (i = 0; i < qconf->n_rx_queue; i++) {
		rx_port_id = qconf->rx_queue_list[i].port_id;
		rx_queue_id = qconf->rx_queue_list[i].queue_id;
		printf(" -- lcoreid=%u portid=%hhu rxqueueid=%hhu --\n",
			lcore_id, rx_port_id, rx_queue_id);
		fflush(stdout);
	}

	prev_tsc = 0;
	timer_tsc = 0;

	while (!force_quit) {

		cur_tsc = rte_rdtsc();

		/*
		 * TX burst queue drain
		 */
		diff_tsc = cur_tsc - prev_tsc;
		if (unlikely(diff_tsc > drain_tsc)) {

			for (i = 0; i < qconf->n_tx_port; ++i) {
				tx_port_id = qconf->tx_port_id[i];
				if (qconf->tx_mbufs[tx_port_id].len == 0)
					continue;
				send_burst(tv, qconf,
					qconf->tx_mbufs[tx_port_id].len,
					tx_port_id);
				qconf->tx_mbufs[tx_port_id].len = 0;
			}
			/* if timer is enabled */
			if (timer_period > 0) {

				/* advance the timer */
				timer_tsc += diff_tsc;

				/* if timer has reached its timeout */
				if (unlikely(timer_tsc >= timer_period)) {

					/* do this only on a sepcific core */
					if (lcore_id == rte_get_master_lcore()) {
						/* reset the timer */
						timer_tsc = 0;
						/** Master lcore is never called. */
					}
				}
			}

			prev_tsc = cur_tsc;
		}

		/*
		 * Read packet from RX queues
		 */
		for (i = 0; i < qconf->n_rx_queue; ++i) {
			rx_port_id = qconf->rx_queue_list[i].port_id;
			rx_queue_id = qconf->rx_queue_list[i].queue_id;
			nb_rx = rte_eth_rx_burst(rx_port_id, rx_queue_id, pkts_burst,
								DPDK_MAX_RX_BURST);
			if (nb_rx == 0)
				continue;

//#if defined(BUILD_DEBUG)
			oryx_logn("rx_port_id %d, rx_queue_id %d, rx_core_id %d",
				rx_port_id, rx_queue_id, tv->lcore);
//#endif			
			/** port ID -> iface */
			iface_lookup_id(pm, rx_port_id, &rx_cpu_iface);			
			iface_counters_add(rx_cpu_iface, rx_cpu_iface->if_counter_ctx->lcore_counter_pkts[QUA_RX][lcore_id], nb_rx);
			oryx_counter_add(&tv->perf_private_ctx0, dtv->counter_pkts, nb_rx);

			/** packet come from sw unit. */
			if(iface_support_marvell_dsa(rx_cpu_iface)) {
				oryx_counter_add(&tv->perf_private_ctx0, dtv->counter_dsa, nb_rx);
				for (j = 0; j < nb_rx; j++) {
					m = pkts_burst[j];
					rte_prefetch0(rte_pktmbuf_mtod(m, void *));	
					/** it costs a lot when calling Packet from mbuf. */
					p = GET_MBUF_PRIVATE(Packet, m);
					pkt = (uint8_t *)rte_pktmbuf_mtod(m, void *);
					pkt_len = m->pkt_len;
					SET_PKT_LEN(p, pkt_len);
					SET_PKT(p, pkt);
					SET_PKT_RX_PORT(p, rx_port_id);
					marvell_dsa_frame(tv, dtv, p, pkt, pkt_len, NULL, m, rx_cpu_iface);
					simple_forward(tv, dtv, p, pkt_len, m, rx_cpu_iface, qconf);
				}				
			} else {
				oryx_counter_add(&tv->perf_private_ctx0, dtv->counter_eth, nb_rx);
				for (j = 0; j < nb_rx; j++) {
					m = pkts_burst[j];
					rte_prefetch0(rte_pktmbuf_mtod(m, void *));
					/** it costs a lot when calling Packet from mbuf. */
					p = GET_MBUF_PRIVATE(Packet, m);
					pkt = (uint8_t *)rte_pktmbuf_mtod(m, void *);
					pkt_len = m->pkt_len;
					SET_PKT_LEN(p, pkt_len);
					SET_PKT(p, pkt);
					SET_PKT_RX_PORT(p, rx_port_id); 
					standard_eth_frame(tv, dtv, p, pkt, pkt_len, NULL, m, rx_cpu_iface);
					simple_forward(tv, dtv, p, pkt_len, m, rx_cpu_iface, qconf);
				}
			}
			//no_opt_send_packets(tv, nb_rx, pkts_burst, rx_cpu_iface, rx_sw_iface /**always null*/, qconf);
		}
	}

	return 0;
}

/* main processing loop */
int
main_loop(__attribute__((unused)) void *ptr_data)
{
	vlib_main_t *vm = (vlib_main_t *)ptr_data;
	struct rte_mbuf *pkts_burst[DPDK_MAX_RX_BURST], *m;
	unsigned lcore_id;
	uint64_t prev_tsc, diff_tsc, cur_tsc, timer_tsc;
	int i, j, nb_rx;
	uint8_t rx_port_id, tx_port_id, rx_queue_id, tx_queue_id;
	struct lcore_conf *qconf;
	ThreadVars *tv;
	DecodeThreadVars *dtv;
	struct iface_t *rx_cpu_iface = NULL;
	vlib_port_main_t *pm = &vlib_port_main;
	vlib_map_main_t *mm = &vlib_map_main;
	int qua = QUA_RX;
	uint16_t pkt_len;
	uint8_t *pkt;
	Packet *p;
	struct ether_hdr *ethh;
	struct ipv4_hdr *ipv4h;
	struct ipv6_hdr *ipv6h;
	uint32_t packet_type = RTE_PTYPE_UNKNOWN;
	uint16_t ether_type;
	void *l3;
	int hdr_len;
	int socketid;
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) /
		US_PER_S * BURST_TX_DRAIN_US;
	
	prev_tsc = 0;

	lcore_id = rte_lcore_id();
	socketid = rte_lcore_to_socket_id(lcore_id);
	tv = &g_tv[lcore_id];
	dtv = &g_dtv[lcore_id];
	qconf = &lcore_conf[lcore_id];
	tv->lcore = lcore_id;

	if (qconf->n_rx_queue == 0) {
		oryx_logn("lcore %u has nothing to do\n", lcore_id);
		return 0;
	}

	while (!(vm->ul_flags & VLIB_DP_INITIALIZED)) {
		printf("lcore %d wait for dataplane ...\n", tv->lcore);
		sleep(1);
	}

	oryx_logn("entering main loop on lcore %u\n", lcore_id);

	for (i = 0; i < qconf->n_rx_queue; i++) {
		rx_port_id = qconf->rx_queue_list[i].port_id;
		rx_queue_id = qconf->rx_queue_list[i].queue_id;
		printf(" -- lcoreid=%u portid=%hhu rxqueueid=%hhu --\n",
			lcore_id, rx_port_id, rx_queue_id);
		fflush(stdout);
	}

	prev_tsc = 0;
	timer_tsc = 0;

	while (!force_quit) {

		cur_tsc = rte_rdtsc();

		/*
		 * TX burst queue drain
		 */
		diff_tsc = cur_tsc - prev_tsc;
		if (unlikely(diff_tsc > drain_tsc)) {

			for (i = 0; i < qconf->n_tx_port; ++i) {
				tx_port_id = qconf->tx_port_id[i];
				if (qconf->tx_mbufs[tx_port_id].len == 0)
					continue;
				send_burst(tv, qconf,
					qconf->tx_mbufs[tx_port_id].len,
					tx_port_id);
				qconf->tx_mbufs[tx_port_id].len = 0;
			}
			/* if timer is enabled */
			if (timer_period > 0) {

				/* advance the timer */
				timer_tsc += diff_tsc;

				/* if timer has reached its timeout */
				if (unlikely(timer_tsc >= timer_period)) {

					/* do this only on a sepcific core */
					if (lcore_id == rte_get_master_lcore()) {
						/* reset the timer */
						timer_tsc = 0;
						/** Master lcore is never called. */
					}
				}
			}

			prev_tsc = cur_tsc;
		}

		while(vm->ul_flags & VLIB_DP_SYNC) {
			if(!(vm->ul_core_mask & (1 << tv->lcore)))
				vm->ul_core_mask |= (1 << tv->lcore);
		}
		
		if(vm->ul_core_mask & (1 << tv->lcore)) {
			g_runtime_acl_config = &acl_config[g_runtime_acl_config_qua % ACL_TABLES];
			vm->ul_core_mask &= ~(1 << tv->lcore);
		}
		
		/*
		 * Read packet from RX queues
		 */
		for (i = 0; i < qconf->n_rx_queue; ++i) {
			rx_port_id = qconf->rx_queue_list[i].port_id;
			rx_queue_id = qconf->rx_queue_list[i].queue_id;
			nb_rx = rte_eth_rx_burst(rx_port_id, rx_queue_id, pkts_burst,
								DPDK_MAX_RX_BURST);
			if (nb_rx == 0)
				continue;

			/** port ID -> iface */
			iface_lookup_id(pm, rx_port_id, &rx_cpu_iface);
			iface_counter_update(rx_cpu_iface, nb_rx, 0, QUA_RX, tv->lcore);

			//iface_counters_add(rx_cpu_iface, rx_cpu_iface->if_counter_ctx->lcore_counter_pkts[QUA_RX][lcore_id], nb_rx);

			oryx_counter_add(&tv->perf_private_ctx0, dtv->counter_pkts, nb_rx);
			oryx_counter_add(&tv->perf_private_ctx0, dtv->counter_eth, nb_rx);

#if defined(BUILD_DEBUG)
			oryx_logn("rx_port_id %d, rx_queue_id %d, rx_core_id %d, rx_cpu_iface %p, iface_id %d",
							rx_port_id, rx_queue_id, tv->lcore,
							rx_cpu_iface, iface_id(rx_cpu_iface));
#endif

			struct acl_search_t acl_search;

			acl_prepare_acl_parameter(tv, dtv, rx_cpu_iface, pkts_burst, &acl_search,
					nb_rx);
			
			if (g_runtime_acl_config->nb_ipv4_rules &&
				acl_search.num_ipv4) {
				{
					rte_acl_classify(
						g_runtime_acl_config->acx_ipv4[socketid],
						acl_search.data_ipv4,
						acl_search.res_ipv4,
						acl_search.num_ipv4,
						DEFAULT_MAX_CATEGORIES);

					acl_send_packets(tv, dtv, rx_cpu_iface,
						qconf,
						acl_search.m_ipv4,
						acl_search.res_ipv4,
						acl_search.num_ipv4);
				}
			}

			if (g_runtime_acl_config->nb_ipv6_rules &&
				acl_search.num_ipv6) {
				{
					rte_acl_classify(
						g_runtime_acl_config->acx_ipv6[socketid],
						acl_search.data_ipv6,
						acl_search.res_ipv6,
						acl_search.num_ipv6,
						DEFAULT_MAX_CATEGORIES);
					
					acl_send_packets(tv, dtv, rx_cpu_iface,
						qconf,
						acl_search.m_ipv6,
						acl_search.res_ipv6,
						acl_search.num_ipv6);
				}
			}
			
			oryx_counter_add(&tv->perf_private_ctx0, dtv->counter_ipv4, acl_search.num_ipv4);
			oryx_counter_add(&tv->perf_private_ctx0, dtv->counter_ipv6, acl_search.num_ipv6);

		}
	}

	return 0;
}

