#ifdef LX_ATH10K
/*
 * SNOC bus layer for Qualcomm Adrastea / WCN3990 (ath10k) on POCO M8 5G (parrot / SM6475).
 * Bridges Linux's ath10k driver (core, ce, htc, htt, wmi-tlv, mac, qmi) to HakiOS's
 * wifi/lx layer (lx_base.c, lx_80211.c, hakios.lx.c, hakios.wifi.c).
 */
#define LX_QMI_AS_IT_IS
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/skbuff.h>

#include "core.h"
#include "ce.h"
#include "debug.h"
#include "hif.h"
#include "htc.h"
#include "snoc.h"
#include "qmi.h"
#include "../regd.h"

#include <lx_os.h>
#include "lx.h"

#define WCN3990_SNOC_BASE	0x22800000ULL
#define WCN3990_SNOC_SIZE	0x00800000UL
#define WCN3990_MSA_PADDR	0x82a00000ULL
#define WCN3990_MSA_SIZE	0x00300000U
#define CE_POLL_PIPE		4
#define DATA_BUDGET		64

static struct {
	unsigned how;
	bool mission;
	bool closed;
	unsigned reads, writes, said, refused;
	volatile bool looking;
	volatile bool looks;
} snoc_bus;

/* ---- logging ----------------------------------------------------------------------------------- */

static void
ath10k_driver_says(const char *kind, const char *format, va_list list)
{
	char line[320];
	size_t length;

	vsnprintf(line, sizeof(line), format, list);
	length = strlen(line);
	printf("ath10k%s: %s%s", kind, line, length != 0 && line[length - 1] == '\n' ? "" : "\n");
}

void
ath10k_info(struct ath10k *ar, const char *format, ...)
{
	va_list list;

	va_start(list, format);
	ath10k_driver_says("", format, list);
	va_end(list);
}

static void
ath10k_driver_complains(const char *kind, const char *format, va_list list)
{
	char line[320];

	vsnprintf(line, sizeof(line), format, list);
	lx_tell("ath10k %s: %s%s", kind, line, line[0] != 0 && line[strlen(line) - 1] == '\n' ? "" : "\n");
}

void
ath10k_warn(struct ath10k *ar, const char *format, ...)
{
	va_list list;

	va_start(list, format);
	ath10k_driver_complains("WARNS", format, list);
	va_end(list);
}

void
ath10k_err(struct ath10k *ar, const char *format, ...)
{
	va_list list;

	va_start(list, format);
	ath10k_driver_complains("ERROR", format, list);
	va_end(list);
}

void
__ath10k_dbg(struct ath10k *ar, enum ath10k_debug_mask mask, const char *format, ...)
{
	va_list list;

	va_start(list, format);
	ath10k_driver_says(" (debug)", format, list);
	va_end(list);
}

void
ath10k_dbg_dump(struct ath10k *ar, enum ath10k_debug_mask mask, const char *msg, const char *prefix, const void *buf, size_t len)
{
}

void
ath10k_debug_print_hwfw_info(struct ath10k *ar)
{
}

void
ath10k_debug_print_board_info(struct ath10k *ar)
{
}

void
ath10k_debug_print_boot_info(struct ath10k *ar)
{
}

void
ath10k_print_driver_info(struct ath10k *ar)
{
}

void
ath_printk(const char *level, const struct ath_common *common, const char *fmt, ...)
{
	va_list list;

	va_start(list, fmt);
	ath10k_driver_says(" (regd)", fmt, list);
	va_end(list);
}

/* ---- Copy Engine tables for WCN3990 SNOC ------------------------------------------------------ */

static void ath10k_snoc_htc_tx_cb(struct ath10k_ce_pipe *ce_state);
static void ath10k_snoc_htt_tx_cb(struct ath10k_ce_pipe *ce_state);
static void ath10k_snoc_htc_rx_cb(struct ath10k_ce_pipe *ce_state);
static void ath10k_snoc_htt_rx_cb(struct ath10k_ce_pipe *ce_state);
static void ath10k_snoc_htt_htc_rx_cb(struct ath10k_ce_pipe *ce_state);
static void ath10k_snoc_pktlog_rx_cb(struct ath10k_ce_pipe *ce_state);

#define WCN3990_SRC_WR_IDX_OFFSET 0x3C
#define WCN3990_DST_WR_IDX_OFFSET 0x40

static struct ath10k_shadow_reg_cfg target_shadow_reg_cfg_map[] = {
	{ .ce_id = __cpu_to_le16(0),  .reg_offset = __cpu_to_le16(WCN3990_SRC_WR_IDX_OFFSET) },
	{ .ce_id = __cpu_to_le16(3),  .reg_offset = __cpu_to_le16(WCN3990_SRC_WR_IDX_OFFSET) },
	{ .ce_id = __cpu_to_le16(4),  .reg_offset = __cpu_to_le16(WCN3990_SRC_WR_IDX_OFFSET) },
	{ .ce_id = __cpu_to_le16(5),  .reg_offset = __cpu_to_le16(WCN3990_SRC_WR_IDX_OFFSET) },
	{ .ce_id = __cpu_to_le16(7),  .reg_offset = __cpu_to_le16(WCN3990_SRC_WR_IDX_OFFSET) },
	{ .ce_id = __cpu_to_le16(1),  .reg_offset = __cpu_to_le16(WCN3990_DST_WR_IDX_OFFSET) },
	{ .ce_id = __cpu_to_le16(2),  .reg_offset = __cpu_to_le16(WCN3990_DST_WR_IDX_OFFSET) },
	{ .ce_id = __cpu_to_le16(7),  .reg_offset = __cpu_to_le16(WCN3990_DST_WR_IDX_OFFSET) },
	{ .ce_id = __cpu_to_le16(8),  .reg_offset = __cpu_to_le16(WCN3990_DST_WR_IDX_OFFSET) },
	{ .ce_id = __cpu_to_le16(9),  .reg_offset = __cpu_to_le16(WCN3990_DST_WR_IDX_OFFSET) },
	{ .ce_id = __cpu_to_le16(10), .reg_offset = __cpu_to_le16(WCN3990_DST_WR_IDX_OFFSET) },
	{ .ce_id = __cpu_to_le16(11), .reg_offset = __cpu_to_le16(WCN3990_DST_WR_IDX_OFFSET) },
};

static struct ce_attr host_ce_config_wlan[] = {
	/* CE0: host->target HTC control streams */
	{ .flags = CE_ATTR_FLAGS, .src_nentries = 16, .src_sz_max = 2048, .dest_nentries = 0, .send_cb = ath10k_snoc_htc_tx_cb },
	/* CE1: target->host HTT + HTC control */
	{ .flags = CE_ATTR_FLAGS, .src_nentries = 0, .src_sz_max = 2048, .dest_nentries = 512, .recv_cb = ath10k_snoc_htt_htc_rx_cb },
	/* CE2: target->host WMI */
	{ .flags = CE_ATTR_FLAGS, .src_nentries = 0, .src_sz_max = 2048, .dest_nentries = 64, .recv_cb = ath10k_snoc_htc_rx_cb },
	/* CE3: host->target WMI */
	{ .flags = CE_ATTR_FLAGS, .src_nentries = 32, .src_sz_max = 2048, .dest_nentries = 0, .send_cb = ath10k_snoc_htc_tx_cb },
	/* CE4: host->target HTT */
	{ .flags = CE_ATTR_FLAGS | CE_ATTR_DIS_INTR, .src_nentries = 2048, .src_sz_max = 256, .dest_nentries = 0, .send_cb = ath10k_snoc_htt_tx_cb },
	/* CE5: target->host HTT */
	{ .flags = CE_ATTR_FLAGS, .src_nentries = 0, .src_sz_max = 512, .dest_nentries = 512, .recv_cb = ath10k_snoc_htt_rx_cb },
	/* CE6: target autonomous hif_memcpy */
	{ .flags = CE_ATTR_FLAGS, .src_nentries = 0, .src_sz_max = 0, .dest_nentries = 0 },
	/* CE7: ce_diag */
	{ .flags = CE_ATTR_FLAGS, .src_nentries = 2, .src_sz_max = 2048, .dest_nentries = 2 },
	/* CE8: Target to uMC */
	{ .flags = CE_ATTR_FLAGS, .src_nentries = 0, .src_sz_max = 2048, .dest_nentries = 128 },
	/* CE9: target->host HTT */
	{ .flags = CE_ATTR_FLAGS, .src_nentries = 0, .src_sz_max = 2048, .dest_nentries = 512, .recv_cb = ath10k_snoc_htt_htc_rx_cb },
	/* CE10: target->host HTT */
	{ .flags = CE_ATTR_FLAGS, .src_nentries = 0, .src_sz_max = 2048, .dest_nentries = 512, .recv_cb = ath10k_snoc_htt_htc_rx_cb },
	/* CE11: target->host PKTLOG */
	{ .flags = CE_ATTR_FLAGS, .src_nentries = 0, .src_sz_max = 2048, .dest_nentries = 512, .recv_cb = ath10k_snoc_pktlog_rx_cb },
};

static struct ce_pipe_config target_ce_config_wlan[] = {
	{ .pipenum = __cpu_to_le32(0),  .pipedir = __cpu_to_le32(PIPEDIR_OUT),   .nentries = __cpu_to_le32(32),   .nbytes_max = __cpu_to_le32(2048),  .flags = __cpu_to_le32(CE_ATTR_FLAGS) },
	{ .pipenum = __cpu_to_le32(1),  .pipedir = __cpu_to_le32(PIPEDIR_IN),    .nentries = __cpu_to_le32(32),   .nbytes_max = __cpu_to_le32(2048),  .flags = __cpu_to_le32(CE_ATTR_FLAGS) },
	{ .pipenum = __cpu_to_le32(2),  .pipedir = __cpu_to_le32(PIPEDIR_IN),    .nentries = __cpu_to_le32(64),   .nbytes_max = __cpu_to_le32(2048),  .flags = __cpu_to_le32(CE_ATTR_FLAGS) },
	{ .pipenum = __cpu_to_le32(3),  .pipedir = __cpu_to_le32(PIPEDIR_OUT),   .nentries = __cpu_to_le32(32),   .nbytes_max = __cpu_to_le32(2048),  .flags = __cpu_to_le32(CE_ATTR_FLAGS) },
	{ .pipenum = __cpu_to_le32(4),  .pipedir = __cpu_to_le32(PIPEDIR_OUT),   .nentries = __cpu_to_le32(256),  .nbytes_max = __cpu_to_le32(256),   .flags = __cpu_to_le32(CE_ATTR_FLAGS | CE_ATTR_DIS_INTR) },
	{ .pipenum = __cpu_to_le32(5),  .pipedir = __cpu_to_le32(PIPEDIR_OUT),   .nentries = __cpu_to_le32(1024), .nbytes_max = __cpu_to_le32(64),    .flags = __cpu_to_le32(CE_ATTR_FLAGS | CE_ATTR_DIS_INTR) },
	{ .pipenum = __cpu_to_le32(6),  .pipedir = __cpu_to_le32(PIPEDIR_INOUT), .nentries = __cpu_to_le32(32),   .nbytes_max = __cpu_to_le32(16384), .flags = __cpu_to_le32(CE_ATTR_FLAGS) },
	{ .pipenum = __cpu_to_le32(7),  .pipedir = __cpu_to_le32(4),             .nentries = __cpu_to_le32(0),    .nbytes_max = __cpu_to_le32(0),     .flags = __cpu_to_le32(CE_ATTR_FLAGS | CE_ATTR_DIS_INTR) },
	{ .pipenum = __cpu_to_le32(8),  .pipedir = __cpu_to_le32(PIPEDIR_IN),    .nentries = __cpu_to_le32(32),   .nbytes_max = __cpu_to_le32(2048),  .flags = __cpu_to_le32(0) },
	{ .pipenum = __cpu_to_le32(9),  .pipedir = __cpu_to_le32(PIPEDIR_IN),    .nentries = __cpu_to_le32(32),   .nbytes_max = __cpu_to_le32(2048),  .flags = __cpu_to_le32(CE_ATTR_FLAGS) },
	{ .pipenum = __cpu_to_le32(10), .pipedir = __cpu_to_le32(PIPEDIR_IN),    .nentries = __cpu_to_le32(32),   .nbytes_max = __cpu_to_le32(2048),  .flags = __cpu_to_le32(CE_ATTR_FLAGS) },
	{ .pipenum = __cpu_to_le32(11), .pipedir = __cpu_to_le32(PIPEDIR_IN),    .nentries = __cpu_to_le32(32),   .nbytes_max = __cpu_to_le32(2048),  .flags = __cpu_to_le32(CE_ATTR_FLAGS) },
};

static struct ce_service_to_pipe target_service_to_ce_map_wlan[] = {
	{ __cpu_to_le32(ATH10K_HTC_SVC_ID_WMI_DATA_VO),      __cpu_to_le32(PIPEDIR_OUT), __cpu_to_le32(3) },
	{ __cpu_to_le32(ATH10K_HTC_SVC_ID_WMI_DATA_VO),      __cpu_to_le32(PIPEDIR_IN),  __cpu_to_le32(2) },
	{ __cpu_to_le32(ATH10K_HTC_SVC_ID_WMI_DATA_BK),      __cpu_to_le32(PIPEDIR_OUT), __cpu_to_le32(3) },
	{ __cpu_to_le32(ATH10K_HTC_SVC_ID_WMI_DATA_BK),      __cpu_to_le32(PIPEDIR_IN),  __cpu_to_le32(2) },
	{ __cpu_to_le32(ATH10K_HTC_SVC_ID_WMI_DATA_BE),      __cpu_to_le32(PIPEDIR_OUT), __cpu_to_le32(3) },
	{ __cpu_to_le32(ATH10K_HTC_SVC_ID_WMI_DATA_BE),      __cpu_to_le32(PIPEDIR_IN),  __cpu_to_le32(2) },
	{ __cpu_to_le32(ATH10K_HTC_SVC_ID_WMI_DATA_VI),      __cpu_to_le32(PIPEDIR_OUT), __cpu_to_le32(3) },
	{ __cpu_to_le32(ATH10K_HTC_SVC_ID_WMI_DATA_VI),      __cpu_to_le32(PIPEDIR_IN),  __cpu_to_le32(2) },
	{ __cpu_to_le32(ATH10K_HTC_SVC_ID_WMI_CONTROL),      __cpu_to_le32(PIPEDIR_OUT), __cpu_to_le32(3) },
	{ __cpu_to_le32(ATH10K_HTC_SVC_ID_WMI_CONTROL),      __cpu_to_le32(PIPEDIR_IN),  __cpu_to_le32(2) },
	{ __cpu_to_le32(ATH10K_HTC_SVC_ID_RSVD_CTRL),        __cpu_to_le32(PIPEDIR_OUT), __cpu_to_le32(0) },
	{ __cpu_to_le32(ATH10K_HTC_SVC_ID_RSVD_CTRL),        __cpu_to_le32(PIPEDIR_IN),  __cpu_to_le32(2) },
	{ __cpu_to_le32(ATH10K_HTC_SVC_ID_TEST_RAW_STREAMS), __cpu_to_le32(PIPEDIR_OUT), __cpu_to_le32(0) },
	{ __cpu_to_le32(ATH10K_HTC_SVC_ID_TEST_RAW_STREAMS), __cpu_to_le32(PIPEDIR_IN),  __cpu_to_le32(2) },
	{ __cpu_to_le32(ATH10K_HTC_SVC_ID_HTT_DATA_MSG),     __cpu_to_le32(PIPEDIR_OUT), __cpu_to_le32(4) },
	{ __cpu_to_le32(ATH10K_HTC_SVC_ID_HTT_DATA_MSG),     __cpu_to_le32(PIPEDIR_IN),  __cpu_to_le32(1) },
	{ __cpu_to_le32(ATH10K_HTC_SVC_ID_TEST_RAW_STREAMS), __cpu_to_le32(PIPEDIR_OUT), __cpu_to_le32(5) },
	{ __cpu_to_le32(ATH10K_HTC_SVC_ID_HTT_DATA2_MSG),    __cpu_to_le32(PIPEDIR_IN),  __cpu_to_le32(9) },
	{ __cpu_to_le32(ATH10K_HTC_SVC_ID_HTT_DATA3_MSG),    __cpu_to_le32(PIPEDIR_IN),  __cpu_to_le32(10) },
	{ __cpu_to_le32(ATH10K_HTC_SVC_ID_HTT_LOG_MSG),      __cpu_to_le32(PIPEDIR_IN),  __cpu_to_le32(11) },
};

/* ---- Register I/O at 0x22800000 --------------------------------------------------------------- */

static bool
snoc_refused(u32 offset)
{
	bool known = offset < WCN3990_SNOC_SIZE;

	if (known && snoc_bus.mission && !snoc_bus.closed) {
		return (false);
	}
	if (snoc_bus.refused++ < 8) {
		lx_tell("lx: Adrastea register %#x is reached (%s): NOT DONE\n", offset,
		    !known ? "out of range" : snoc_bus.closed ? "after bus stopped" : "outside mission mode");
	}
	return (true);
}

static u32
ath10k_snoc_read32(struct ath10k *ar, u32 offset)
{
	struct ath10k_snoc *ar_snoc = ath10k_snoc_priv(ar);
	u32 val = 0;

	if (snoc_refused(offset)) {
		return (0);
	}
	snoc_bus.reads++;
	if (snoc_bus.how >= 2 && ar_snoc->mem != NULL) {
		val = ioread32((u8 *)ar_snoc->mem + offset);
	}
	return (val);
}

static void
ath10k_snoc_write32(struct ath10k *ar, u32 offset, u32 value)
{
	struct ath10k_snoc *ar_snoc = ath10k_snoc_priv(ar);

	if (snoc_refused(offset)) {
		return;
	}
	snoc_bus.writes++;
	if (snoc_bus.how >= 2 && ar_snoc->mem != NULL) {
		__asm __volatile("dsb sy");
		iowrite32(value, (u8 *)ar_snoc->mem + offset);
	}
}

/* ---- Copy Engine Polling Thread & Callbacks --------------------------------------------------- */

/*
 * Poll a Copy Engine pipe via DDR RRI only (never touching unshadowed CE MMIO
 * registers like HOST_IS, which fault the AXI/NoC bus if WCN3990 is asleep).
 */
static void
snoc_service_pipe(struct ath10k *ar, unsigned int ce_id)
{
	struct ath10k_ce *ce = ath10k_ce_priv(ar);
	struct ath10k_ce_pipe *ce_state = &ce->ce_states[ce_id];

	if (ce_state->recv_cb) {
		ce_state->recv_cb(ce_state);
	}
	if (ce_state->send_cb) {
		ce_state->send_cb(ce_state);
	}
}

static void
snoc_bus_looks(void *argument)
{
	struct ath10k *ar = argument;
	unsigned long long said = lx_os_nanoseconds();

	snoc_bus.looks = true;
	while (snoc_bus.looking) {
		bool more = false;
		int done;

		if (lx_os_nanoseconds() - said > 300000000000ull) {
			said = lx_os_nanoseconds();
			lx_station_say();
			lx_memory_say();
		}
		for (unsigned i = 0; i < CE_COUNT; i++) {
			snoc_service_pipe(ar, i);
		}
		done = ath10k_htt_txrx_compl_task(ar, DATA_BUDGET);
		more |= done >= DATA_BUDGET;
		if (!more) {
			lx_pause_ms(1);
		}
	}
	snoc_bus.looks = false;
}

static void
snoc_bus_quiet(void)
{
	snoc_bus.looking = false;
	while (snoc_bus.looks) {
		lx_pause_ms(1);
	}
}

static int
__ath10k_snoc_rx_post_buf(struct ath10k_snoc_pipe *pipe)
{
	struct ath10k_ce_pipe *ce_pipe = pipe->ce_hdl;
	struct ath10k *ar = pipe->hif_ce_state;
	struct ath10k_ce *ce = ath10k_ce_priv(ar);
	struct sk_buff *skb;
	dma_addr_t paddr;
	int ret;

	skb = dev_alloc_skb(pipe->buf_sz);
	if (!skb) {
		return (-ENOMEM);
	}
	paddr = dma_map_single(ar->dev, skb->data, skb->len + skb_tailroom(skb), DMA_FROM_DEVICE);
	if (unlikely(dma_mapping_error(ar->dev, paddr))) {
		dev_kfree_skb_any(skb);
		return (-EIO);
	}
	ATH10K_SKB_RXCB(skb)->paddr = paddr;
	spin_lock_bh(&ce->ce_lock);
	ret = ce_pipe->ops->ce_rx_post_buf(ce_pipe, skb, paddr);
	spin_unlock_bh(&ce->ce_lock);
	if (ret) {
		dma_unmap_single(ar->dev, paddr, skb->len + skb_tailroom(skb), DMA_FROM_DEVICE);
		dev_kfree_skb_any(skb);
		return (ret);
	}
	return (0);
}

static void
ath10k_snoc_rx_post_pipe(struct ath10k_snoc_pipe *pipe)
{
	struct ath10k *ar = pipe->hif_ce_state;
	struct ath10k_ce *ce = ath10k_ce_priv(ar);
	struct ath10k_snoc *ar_snoc = ath10k_snoc_priv(ar);
	struct ath10k_ce_pipe *ce_pipe = pipe->ce_hdl;
	int ret, num;

	if (pipe->buf_sz == 0 || !ce_pipe->dest_ring) {
		return;
	}
	spin_lock_bh(&ce->ce_lock);
	num = __ath10k_ce_rx_num_free_bufs(ce_pipe);
	spin_unlock_bh(&ce->ce_lock);
	while (num--) {
		ret = __ath10k_snoc_rx_post_buf(pipe);
		if (ret) {
			if (ret != -ENOSPC) {
				mod_timer(&ar_snoc->rx_post_retry, jiffies + 50);
			}
			break;
		}
	}
}

static void
ath10k_snoc_rx_post(struct ath10k *ar)
{
	struct ath10k_snoc *ar_snoc = ath10k_snoc_priv(ar);

	for (int i = 0; i < CE_COUNT; i++) {
		ath10k_snoc_rx_post_pipe(&ar_snoc->pipe_info[i]);
	}
}

static void
ath10k_snoc_process_rx_cb(struct ath10k_ce_pipe *ce_state, void (*callback)(struct ath10k *ar, struct sk_buff *skb))
{
	struct ath10k *ar = ce_state->ar;
	struct ath10k_snoc *ar_snoc = ath10k_snoc_priv(ar);
	struct ath10k_snoc_pipe *pipe_info = &ar_snoc->pipe_info[ce_state->id];
	struct sk_buff *skb;
	struct sk_buff_head list;
	void *transfer_context;
	unsigned int nbytes, max_nbytes;

	__skb_queue_head_init(&list);
	while (ath10k_ce_completed_recv_next(ce_state, &transfer_context, &nbytes) == 0) {
		skb = transfer_context;
		max_nbytes = skb->len + skb_tailroom(skb);
		dma_unmap_single(ar->dev, ATH10K_SKB_RXCB(skb)->paddr, max_nbytes, DMA_FROM_DEVICE);
		if (unlikely(max_nbytes < nbytes)) {
			dev_kfree_skb_any(skb);
			continue;
		}
		skb_put(skb, nbytes);
		__skb_queue_tail(&list, skb);
	}
	while ((skb = __skb_dequeue(&list))) {
		callback(ar, skb);
	}
	ath10k_snoc_rx_post_pipe(pipe_info);
}

static void
ath10k_snoc_htc_rx_cb(struct ath10k_ce_pipe *ce_state)
{
	ath10k_snoc_process_rx_cb(ce_state, ath10k_htc_rx_completion_handler);
}

static void
ath10k_snoc_htt_htc_rx_cb(struct ath10k_ce_pipe *ce_state)
{
	snoc_service_pipe(ce_state->ar, CE_POLL_PIPE);
	ath10k_snoc_process_rx_cb(ce_state, ath10k_htc_rx_completion_handler);
}

static void
ath10k_snoc_pktlog_rx_cb(struct ath10k_ce_pipe *ce_state)
{
	ath10k_snoc_process_rx_cb(ce_state, ath10k_htc_rx_completion_handler);
}

static void
ath10k_snoc_htt_rx_deliver(struct ath10k *ar, struct sk_buff *skb)
{
	skb_pull(skb, sizeof(struct ath10k_htc_hdr));
	ath10k_htt_t2h_msg_handler(ar, skb);
}

static void
ath10k_snoc_htt_rx_cb(struct ath10k_ce_pipe *ce_state)
{
	snoc_service_pipe(ce_state->ar, CE_POLL_PIPE);
	ath10k_snoc_process_rx_cb(ce_state, ath10k_snoc_htt_rx_deliver);
}

static void
ath10k_snoc_rx_replenish_retry(struct timer_list *t)
{
	struct ath10k_snoc *ar_snoc = timer_container_of(ar_snoc, t, rx_post_retry);

	ath10k_snoc_rx_post(ar_snoc->ar);
}

static void
ath10k_snoc_htc_tx_cb(struct ath10k_ce_pipe *ce_state)
{
	struct ath10k *ar = ce_state->ar;
	struct sk_buff_head list;
	struct sk_buff *skb;

	__skb_queue_head_init(&list);
	while (ath10k_ce_completed_send_next(ce_state, (void **)&skb) == 0) {
		if (!skb) {
			continue;
		}
		__skb_queue_tail(&list, skb);
	}
	while ((skb = __skb_dequeue(&list))) {
		ath10k_htc_tx_completion_handler(ar, skb);
	}
}

static void
ath10k_snoc_htt_tx_cb(struct ath10k_ce_pipe *ce_state)
{
	struct ath10k *ar = ce_state->ar;
	struct sk_buff *skb;

	while (ath10k_ce_completed_send_next(ce_state, (void **)&skb) == 0) {
		if (!skb) {
			continue;
		}
		dma_unmap_single(ar->dev, ATH10K_SKB_CB(skb)->paddr, skb->len, DMA_TO_DEVICE);
		ath10k_htt_hif_tx_complete(ar, skb);
	}
}

static int
ath10k_snoc_hif_tx_sg(struct ath10k *ar, u8 pipe_id, struct ath10k_hif_sg_item *items, int n_items)
{
	struct ath10k_snoc *ar_snoc = ath10k_snoc_priv(ar);
	struct ath10k_ce *ce = ath10k_ce_priv(ar);
	struct ath10k_snoc_pipe *snoc_pipe = &ar_snoc->pipe_info[pipe_id];
	struct ath10k_ce_pipe *ce_pipe = snoc_pipe->ce_hdl;
	int err, i = 0;

	spin_lock_bh(&ce->ce_lock);
	for (i = 0; i < n_items - 1; i++) {
		err = ath10k_ce_send_nolock(ce_pipe, items[i].transfer_context, items[i].paddr, items[i].len,
		    items[i].transfer_id, CE_SEND_FLAG_GATHER);
		if (err) {
			goto err;
		}
	}
	err = ath10k_ce_send_nolock(ce_pipe, items[i].transfer_context, items[i].paddr, items[i].len,
	    items[i].transfer_id, 0);
	if (err) {
		goto err;
	}
	spin_unlock_bh(&ce->ce_lock);
	return (0);
err:
	for (; i > 0; i--) {
		__ath10k_ce_send_revert(ce_pipe);
	}
	spin_unlock_bh(&ce->ce_lock);
	return (err);
}

static int
ath10k_snoc_hif_get_target_info(struct ath10k *ar, struct bmi_target_info *target_info)
{
	target_info->version = ATH10K_HW_WCN3990;
	target_info->type = ATH10K_HW_WCN3990;
	return (0);
}

static u16
ath10k_snoc_hif_get_free_queue_number(struct ath10k *ar, u8 pipe)
{
	struct ath10k_snoc *ar_snoc = ath10k_snoc_priv(ar);

	return (ath10k_ce_num_free_src_entries(ar_snoc->pipe_info[pipe].ce_hdl));
}

static void
ath10k_snoc_hif_send_complete_check(struct ath10k *ar, u8 pipe, int force)
{
	if (!force) {
		int resources = ath10k_snoc_hif_get_free_queue_number(ar, pipe);

		if (resources > (host_ce_config_wlan[pipe].src_nentries >> 1)) {
			return;
		}
	}
	snoc_service_pipe(ar, pipe);
}

static int
ath10k_snoc_hif_map_service_to_pipe(struct ath10k *ar, u16 service_id, u8 *ul_pipe, u8 *dl_pipe)
{
	const struct ce_service_to_pipe *entry;
	bool ul_set = false, dl_set = false;

	for (int i = 0; i < ARRAY_SIZE(target_service_to_ce_map_wlan); i++) {
		entry = &target_service_to_ce_map_wlan[i];
		if (__le32_to_cpu(entry->service_id) != service_id) {
			continue;
		}
		switch (__le32_to_cpu(entry->pipedir)) {
		case PIPEDIR_IN:
			*dl_pipe = __le32_to_cpu(entry->pipenum);
			dl_set = true;
			break;
		case PIPEDIR_OUT:
			*ul_pipe = __le32_to_cpu(entry->pipenum);
			ul_set = true;
			break;
		case PIPEDIR_INOUT:
			*dl_pipe = __le32_to_cpu(entry->pipenum);
			*ul_pipe = __le32_to_cpu(entry->pipenum);
			dl_set = true;
			ul_set = true;
			break;
		}
	}
	return ((!ul_set || !dl_set) ? -ENOENT : 0);
}

static void
ath10k_snoc_hif_get_default_pipe(struct ath10k *ar, u8 *ul_pipe, u8 *dl_pipe)
{
	(void)ath10k_snoc_hif_map_service_to_pipe(ar, ATH10K_HTC_SVC_ID_RSVD_CTRL, ul_pipe, dl_pipe);
}

static void
ath10k_snoc_rx_pipe_cleanup(struct ath10k_snoc_pipe *snoc_pipe)
{
	struct ath10k_ce_pipe *ce_pipe = snoc_pipe->ce_hdl;
	struct ath10k_ce_ring *ce_ring = ce_pipe->dest_ring;
	struct ath10k *ar = snoc_pipe->hif_ce_state;
	struct sk_buff *skb;

	if (!ce_ring || !snoc_pipe->buf_sz) {
		return;
	}
	for (int i = 0; i < ce_ring->nentries; i++) {
		skb = ce_ring->per_transfer_context[i];
		if (!skb) {
			continue;
		}
		ce_ring->per_transfer_context[i] = NULL;
		dma_unmap_single(ar->dev, ATH10K_SKB_RXCB(skb)->paddr, skb->len + skb_tailroom(skb), DMA_FROM_DEVICE);
		dev_kfree_skb_any(skb);
	}
}

static void
ath10k_snoc_tx_pipe_cleanup(struct ath10k_snoc_pipe *snoc_pipe)
{
	struct ath10k_ce_pipe *ce_pipe = snoc_pipe->ce_hdl;
	struct ath10k_ce_ring *ce_ring = ce_pipe->src_ring;
	struct ath10k *ar = snoc_pipe->hif_ce_state;
	struct sk_buff *skb;

	if (!ce_ring || !snoc_pipe->buf_sz) {
		return;
	}
	for (int i = 0; i < ce_ring->nentries; i++) {
		skb = ce_ring->per_transfer_context[i];
		if (!skb) {
			continue;
		}
		ce_ring->per_transfer_context[i] = NULL;
		ath10k_htc_tx_completion_handler(ar, skb);
	}
}

static void
ath10k_snoc_buffer_cleanup(struct ath10k *ar)
{
	struct ath10k_snoc *ar_snoc = ath10k_snoc_priv(ar);

	timer_delete_sync(&ar_snoc->rx_post_retry);
	for (int pipe_num = 0; pipe_num < CE_COUNT; pipe_num++) {
		struct ath10k_snoc_pipe *pipe_info = &ar_snoc->pipe_info[pipe_num];

		ath10k_snoc_rx_pipe_cleanup(pipe_info);
		ath10k_snoc_tx_pipe_cleanup(pipe_info);
	}
}

static int
ath10k_snoc_hif_start(struct ath10k *ar)
{
	struct ath10k_snoc *ar_snoc = ath10k_snoc_priv(ar);

	snoc_bus.closed = false;
	bitmap_clear(ar_snoc->pending_ce_irqs, 0, CE_COUNT_MAX);
	ath10k_snoc_rx_post(ar);
	clear_bit(ATH10K_SNOC_FLAG_RECOVERY, &ar_snoc->flags);
	snoc_bus.looking = true;
	if (lx_os_thread("wifi-rings", snoc_bus_looks, ar) != 0) {
		snoc_bus.looking = false;
		return (-ENOMEM);
	}
	lx_tell("lx: Adrastea SNOC Copy Engines are set up and polling thread started\n");
	return (0);
}

static void
ath10k_snoc_hif_stop(struct ath10k *ar)
{
	snoc_bus_quiet();
	snoc_bus.closed = true;
	ath10k_snoc_buffer_cleanup(ar);
	printf("lx: Adrastea SNOC bus stopped (%u reads, %u writes)\n", snoc_bus.reads, snoc_bus.writes);
}

static int
ath10k_snoc_init_pipes(struct ath10k *ar)
{
	int i, ret;

	for (i = 0; i < CE_COUNT; i++) {
		ret = ath10k_ce_init_pipe(ar, i, &host_ce_config_wlan[i]);
		if (ret) {
			return (ret);
		}
	}
	return (0);
}

static int
ath10k_snoc_wlan_enable(struct ath10k *ar, enum ath10k_firmware_mode fw_mode)
{
	struct ath10k_tgt_pipe_cfg tgt_cfg[CE_COUNT_MAX];
	struct ath10k_qmi_wlan_enable_cfg cfg;
	enum wlfw_driver_mode_enum_v01 mode;

	for (int pipe_num = 0; pipe_num < CE_COUNT_MAX; pipe_num++) {
		tgt_cfg[pipe_num].pipe_num = target_ce_config_wlan[pipe_num].pipenum;
		tgt_cfg[pipe_num].pipe_dir = target_ce_config_wlan[pipe_num].pipedir;
		tgt_cfg[pipe_num].nentries = target_ce_config_wlan[pipe_num].nentries;
		tgt_cfg[pipe_num].nbytes_max = target_ce_config_wlan[pipe_num].nbytes_max;
		tgt_cfg[pipe_num].flags = target_ce_config_wlan[pipe_num].flags;
		tgt_cfg[pipe_num].reserved = 0;
	}
	cfg.num_ce_tgt_cfg = sizeof(target_ce_config_wlan) / sizeof(struct ath10k_tgt_pipe_cfg);
	cfg.ce_tgt_cfg = (struct ath10k_tgt_pipe_cfg *)&tgt_cfg;
	cfg.num_ce_svc_pipe_cfg = sizeof(target_service_to_ce_map_wlan) / sizeof(struct ath10k_svc_pipe_cfg);
	cfg.ce_svc_cfg = (struct ath10k_svc_pipe_cfg *)&target_service_to_ce_map_wlan;
	cfg.num_shadow_reg_cfg = ARRAY_SIZE(target_shadow_reg_cfg_map);
	cfg.shadow_reg_cfg = (struct ath10k_shadow_reg_cfg *)&target_shadow_reg_cfg_map;

	mode = (fw_mode == ATH10K_FIRMWARE_MODE_UTF) ? QMI_WLFW_FTM_V01 : QMI_WLFW_MISSION_V01;
	return (ath10k_qmi_wlan_enable(ar, &cfg, mode, NULL));
}

static void
ath10k_snoc_wlan_disable(struct ath10k *ar)
{
	struct ath10k_snoc *ar_snoc = ath10k_snoc_priv(ar);

	if (!test_bit(ATH10K_FLAG_CRASH_FLUSH, &ar->dev_flags) || !test_bit(ATH10K_SNOC_FLAG_RECOVERY, &ar_snoc->flags)) {
		ath10k_qmi_wlan_disable(ar);
	}
}

static void
ath10k_snoc_hif_power_down(struct ath10k *ar)
{
	snoc_bus_quiet();
	ath10k_snoc_wlan_disable(ar);
	ath10k_ce_free_rri(ar);
}

static int
ath10k_snoc_hif_power_up(struct ath10k *ar, enum ath10k_firmware_mode fw_mode)
{
	int ret;

	ret = ath10k_snoc_wlan_enable(ar, fw_mode);
	if (ret) {
		ath10k_err(ar, "failed to enable wcn3990: %d\n", ret);
		return (ret);
	}
	snoc_bus.closed = false;
	ath10k_ce_alloc_rri(ar);
	ret = ath10k_snoc_init_pipes(ar);
	if (ret) {
		ath10k_ce_free_rri(ar);
		ath10k_snoc_wlan_disable(ar);
		return (ret);
	}
	ath10k_ce_disable_interrupts(ar);
	return (0);
}

static int
ath10k_snoc_hif_set_target_log_mode(struct ath10k *ar, u8 fw_log_mode)
{
	u8 fw_dbg_mode = fw_log_mode ? ATH10K_ENABLE_FW_LOG_CE : ATH10K_ENABLE_FW_LOG_DIAG;

	return (ath10k_qmi_set_fw_log_mode(ar, fw_dbg_mode));
}

static const struct ath10k_hif_ops ath10k_snoc_hif_ops = {
	.read32			= ath10k_snoc_read32,
	.write32		= ath10k_snoc_write32,
	.start			= ath10k_snoc_hif_start,
	.stop			= ath10k_snoc_hif_stop,
	.map_service_to_pipe	= ath10k_snoc_hif_map_service_to_pipe,
	.get_default_pipe	= ath10k_snoc_hif_get_default_pipe,
	.power_up		= ath10k_snoc_hif_power_up,
	.power_down		= ath10k_snoc_hif_power_down,
	.tx_sg			= ath10k_snoc_hif_tx_sg,
	.send_complete_check	= ath10k_snoc_hif_send_complete_check,
	.get_free_queue_number	= ath10k_snoc_hif_get_free_queue_number,
	.get_target_info	= ath10k_snoc_hif_get_target_info,
	.set_target_log_mode	= ath10k_snoc_hif_set_target_log_mode,
};

static const struct ath10k_bus_ops ath10k_snoc_bus_ops = {
	.read32			= ath10k_snoc_read32,
	.write32		= ath10k_snoc_write32,
};

void
ath10k_snoc_fw_crashed_dump(struct ath10k *ar)
{
}

int
ath10k_snoc_fw_indication(struct ath10k *ar, u64 type)
{
	struct ath10k_snoc *ar_snoc = ath10k_snoc_priv(ar);
	struct ath10k_bus_params bus_params = {};
	int ret;

	if (test_bit(ATH10K_SNOC_FLAG_UNREGISTERING, &ar_snoc->flags)) {
		return (0);
	}
	switch (type) {
	case ATH10K_QMI_EVENT_FW_READY_IND:
		if (test_bit(ATH10K_SNOC_FLAG_REGISTERED, &ar_snoc->flags)) {
			break;
		}
		bus_params.dev_type = ATH10K_DEV_TYPE_LL;
		bus_params.chip_id = ar_snoc->target_info.soc_version;
		ret = ath10k_core_register(ar, &bus_params);
		if (ret) {
			ath10k_err(ar, "Failed to register driver core: %d\n", ret);
			return (ret);
		}
		set_bit(ATH10K_SNOC_FLAG_REGISTERED, &ar_snoc->flags);
		break;
	case ATH10K_QMI_EVENT_FW_DOWN_IND:
		set_bit(ATH10K_SNOC_FLAG_RECOVERY, &ar_snoc->flags);
		set_bit(ATH10K_FLAG_CRASH_FLUSH, &ar->dev_flags);
		break;
	default:
		return (-EINVAL);
	}
	return (0);
}

/* ---- QMI intercept for Adrastea --------------------------------------------------------------- */

static void
send_regdb_before_bdf(struct qmi_handle *qmi, struct sockaddr_qrtr *sq)
{
	static struct wlfw_bdf_download_req_msg_v01 req;
	struct wlfw_bdf_download_resp_msg_v01 resp;
	struct qmi_txn txn;
	const void *data = NULL;
	unsigned long size = 0, remaining;
	const u8 *temp;
	int ret;

	if (lx_os_firmware("regdb.bin", &data, &size) != 0 || data == NULL || size == 0) {
		return;
	}
	temp = data;
	remaining = size;
	bzero(&req, sizeof(req));
	while (remaining) {
		req.valid = 1;
		req.file_id_valid = 1;
		req.file_id = 0;
		req.total_size_valid = 1;
		req.total_size = (u32)size;
		req.seg_id_valid = 1;
		req.data_valid = 1;
		req.end_valid = 1;
		req.bdf_type_valid = 1;
		req.bdf_type = 4;	/* BDF_TYPE_REGDB */
		if (remaining > QMI_WLFW_MAX_DATA_SIZE_V01) {
			req.data_len = QMI_WLFW_MAX_DATA_SIZE_V01;
			req.end = 0;
		} else {
			req.data_len = (u32)remaining;
			req.end = 1;
		}
		memcpy(req.data, temp, req.data_len);
		bzero(&resp, sizeof(resp));
		ret = qmi_txn_init(qmi, &txn, wlfw_bdf_download_resp_msg_v01_ei, &resp);
		if (ret < 0) {
			break;
		}
		ret = qmi_send_request(qmi, sq, &txn, QMI_WLFW_BDF_DOWNLOAD_REQ_V01,
		    WLFW_BDF_DOWNLOAD_REQ_MSG_V01_MAX_MSG_LEN, wlfw_bdf_download_req_msg_v01_ei, &req);
		if (ret < 0) {
			qmi_txn_cancel(&txn);
			break;
		}
		ret = qmi_txn_wait(&txn, 10 * HZ);
		if (ret < 0) {
			break;
		}
		remaining -= req.data_len;
		temp += req.data_len;
		req.seg_id++;
	}
	printf("lx: regdb.bin (%lu bytes) sent to WLFW before board data\n", size);
}

int
lx_qmi_send_request(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, int msg_id, size_t largest,
    const struct qmi_elem_info *ei, const void *structure)
{
	if (msg_id == QMI_WLFW_BDF_DOWNLOAD_REQ_V01) {
		struct wlfw_bdf_download_req_msg_v01 *req = (struct wlfw_bdf_download_req_msg_v01 *)(uintptr_t)structure;

		if (req->seg_id == 0 && !req->bdf_type_valid) {
			send_regdb_before_bdf(qmi, sq);
		}
		req->bdf_type_valid = 1;
		req->bdf_type = 0;
	}
	if (msg_id == QMI_WLFW_WLAN_MODE_REQ_V01) {
		u32 mode = ((const struct wlfw_wlan_mode_req_msg_v01 *)structure)->mode;

		if (mode != QMI_WLFW_MISSION_V01) {
			snoc_bus_quiet();
		}
		snoc_bus.mission = mode == QMI_WLFW_MISSION_V01;
		lx_tell("lx: the firmware is asked for mode %u (0: mission, 4: off)\n", mode);
	}
	return (qmi_send_request(qmi, sq, txn, msg_id, largest, ei, structure));
}

/* ---- Device Tree & Platform stubs ------------------------------------------------------------- */

static const struct property snoc_props[] = {
	{ .name = "qcom,msa-fixed-perm", .length = 0, .value = "" },
	{ .name = "qcom,no-msa-ready-indicator", .length = 0, .value = "" },
};

const struct property *
of_find_property(const struct device_node *np, const char *name, int *lenp)
{
	for (unsigned i = 0; i < ARRAY_SIZE(snoc_props); i++) {
		if (strcmp(name, snoc_props[i].name) == 0) {
			if (lenp != NULL) {
				*lenp = snoc_props[i].length;
			}
			return (&snoc_props[i]);
		}
	}
	return (NULL);
}

const void *
of_get_property(const struct device_node *np, const char *name, int *lenp)
{
	const struct property *prop = of_find_property(np, name, lenp);

	return (prop != NULL ? prop->value : NULL);
}

int
of_property_read_string(const struct device_node *np, const char *name, const char **out)
{
	return (-EINVAL);
}

bool
of_machine_is_compatible(const char *compatible)
{
	return (false);
}

int
of_reserved_mem_region_to_resource(const struct device_node *np, unsigned int index, struct resource *res)
{
	return (-ENODEV);
}

int
linuxkpi_device_get_mac_address(struct device *dev, char *address)
{
	return (lx_os_radio_address((unsigned char *)address) ? 0 : -ENOENT);
}

/* ---- Entries: lx_start, lx_prepare, lx_stop, lx_transmit -------------------------------------- */

static struct ath10k *volatile snoc_driver;

int
lx_prepare(void)
{
	return (lx_base_init() != 0 || lx_pool_init() != 0 ? ENOMEM : 0);
}

int
lx_start(void)
{
	static struct device device;
	static struct device_node node;
	struct ath10k *ar;
	struct ath10k_snoc *ar_snoc;
	struct ath10k_ce *ce;
	unsigned long long began;
	bool up;
	int ret;

	snoc_bus.how = lx_os_option("wifi_lx");
	if (snoc_bus.how == 0) {
		snoc_bus.how = 2;
	}
	if (lx_prepare() != 0) {
		lx_tell("lx: no thread, or no memory the radio can be given\n");
		return (ENOMEM);
	}
	if (!lx_os_rproc_boot() == 0) {
		/* rproc is already booted by hakios.wifi.c step 11 */
	}
	if (lx_os_option("wifi_quiet")) {
		ath10k_debug_mask = 0;
	} else {
		ath10k_debug_mask = ATH10K_DBG_QMI | ATH10K_DBG_BOOT | ATH10K_DBG_SNOC | ATH10K_DBG_WMI | ATH10K_DBG_MAC | ATH10K_DBG_REGULATORY;
	}
	ath10k_frame_mode = ATH10K_HW_TXRX_ETHERNET;
	device.of_node = &node;
	device.kobj.name = "wifi";

	ar = ath10k_core_create(sizeof(*ar_snoc), &device, ATH10K_BUS_SNOC, ATH10K_HW_WCN3990, &ath10k_snoc_hif_ops);
	if (ar == NULL) {
		return (ENOMEM);
	}
	ar_snoc = ath10k_snoc_priv(ar);
	ar_snoc->ar = ar;
	ar_snoc->ce.bus_ops = &ath10k_snoc_bus_ops;
	ar->ce_priv = &ar_snoc->ce;
	ar_snoc->mem_pa = WCN3990_SNOC_BASE;
	ar_snoc->mem = lx_os_map_registers(WCN3990_SNOC_BASE, WCN3990_SNOC_SIZE);
	if (ar_snoc->mem == NULL) {
		lx_tell("lx: failed to map Adrastea SNOC registers at %#llx\n", WCN3990_SNOC_BASE);
		return (ENOMEM);
	}
	ar->msa.paddr = WCN3990_MSA_PADDR;
	ar->msa.mem_size = WCN3990_MSA_SIZE;

	ce = ath10k_ce_priv(ar);
	timer_setup(&ar_snoc->rx_post_retry, ath10k_snoc_rx_replenish_retry, 0);
	spin_lock_init(&ce->ce_lock);
	for (int i = 0; i < CE_COUNT; i++) {
		struct ath10k_snoc_pipe *pipe = &ar_snoc->pipe_info[i];

		pipe->ce_hdl = &ce->ce_states[i];
		pipe->pipe_num = i;
		pipe->hif_ce_state = ar;
		ret = ath10k_ce_alloc_pipe(ar, i, &host_ce_config_wlan[i]);
		if (ret) {
			lx_tell("lx: failed to allocate CE pipe %d: %d\n", i, ret);
			return (-ret);
		}
		pipe->buf_sz = host_ce_config_wlan[i].src_sz_max;
	}

	printf("lx: Adrastea driver (\"wcn3990 hw1.0\") is set up; QMI exchange begins\n");
	ret = ath10k_qmi_init(ar, WCN3990_MSA_SIZE);
	if (ret) {
		lx_tell("lx: ath10k_qmi_init failed: %d\n", ret);
		return (-ret);
	}

	began = lx_os_nanoseconds();
	while (!test_bit(ATH10K_FLAG_CORE_REGISTERED, &ar->dev_flags) &&
	    lx_os_nanoseconds() - began < 60000000000ull) {
		lx_pause_ms(10);
	}
	up = test_bit(ATH10K_FLAG_CORE_REGISTERED, &ar->dev_flags);
	lx_tell("lx: %llu ms after start the Adrastea driver %s; dev_flags: %#lx\n",
	    (lx_os_nanoseconds() - began) / 1000000ull,
	    up ? "IS REGISTERED with mac80211" : "DID NOT REGISTER", ar->dev_flags);
	if (!up) {
		ath10k_qmi_deinit(ar);
		return (ETIMEDOUT);
	}

	snoc_driver = ar;
	if (lx_os_option("wifi_station") || lx_os_option("wifi")) {
		if ((ret = lx_station_offer()) == 0) {
			return (0);
		}
	} else if (lx_os_option("wifi_join")) {
		if ((ret = lx_station_join()) == 0) {
			if (lx_os_option("wifi_hold")) {
				lx_pause_ms(1000 * lx_os_option("wifi_hold"));
				lx_station_say();
			}
			return (0);
		}
	} else {
		ret = lx_os_option("wifi_scan") ? lx_station_scan() : 0;
		if (ret == 0 && lx_os_option("wifi_hold")) {
			lx_tell("lx: holding radio in mission mode for %u s\n", lx_os_option("wifi_hold"));
			lx_pause_ms(1000 * lx_os_option("wifi_hold"));
			lx_tell("lx: hold complete\n");
		}
	}
	lx_stop();
	return (ret);
}

void
lx_stop(void)
{
	struct ath10k *ar = snoc_driver;

	if (ar == NULL) {
		return;
	}
	snoc_driver = NULL;
	lx_station_withdraw();
	lx_station_leave();
	ath10k_core_unregister(ar);
	ath10k_qmi_deinit(ar);
	lx_tell("lx: Adrastea driver is down again\n");
}

int
lx_transmit(const unsigned char *frame, unsigned long length)
{
	return (snoc_driver != NULL ? lx_station_send(frame, length) : ENETDOWN);
}
#endif

