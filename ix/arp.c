/*
 * arp.c - Address Resolution Protocol support
 *
 * See RFC 826 for more details.
 *
 * FIXME: currently we don't support RARP (RFC 903). It's low
 * priority since the protocol is obsolete, but it wouldn't be
 * hard to add at all.
 */

#define DEBUG 1

#include <ix/stddef.h>
#include <ix/errno.h>
#include <ix/list.h>
#include <ix/timer.h>
#include <ix/hash.h>
#include <ix/mempool.h>
#include <ix/log.h>

#include <net/ethernet.h>
#include <net/ip.h>
#include <net/arp.h>

#include "net.h"
#include "cfg.h"

#define ARP_PKT_SIZE (sizeof(struct eth_hdr) +		\
		      sizeof(struct arp_hdr) +		\
		      sizeof(struct arp_hdr_ethip))

/* NOTE: this struct is exactly a cache line in size */
struct arp_entry {
	struct ip_addr		addr;
	uint32_t		pad;
	struct eth_addr		mac;
	uint8_t			flags;
	uint8_t			retries;
	struct timer		timer;
	struct hlist_node	link;
};

#define ARP_FLAG_RESOLVING	0x1
#define ARP_FLAG_VALID		0x2

#define ARP_REFRESH_TIMEOUT	10 * ONE_SECOND
#define ARP_RESOLVE_TIMEOUT	1 * ONE_SECOND
#define ARP_RETRY_TIMEOUT	1 * ONE_SECOND
#define ARP_MAX_ATTEMPTS	3

#define ARP_MAX_ENTRIES		65536
#define ARP_HASH_SEED		0xa36bdcbe

static struct mempool		arp_mempool;
static struct hlist_head	arp_tbl[ARP_MAX_ENTRIES];

static void arp_timer_handler(struct timer *t);

static inline int arp_ip_to_idx(struct ip_addr *addr)
{
	int idx = hash_crc32c_one(ARP_HASH_SEED, addr->addr);
	idx &= ARP_MAX_ENTRIES - 1;
	return idx;
}

static struct arp_entry *arp_lookup(struct ip_addr *addr, bool create_okay)
{
	struct arp_entry *e;
	struct hlist_node *pos;
	struct hlist_head *h = &arp_tbl[arp_ip_to_idx(addr)];

	hlist_for_each(h, pos) {
		e = hlist_entry(pos, struct arp_entry, link);
		if (e->addr.addr == addr->addr)
			return e;
	}

	if (create_okay) {
		e = (struct arp_entry *)mempool_alloc(&arp_mempool);
		if (unlikely(!e))
			return NULL;

		e->addr.addr = addr->addr;
		e->flags = 0;
		e->retries = 0;
		timer_init_entry(&e->timer, &arp_timer_handler);
		hlist_add_head(h, &e->link);
		return e;
	}

	return NULL;
}

static int arp_update_mac(struct ip_addr *addr,
			  struct eth_addr *mac, bool create_okay)
{
	struct arp_entry *e = arp_lookup(addr, create_okay);
	if (unlikely(!e))
		return -ENOMEM;

	e->mac = *mac;
	e->flags = ARP_FLAG_VALID;
	e->retries = 0;
	timer_mod(&e->timer, ARP_REFRESH_TIMEOUT);

	return 0;
}

#ifdef DEBUG
static void arp_dump_pkt(uint16_t op, struct arp_hdr_ethip *ethip)
{
	struct eth_addr *smac = &ethip->sender_mac;
	struct eth_addr *tmac = &ethip->target_mac;
	uint32_t sip, tip;

	sip = ntoh32(ethip->sender_ip.addr);
	tip = ntoh32(ethip->target_ip.addr);

	log_debug("arp: packet dump: op is %s\n",
		  (op == ARP_OP_REQUEST) ? "request" : "response");
	log_debug("arp:\tsender MAC:\t%02X:%02X:%02X:%02X:%02X:%02X\n",
		  smac->addr[0], smac->addr[1], smac->addr[2],
                  smac->addr[3], smac->addr[4], smac->addr[5]);
	log_debug("arp:\tsender IP:\t%d.%d.%d.%d\n",
		  ((sip >> 24) & 0xff), ((sip >> 16) & 0xff),
		  ((sip >> 8) & 0xff), (sip & 0xff));
	log_debug("arp:\ttarget MAC:\t%02X:%02X:%02X:%02X:%02X:%02X\n",
		  tmac->addr[0], tmac->addr[1], tmac->addr[2],
                  tmac->addr[3], tmac->addr[4], tmac->addr[5]);
	log_debug("arp:\ttarget IP:\t%d.%d.%d.%d\n",
		  ((tip >> 24) & 0xff), ((tip >> 16) & 0xff),
		  ((tip >> 8) & 0xff), (tip & 0xff));
}
#endif /* DEBUG */

static int arp_send_pkt(uint16_t op,
			struct ip_addr *target_ip,
			struct eth_addr *target_mac)
{
	int ret;
	struct rte_mbuf *pkt;
	struct eth_hdr *ethhdr;
	struct arp_hdr *arphdr;
	struct arp_hdr_ethip *ethip;

	pkt = dpdk_alloc_mbuf();
	if (unlikely(!pkt))
		return -ENOMEM;

	ethhdr = rte_pktmbuf_mtod(pkt, struct eth_hdr *);
	arphdr = next_hdr(ethhdr, struct arp_hdr *);
	ethip = next_hdr(arphdr, struct arp_hdr_ethip *);

	ethhdr->dhost = *target_mac;
	ethhdr->shost = cfg_mac;
	ethhdr->type = hton16(ETHTYPE_ARP);

	arphdr->htype = hton16(ARP_HTYPE_ETHER);
	arphdr->ptype = hton16(ETHTYPE_IP);
	arphdr->hlen = sizeof(struct eth_addr);
	arphdr->plen = sizeof(struct ip_addr);
	arphdr->op = hton16(op);

	ethip->sender_mac = cfg_mac;
	ethip->sender_ip.addr = hton32(cfg_host_addr.addr);
	ethip->target_mac = *target_mac;
	ethip->target_ip.addr = hton32(target_ip->addr);

	pkt->pkt.pkt_len = ARP_PKT_SIZE;
	pkt->pkt.data_len = ARP_PKT_SIZE;
	pkt->pkt.nb_segs = 1;

	ret = dpdk_tx_one_pkt(pkt);

	if (unlikely(ret != 1)) {
		rte_pktmbuf_free(pkt);
		return -EIO;
	}

	log_debug("arp: sending an ARP packet\n");
	arp_dump_pkt(op, ethip);

	return 0;
}

static int arp_send_response_reuse(struct rte_mbuf *pkt,
				   struct arp_hdr *arphdr,
				   struct arp_hdr_ethip *ethip)
{
	int ret;
	struct eth_hdr *ethhdr = rte_pktmbuf_mtod(pkt, struct eth_hdr *);
	ethhdr->dhost = ethhdr->shost;
	arphdr->op = hton16(ARP_OP_REPLY);
	ethip->target_mac = ethip->sender_mac;
	ethip->target_ip.addr = ethip->sender_ip.addr;

	ethhdr->shost = cfg_mac;
	ethip->sender_ip.addr = hton32(cfg_host_addr.addr);
	ethip->sender_mac = cfg_mac;

	pkt->pkt.pkt_len = ARP_PKT_SIZE;
	pkt->pkt.data_len = ARP_PKT_SIZE;
	pkt->pkt.nb_segs = 1;

	ret = dpdk_tx_one_pkt(pkt);

	if (unlikely(ret != 1)) {
		rte_pktmbuf_free(pkt);
		return -EIO;
	}

	log_debug("arp: sending an ARP response (reuse)\n");
	arp_dump_pkt(ARP_OP_REPLY, ethip);

	return 0;
}

/**
 * arp_process_pkt - handles an ARP request from the network
 * @pkt: the packet
 * @hdr: the ARP header (inside the packet)
 */
void arp_process_pkt(struct rte_mbuf *pkt, struct arp_hdr *hdr)
{
	int op;
	struct arp_hdr_ethip *ethip;
	struct ip_addr sender_ip, target_ip;
	bool am_target;

	if (!enough_space(pkt, hdr))
		goto out;

	/* make sure the arp header is valid */
	if (ntoh16(hdr->htype) != ARP_HTYPE_ETHER ||
	    ntoh16(hdr->ptype) != ETHTYPE_IP ||
	    hdr->hlen != sizeof(struct ether_addr) ||
	    hdr->plen != sizeof(struct ip_addr))
		goto out;

	/* now validate the variable length portion */
	ethip = next_hdr(hdr, struct arp_hdr_ethip *);
	if (!enough_space(pkt, ethip))
		goto out;

	op = ntoh16(hdr->op);
	sender_ip.addr = ntoh32(ethip->sender_ip.addr);
	target_ip.addr = ntoh32(ethip->target_ip.addr);

	log_debug("arp: recieved an ARP packet\n");
	arp_dump_pkt(op, ethip);

	/* refuse ARP packets with multicast source MAC's */
	if (eth_addr_is_multicast(&ethip->sender_mac))
		goto out;

	am_target = (cfg_host_addr.addr == target_ip.addr);
	arp_update_mac(&sender_ip, &ethip->sender_mac, am_target);

	if (am_target && op == ARP_OP_REQUEST) {
		arp_send_response_reuse(pkt, hdr, ethip);
		return;
	}

out:
	rte_pktmbuf_free(pkt);
}

/**
 * arp_lookup_mac - gives back a MAC value for a given IP address
 * @addr: the IP address to lookup
 * @mac: a buffer to store the MAC value
 *
 * Returns 0 if successful, -EAGAIN if waiting to resolve, otherwise fail.
 */
int arp_lookup_mac(struct ip_addr *addr, struct eth_addr *mac)
{
	struct arp_entry *e = arp_lookup(addr, false);
	if (!e)
		return -ENOENT;

	if (!(e->flags & ARP_FLAG_VALID)) {
		if (!timer_pending(&e->timer)) {
			struct eth_addr target = ETH_ADDR_BROADCAST;

			e->flags |= ARP_FLAG_RESOLVING;
			arp_send_pkt(ARP_OP_REQUEST, addr, &target);
			timer_add(&e->timer, ARP_RESOLVE_TIMEOUT);
		}
		return -EAGAIN;
	}

	*mac = e->mac;
	return 0;
}

static void arp_timer_handler(struct timer *t)
{
	struct arp_entry *e = container_of(t, struct arp_entry, timer);

	e->retries++;
	if (e->retries >= ARP_MAX_ATTEMPTS) {
		hlist_del(&e->link);
		mempool_free(&arp_mempool, e);
		return;
	}

	e->flags |= ARP_FLAG_RESOLVING;

	if (e->flags & ARP_FLAG_VALID) {
		arp_send_pkt(ARP_OP_REQUEST, &e->addr, &e->mac);
	} else {
		struct eth_addr target = ETH_ADDR_BROADCAST;
		arp_send_pkt(ARP_OP_REQUEST, &e->addr, &target);
	}

	timer_add(t, ARP_RETRY_TIMEOUT);
}

/**
 * arp_init - initializes the ARP service
 */
int arp_init(void)
{
	return mempool_create(&arp_mempool, ARP_MAX_ENTRIES,
			      sizeof(struct arp_entry));
}
