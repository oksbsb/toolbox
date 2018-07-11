#include "oryx.h"
#include "geo_cdr_table.h"
#include "geo_capture.h"
#include "geo_htable.h"
#include "geo_cdr_mpool.h"
#include "geo_cdr_queue.h"
#include "geo_cdr_persistence.h"

volatile bool force_quit = false;

struct geo_stat_t geo_stat;
GEODecodeThreadVars gdtv[cdr_num];
GEOThreadVars		gtv[cdr_num];
void *cdr_pool;
struct qctx_t		*cdr_refill_queue;
struct qctx_t		*cdr_raw_data_queue;

static __oryx_always_inline__
void geo_register_perf_counters(GEODecodeThreadVars *dtv, GEOThreadVars *tv, int cdr_index)
{	
	tv = tv;
	
	dtv->cdr_index = cdr_index;

	/* register counters */
	dtv->counter_pkts				= 0;
	dtv->counter_bytes				= 0;
	dtv->counter_invalid			= 0;
	dtv->counter_no_mtmsi			= 0;
	dtv->counter_mtmsi				= 0;
	dtv->counter_mme_ue_s1ap_id		= 0;
	dtv->counter_no_mme_ue_s1ap_id	= 0;
	
    return;
}

static __oryx_always_inline__
int geo_decode(const struct pcap_pkthdr *hdr, char *raw_pkt , int *is_udp_pkt , int *pl_off , int *pl_len) 
{
	struct ethhdr_t		*ethh;
	struct ipv4hdr_t	*iph;
	struct udphdr_t		*udph;
	int 				len;
	uint32_t			p_offset = 0;
	uint32_t 			hlen;
	uint32_t			version;
	int					length = hdr->len;

	iph = NULL;
	
	ethh = (struct ethhdr_t *)(void *)&raw_pkt[0];
	p_offset += sizeof(struct ethhdr_t);

	/*
 	*  Figure out which layer 2 protocol the frame belongs to and call
 	*  the corresponding decoding module.  The protocol field of an 
 	*  Ethernet II header is the 13th + 14th byte.  This is an endian
 	*  independent way of extracting a big endian short from memory.  We
 	*  extract the first byte and make it the big byte and then extract
 	*  the next byte and make it the small byte.
 	*/
	switch (ntoh16(ethh->eth_type)) 
	{
		case 0x0800:	/* IPv4 */
			iph = (struct ipv4hdr_t  *)&raw_pkt[p_offset];
			break;
		case 0x8100:
			iph = (struct ipv4hdr_t  *)&raw_pkt[p_offset + 4];
			p_offset += 4;
			break;
		default:
			/* We're not bothering with 802.3 or anything else */
			return -1;
	}

	p_offset += sizeof(struct ipv4hdr_t);
	
    len     = ntohs(IPv4_GET_RAW_IPLEN(iph));
    hlen    = IPv4_GET_RAW_HLEN(iph); /* header length */
    version = IPv4_GET_RAW_VER(iph);/* ip version */
/*
	oryx_logn("plen %zd, iplen %zd, hlen %zd, ipv %zd, proto %zd",
		length, len, hlen, version, IPv4_GET_RAW_IPPROTO(iph));
*/	
    /* check version */
    if(version != 4)
	{
		fprintf(stdout,"Unknown version %d\n",version);
		return -1;
    }

    /* check header length */
    if(hlen < 5 ) 
	{
		fprintf(stdout, "bad-hlen %d \n",hlen);
		return -1;
    }

    /* see if we have as much packet as we should */
    if(length < len) 
	{
        fprintf(stdout,"\ntruncated IP - %d bytes missing(max caplen %d) \n",len - length , length);
		return -1;
	}
	
	if(IPv4_GET_RAW_IPPROTO(iph) == 17) {
		{
			udph = (struct udphdr_t *)((char *)iph + (IPv4_GET_RAW_HLEN(iph) << 0x02));
			p_offset += sizeof(struct udphdr_t);
			(*is_udp_pkt)	= 1;
			(*pl_len)		= (UDP_GET_RAW_LEN(udph) - 8);	/** without udphdr */
			(*pl_off)		= p_offset;
		}
	}

	return 0;
}

static __oryx_always_inline__
void init_hash_key (struct geo_htable_key_t *h, struct geo_key_info_t *gk){
		/* This MUST need */
		h->mme_code =	gk->mme_code;

#if (GEO_CDR_HASH_MODE == GEO_CDR_HASH_MODE_M_TMSI)
		h->v	=	gk->m_tmsi;
#else
		h->v	=	gk->mme_ue_s1ap_id;
#endif
}

static __oryx_always_inline__
int geo_refill_prepare(struct geo_cdr_table_t *gct,
			struct geo_cdr_entry_t *gce, struct geo_key_info_t *gk, GEODecodeThreadVars *dtv, GEOThreadVars *tv)
{
	uint32_t hk_size = sizeof(struct geo_htable_key_t);
	struct geo_htable_key_t hk = GEO_CDR_HASH_KEY_INIT_VAL, *h;

	tv = tv;
	gct = gct;
	dtv = dtv;
	
	init_hash_key(&hk, gk);
	h = (struct geo_htable_key_t *)oryx_htable_lookup(geo_cdr_hash_table, &hk, hk_size);		
	if (unlikely (!h)) {
		/** if no such hk, allocate a new one and insert to htable. */
		h = alloc_hk();
		init_hash_key(h, gk);
		BUG_ON(oryx_htable_add(geo_cdr_hash_table, h, hk_size) != 0);
	}

	BUG_ON(h == NULL);
	BUG_ON(h->inner_cdr_queue == NULL);
	BUG_ON(h->mme_code != gk->mme_code);
#if (GEO_CDR_HASH_MODE == GEO_CDR_HASH_MODE_M_TMSI)
	BUG_ON(h->v	!= gk->m_tmsi);
#else
	BUG_ON(h->v	!= gk->mme_ue_s1ap_id);
#endif

	/** hold the hash entry. */
	gce->hash_entry = h;
	
	/*
	 * Store IMSI for refill prepare if there is such information.
	 */
	if (gk->ul_flags & GEO_CDR_KEY_INFO_APPEAR_IMSI) {
		if (!(h->ul_flags & GEO_CDR_HASH_KEY_APPEAR_IMSI)) {
			strncpy((char *)&h->imsi[0], &gk->imsi[0], 18);
			h->ul_flags |= GEO_CDR_HASH_KEY_APPEAR_IMSI;		
		}
	}
	
	return 0;
}

static __oryx_always_inline__
void geo_update_stats(struct geo_key_info_t *gk,
	GEODecodeThreadVars *dtv, GEOThreadVars *tv)
{
	tv = tv;
	
	dtv->counter_pkts			+= 1;

	if (gk->ul_flags & GEO_CDR_KEY_INFO_APPEAR_IMSI)
		dtv->counter_has_imsi ++;
	else
		dtv->counter_no_imsi ++;

	if(gk->ul_flags & GEO_CDR_KEY_INFO_APPEAR_MTMSI) {
		dtv->counter_mtmsi ++;
		if(gk->ul_flags & GEO_CDR_KEY_INFO_APPEAR_IMSI)
			dtv->counter_has_imsi_mtmsi ++;
	} else {
		dtv->counter_no_mtmsi ++;
	}
	
	if(gk->ul_flags & GEO_CDR_KEY_INFO_APPEAR_S1APID) {
		dtv->counter_mme_ue_s1ap_id ++;
		if(gk->ul_flags & GEO_CDR_KEY_INFO_APPEAR_IMSI)
			dtv->counter_has_imsi_mme_ue_s1apid ++;
	} else {
		dtv->counter_no_mme_ue_s1ap_id ++;	
	}

	if(gk->ul_flags & GEO_CDR_KEY_INFO_BYPASS) {
		dtv->counter_bypassed ++;
		/* there is no enough info for refill, so we bypass it ASAP. */
		geo_stat.rx_bypass_cdr_pkts ++;
	}
}


static __oryx_always_inline__
void geo_pkt_handler(u_char __oryx_unused_param__ *argv,
		const struct pcap_pkthdr *pcaphdr,
		u_char *packet)
{
	int			is_udp = 0;
	int			pl = 0;
	int			pl_off = 0;
	int			cdr_index = 0;
	full_record_cdr_head_t * cdr_head;
	char *cdr_start = NULL;
	struct geo_key_info_t gk = GEO_CDR_KEY_INFO_INIT_VAL;
	struct geo_cdr_entry_t *gce;
	GEODecodeThreadVars *dtv;
	GEOThreadVars *tv;
	int i;

	geo_decode(pcaphdr, (char *)packet, &is_udp, &pl_off, &pl);
	if (!is_udp)
		return;
	
	geo_stat.rx_pkts ++;

	cdr_start = (char *)&packet[pl_off];
	GetCDRIndex(cdr_start, &cdr_index);
	cdr_head = (full_record_cdr_head_t *)cdr_start;
	if((pl != CDR_LEN(cdr_head)) ||
		(cdr_index >= cdr_num || cdr_index == cdr_source)) {
		oryx_logn("plen %u, pl_off %u(%zd), pl_len %u, cdr_len %u",
						pcaphdr->len,
						pl_off,
						(sizeof(struct ethhdr_t) + sizeof(struct ipv4hdr_t) + sizeof(struct udphdr_t)),
						pl, CDR_LEN(cdr_head));
		geo_stat.rx_invalid_cdr_pkts ++;
		return;
	}
	
	geo_stat.rx_cdr_pkts ++;

	/* */
	if(cdr_index != cdr_s1_emm_signal &&
			cdr_index != cdr_s1ap_handover_signal &&
			cdr_index != cdr_s1_mme_signal)	{			
		geo_stat.rx_bypass_cdr_pkts ++;
		/** we do not care about other signals. */
		return;
	}
	geo_stat.rx_valid_cdr_pkts ++;

	dtv		= &gdtv[cdr_index];
	tv		= &gtv[cdr_index];

	geo_get_key_info(cdr_start, &gk, cdr_index);
	
	geo_update_stats(&gk, dtv, tv);
	
	/* Prepare for those CDRs which will not be bypassed. */
	if (gk.ul_flags & GEO_CDR_KEY_INFO_BYPASS) {
		geo_key_info_dump(&gk, stdout);
		return;
	}

#if 1
	/* New a cdr entry */
	/* Then this new packet to queue. */
	gce = mpool_alloc(cdr_pool);
	BUG_ON(gce == NULL);

	/* hold cdr data by gce which allocated from cdr_pool. */
	memcpy(gce->data, cdr_start, pl);
	gce->data_size = pl;
	geo_key_info_init(&gce->gki, &gk);	

	fq_equeue(cdr_raw_data_queue, gce);
#endif
}

static struct netdev_t geo_netdev = {
	.handler = NULL,
	.devname = "ens33",
	.dispatch = geo_pkt_handler,
	.private = NULL,
};

void *geo_libpcap_running_fn(void *argv)
{
	int64_t rank_acc;
	struct netdev_t *netdev = (struct netdev_t *)argv;
	
	atomic64_set(&netdev->rank, 0);

	netdev_open(netdev);
	
	FOREVER {
		if (force_quit)
			break;
		
		if (!netdev->handler)
			continue;
		
		rank_acc = pcap_dispatch(netdev->handler,
			1024, 
			netdev->dispatch, 
			(u_char *)netdev);

		if (rank_acc >= 0) {
			atomic64_add (&netdev->rank, rank_acc);
		} else {
			pcap_close (netdev->handler);
			netdev->handler = NULL;
			switch (rank_acc) {
				case -1:
				case -2:
				case -3:
				default:
					printf("pcap_dispatch=%ld\n", rank_acc);
					break;
			}
		}
	}

	oryx_task_deregistry_id(pthread_self());
	return NULL;
}

void *geo_refill_prepare_fn (void *argv)
{
	GEODecodeThreadVars *dtv;
	GEOThreadVars *tv;
	struct geo_cdr_table_t *gct;
	struct geo_cdr_entry_t *gce;
	struct geo_key_info_t *gk;
	
	FOREVER {
		if (!(gce = fq_dequeue(cdr_raw_data_queue)))
			continue;

		gk = &gce->gki;
		
#if 0
		gct 	= &geo_cdr_tables[gk->cdr_index];
		dtv		= &gdtv[gk->cdr_index];
		tv		= &gtv[gk->cdr_index];

		geo_refill_prepare(gct, gce, gk, dtv, tv);	
		fq_equeue(cdr_refill_queue, gce);
#else
		geo_key_info_dump(gk, geo_cdr_tables[gk->cdr_index]->lf.fp);
#endif

	}

	oryx_task_deregistry_id(pthread_self());
	return NULL;
}


static __oryx_always_inline__
int do_refill(struct   geo_htable_key_t *hk , struct geo_cdr_entry_t *gce, int cdr_index,
		GEODecodeThreadVars *dtv, GEOThreadVars *tv)
{
	uint8_t		mme_code = -1;
	uint32_t	hkv = -1;
	char		*imsi;

	switch(cdr_index)
	{
		case cdr_s1_emm_signal:
			{
				full_record_s1_emm_signal_t *v = (full_record_s1_emm_signal_t *)gce->data;
				mme_code	= v->mme_code;
#if (GEO_CDR_HASH_MODE == GEO_CDR_HASH_MODE_M_TMSI)
				hkv			= v->m_tmsi;
#else
				hkv			= v->mme_ue_s1ap_id;
#endif
				imsi		= (char *)&v->imsi[0];
				break;
			}
		case cdr_s1ap_handover_signal:
			{
				full_record_s1ap_handover_signal_t *v = (full_record_s1ap_handover_signal_t *)gce->data;
				mme_code	= v->mme_code;
#if (GEO_CDR_HASH_MODE == GEO_CDR_HASH_MODE_M_TMSI)
				hkv			= v->m_tmsi;
#else
				hkv			= v->mme_ue_s1apid;
#endif
				imsi		= (char *)&v->imsi[0];

				break;
			}
		case cdr_s1_mme_signal:
			{
				full_record_s1_mme_signal_t * v = (full_record_s1_mme_signal_t *)gce->data;
				mme_code	= v->mme_code;
#if (GEO_CDR_HASH_MODE == GEO_CDR_HASH_MODE_M_TMSI)
				hkv			= v->m_tmsi;
#else
				hkv			= v->mme_ue_s1ap_id;
#endif
				imsi		= (char *)&v->imsi[0];

				break;
			}
		default:
			return -1;
	}

	if((mme_code != hk->mme_code) || (hkv != hk->v)) {
		fprintf(cdr_record_error_fp.fp, "mme_code %d =? %d,  hkv %d =? %d\n", mme_code, hk->mme_code, hkv, hk->v);
		return -1;
	}

	if (imsi[0] == 0) {
		/*ReFill */
		//dump_imsi("Before", imsi);
		strncpy(imsi, (char *)&hk->imsi[0], 18);
		//dump_imsi("After", imsi);
		geo_stat.total_actually_refill_pkts ++;
		dtv->counter_refilled_deta ++;

		struct geo_key_info_t *gk = &gce->gki;
		log_refill_result(hk, cdr_index, cdr_record_refill_result_fp.fp);
	}

	geo_stat.total_refill_pkts ++;
	dtv->counter_total_refill ++;
	
	return 0;
}

void *geo_refill_fn (void *argv)
{
	GEODecodeThreadVars *dtv;
	GEOThreadVars *tv;
	struct geo_cdr_table_t *gct;
	struct geo_htable_key_t *h;
	struct geo_cdr_entry_t *gce;
	struct geo_key_info_t *gk;
	
	FOREVER {
		if(!(gce = fq_dequeue(cdr_refill_queue)))
			continue;

		gk	= &gce->gki;
		dtv	= &gdtv [gk->cdr_index];
		tv	= &gtv  [gk->cdr_index];
		h	= gce->hash_entry;

		BUG_ON(h == NULL);
		BUG_ON(h->inner_cdr_queue == NULL);

#if defined(GEO_CDR_HAVE_REFILL_CACHE)
		struct geo_cdr_entry_t *gce0;
		if(h->ul_flags & GEO_CDR_HASH_KEY_APPEAR_IMSI) {
			if(fq_length((struct qctx_t *)h->inner_cdr_queue) == 0) {
				do_refill(h, gce, gk->cdr_index, dtv, tv);
				mpool_free(cdr_pool, gce);
			} else {
				while(fq_length((struct qctx_t *)h->inner_cdr_queue) != 0) {
					gce0 = fq_dequeue((struct qctx_t *)h->inner_cdr_queue);
					do_refill(h, gce0, gk->cdr_index, dtv, tv);
					mpool_free(cdr_pool, gce0);
				}
			}
		}  else {
			/* equeue this CDR entry to local queue which hold by HASH entry.
			 * THIS queue is created at stage of refill_prepare,
			 * when a new hash entry is created. */
			fq_equeue((struct qctx_t *)h->inner_cdr_queue, gce);
		}
#else
		if(h->ul_flags & GEO_CDR_HASH_KEY_APPEAR_IMSI)
			do_refill(h, gce, gk->cdr_index, dtv, tv);
		mpool_free(cdr_pool, gce);
#endif
	}
	
	oryx_task_deregistry_id(pthread_self());
	return NULL;
}

static struct oryx_task_t geo_rx_task =
{
	.module = THIS,
	.sc_alias = "Netdev Task",
	.fn_handler = geo_libpcap_running_fn,
	.ul_lcore = INVALID_CORE,
	.ul_prio = KERNEL_SCHED,
	.argc = 1,
	.argv = &geo_netdev,
	.ul_flags = 0,	/** Can not be recyclable. */
};

static struct oryx_task_t geo_refill_prepare_task =
{
	.module = THIS,
	.sc_alias = "CDR Refill Prepare",
	.fn_handler = geo_refill_prepare_fn,
	.ul_lcore = INVALID_CORE,
	.ul_prio = KERNEL_SCHED,
	.argc = 0,
	.argv = NULL,
	.ul_flags = 0,	/** Can not be recyclable. */
};

static struct oryx_task_t geo_refill_task =
{
	.module = THIS,
	.sc_alias = "CDR Refill",
	.fn_handler = geo_refill_fn,
	.ul_lcore = INVALID_CORE,
	.ul_prio = KERNEL_SCHED,
	.argc = 0,
	.argv = NULL,
	.ul_flags = 0,	/** Can not be recyclable. */
};

static __oryx_always_inline__ 
void geo_pcap_perf_tmr_handler(struct oryx_timer_t *tmr, int __oryx_unused_param__ argc, 
                char **argv)
{
	int i;
	uint64_t rx_pkts;
	uint64_t rx_cdr_pkts;
	uint64_t rx_signal_cdr_pkts;
	uint64_t rx_valid_cdr_pkts;
	uint64_t rx_bypass_cdr_pkts;
	uint64_t rx_cdr_mtmsi = 0;			
	uint64_t rx_cdr_no_mtmsi = 0;
	uint64_t rx_cdr_mme_ue_s1ap_id = 0;
	uint64_t rx_cdr_no_mme_ue_s1ap_id = 0;
	uint64_t rx_cdr_imsi_mme_ue_s1ap_id = 0;
	uint64_t rx_cdr_imsi_mtmis = 0;
	uint64_t rx_cdr_has_imsi_pkts = 0;
	uint64_t rx_cdr_no_imsi_pkts = 0;
	uint64_t rx_cdr_total_refill_pkts = 0;
	uint64_t rx_cdr_refilled_deta = 0;

	char file_clear_cmd[64] = "cat /dev/null > ";
	GEODecodeThreadVars		*dtv;
	GEOThreadVars			*tv;
	struct geo_cdr_table_t	*gct;

	rx_pkts				= geo_stat.rx_pkts;
	rx_cdr_pkts			= geo_stat.rx_cdr_pkts;
	rx_bypass_cdr_pkts	= geo_stat.rx_bypass_cdr_pkts;
	rx_valid_cdr_pkts	= geo_stat.rx_valid_cdr_pkts;
	
#if 1

	strcat (file_clear_cmd, cdr_stats_out_fp.fp_path);
	system(file_clear_cmd);
	fseek(cdr_stats_out_fp.fp, 0, SEEK_SET);
	
	fprintf(cdr_stats_out_fp.fp, "Rx valid CDR %zd/%zd(%zd), bypassed %zd, refilled %zd\n\n",
		rx_valid_cdr_pkts, rx_cdr_pkts, rx_pkts, rx_bypass_cdr_pkts, geo_stat.total_actually_refill_pkts);
	
	for (i = 0; i < cdr_num; i ++) {

		dtv	= &gdtv[i];
		tv	= &gtv[i];
		gct	= geo_cdr_tables[i];
		
		if(i == cdr_s1_mme_signal ||
			i == cdr_s1_emm_signal ||
			i == cdr_s1ap_handover_signal) {

			rx_signal_cdr_pkts	= dtv->counter_pkts;

			rx_cdr_no_mtmsi		= dtv->counter_no_mtmsi;
			rx_cdr_mtmsi		= dtv->counter_mtmsi;
			rx_cdr_no_mme_ue_s1ap_id		= dtv->counter_no_mme_ue_s1ap_id;
			rx_cdr_mme_ue_s1ap_id			= dtv->counter_mme_ue_s1ap_id;
			rx_cdr_has_imsi_pkts			= dtv->counter_has_imsi;
			rx_cdr_no_imsi_pkts				= dtv->counter_no_imsi;
			rx_cdr_total_refill_pkts		= dtv->counter_total_refill;
			rx_cdr_refilled_deta			= dtv->counter_refilled_deta;
			rx_cdr_imsi_mme_ue_s1ap_id		= dtv->counter_has_imsi_mme_ue_s1apid;
			rx_cdr_imsi_mtmis				= dtv->counter_has_imsi_mtmsi;
			
			if(rx_signal_cdr_pkts) {
				
				fprintf(cdr_stats_out_fp.fp, "%32s\n", gct->cdr_name);
				fprintf(cdr_stats_out_fp.fp, "%32s%32s%16lu%16.2f%%\n", " ", "rx_pkts: ", rx_signal_cdr_pkts, ratio_of(rx_signal_cdr_pkts, rx_valid_cdr_pkts));
				fprintf(cdr_stats_out_fp.fp, "%32s%32s%16lu%16.2f%%\n", " ", "rx_mtmis: ", rx_cdr_mtmsi, ratio_of(rx_cdr_mtmsi, rx_signal_cdr_pkts));
				fprintf(cdr_stats_out_fp.fp, "%32s%32s%16lu%16.2f%%\n", " ", "rx_mme_ue_s1ap_id: ", rx_cdr_mme_ue_s1ap_id, ratio_of(rx_cdr_mme_ue_s1ap_id, rx_signal_cdr_pkts));
				fprintf(cdr_stats_out_fp.fp, "%32s%32s%16lu%16.2f%\n",  " ", "rx_has_imsi: ", rx_cdr_has_imsi_pkts, ratio_of(rx_cdr_has_imsi_pkts, rx_signal_cdr_pkts));
				fprintf(cdr_stats_out_fp.fp, "%32s%32s%16lu%16.2f%%\n", " ", "rx_no_imsi: ",  rx_cdr_no_imsi_pkts, ratio_of(rx_cdr_no_imsi_pkts, rx_signal_cdr_pkts));
				fprintf(cdr_stats_out_fp.fp, "%32s%32s%16lu%16.2f%%\n", " ", "rx_imsi_and_mtmsi: ", rx_cdr_imsi_mtmis, ratio_of(rx_cdr_imsi_mtmis, rx_cdr_mtmsi));
				fprintf(cdr_stats_out_fp.fp, "%32s%32s%16lu%16.2f%%\n", " ", "rx_imsi_and_s1apid: ", rx_cdr_imsi_mme_ue_s1ap_id, ratio_of(rx_cdr_imsi_mme_ue_s1ap_id, rx_cdr_mme_ue_s1ap_id));
				fprintf(cdr_stats_out_fp.fp, "%32s%32s%16lu%16.2f% (+%u, %-03.2f%%)\n", " ", "refill result: ",
									rx_cdr_has_imsi_pkts,
									ratio_of(rx_cdr_has_imsi_pkts, rx_signal_cdr_pkts),
									rx_cdr_refilled_deta,
									ratio_of(rx_cdr_total_refill_pkts, rx_signal_cdr_pkts));

				fprintf(cdr_stats_out_fp.fp, "\n\n");
			}
		}
	}
#endif
}

static void geo_cdr_age(ht_value_t __oryx_unused_param__ v,
								uint32_t __oryx_unused_param__ s,
								void *opaque,
								int __oryx_unused_param__ opaque_size) {
	struct geo_htable_key_t *hk = (struct geo_htable_key_t *)v;
	//if(hk->mme_code == 255)
	//printf("hk->mme_code %u, hk->v %u\n", hk->mme_code, hk->v);
}

static __oryx_always_inline__ 
void geo_refill_queue_age_tmr_handler(struct oryx_timer_t *tmr, int __oryx_unused_param__ argc, 
				char **argv) {
	int refcount = oryx_htable_foreach_elem(geo_cdr_hash_table,
				geo_cdr_age, NULL, -1);
	//oryx_htable_print(geo_cdr_hash_table);
}

static __oryx_always_inline__ 
void geo_cdr_load_db_tmr_handler(struct oryx_timer_t *tmr, int __oryx_unused_param__ argc, 
				char **argv) {
	int i;
	struct geo_cdr_table_t *gct;
	int fd = -1;
	int nread;
	MD5_CTX ctx;
	char data_buf[1024];
	unsigned char md5[16];
	char sql[256];
	  
	for (i = 0; i < cdr_num; i ++) {
		gct = geo_cdr_tables[i];
		if(!gct) continue;
		
		MD5Init(&ctx);

		fd = open(gct->lf.fp_path, O_RDONLY);

		if(fd == -1)	continue;
		while(nread = read(fd, data_buf, sizeof(data_buf)), nread > 0)
			MD5Update(&ctx, data_buf, nread);

		MD5Final(md5, &ctx);

		close(fd);
		
		if(!memcmp(gct->lf.md5, md5, 16)) {
			continue;
		}
		
		/** hold the md5. */
		memcpy(gct->lf.md5, md5, 16);
		//lseek(fd, 0, SEEK_SET);
		/** db load */
		sprintf(sql, "DELETE from %s",
				geo_cdr_tables[i]->cdr_name);
		sqlctx->sql_cmd_type = SQLCMD_DELETE;
		sqlee_run (sqlctx, sql);

		sprintf(sql, "LOAD DATA LOCAL INFILE '%s' into table %s", gct->lf.fp_path, geo_cdr_tables[i]->cdr_name);
		sqlctx->sql_cmd_type = SQLCMD_LOAD_INFILE;
		sqlee_run (sqlctx, sql);
		
	}

}

void cdr_table_register(struct geo_cdr_table_t *gct)
{
	if(gct->cdr_index > cdr_num)
		return;
	
	if (oryx_path_exsit (gct->lf.fp_path))
		oryx_path_remove(gct->lf.fp_path);
	gct->lf.fp = fopen(gct->lf.fp_path, "wa+");
	BUG_ON(gct->lf.fp == NULL);
	
	geo_cdr_tables[gct->cdr_index] = gct;
}

static struct geo_cdr_table_t cdr_s1_mme = {
	.lf = {
		.fp_path	=	"/home/tsihang/data/cdr_s1mme.txt",
		.fp_comment =	"valid cdr record out logging.",
		.fp 		=	NULL,
	},
	.cdr_index = cdr_s1_mme_signal,
	.table_id = 0,
	.cdr_name = "s1_mme_signal",
	.length	= sizeof(full_record_s1_mme_signal_t),
};

static struct geo_cdr_table_t cdr_s1_emm = {
	.lf = {
		.fp_path	=	"/home/tsihang/data/cdr_s1emm.txt",
		.fp_comment =	"valid cdr record out logging.",
		.fp 		=	NULL,
	},
	.cdr_index = cdr_s1_emm_signal,
	.table_id = 0,
	.cdr_name = "s1_emm_signal",
	.length	= sizeof(full_record_s1_emm_signal_t),
};

static struct geo_cdr_table_t cdr_s1ap_handover = {
	.lf = {
		.fp_path	=	"/home/tsihang/data/cdr_s1ap_handover.txt",
		.fp_comment =	"valid cdr record out logging.",
		.fp 		=	NULL,
	},
	.cdr_index = cdr_s1ap_handover_signal,
	.table_id = 0,
	.cdr_name = "s1ap_handover_signal",
	.length	= sizeof(full_record_s1ap_handover_signal_t),
};

void geo_start_pcap(void) {

	int i;
	GEODecodeThreadVars *dtv;
	GEOThreadVars *tv;


	fq_new("CDR Refill QUEUE", &cdr_refill_queue);
	fq_new("CDR Packet QUEUE", &cdr_raw_data_queue);

	/** Create CDR memory pool. */
	mpool_init(&cdr_pool, "mempool for cdr",
					1000, sizeof(struct geo_cdr_entry_t) + 512, 64, NULL);

	/** create a hash table for refill */
	geo_cdr_hash_table = oryx_htable_init(DEFAULT_HASH_CHAIN_SIZE, 
							ht_geo_cdr_hval, ht_geo_cdr_cmp, ht_geo_cdr_free, 0);

	if (geo_cdr_hash_table == NULL) {
		printf ("vlib iface main init error!\n");
		exit(0);
	}
	
	for (i = 0; i < cdr_num; i ++) {
		dtv	= &gdtv[i];
		tv	= &gtv[i];
		geo_register_perf_counters(dtv, tv, i);
		geo_cdr_tables[i] = NULL;
	}

	cdr_table_register(&cdr_s1ap_handover);
	cdr_table_register(&cdr_s1_emm);
	cdr_table_register(&cdr_s1_mme);

	printf("[%d]sizeof(emm) %zdB [%d]sizeof(mme) %zdB [%d]sizeof(handover) %zdB\n",
		cdr_s1_emm.cdr_index, cdr_s1_emm.length,
		cdr_s1_mme.cdr_index, cdr_s1_mme.length,
		cdr_s1ap_handover.cdr_index, cdr_s1ap_handover.length);


	geo_cdr_persistence_init();

	
	oryx_task_registry(&geo_rx_task);
	oryx_task_registry(&geo_refill_task);
	oryx_task_registry(&geo_refill_prepare_task);

	oryx_tmr_start(oryx_tmr_create (1, "dp_perf_tmr", (TMR_OPTIONS_PERIODIC | TMR_OPTIONS_ADVANCED),
											  geo_pcap_perf_tmr_handler, 0, NULL, 3000));
	oryx_tmr_start(oryx_tmr_create (1, "refill_queue_age_tmr", (TMR_OPTIONS_PERIODIC | TMR_OPTIONS_ADVANCED),
												  geo_refill_queue_age_tmr_handler, 0, NULL, 3000));
	oryx_tmr_start(oryx_tmr_create (1, "cdr_load_db_tmr", (TMR_OPTIONS_PERIODIC | TMR_OPTIONS_ADVANCED),
												  geo_cdr_load_db_tmr_handler, 0, NULL, 3000));

}

void geo_end_pcap() {
	printf("Closing netdev %s...", geo_netdev.devname);
	printf(" Done\n");
	printf("Bye...\n");
}

