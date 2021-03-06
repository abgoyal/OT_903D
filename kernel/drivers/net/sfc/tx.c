

#include <linux/pci.h>
#include <linux/tcp.h>
#include <linux/ip.h>
#include <linux/in.h>
#include <linux/ipv6.h>
#include <linux/slab.h>
#include <net/ipv6.h>
#include <linux/if_ether.h>
#include <linux/highmem.h>
#include "net_driver.h"
#include "efx.h"
#include "nic.h"
#include "workarounds.h"

#define EFX_TXQ_THRESHOLD (EFX_TXQ_MASK / 2u)

void efx_stop_queue(struct efx_channel *channel)
{
	struct efx_nic *efx = channel->efx;

	if (!channel->tx_queue)
		return;

	spin_lock_bh(&channel->tx_stop_lock);
	EFX_TRACE(efx, "stop TX queue\n");

	atomic_inc(&channel->tx_stop_count);
	netif_tx_stop_queue(
		netdev_get_tx_queue(
			efx->net_dev,
			channel->tx_queue->queue / EFX_TXQ_TYPES));

	spin_unlock_bh(&channel->tx_stop_lock);
}

/* Decrement core TX queue stop count and wake it if the count is 0 */
void efx_wake_queue(struct efx_channel *channel)
{
	struct efx_nic *efx = channel->efx;

	if (!channel->tx_queue)
		return;

	local_bh_disable();
	if (atomic_dec_and_lock(&channel->tx_stop_count,
				&channel->tx_stop_lock)) {
		EFX_TRACE(efx, "waking TX queue\n");
		netif_tx_wake_queue(
			netdev_get_tx_queue(
				efx->net_dev,
				channel->tx_queue->queue / EFX_TXQ_TYPES));
		spin_unlock(&channel->tx_stop_lock);
	}
	local_bh_enable();
}

static void efx_dequeue_buffer(struct efx_tx_queue *tx_queue,
			       struct efx_tx_buffer *buffer)
{
	if (buffer->unmap_len) {
		struct pci_dev *pci_dev = tx_queue->efx->pci_dev;
		dma_addr_t unmap_addr = (buffer->dma_addr + buffer->len -
					 buffer->unmap_len);
		if (buffer->unmap_single)
			pci_unmap_single(pci_dev, unmap_addr, buffer->unmap_len,
					 PCI_DMA_TODEVICE);
		else
			pci_unmap_page(pci_dev, unmap_addr, buffer->unmap_len,
				       PCI_DMA_TODEVICE);
		buffer->unmap_len = 0;
		buffer->unmap_single = false;
	}

	if (buffer->skb) {
		dev_kfree_skb_any((struct sk_buff *) buffer->skb);
		buffer->skb = NULL;
		EFX_TRACE(tx_queue->efx, "TX queue %d transmission id %x "
			  "complete\n", tx_queue->queue, read_ptr);
	}
}

struct efx_tso_header {
	union {
		struct efx_tso_header *next;
		size_t unmap_len;
	};
	dma_addr_t dma_addr;
};

static int efx_enqueue_skb_tso(struct efx_tx_queue *tx_queue,
			       struct sk_buff *skb);
static void efx_fini_tso(struct efx_tx_queue *tx_queue);
static void efx_tsoh_heap_free(struct efx_tx_queue *tx_queue,
			       struct efx_tso_header *tsoh);

static void efx_tsoh_free(struct efx_tx_queue *tx_queue,
			  struct efx_tx_buffer *buffer)
{
	if (buffer->tsoh) {
		if (likely(!buffer->tsoh->unmap_len)) {
			buffer->tsoh->next = tx_queue->tso_headers_free;
			tx_queue->tso_headers_free = buffer->tsoh;
		} else {
			efx_tsoh_heap_free(tx_queue, buffer->tsoh);
		}
		buffer->tsoh = NULL;
	}
}


static inline unsigned
efx_max_tx_len(struct efx_nic *efx, dma_addr_t dma_addr)
{
	/* Depending on the NIC revision, we can use descriptor
	 * lengths up to 8K or 8K-1.  However, since PCI Express
	 * devices must split read requests at 4K boundaries, there is
	 * little benefit from using descriptors that cross those
	 * boundaries and we keep things simple by not doing so.
	 */
	unsigned len = (~dma_addr & 0xfff) + 1;

	/* Work around hardware bug for unaligned buffers. */
	if (EFX_WORKAROUND_5391(efx) && (dma_addr & 0xf))
		len = min_t(unsigned, len, 512 - (dma_addr & 0xf));

	return len;
}

netdev_tx_t efx_enqueue_skb(struct efx_tx_queue *tx_queue, struct sk_buff *skb)
{
	struct efx_nic *efx = tx_queue->efx;
	struct pci_dev *pci_dev = efx->pci_dev;
	struct efx_tx_buffer *buffer;
	skb_frag_t *fragment;
	struct page *page;
	int page_offset;
	unsigned int len, unmap_len = 0, fill_level, insert_ptr;
	dma_addr_t dma_addr, unmap_addr = 0;
	unsigned int dma_len;
	bool unmap_single;
	int q_space, i = 0;
	netdev_tx_t rc = NETDEV_TX_OK;

	EFX_BUG_ON_PARANOID(tx_queue->write_count != tx_queue->insert_count);

	if (skb_shinfo(skb)->gso_size)
		return efx_enqueue_skb_tso(tx_queue, skb);

	/* Get size of the initial fragment */
	len = skb_headlen(skb);

	/* Pad if necessary */
	if (EFX_WORKAROUND_15592(efx) && skb->len <= 32) {
		EFX_BUG_ON_PARANOID(skb->data_len);
		len = 32 + 1;
		if (skb_pad(skb, len - skb->len))
			return NETDEV_TX_OK;
	}

	fill_level = tx_queue->insert_count - tx_queue->old_read_count;
	q_space = EFX_TXQ_MASK - 1 - fill_level;

	/* Map for DMA.  Use pci_map_single rather than pci_map_page
	 * since this is more efficient on machines with sparse
	 * memory.
	 */
	unmap_single = true;
	dma_addr = pci_map_single(pci_dev, skb->data, len, PCI_DMA_TODEVICE);

	/* Process all fragments */
	while (1) {
		if (unlikely(pci_dma_mapping_error(pci_dev, dma_addr)))
			goto pci_err;

		/* Store fields for marking in the per-fragment final
		 * descriptor */
		unmap_len = len;
		unmap_addr = dma_addr;

		/* Add to TX queue, splitting across DMA boundaries */
		do {
			if (unlikely(q_space-- <= 0)) {
				/* It might be that completions have
				 * happened since the xmit path last
				 * checked.  Update the xmit path's
				 * copy of read_count.
				 */
				++tx_queue->stopped;
				/* This memory barrier protects the
				 * change of stopped from the access
				 * of read_count. */
				smp_mb();
				tx_queue->old_read_count =
					*(volatile unsigned *)
					&tx_queue->read_count;
				fill_level = (tx_queue->insert_count
					      - tx_queue->old_read_count);
				q_space = EFX_TXQ_MASK - 1 - fill_level;
				if (unlikely(q_space-- <= 0))
					goto stop;
				smp_mb();
				--tx_queue->stopped;
			}

			insert_ptr = tx_queue->insert_count & EFX_TXQ_MASK;
			buffer = &tx_queue->buffer[insert_ptr];
			efx_tsoh_free(tx_queue, buffer);
			EFX_BUG_ON_PARANOID(buffer->tsoh);
			EFX_BUG_ON_PARANOID(buffer->skb);
			EFX_BUG_ON_PARANOID(buffer->len);
			EFX_BUG_ON_PARANOID(!buffer->continuation);
			EFX_BUG_ON_PARANOID(buffer->unmap_len);

			dma_len = efx_max_tx_len(efx, dma_addr);
			if (likely(dma_len >= len))
				dma_len = len;

			/* Fill out per descriptor fields */
			buffer->len = dma_len;
			buffer->dma_addr = dma_addr;
			len -= dma_len;
			dma_addr += dma_len;
			++tx_queue->insert_count;
		} while (len);

		/* Transfer ownership of the unmapping to the final buffer */
		buffer->unmap_single = unmap_single;
		buffer->unmap_len = unmap_len;
		unmap_len = 0;

		/* Get address and size of next fragment */
		if (i >= skb_shinfo(skb)->nr_frags)
			break;
		fragment = &skb_shinfo(skb)->frags[i];
		len = fragment->size;
		page = fragment->page;
		page_offset = fragment->page_offset;
		i++;
		/* Map for DMA */
		unmap_single = false;
		dma_addr = pci_map_page(pci_dev, page, page_offset, len,
					PCI_DMA_TODEVICE);
	}

	/* Transfer ownership of the skb to the final buffer */
	buffer->skb = skb;
	buffer->continuation = false;

	/* Pass off to hardware */
	efx_nic_push_buffers(tx_queue);

	return NETDEV_TX_OK;

 pci_err:
	EFX_ERR_RL(efx, " TX queue %d could not map skb with %d bytes %d "
		   "fragments for DMA\n", tx_queue->queue, skb->len,
		   skb_shinfo(skb)->nr_frags + 1);

	/* Mark the packet as transmitted, and free the SKB ourselves */
	dev_kfree_skb_any(skb);
	goto unwind;

 stop:
	rc = NETDEV_TX_BUSY;

	if (tx_queue->stopped == 1)
		efx_stop_queue(tx_queue->channel);

 unwind:
	/* Work backwards until we hit the original insert pointer value */
	while (tx_queue->insert_count != tx_queue->write_count) {
		--tx_queue->insert_count;
		insert_ptr = tx_queue->insert_count & EFX_TXQ_MASK;
		buffer = &tx_queue->buffer[insert_ptr];
		efx_dequeue_buffer(tx_queue, buffer);
		buffer->len = 0;
	}

	/* Free the fragment we were mid-way through pushing */
	if (unmap_len) {
		if (unmap_single)
			pci_unmap_single(pci_dev, unmap_addr, unmap_len,
					 PCI_DMA_TODEVICE);
		else
			pci_unmap_page(pci_dev, unmap_addr, unmap_len,
				       PCI_DMA_TODEVICE);
	}

	return rc;
}

static void efx_dequeue_buffers(struct efx_tx_queue *tx_queue,
				unsigned int index)
{
	struct efx_nic *efx = tx_queue->efx;
	unsigned int stop_index, read_ptr;

	stop_index = (index + 1) & EFX_TXQ_MASK;
	read_ptr = tx_queue->read_count & EFX_TXQ_MASK;

	while (read_ptr != stop_index) {
		struct efx_tx_buffer *buffer = &tx_queue->buffer[read_ptr];
		if (unlikely(buffer->len == 0)) {
			EFX_ERR(tx_queue->efx, "TX queue %d spurious TX "
				"completion id %x\n", tx_queue->queue,
				read_ptr);
			efx_schedule_reset(efx, RESET_TYPE_TX_SKIP);
			return;
		}

		efx_dequeue_buffer(tx_queue, buffer);
		buffer->continuation = true;
		buffer->len = 0;

		++tx_queue->read_count;
		read_ptr = tx_queue->read_count & EFX_TXQ_MASK;
	}
}

netdev_tx_t efx_hard_start_xmit(struct sk_buff *skb,
				      struct net_device *net_dev)
{
	struct efx_nic *efx = netdev_priv(net_dev);
	struct efx_tx_queue *tx_queue;

	if (unlikely(efx->port_inhibited))
		return NETDEV_TX_BUSY;

	tx_queue = &efx->tx_queue[EFX_TXQ_TYPES * skb_get_queue_mapping(skb)];
	if (likely(skb->ip_summed == CHECKSUM_PARTIAL))
		tx_queue += EFX_TXQ_TYPE_OFFLOAD;

	return efx_enqueue_skb(tx_queue, skb);
}

void efx_xmit_done(struct efx_tx_queue *tx_queue, unsigned int index)
{
	unsigned fill_level;
	struct efx_nic *efx = tx_queue->efx;

	EFX_BUG_ON_PARANOID(index > EFX_TXQ_MASK);

	efx_dequeue_buffers(tx_queue, index);

	/* See if we need to restart the netif queue.  This barrier
	 * separates the update of read_count from the test of
	 * stopped. */
	smp_mb();
	if (unlikely(tx_queue->stopped) && likely(efx->port_enabled)) {
		fill_level = tx_queue->insert_count - tx_queue->read_count;
		if (fill_level < EFX_TXQ_THRESHOLD) {
			EFX_BUG_ON_PARANOID(!efx_dev_registered(efx));

			/* Do this under netif_tx_lock(), to avoid racing
			 * with efx_xmit(). */
			netif_tx_lock(efx->net_dev);
			if (tx_queue->stopped) {
				tx_queue->stopped = 0;
				efx_wake_queue(tx_queue->channel);
			}
			netif_tx_unlock(efx->net_dev);
		}
	}
}

int efx_probe_tx_queue(struct efx_tx_queue *tx_queue)
{
	struct efx_nic *efx = tx_queue->efx;
	unsigned int txq_size;
	int i, rc;

	EFX_LOG(efx, "creating TX queue %d\n", tx_queue->queue);

	/* Allocate software ring */
	txq_size = EFX_TXQ_SIZE * sizeof(*tx_queue->buffer);
	tx_queue->buffer = kzalloc(txq_size, GFP_KERNEL);
	if (!tx_queue->buffer)
		return -ENOMEM;
	for (i = 0; i <= EFX_TXQ_MASK; ++i)
		tx_queue->buffer[i].continuation = true;

	/* Allocate hardware ring */
	rc = efx_nic_probe_tx(tx_queue);
	if (rc)
		goto fail;

	return 0;

 fail:
	kfree(tx_queue->buffer);
	tx_queue->buffer = NULL;
	return rc;
}

void efx_init_tx_queue(struct efx_tx_queue *tx_queue)
{
	EFX_LOG(tx_queue->efx, "initialising TX queue %d\n", tx_queue->queue);

	tx_queue->insert_count = 0;
	tx_queue->write_count = 0;
	tx_queue->read_count = 0;
	tx_queue->old_read_count = 0;
	BUG_ON(tx_queue->stopped);

	/* Set up TX descriptor ring */
	efx_nic_init_tx(tx_queue);
}

void efx_release_tx_buffers(struct efx_tx_queue *tx_queue)
{
	struct efx_tx_buffer *buffer;

	if (!tx_queue->buffer)
		return;

	/* Free any buffers left in the ring */
	while (tx_queue->read_count != tx_queue->write_count) {
		buffer = &tx_queue->buffer[tx_queue->read_count & EFX_TXQ_MASK];
		efx_dequeue_buffer(tx_queue, buffer);
		buffer->continuation = true;
		buffer->len = 0;

		++tx_queue->read_count;
	}
}

void efx_fini_tx_queue(struct efx_tx_queue *tx_queue)
{
	EFX_LOG(tx_queue->efx, "shutting down TX queue %d\n", tx_queue->queue);

	/* Flush TX queue, remove descriptor ring */
	efx_nic_fini_tx(tx_queue);

	efx_release_tx_buffers(tx_queue);

	/* Free up TSO header cache */
	efx_fini_tso(tx_queue);

	/* Release queue's stop on port, if any */
	if (tx_queue->stopped) {
		tx_queue->stopped = 0;
		efx_wake_queue(tx_queue->channel);
	}
}

void efx_remove_tx_queue(struct efx_tx_queue *tx_queue)
{
	EFX_LOG(tx_queue->efx, "destroying TX queue %d\n", tx_queue->queue);
	efx_nic_remove_tx(tx_queue);

	kfree(tx_queue->buffer);
	tx_queue->buffer = NULL;
}



#ifdef CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS
#define TSOH_OFFSET	0
#else
#define TSOH_OFFSET	NET_IP_ALIGN
#endif

#define TSOH_BUFFER(tsoh)	((u8 *)(tsoh + 1) + TSOH_OFFSET)

/* Total size of struct efx_tso_header, buffer and padding */
#define TSOH_SIZE(hdr_len)					\
	(sizeof(struct efx_tso_header) + TSOH_OFFSET + hdr_len)

#define TSOH_STD_SIZE		128

#define PTR_DIFF(p1, p2)  ((u8 *)(p1) - (u8 *)(p2))
#define ETH_HDR_LEN(skb)  (skb_network_header(skb) - (skb)->data)
#define SKB_TCP_OFF(skb)  PTR_DIFF(tcp_hdr(skb), (skb)->data)
#define SKB_IPV4_OFF(skb) PTR_DIFF(ip_hdr(skb), (skb)->data)
#define SKB_IPV6_OFF(skb) PTR_DIFF(ipv6_hdr(skb), (skb)->data)

struct tso_state {
	/* Output position */
	unsigned out_len;
	unsigned seqnum;
	unsigned ipv4_id;
	unsigned packet_space;

	/* Input position */
	dma_addr_t dma_addr;
	unsigned in_len;
	unsigned unmap_len;
	dma_addr_t unmap_addr;
	bool unmap_single;

	__be16 protocol;
	unsigned header_len;
	int full_packet_size;
};


static __be16 efx_tso_check_protocol(struct sk_buff *skb)
{
	__be16 protocol = skb->protocol;

	EFX_BUG_ON_PARANOID(((struct ethhdr *)skb->data)->h_proto !=
			    protocol);
	if (protocol == htons(ETH_P_8021Q)) {
		/* Find the encapsulated protocol; reset network header
		 * and transport header based on that. */
		struct vlan_ethhdr *veh = (struct vlan_ethhdr *)skb->data;
		protocol = veh->h_vlan_encapsulated_proto;
		skb_set_network_header(skb, sizeof(*veh));
		if (protocol == htons(ETH_P_IP))
			skb_set_transport_header(skb, sizeof(*veh) +
						 4 * ip_hdr(skb)->ihl);
		else if (protocol == htons(ETH_P_IPV6))
			skb_set_transport_header(skb, sizeof(*veh) +
						 sizeof(struct ipv6hdr));
	}

	if (protocol == htons(ETH_P_IP)) {
		EFX_BUG_ON_PARANOID(ip_hdr(skb)->protocol != IPPROTO_TCP);
	} else {
		EFX_BUG_ON_PARANOID(protocol != htons(ETH_P_IPV6));
		EFX_BUG_ON_PARANOID(ipv6_hdr(skb)->nexthdr != NEXTHDR_TCP);
	}
	EFX_BUG_ON_PARANOID((PTR_DIFF(tcp_hdr(skb), skb->data)
			     + (tcp_hdr(skb)->doff << 2u)) >
			    skb_headlen(skb));

	return protocol;
}


static int efx_tsoh_block_alloc(struct efx_tx_queue *tx_queue)
{

	struct pci_dev *pci_dev = tx_queue->efx->pci_dev;
	struct efx_tso_header *tsoh;
	dma_addr_t dma_addr;
	u8 *base_kva, *kva;

	base_kva = pci_alloc_consistent(pci_dev, PAGE_SIZE, &dma_addr);
	if (base_kva == NULL) {
		EFX_ERR(tx_queue->efx, "Unable to allocate page for TSO"
			" headers\n");
		return -ENOMEM;
	}

	/* pci_alloc_consistent() allocates pages. */
	EFX_BUG_ON_PARANOID(dma_addr & (PAGE_SIZE - 1u));

	for (kva = base_kva; kva < base_kva + PAGE_SIZE; kva += TSOH_STD_SIZE) {
		tsoh = (struct efx_tso_header *)kva;
		tsoh->dma_addr = dma_addr + (TSOH_BUFFER(tsoh) - base_kva);
		tsoh->next = tx_queue->tso_headers_free;
		tx_queue->tso_headers_free = tsoh;
	}

	return 0;
}


/* Free up a TSO header, and all others in the same page. */
static void efx_tsoh_block_free(struct efx_tx_queue *tx_queue,
				struct efx_tso_header *tsoh,
				struct pci_dev *pci_dev)
{
	struct efx_tso_header **p;
	unsigned long base_kva;
	dma_addr_t base_dma;

	base_kva = (unsigned long)tsoh & PAGE_MASK;
	base_dma = tsoh->dma_addr & PAGE_MASK;

	p = &tx_queue->tso_headers_free;
	while (*p != NULL) {
		if (((unsigned long)*p & PAGE_MASK) == base_kva)
			*p = (*p)->next;
		else
			p = &(*p)->next;
	}

	pci_free_consistent(pci_dev, PAGE_SIZE, (void *)base_kva, base_dma);
}

static struct efx_tso_header *
efx_tsoh_heap_alloc(struct efx_tx_queue *tx_queue, size_t header_len)
{
	struct efx_tso_header *tsoh;

	tsoh = kmalloc(TSOH_SIZE(header_len), GFP_ATOMIC | GFP_DMA);
	if (unlikely(!tsoh))
		return NULL;

	tsoh->dma_addr = pci_map_single(tx_queue->efx->pci_dev,
					TSOH_BUFFER(tsoh), header_len,
					PCI_DMA_TODEVICE);
	if (unlikely(pci_dma_mapping_error(tx_queue->efx->pci_dev,
					   tsoh->dma_addr))) {
		kfree(tsoh);
		return NULL;
	}

	tsoh->unmap_len = header_len;
	return tsoh;
}

static void
efx_tsoh_heap_free(struct efx_tx_queue *tx_queue, struct efx_tso_header *tsoh)
{
	pci_unmap_single(tx_queue->efx->pci_dev,
			 tsoh->dma_addr, tsoh->unmap_len,
			 PCI_DMA_TODEVICE);
	kfree(tsoh);
}

static int efx_tx_queue_insert(struct efx_tx_queue *tx_queue,
			       dma_addr_t dma_addr, unsigned len,
			       struct efx_tx_buffer **final_buffer)
{
	struct efx_tx_buffer *buffer;
	struct efx_nic *efx = tx_queue->efx;
	unsigned dma_len, fill_level, insert_ptr;
	int q_space;

	EFX_BUG_ON_PARANOID(len <= 0);

	fill_level = tx_queue->insert_count - tx_queue->old_read_count;
	/* -1 as there is no way to represent all descriptors used */
	q_space = EFX_TXQ_MASK - 1 - fill_level;

	while (1) {
		if (unlikely(q_space-- <= 0)) {
			/* It might be that completions have happened
			 * since the xmit path last checked.  Update
			 * the xmit path's copy of read_count.
			 */
			++tx_queue->stopped;
			/* This memory barrier protects the change of
			 * stopped from the access of read_count. */
			smp_mb();
			tx_queue->old_read_count =
				*(volatile unsigned *)&tx_queue->read_count;
			fill_level = (tx_queue->insert_count
				      - tx_queue->old_read_count);
			q_space = EFX_TXQ_MASK - 1 - fill_level;
			if (unlikely(q_space-- <= 0)) {
				*final_buffer = NULL;
				return 1;
			}
			smp_mb();
			--tx_queue->stopped;
		}

		insert_ptr = tx_queue->insert_count & EFX_TXQ_MASK;
		buffer = &tx_queue->buffer[insert_ptr];
		++tx_queue->insert_count;

		EFX_BUG_ON_PARANOID(tx_queue->insert_count -
				    tx_queue->read_count >
				    EFX_TXQ_MASK);

		efx_tsoh_free(tx_queue, buffer);
		EFX_BUG_ON_PARANOID(buffer->len);
		EFX_BUG_ON_PARANOID(buffer->unmap_len);
		EFX_BUG_ON_PARANOID(buffer->skb);
		EFX_BUG_ON_PARANOID(!buffer->continuation);
		EFX_BUG_ON_PARANOID(buffer->tsoh);

		buffer->dma_addr = dma_addr;

		dma_len = efx_max_tx_len(efx, dma_addr);

		/* If there is enough space to send then do so */
		if (dma_len >= len)
			break;

		buffer->len = dma_len; /* Don't set the other members */
		dma_addr += dma_len;
		len -= dma_len;
	}

	EFX_BUG_ON_PARANOID(!len);
	buffer->len = len;
	*final_buffer = buffer;
	return 0;
}


static void efx_tso_put_header(struct efx_tx_queue *tx_queue,
			       struct efx_tso_header *tsoh, unsigned len)
{
	struct efx_tx_buffer *buffer;

	buffer = &tx_queue->buffer[tx_queue->insert_count & EFX_TXQ_MASK];
	efx_tsoh_free(tx_queue, buffer);
	EFX_BUG_ON_PARANOID(buffer->len);
	EFX_BUG_ON_PARANOID(buffer->unmap_len);
	EFX_BUG_ON_PARANOID(buffer->skb);
	EFX_BUG_ON_PARANOID(!buffer->continuation);
	EFX_BUG_ON_PARANOID(buffer->tsoh);
	buffer->len = len;
	buffer->dma_addr = tsoh->dma_addr;
	buffer->tsoh = tsoh;

	++tx_queue->insert_count;
}


/* Remove descriptors put into a tx_queue. */
static void efx_enqueue_unwind(struct efx_tx_queue *tx_queue)
{
	struct efx_tx_buffer *buffer;
	dma_addr_t unmap_addr;

	/* Work backwards until we hit the original insert pointer value */
	while (tx_queue->insert_count != tx_queue->write_count) {
		--tx_queue->insert_count;
		buffer = &tx_queue->buffer[tx_queue->insert_count &
					   EFX_TXQ_MASK];
		efx_tsoh_free(tx_queue, buffer);
		EFX_BUG_ON_PARANOID(buffer->skb);
		if (buffer->unmap_len) {
			unmap_addr = (buffer->dma_addr + buffer->len -
				      buffer->unmap_len);
			if (buffer->unmap_single)
				pci_unmap_single(tx_queue->efx->pci_dev,
						 unmap_addr, buffer->unmap_len,
						 PCI_DMA_TODEVICE);
			else
				pci_unmap_page(tx_queue->efx->pci_dev,
					       unmap_addr, buffer->unmap_len,
					       PCI_DMA_TODEVICE);
			buffer->unmap_len = 0;
		}
		buffer->len = 0;
		buffer->continuation = true;
	}
}


/* Parse the SKB header and initialise state. */
static void tso_start(struct tso_state *st, const struct sk_buff *skb)
{
	/* All ethernet/IP/TCP headers combined size is TCP header size
	 * plus offset of TCP header relative to start of packet.
	 */
	st->header_len = ((tcp_hdr(skb)->doff << 2u)
			  + PTR_DIFF(tcp_hdr(skb), skb->data));
	st->full_packet_size = st->header_len + skb_shinfo(skb)->gso_size;

	if (st->protocol == htons(ETH_P_IP))
		st->ipv4_id = ntohs(ip_hdr(skb)->id);
	else
		st->ipv4_id = 0;
	st->seqnum = ntohl(tcp_hdr(skb)->seq);

	EFX_BUG_ON_PARANOID(tcp_hdr(skb)->urg);
	EFX_BUG_ON_PARANOID(tcp_hdr(skb)->syn);
	EFX_BUG_ON_PARANOID(tcp_hdr(skb)->rst);

	st->packet_space = st->full_packet_size;
	st->out_len = skb->len - st->header_len;
	st->unmap_len = 0;
	st->unmap_single = false;
}

static int tso_get_fragment(struct tso_state *st, struct efx_nic *efx,
			    skb_frag_t *frag)
{
	st->unmap_addr = pci_map_page(efx->pci_dev, frag->page,
				      frag->page_offset, frag->size,
				      PCI_DMA_TODEVICE);
	if (likely(!pci_dma_mapping_error(efx->pci_dev, st->unmap_addr))) {
		st->unmap_single = false;
		st->unmap_len = frag->size;
		st->in_len = frag->size;
		st->dma_addr = st->unmap_addr;
		return 0;
	}
	return -ENOMEM;
}

static int tso_get_head_fragment(struct tso_state *st, struct efx_nic *efx,
				 const struct sk_buff *skb)
{
	int hl = st->header_len;
	int len = skb_headlen(skb) - hl;

	st->unmap_addr = pci_map_single(efx->pci_dev, skb->data + hl,
					len, PCI_DMA_TODEVICE);
	if (likely(!pci_dma_mapping_error(efx->pci_dev, st->unmap_addr))) {
		st->unmap_single = true;
		st->unmap_len = len;
		st->in_len = len;
		st->dma_addr = st->unmap_addr;
		return 0;
	}
	return -ENOMEM;
}


static int tso_fill_packet_with_fragment(struct efx_tx_queue *tx_queue,
					 const struct sk_buff *skb,
					 struct tso_state *st)
{
	struct efx_tx_buffer *buffer;
	int n, end_of_packet, rc;

	if (st->in_len == 0)
		return 0;
	if (st->packet_space == 0)
		return 0;

	EFX_BUG_ON_PARANOID(st->in_len <= 0);
	EFX_BUG_ON_PARANOID(st->packet_space <= 0);

	n = min(st->in_len, st->packet_space);

	st->packet_space -= n;
	st->out_len -= n;
	st->in_len -= n;

	rc = efx_tx_queue_insert(tx_queue, st->dma_addr, n, &buffer);
	if (likely(rc == 0)) {
		if (st->out_len == 0)
			/* Transfer ownership of the skb */
			buffer->skb = skb;

		end_of_packet = st->out_len == 0 || st->packet_space == 0;
		buffer->continuation = !end_of_packet;

		if (st->in_len == 0) {
			/* Transfer ownership of the pci mapping */
			buffer->unmap_len = st->unmap_len;
			buffer->unmap_single = st->unmap_single;
			st->unmap_len = 0;
		}
	}

	st->dma_addr += n;
	return rc;
}


static int tso_start_new_packet(struct efx_tx_queue *tx_queue,
				const struct sk_buff *skb,
				struct tso_state *st)
{
	struct efx_tso_header *tsoh;
	struct tcphdr *tsoh_th;
	unsigned ip_length;
	u8 *header;

	/* Allocate a DMA-mapped header buffer. */
	if (likely(TSOH_SIZE(st->header_len) <= TSOH_STD_SIZE)) {
		if (tx_queue->tso_headers_free == NULL) {
			if (efx_tsoh_block_alloc(tx_queue))
				return -1;
		}
		EFX_BUG_ON_PARANOID(!tx_queue->tso_headers_free);
		tsoh = tx_queue->tso_headers_free;
		tx_queue->tso_headers_free = tsoh->next;
		tsoh->unmap_len = 0;
	} else {
		tx_queue->tso_long_headers++;
		tsoh = efx_tsoh_heap_alloc(tx_queue, st->header_len);
		if (unlikely(!tsoh))
			return -1;
	}

	header = TSOH_BUFFER(tsoh);
	tsoh_th = (struct tcphdr *)(header + SKB_TCP_OFF(skb));

	/* Copy and update the headers. */
	memcpy(header, skb->data, st->header_len);

	tsoh_th->seq = htonl(st->seqnum);
	st->seqnum += skb_shinfo(skb)->gso_size;
	if (st->out_len > skb_shinfo(skb)->gso_size) {
		/* This packet will not finish the TSO burst. */
		ip_length = st->full_packet_size - ETH_HDR_LEN(skb);
		tsoh_th->fin = 0;
		tsoh_th->psh = 0;
	} else {
		/* This packet will be the last in the TSO burst. */
		ip_length = st->header_len - ETH_HDR_LEN(skb) + st->out_len;
		tsoh_th->fin = tcp_hdr(skb)->fin;
		tsoh_th->psh = tcp_hdr(skb)->psh;
	}

	if (st->protocol == htons(ETH_P_IP)) {
		struct iphdr *tsoh_iph =
			(struct iphdr *)(header + SKB_IPV4_OFF(skb));

		tsoh_iph->tot_len = htons(ip_length);

		/* Linux leaves suitable gaps in the IP ID space for us to fill. */
		tsoh_iph->id = htons(st->ipv4_id);
		st->ipv4_id++;
	} else {
		struct ipv6hdr *tsoh_iph =
			(struct ipv6hdr *)(header + SKB_IPV6_OFF(skb));

		tsoh_iph->payload_len = htons(ip_length - sizeof(*tsoh_iph));
	}

	st->packet_space = skb_shinfo(skb)->gso_size;
	++tx_queue->tso_packets;

	/* Form a descriptor for this header. */
	efx_tso_put_header(tx_queue, tsoh, st->header_len);

	return 0;
}


static int efx_enqueue_skb_tso(struct efx_tx_queue *tx_queue,
			       struct sk_buff *skb)
{
	struct efx_nic *efx = tx_queue->efx;
	int frag_i, rc, rc2 = NETDEV_TX_OK;
	struct tso_state state;

	/* Find the packet protocol and sanity-check it */
	state.protocol = efx_tso_check_protocol(skb);

	EFX_BUG_ON_PARANOID(tx_queue->write_count != tx_queue->insert_count);

	tso_start(&state, skb);

	/* Assume that skb header area contains exactly the headers, and
	 * all payload is in the frag list.
	 */
	if (skb_headlen(skb) == state.header_len) {
		/* Grab the first payload fragment. */
		EFX_BUG_ON_PARANOID(skb_shinfo(skb)->nr_frags < 1);
		frag_i = 0;
		rc = tso_get_fragment(&state, efx,
				      skb_shinfo(skb)->frags + frag_i);
		if (rc)
			goto mem_err;
	} else {
		rc = tso_get_head_fragment(&state, efx, skb);
		if (rc)
			goto mem_err;
		frag_i = -1;
	}

	if (tso_start_new_packet(tx_queue, skb, &state) < 0)
		goto mem_err;

	while (1) {
		rc = tso_fill_packet_with_fragment(tx_queue, skb, &state);
		if (unlikely(rc))
			goto stop;

		/* Move onto the next fragment? */
		if (state.in_len == 0) {
			if (++frag_i >= skb_shinfo(skb)->nr_frags)
				/* End of payload reached. */
				break;
			rc = tso_get_fragment(&state, efx,
					      skb_shinfo(skb)->frags + frag_i);
			if (rc)
				goto mem_err;
		}

		/* Start at new packet? */
		if (state.packet_space == 0 &&
		    tso_start_new_packet(tx_queue, skb, &state) < 0)
			goto mem_err;
	}

	/* Pass off to hardware */
	efx_nic_push_buffers(tx_queue);

	tx_queue->tso_bursts++;
	return NETDEV_TX_OK;

 mem_err:
	EFX_ERR(efx, "Out of memory for TSO headers, or PCI mapping error\n");
	dev_kfree_skb_any(skb);
	goto unwind;

 stop:
	rc2 = NETDEV_TX_BUSY;

	/* Stop the queue if it wasn't stopped before. */
	if (tx_queue->stopped == 1)
		efx_stop_queue(tx_queue->channel);

 unwind:
	/* Free the DMA mapping we were in the process of writing out */
	if (state.unmap_len) {
		if (state.unmap_single)
			pci_unmap_single(efx->pci_dev, state.unmap_addr,
					 state.unmap_len, PCI_DMA_TODEVICE);
		else
			pci_unmap_page(efx->pci_dev, state.unmap_addr,
				       state.unmap_len, PCI_DMA_TODEVICE);
	}

	efx_enqueue_unwind(tx_queue);
	return rc2;
}


static void efx_fini_tso(struct efx_tx_queue *tx_queue)
{
	unsigned i;

	if (tx_queue->buffer) {
		for (i = 0; i <= EFX_TXQ_MASK; ++i)
			efx_tsoh_free(tx_queue, &tx_queue->buffer[i]);
	}

	while (tx_queue->tso_headers_free != NULL)
		efx_tsoh_block_free(tx_queue, tx_queue->tso_headers_free,
				    tx_queue->efx->pci_dev);
}
