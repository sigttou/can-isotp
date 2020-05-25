/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/*
 * isotp.c - ISO 15765-2 CAN transport protocol for protocol family CAN
 *
 * WARNING: This is ALPHA code for discussions and first tests that should
 *          not be used in production environments.
 *
 * In the discussion the Socket-API to the userspace or the ISO-TP socket
 * options or the return values we may change! Current behaviour:
 *
 * - no ISO-TP specific return values are provided to the userspace
 * - when a transfer (tx) is on the run the next write() blocks until it's done
 * - no support for sending wait frames to the data source in the rx path
 *
 * Copyright (c) 2018 Volkswagen Group Electronic Research
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of Volkswagen nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * Alternatively, provided that this notice is retained in full, this
 * software may be distributed under the terms of the GNU General
 * Public License ("GPL") version 2, in which case the provisions of the
 * GPL apply INSTEAD OF those given above.
 *
 * The provided data structures and external interfaces from this code
 * are not restricted to be used by modules with a GPL compatible license.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * Send feedback to <linux-can@vger.kernel.org>
 *
 */

#include <linux/module.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/hrtimer.h>
#include <linux/wait.h>
#include <linux/uio.h>
#include <linux/net.h>
#include <linux/netdevice.h>
#include <linux/socket.h>
#include <linux/if_arp.h>
#include <linux/skbuff.h>
#include <linux/can.h>
#include <linux/can/core.h>
#include <linux/can/skb.h>
#include <linux/can/isotp.h>
#include <net/sock.h>
#include <net/net_namespace.h>

#define CAN_ISOTP_VERSION "20200115"
static __initdata const char banner[] =
	KERN_INFO "can: isotp protocol (rev " CAN_ISOTP_VERSION " alpha)\n";

MODULE_DESCRIPTION("PF_CAN isotp 15765-2:2016 protocol");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Oliver Hartkopp <socketcan@hartkopp.net>");
MODULE_ALIAS("can-proto-6");

#if LINUX_VERSION_CODE < KERNEL_VERSION(5,4,0)
#error This module needs Kernel 5.4 or newer
#endif

#define DBG(fmt, args...) (printk( KERN_DEBUG "can-isotp: %s: " fmt, \
				   __func__, ##args))
#undef DBG
#define DBG(fmt, args...)

#define SINGLE_MASK(id) ((id & CAN_EFF_FLAG) ? \
			 (CAN_EFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG) : \
			 (CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG))

/*
  ISO 15765-2:2016 supports more than 4095 byte per ISO PDU as the FF_DL can
  take full 32 bit values (4 Gbyte). We would need some good concept to handle
  this between user space and kernel space. For now increase the static buffer
  to something about 8 kbyte to be able to test this new functionality.
*/
#define MAX_MSG_LENGTH 8200

/* N_PCI type values in bits 7-4 of N_PCI bytes */
#define N_PCI_SF	0x00 /* single frame */
#define N_PCI_FF	0x10 /* first frame */
#define N_PCI_CF	0x20 /* consecutive frame */
#define N_PCI_FC	0x30 /* flow control */

#define N_PCI_SZ 1	/* size of the PCI byte #1 */
#define SF_PCI_SZ4 1	/* size of SingleFrame PCI including 4 bit SF_DL */
#define SF_PCI_SZ8 2	/* size of SingleFrame PCI including 8 bit SF_DL */
#define FF_PCI_SZ12 2	/* size of FirstFrame PCI including 12 bit FF_DL */
#define FF_PCI_SZ32 6	/* size of FirstFrame PCI including 32 bit FF_DL */
#define FC_CONTENT_SZ 3	/* flow control content size in byte (FS/BS/STmin) */

#define ISOTP_CHECK_PADDING (CAN_ISOTP_CHK_PAD_LEN | CAN_ISOTP_CHK_PAD_DATA)

/* Flow Status given in FC frame */
#define ISOTP_FC_CTS	0	/* clear to send */
#define ISOTP_FC_WT	1	/* wait */
#define ISOTP_FC_OVFLW	2	/* overflow */

enum {
	ISOTP_IDLE = 0,
	ISOTP_WAIT_FIRST_FC,
	ISOTP_WAIT_FC,
	ISOTP_WAIT_DATA,
	ISOTP_SENDING
};

struct tpcon {
	int idx;
	int len;
	u8  state;
	u8  bs;
	u8  sn;
	u8  ll_dl;
	u8  buf[MAX_MSG_LENGTH+1];
};

struct isotp_sock {
	struct sock sk;
	int bound;
	int ifindex;
	canid_t txid;
	canid_t rxid;
	ktime_t tx_gap;
	ktime_t lastrxcf_tstamp;
	struct hrtimer rxtimer, txtimer;
	struct can_isotp_options opt;
	struct can_isotp_fc_options rxfc, txfc;
	struct can_isotp_ll_options ll;
	__u32 force_tx_stmin;
	__u32 force_rx_stmin;
	struct tpcon rx, tx;
	struct notifier_block notifier;
	wait_queue_head_t wait;
};

static inline struct isotp_sock *isotp_sk(const struct sock *sk)
{
	return (struct isotp_sock *)sk;
}

static enum hrtimer_restart isotp_rx_timer_handler(struct hrtimer *hrtimer)
{
	struct isotp_sock *so = container_of(hrtimer, struct isotp_sock,
					     rxtimer);
	if (so->rx.state == ISOTP_WAIT_DATA) {
		DBG("we did not get new data frames in time.\n");

		/* reset tx state */
		so->rx.state = ISOTP_IDLE;
	}

	return HRTIMER_NORESTART;
}

static int isotp_send_fc(struct sock *sk, int ae, u8 flowstatus)
{
	struct net_device *dev;
	struct sk_buff *nskb;
	struct canfd_frame *ncf;
	struct isotp_sock *so = isotp_sk(sk);
	int can_send_ret;

	nskb = alloc_skb(so->ll.mtu + sizeof(struct can_skb_priv), gfp_any());
	if (!nskb)
		return 1;

	dev = dev_get_by_index(sock_net(sk), so->ifindex);
	if (!dev) {
		kfree_skb(nskb);
		return 1;
	}

	can_skb_reserve(nskb);
	can_skb_prv(nskb)->ifindex = dev->ifindex;
	can_skb_prv(nskb)->skbcnt = 0;

	nskb->dev = dev;
	can_skb_set_owner(nskb, sk);
	ncf = (struct canfd_frame *) nskb->data;
	skb_put(nskb, so->ll.mtu);

	/* create & send flow control reply */
	ncf->can_id = so->txid;

	if (so->opt.flags & CAN_ISOTP_TX_PADDING) {
		memset(ncf->data, so->opt.txpad_content, CAN_MAX_DLEN);
		ncf->len = CAN_MAX_DLEN;
	} else
		ncf->len = ae + FC_CONTENT_SZ;

	ncf->data[ae] = N_PCI_FC | flowstatus;
	ncf->data[ae + 1] = so->rxfc.bs;
	ncf->data[ae + 2] = so->rxfc.stmin;

	if (ae)
		ncf->data[0] = so->opt.ext_address;

	if (so->ll.mtu == CANFD_MTU)
		ncf->flags = so->ll.tx_flags;

	can_send_ret = can_send(nskb, 1);
	if (can_send_ret)
		printk(KERN_CRIT "can-isotp: %s: can_send_ret %d\n",
		       __func__, can_send_ret);

	dev_put(dev);

	/* reset blocksize counter */
	so->rx.bs = 0;

	/* reset last CF frame rx timestamp for rx stmin enforcement */
	so->lastrxcf_tstamp = ktime_set(0,0);

	/* start rx timeout watchdog */
	hrtimer_start(&so->rxtimer, ktime_set(1,0), HRTIMER_MODE_REL_SOFT);
	return 0;
}

static void isotp_rcv_skb(struct sk_buff *skb, struct sock *sk)
{
	struct sockaddr_can *addr = (struct sockaddr_can *)skb->cb;

	BUILD_BUG_ON(sizeof(skb->cb) < sizeof(struct sockaddr_can));

	memset(addr, 0, sizeof(*addr));
	addr->can_family  = AF_CAN;
	addr->can_ifindex = skb->dev->ifindex;

	if (sock_queue_rcv_skb(sk, skb) < 0)
		kfree_skb(skb);
}

static u8 padlen(u8 datalen)
{
	const u8 plen[] = {8,  8,  8,  8,  8,  8,  8,  8,  8,	/* 0 - 8 */
			   12, 12, 12, 12,			/* 9 - 12 */
			   16, 16, 16, 16,			/* 13 - 16 */
			   20, 20, 20, 20,			/* 17 - 20 */
			   24, 24, 24, 24,			/* 21 - 24 */
			   32, 32, 32, 32, 32, 32, 32, 32,	/* 25 - 32 */
			   48, 48, 48, 48, 48, 48, 48, 48,	/* 33 - 40 */
			   48, 48, 48, 48, 48, 48, 48, 48};	/* 41 - 48 */

	if (datalen > 48)
		return 64;

	return plen[datalen];
}

/* check for length optimization and return 1/true when the check fails */
static int check_optimized(struct canfd_frame *cf, int start_index)
{
	/*
	 * for CAN_DL <= 8 the start_index is equal to the CAN_DL as the
	 * padding would start at this point. E.g. if the padding would
	 * start at cf.data[7] cf->len has to be 7 to be optimal.
	 * Note: The data[] index starts with zero.
	 */
	if (cf->len <= CAN_MAX_DLEN)
		return (cf->len != start_index);

	/*
	 * This relation is also valid in the non-linear DLC range, where
	 * we need to take care of the minimal next possible CAN_DL.
	 * The correct check would be (padlen(cf->len) != padlen(start_index)).
	 * But as cf->len can only take discrete values from 12, .., 64 at this
	 * point the padlen(cf->len) is always equal to cf->len.
	 */
	return (cf->len != padlen(start_index));
}

/* check padding and return 1/true when the check fails */
static int check_pad(struct isotp_sock *so, struct canfd_frame *cf,
		     int start_index, __u8 content)
{
	int i;

	/* no RX_PADDING value => check length of optimized frame length */
	if (!(so->opt.flags & CAN_ISOTP_RX_PADDING)) {

		if (so->opt.flags & CAN_ISOTP_CHK_PAD_LEN)
			return check_optimized(cf, start_index);

		/* no valid test against empty value => ignore frame */
		return 1;
	}

	/* check datalength of correctly padded CAN frame */
	if ((so->opt.flags & CAN_ISOTP_CHK_PAD_LEN) &&
	    cf->len != padlen(cf->len))
			return 1;

	/* check padding content */
	if (so->opt.flags & CAN_ISOTP_CHK_PAD_DATA) {
		for (i = start_index; i < cf->len; i++)
			if (cf->data[i] != content)
				return 1;
	}
	return 0;
}

static int isotp_rcv_fc(struct isotp_sock *so, struct canfd_frame *cf, int ae)
{
	if (so->tx.state != ISOTP_WAIT_FC &&
	    so->tx.state != ISOTP_WAIT_FIRST_FC)
		return 0;

	hrtimer_cancel(&so->txtimer);

	if ((cf->len < ae + FC_CONTENT_SZ) ||
	    ((so->opt.flags & ISOTP_CHECK_PADDING) &&
	     check_pad(so, cf, ae + FC_CONTENT_SZ, so->opt.rxpad_content))) {
		so->tx.state = ISOTP_IDLE;
		wake_up_interruptible(&so->wait);
		return 1;
	}

	/* get communication parameters only from the first FC frame */
	if (so->tx.state == ISOTP_WAIT_FIRST_FC) {

		so->txfc.bs = cf->data[ae + 1];
		so->txfc.stmin = cf->data[ae + 2];

		/* fix wrong STmin values according spec */
		if ((so->txfc.stmin > 0x7F) &&
		    ((so->txfc.stmin < 0xF1) || (so->txfc.stmin > 0xF9)))
			so->txfc.stmin = 0x7F;

		so->tx_gap = ktime_set(0,0);
		/* add transmission time for CAN frame N_As */
		so->tx_gap = ktime_add_ns(so->tx_gap, so->opt.frame_txtime);
		/* add waiting time for consecutive frames N_Cs */
		if (so->opt.flags & CAN_ISOTP_FORCE_TXSTMIN)
			so->tx_gap = ktime_add_ns(so->tx_gap,
						  so->force_tx_stmin);
		else if (so->txfc.stmin < 0x80)
			so->tx_gap = ktime_add_ns(so->tx_gap,
						  so->txfc.stmin * 1000000);
		else
			so->tx_gap = ktime_add_ns(so->tx_gap,
						  (so->txfc.stmin - 0xF0)
						  * 100000);
		so->tx.state = ISOTP_WAIT_FC;
	}

	DBG("FC frame: FS %d, BS %d, STmin 0x%02X, tx_gap %lld\n",
	    cf->data[ae] & 0x0F & 0x0F, so->txfc.bs, so->txfc.stmin,
	    (long long)so->tx_gap);

	switch (cf->data[ae] & 0x0F) {

	case ISOTP_FC_CTS:
		so->tx.bs = 0;
		so->tx.state = ISOTP_SENDING;
		DBG("starting txtimer for sending\n");
		/* start cyclic timer for sending CF frame */
		hrtimer_start(&so->txtimer, so->tx_gap,
			      HRTIMER_MODE_REL_SOFT);
		break;

	case ISOTP_FC_WT:
		DBG("starting waiting for next FC\n");
		/* start timer to wait for next FC frame */
		hrtimer_start(&so->txtimer, ktime_set(1,0),
			      HRTIMER_MODE_REL_SOFT);
		break;

	case ISOTP_FC_OVFLW:
		DBG("overflow in receiver side\n");

	default:
		/* stop this tx job. TODO: error reporting? */
		so->tx.state = ISOTP_IDLE;
		wake_up_interruptible(&so->wait);
	}
	return 0;
}

static int isotp_rcv_sf(struct sock *sk, struct canfd_frame *cf, int pcilen,
			struct sk_buff *skb, int len)
{
	struct isotp_sock *so = isotp_sk(sk);
	struct sk_buff *nskb;

	hrtimer_cancel(&so->rxtimer);
	so->rx.state = ISOTP_IDLE;

	if (!len || len > cf->len - pcilen)
		return 1;

	if ((so->opt.flags & ISOTP_CHECK_PADDING) &&
	    check_pad(so, cf, pcilen + len, so->opt.rxpad_content))
		return 1;

	nskb = alloc_skb(len, gfp_any());
	if (!nskb)
		return 1;

	memcpy(skb_put(nskb, len), &cf->data[pcilen], len);

	nskb->tstamp = skb->tstamp;
	nskb->dev = skb->dev;
	isotp_rcv_skb(nskb, sk);
	return 0;
}

static int isotp_rcv_ff(struct sock *sk, struct canfd_frame *cf, int ae)
{
	struct isotp_sock *so = isotp_sk(sk);
	int i;
	int off;
	int ff_pci_sz;

	hrtimer_cancel(&so->rxtimer);
	so->rx.state = ISOTP_IDLE;

	/* get the used sender LL_DL from the (first) CAN frame data length */
	so->rx.ll_dl = padlen(cf->len);

	/* the first frame has to use the entire frame up to LL_DL length */
	if (cf->len != so->rx.ll_dl)
		return 1;

	/* get the FF_DL */
	so->rx.len = (cf->data[ae] & 0x0F) << 8;
	so->rx.len += cf->data[ae + 1];

	/* Check for FF_DL escape sequence supporting 32 bit PDU length */
	if (so->rx.len)
		ff_pci_sz = FF_PCI_SZ12;
	else {
		/* FF_DL = 0 => get real length from next 4 bytes */
		so->rx.len = cf->data[ae + 2] << 24;
		so->rx.len += cf->data[ae + 3] << 16;
		so->rx.len += cf->data[ae + 4] << 8;
		so->rx.len += cf->data[ae + 5];
		ff_pci_sz = FF_PCI_SZ32;
	}

	/* take care of a potential SF_DL ESC offset for TX_DL > 8 */
	off = (so->rx.ll_dl > CAN_MAX_DLEN)? 1:0;

	if (so->rx.len + ae + off + ff_pci_sz < so->rx.ll_dl)
		return 1;

	if (so->rx.len > MAX_MSG_LENGTH) {
		/* send FC frame with overflow status */
		isotp_send_fc(sk, ae, ISOTP_FC_OVFLW);
		return 1;
	}

	/* copy the first received data bytes */
	so->rx.idx = 0;
	for (i = ae + ff_pci_sz; i < so->rx.ll_dl; i++)
		so->rx.buf[so->rx.idx++] = cf->data[i];

	/* initial setup for this pdu receiption */
	so->rx.sn = 1;
	so->rx.state = ISOTP_WAIT_DATA;

	/* no creation of flow control frames */
	if (so->opt.flags & CAN_ISOTP_LISTEN_MODE)
		return 0;

	/* send our first FC frame */
	isotp_send_fc(sk, ae, ISOTP_FC_CTS);
	return 0;
}

static int isotp_rcv_cf(struct sock *sk, struct canfd_frame *cf, int ae,
			struct sk_buff *skb)
{
	struct isotp_sock *so = isotp_sk(sk);
	struct sk_buff *nskb;
	int i;

	if (so->rx.state != ISOTP_WAIT_DATA)
		return 0;

	/* drop if timestamp gap is less than force_rx_stmin nano secs */
	if (so->opt.flags & CAN_ISOTP_FORCE_RXSTMIN) {

		if (ktime_to_ns(ktime_sub(skb->tstamp, so->lastrxcf_tstamp)) <
		    so->force_rx_stmin)
			return 0;

		so->lastrxcf_tstamp = skb->tstamp;
	}

	hrtimer_cancel(&so->rxtimer);

	/* CFs are never longer than the FF */
	if (cf->len > so->rx.ll_dl)
		return 1;

	/* CFs have usually the LL_DL length */
	if (cf->len < so->rx.ll_dl) {
		/* this is only allowed for the last CF */
		if (so->rx.len - so->rx.idx > so->rx.ll_dl - ae - N_PCI_SZ)
			return 1;
	}

	if ((cf->data[ae] & 0x0F) != so->rx.sn) {
		DBG("wrong sn %d. expected %d.\n",
		    cf->data[ae] & 0x0F, so->rx.sn);
		/* some error reporting? */
		so->rx.state = ISOTP_IDLE;
		return 1;
	}
	so->rx.sn++;
	so->rx.sn %= 16;

	for (i = ae + N_PCI_SZ; i < cf->len; i++) {
		so->rx.buf[so->rx.idx++] = cf->data[i];
		if (so->rx.idx >= so->rx.len)
			break;
	}

	if (so->rx.idx >= so->rx.len) {

		/* we are done */
		so->rx.state = ISOTP_IDLE;

		if ((so->opt.flags & ISOTP_CHECK_PADDING) &&
		    check_pad(so, cf, i+1, so->opt.rxpad_content))
			return 1;

		nskb = alloc_skb(so->rx.len, gfp_any());
		if (!nskb)
			return 1;

		memcpy(skb_put(nskb, so->rx.len), so->rx.buf,
		       so->rx.len);

		nskb->tstamp = skb->tstamp;
		nskb->dev = skb->dev;
		isotp_rcv_skb(nskb, sk);
		return 0;
	}

	/* no creation of flow control frames */
	if (so->opt.flags & CAN_ISOTP_LISTEN_MODE)
		return 0;

	/* perform blocksize handling, if enabled */
	if (!so->rxfc.bs || ++so->rx.bs < so->rxfc.bs) {

		/* start rx timeout watchdog */
		hrtimer_start(&so->rxtimer, ktime_set(1,0),
			      HRTIMER_MODE_REL_SOFT);
		return 0;
	}

	/* we reached the specified blocksize so->rxfc.bs */
	isotp_send_fc(sk, ae, ISOTP_FC_CTS);
	return 0;
}

static void isotp_rcv(struct sk_buff *skb, void *data)
{
	struct sock *sk = (struct sock *)data;
	struct isotp_sock *so = isotp_sk(sk);
	struct canfd_frame *cf;
	int ae = (so->opt.flags & CAN_ISOTP_EXTEND_ADDR)? 1:0;
	u8 n_pci_type, sf_dl;

	BUG_ON(skb->len != CAN_MTU && skb->len != CANFD_MTU);

	/*
	 * Strictly receive only frames with the configured MTU size
	 * => clear separation of CAN2.0 / CAN FD transport channels
	 */
	if (skb->len != so->ll.mtu)
		return;

	cf = (struct canfd_frame *) skb->data;

	/* if enabled: check receiption of my configured extended address */
	if (ae && cf->data[0] != so->opt.rx_ext_address)
		return;

	n_pci_type = cf->data[ae] & 0xF0;

	if (so->opt.flags & CAN_ISOTP_HALF_DUPLEX) {
		/* check rx/tx path half duplex expectations */
		if ((so->tx.state != ISOTP_IDLE && n_pci_type != N_PCI_FC) ||
		    (so->rx.state != ISOTP_IDLE && n_pci_type == N_PCI_FC))
			return;
	}

	switch (n_pci_type) {
	case N_PCI_FC:
		/* tx path: flow control frame containing the FC parameters */
		isotp_rcv_fc(so, cf, ae);
		break;

	case N_PCI_SF:
		/*
		 * rx path: single frame
		 *
		 * As we do not have a rx.ll_dl configuration, we can only test
		 * if the CAN frames payload length matches the LL_DL == 8
		 * requirements - no matter if it's CAN 2.0 or CAN FD
		 */

		/* get the SF_DL from the N_PCI byte */
		sf_dl = cf->data[ae] & 0x0F;

		if (cf->len <= CAN_MAX_DLEN)
			isotp_rcv_sf(sk, cf, SF_PCI_SZ4 + ae, skb, sf_dl);
		else if (skb->len == CANFD_MTU) {
			/*
			 * We have a CAN FD frame and CAN_DL is greater than 8:
			 * Only frames with the SF_DL == 0 ESC value are valid.
			 *
			 * If so take care of the increased SF PCI size
			 * (SF_PCI_SZ8) to point to the message content behind
			 * the extended SF PCI info and get the real SF_DL
			 * length value from the formerly first data byte.
			 */
			if (sf_dl == 0)
				isotp_rcv_sf(sk, cf, SF_PCI_SZ8 + ae, skb,
					     cf->data[SF_PCI_SZ4 + ae]);
		}
		break;

	case N_PCI_FF:
		/* rx path: first frame */
		isotp_rcv_ff(sk, cf, ae);
		break;

	case N_PCI_CF:
		/* rx path: consecutive frame */
		isotp_rcv_cf(sk, cf, ae, skb);
		break;

	}
}

static void isotp_fill_dataframe(struct canfd_frame *cf, struct isotp_sock *so,
				 int ae, int off)
{
	int pcilen = N_PCI_SZ + ae + off;
	int space = so->tx.ll_dl - pcilen;
	int num = min_t(int, so->tx.len - so->tx.idx, space);
	int i;

	cf->can_id = so->txid;
	cf->len = num + pcilen;

	if (num < space) {
		if (so->opt.flags & CAN_ISOTP_TX_PADDING) {
			/* user requested padding */
			cf->len = padlen(cf->len);
			memset(cf->data, so->opt.txpad_content, cf->len);
		} else if (cf->len > CAN_MAX_DLEN) {
			/* mandatory padding for CAN FD frames */
			cf->len = padlen(cf->len);
			memset(cf->data, CAN_ISOTP_DEFAULT_PAD_CONTENT,
			       cf->len);
		}
	}

	for (i = 0; i < num; i++)
		cf->data[pcilen + i] = so->tx.buf[so->tx.idx++];

	if (ae)
		cf->data[0] = so->opt.ext_address;
}

static void isotp_create_fframe(struct canfd_frame *cf, struct isotp_sock *so,
				int ae)
{
	int i;
	int ff_pci_sz;

	cf->can_id = so->txid;
	cf->len = so->tx.ll_dl;
	if (ae)
		cf->data[0] = so->opt.ext_address;

	/* create N_PCI bytes with 12/32 bit FF_DL data length */
	if (so->tx.len > 4095) {
		/* use 32 bit FF_DL notation */
		cf->data[ae] = N_PCI_FF;
		cf->data[ae + 1] = 0;
		cf->data[ae + 2] = (u8) (so->tx.len >> 24) & 0xFFU;
		cf->data[ae + 3] = (u8) (so->tx.len >> 16) & 0xFFU;
		cf->data[ae + 4] = (u8) (so->tx.len >> 8) & 0xFFU;
		cf->data[ae + 5] = (u8) so->tx.len & 0xFFU;
		ff_pci_sz = FF_PCI_SZ32;
	} else {
		/* use 12 bit FF_DL notation */
		cf->data[ae] = (u8) (so->tx.len>>8) | N_PCI_FF;
		cf->data[ae + 1] = (u8) so->tx.len & 0xFFU;
		ff_pci_sz = FF_PCI_SZ12;
	}

	/* add first data bytes depending on ae */
	for (i = ae + ff_pci_sz; i < so->tx.ll_dl; i++)
		cf->data[i] = so->tx.buf[so->tx.idx++];

	so->tx.sn = 1;
	so->tx.state = ISOTP_WAIT_FIRST_FC;
}

static enum hrtimer_restart isotp_tx_timer_handler(struct hrtimer *hrtimer)
{
	struct isotp_sock *so = container_of(hrtimer, struct isotp_sock,
					     txtimer);
	struct sock *sk = &so->sk;
	struct sk_buff *skb;
	struct net_device *dev;
	struct canfd_frame *cf;
	enum hrtimer_restart restart = HRTIMER_NORESTART;
	int can_send_ret;
	int ae = (so->opt.flags & CAN_ISOTP_EXTEND_ADDR)? 1:0;

	switch (so->tx.state) {

	case ISOTP_WAIT_FC:
	case ISOTP_WAIT_FIRST_FC:

		/* we did not get any flow control frame in time */

		DBG("we did not get FC frame in time.\n");
		/* report 'communication error on send' */
		sk->sk_err = ECOMM;
		if (!sock_flag(sk, SOCK_DEAD))
			sk->sk_error_report(sk);

		/* reset tx state */
		so->tx.state = ISOTP_IDLE;
		wake_up_interruptible(&so->wait);
		break;

	case ISOTP_SENDING:

		/* push out the next segmented pdu */

		DBG("next pdu to send.\n");

		dev = dev_get_by_index(sock_net(sk), so->ifindex);
		if (!dev)
			break;

isotp_tx_burst:
		skb = alloc_skb(so->ll.mtu + sizeof(struct can_skb_priv),
				gfp_any());
		if (!skb) {
			dev_put(dev);
			break;
		}

		can_skb_reserve(skb);
		can_skb_prv(skb)->ifindex = dev->ifindex;
		can_skb_prv(skb)->skbcnt = 0;

		cf = (struct canfd_frame *)skb->data;
		skb_put(skb, so->ll.mtu);

		/* create consecutive frame */
		isotp_fill_dataframe(cf, so, ae, 0);

		/* place consecutive frame N_PCI in appropriate index */
		cf->data[ae] = N_PCI_CF | so->tx.sn++;
		so->tx.sn %= 16;
		so->tx.bs++;

		if (so->ll.mtu == CANFD_MTU)
			cf->flags = so->ll.tx_flags;

		skb->dev = dev;
		can_skb_set_owner(skb, sk);

		can_send_ret = can_send(skb, 1);
		if (can_send_ret)
			printk(KERN_CRIT "can-isotp: %s: can_send_ret %d\n",
			       __func__, can_send_ret);

		if (so->tx.idx >= so->tx.len) {
			/* we are done */
			DBG("we are done\n");
			so->tx.state = ISOTP_IDLE;
			dev_put(dev);
			wake_up_interruptible(&so->wait);
			break;
		}

		if (so->txfc.bs && so->tx.bs >= so->txfc.bs) {
			/* stop and wait for FC */
			DBG("BS stop and wait for FC\n");
			so->tx.state = ISOTP_WAIT_FC;
			dev_put(dev);
			hrtimer_set_expires(&so->txtimer,
					    ktime_add(ktime_get(),
						      ktime_set(1,0)));
			restart = HRTIMER_RESTART;
			break;
		}

		/* no gap between data frames needed => use burst mode */
		if (!so->tx_gap)
			goto isotp_tx_burst;

		/* start timer to send next data frame with correct delay */
		dev_put(dev);
		hrtimer_set_expires(&so->txtimer,
				    ktime_add(ktime_get(), so->tx_gap));
		restart = HRTIMER_RESTART;
		break;

	default:
		BUG_ON(1);
	}

	return restart;
}

static int isotp_sendmsg(struct socket *sock, struct msghdr *msg, size_t size)
{
	struct sock *sk = sock->sk;
	struct isotp_sock *so = isotp_sk(sk);
	struct sk_buff *skb;
	struct net_device *dev;
	struct canfd_frame *cf;
	int ae = (so->opt.flags & CAN_ISOTP_EXTEND_ADDR)? 1:0;
	int off;
	int err;

	if (!so->bound)
		return -EADDRNOTAVAIL;

	/* we do not support multiple buffers - for now */
	if (so->tx.state != ISOTP_IDLE) {
		if (msg->msg_flags & MSG_DONTWAIT)
			return -EAGAIN;

		/* wait for complete transmission of current pdu */
		wait_event_interruptible(so->wait, so->tx.state == ISOTP_IDLE);
	}

	if (!size || size > MAX_MSG_LENGTH)
		return -EINVAL;

	err = memcpy_from_msg(so->tx.buf, msg, size);
	if (err < 0)
		return err;

	dev = dev_get_by_index(sock_net(sk), so->ifindex);
	if (!dev)
		return -ENXIO;

	skb = sock_alloc_send_skb(sk, so->ll.mtu + sizeof(struct can_skb_priv),
				  msg->msg_flags & MSG_DONTWAIT, &err);
	if (!skb) {
		dev_put(dev);
		return err;
	}

	can_skb_reserve(skb);
	can_skb_prv(skb)->ifindex = dev->ifindex;
	can_skb_prv(skb)->skbcnt = 0;

	so->tx.state = ISOTP_SENDING;
	so->tx.len = size;
	so->tx.idx = 0;

	cf = (struct canfd_frame *)skb->data;
	skb_put(skb, so->ll.mtu);

	/* take care of a potential SF_DL ESC offset for TX_DL > 8 */
	off = (so->tx.ll_dl > CAN_MAX_DLEN)? 1:0;

	/* check for single frame transmission depending on TX_DL */
	if (size <= so->tx.ll_dl - SF_PCI_SZ4 - ae - off) {

		/*
		 * The message size generally fits into a SingleFrame - good.
		 *
		 * SF_DL ESC offset optimization:
		 *
		 * When TX_DL is greater 8 but the message would still fit
		 * into a 8 byte CAN frame, we can omit the offset.
		 * This prevents a protocol caused length extension from
		 * CAN_DL = 8 to CAN_DL = 12 due to the SF_SL ESC handling.
		 */
		if (size <= CAN_MAX_DLEN - SF_PCI_SZ4 - ae)
			off = 0;

		isotp_fill_dataframe(cf, so, ae, off);

		/* place single frame N_PCI w/o length in appropriate index */
		cf->data[ae] = N_PCI_SF;

		/* place SF_DL size value depending on the SF_DL ESC offset */
		if (off)
			cf->data[SF_PCI_SZ4 + ae] = size;
		else
			cf->data[ae] |= size;

		so->tx.state = ISOTP_IDLE;
		wake_up_interruptible(&so->wait);
	} else {
		/* send first frame and wait for FC */

		isotp_create_fframe(cf, so, ae);

		DBG("starting txtimer for fc\n");
		/* start timeout for FC */
		hrtimer_start(&so->txtimer, ktime_set(1,0), HRTIMER_MODE_REL_SOFT);
	}

	/* send the first or only CAN frame */
	if (so->ll.mtu == CANFD_MTU)
		cf->flags = so->ll.tx_flags;

	skb->dev = dev;
	skb->sk  = sk;
	err = can_send(skb, 1);
	dev_put(dev);
	if (err) {
		printk(KERN_CRIT "can-isotp: %s: can_send_ret %d\n",
		       __func__, err);
		return err;
	}

	return size;
}

static int isotp_recvmsg(struct socket *sock, struct msghdr *msg, size_t size,
			 int flags)
{
	struct sock *sk = sock->sk;
	struct sk_buff *skb;
	int err = 0;
	int noblock;

	noblock =  flags & MSG_DONTWAIT;
	flags   &= ~MSG_DONTWAIT;

	skb = skb_recv_datagram(sk, flags, noblock, &err);
	if (!skb)
		return err;

	if (size < skb->len)
		msg->msg_flags |= MSG_TRUNC;
	else
		size = skb->len;

	err = memcpy_to_msg(msg, skb->data, size);
	if (err < 0) {
		skb_free_datagram(sk, skb);
		return err;
	}

	sock_recv_timestamp(msg, sk, skb);

	if (msg->msg_name) {
		msg->msg_namelen = sizeof(struct sockaddr_can);
		memcpy(msg->msg_name, skb->cb, msg->msg_namelen);
	}

	skb_free_datagram(sk, skb);

	return size;
}

static int isotp_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct isotp_sock *so;
	struct net *net;

	if (!sk)
		return 0;

	so = isotp_sk(sk);
	net = sock_net(sk);

	/* wait for complete transmission of current pdu */
	wait_event_interruptible(so->wait, so->tx.state == ISOTP_IDLE);

	unregister_netdevice_notifier(&so->notifier);

	lock_sock(sk);

	hrtimer_cancel(&so->txtimer);
	hrtimer_cancel(&so->rxtimer);

	/* remove current filters & unregister */
	if (so->bound) {
		if (so->ifindex) {
			struct net_device *dev;

			dev = dev_get_by_index(net, so->ifindex);
			if (dev) {
				can_rx_unregister(net, dev, so->rxid,
						  SINGLE_MASK(so->rxid),
						  isotp_rcv, sk);
				dev_put(dev);
			}
		}
	}

	so->ifindex = 0;
	so->bound   = 0;

	sock_orphan(sk);
	sock->sk = NULL;

	release_sock(sk);
	sock_put(sk);

	return 0;
}

static int isotp_bind(struct socket *sock, struct sockaddr *uaddr, int len)
{
	struct sockaddr_can *addr = (struct sockaddr_can *)uaddr;
	struct sock *sk = sock->sk;
	struct isotp_sock *so = isotp_sk(sk);
	struct net *net = sock_net(sk);
	int ifindex;
	struct net_device *dev;
	int err = 0;
	int notify_enetdown = 0;

	if (len < CAN_REQUIRED_SIZE(struct sockaddr_can, can_addr.tp))
		return -EINVAL;

	if (addr->can_addr.tp.rx_id == addr->can_addr.tp.tx_id)
		return -EADDRNOTAVAIL;

	if ((addr->can_addr.tp.rx_id | addr->can_addr.tp.tx_id) &
	    (CAN_ERR_FLAG | CAN_RTR_FLAG))
		return -EADDRNOTAVAIL;

	if (!addr->can_ifindex)
		return -ENODEV;

	lock_sock(sk);

	if (so->bound && addr->can_ifindex == so->ifindex &&
	    addr->can_addr.tp.rx_id == so->rxid &&
	    addr->can_addr.tp.tx_id == so->txid)
		goto out;

	dev = dev_get_by_index(net, addr->can_ifindex);
	if (!dev) {
		err = -ENODEV;
		goto out;
	}
	if (dev->type != ARPHRD_CAN) {
		dev_put(dev);
		err = -ENODEV;
		goto out;
	}
	if (dev->mtu < so->ll.mtu) {
		dev_put(dev);
		err = -EINVAL;
		goto out;
	}
	if (!(dev->flags & IFF_UP))
		notify_enetdown = 1;

	ifindex = dev->ifindex;

	can_rx_register(net, dev, addr->can_addr.tp.rx_id,
			SINGLE_MASK(addr->can_addr.tp.rx_id), isotp_rcv, sk,
			"isotp", sk);

	dev_put(dev);

	if (so->bound) {
		/* unregister old filter */
		if (so->ifindex) {
			dev = dev_get_by_index(net, so->ifindex);
			if (dev) {
				can_rx_unregister(net, dev, so->rxid,
						  SINGLE_MASK(so->rxid),
						  isotp_rcv, sk);
				dev_put(dev);
			}
		}
	}

	/* switch to new settings */
	so->ifindex = ifindex;
	so->rxid = addr->can_addr.tp.rx_id;
	so->txid = addr->can_addr.tp.tx_id;
	so->bound = 1;

out:
	release_sock(sk);

	if (notify_enetdown) {
		sk->sk_err = ENETDOWN;
		if (!sock_flag(sk, SOCK_DEAD))
			sk->sk_error_report(sk);
	}

	return err;
}

static int isotp_getname(struct socket *sock, struct sockaddr *uaddr, int peer)
{
	struct sockaddr_can *addr = (struct sockaddr_can *)uaddr;
	struct sock *sk = sock->sk;
	struct isotp_sock *so = isotp_sk(sk);

	if (peer)
		return -EOPNOTSUPP;

	addr->can_family  = AF_CAN;
	addr->can_ifindex = so->ifindex;
	addr->can_addr.tp.rx_id = so->rxid;
	addr->can_addr.tp.tx_id = so->txid;

	return sizeof(*addr);
}

static int isotp_setsockopt(struct socket *sock, int level, int optname,
			    char __user *optval, unsigned int optlen)
{
	struct sock *sk = sock->sk;
	struct isotp_sock *so = isotp_sk(sk);
	int ret = 0;

	if (level != SOL_CAN_ISOTP)
		return -EINVAL;
	if (optlen < 0)
		return -EINVAL;

	switch (optname) {

	case CAN_ISOTP_OPTS:
		if (optlen != sizeof(struct can_isotp_options))
			return -EINVAL;

		if (copy_from_user(&so->opt, optval, optlen))
			return -EFAULT;

		/* no separate rx_ext_address is given => use ext_address */
		if (!(so->opt.flags & CAN_ISOTP_RX_EXT_ADDR))
			so->opt.rx_ext_address = so->opt.ext_address;
		break;

	case CAN_ISOTP_RECV_FC:
		if (optlen != sizeof(struct can_isotp_fc_options))
			return -EINVAL;

		if (copy_from_user(&so->rxfc, optval, optlen))
			return -EFAULT;
		break;

	case CAN_ISOTP_TX_STMIN:
		if (optlen != sizeof(__u32))
			return -EINVAL;

		if (copy_from_user(&so->force_tx_stmin, optval, optlen))
			return -EFAULT;
		break;

	case CAN_ISOTP_RX_STMIN:
		if (optlen != sizeof(__u32))
			return -EINVAL;

		if (copy_from_user(&so->force_rx_stmin, optval, optlen))
			return -EFAULT;
		break;

	case CAN_ISOTP_LL_OPTS:
		if (optlen != sizeof(struct can_isotp_ll_options))
			return -EINVAL;
		else {
			struct can_isotp_ll_options ll;

			if (copy_from_user(&ll, optval, optlen))
				return -EFAULT;

			/* check for correct ISO 11898-1 DLC data lentgh */
			if (ll.tx_dl != padlen(ll.tx_dl))
				return -EINVAL;

			if (ll.mtu != CAN_MTU && ll.mtu != CANFD_MTU)
				return -EINVAL;

			if (ll.mtu == CAN_MTU && ll.tx_dl > CAN_MAX_DLEN)
				return -EINVAL;

			memcpy(&so->ll, &ll, sizeof(ll));

			/* set ll_dl for tx path to similar place as for rx */
			so->tx.ll_dl = ll.tx_dl;
		}
		break;

	default:
		ret = -ENOPROTOOPT;
	}

	return ret;
}

static int isotp_getsockopt(struct socket *sock, int level, int optname,
			  char __user *optval, int __user *optlen)
{
	struct sock *sk = sock->sk;
	struct isotp_sock *so = isotp_sk(sk);
	int len;
	void *val;

	if (level != SOL_CAN_ISOTP)
		return -EINVAL;
	if (get_user(len, optlen))
		return -EFAULT;
	if (len < 0)
		return -EINVAL;

	switch (optname) {

	case CAN_ISOTP_OPTS:
		len = min_t(int, len, sizeof(struct can_isotp_options));
		val = &so->opt;
		break;

	case CAN_ISOTP_RECV_FC:
		len = min_t(int, len, sizeof(struct can_isotp_fc_options));
		val = &so->rxfc;
		break;

	case CAN_ISOTP_TX_STMIN:
		len = min_t(int, len, sizeof(__u32));
		val = &so->force_tx_stmin;
		break;

	case CAN_ISOTP_RX_STMIN:
		len = min_t(int, len, sizeof(__u32));
		val = &so->force_rx_stmin;
		break;

	case CAN_ISOTP_LL_OPTS:
		len = min_t(int, len, sizeof(struct can_isotp_ll_options));
		val = &so->ll;
		break;

	default:
		return -ENOPROTOOPT;
	}

	if (put_user(len, optlen))
		return -EFAULT;
	if (copy_to_user(optval, val, len))
		return -EFAULT;
	return 0;
}

static int isotp_notifier(struct notifier_block *nb, unsigned long msg,
			  void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct isotp_sock *so = container_of(nb, struct isotp_sock, notifier);
	struct sock *sk = &so->sk;

	if (!net_eq(dev_net(dev), sock_net(sk)))
		return NOTIFY_DONE;

	if (dev->type != ARPHRD_CAN)
		return NOTIFY_DONE;

	if (so->ifindex != dev->ifindex)
		return NOTIFY_DONE;

	switch (msg) {

	case NETDEV_UNREGISTER:
		lock_sock(sk);
		/* remove current filters & unregister */
		if (so->bound)
			can_rx_unregister(dev_net(dev), dev, so->rxid,
					  SINGLE_MASK(so->rxid),
					  isotp_rcv, sk);

		so->ifindex = 0;
		so->bound   = 0;
		release_sock(sk);

		sk->sk_err = ENODEV;
		if (!sock_flag(sk, SOCK_DEAD))
			sk->sk_error_report(sk);
		break;

	case NETDEV_DOWN:
		sk->sk_err = ENETDOWN;
		if (!sock_flag(sk, SOCK_DEAD))
			sk->sk_error_report(sk);
		break;
	}

	return NOTIFY_DONE;
}

static int isotp_init(struct sock *sk)
{
	struct isotp_sock *so = isotp_sk(sk);

	so->ifindex = 0;
	so->bound   = 0;

	so->opt.flags		= CAN_ISOTP_DEFAULT_FLAGS;
	so->opt.ext_address	= CAN_ISOTP_DEFAULT_EXT_ADDRESS;
	so->opt.rx_ext_address	= CAN_ISOTP_DEFAULT_EXT_ADDRESS;
	so->opt.rxpad_content	= CAN_ISOTP_DEFAULT_PAD_CONTENT;
	so->opt.txpad_content	= CAN_ISOTP_DEFAULT_PAD_CONTENT;
	so->opt.frame_txtime	= CAN_ISOTP_DEFAULT_FRAME_TXTIME;
	so->rxfc.bs		= CAN_ISOTP_DEFAULT_RECV_BS;
	so->rxfc.stmin		= CAN_ISOTP_DEFAULT_RECV_STMIN;
	so->rxfc.wftmax		= CAN_ISOTP_DEFAULT_RECV_WFTMAX;
	so->ll.mtu		= CAN_ISOTP_DEFAULT_LL_MTU;
	so->ll.tx_dl		= CAN_ISOTP_DEFAULT_LL_TX_DL;
	so->ll.tx_flags		= CAN_ISOTP_DEFAULT_LL_TX_FLAGS;

	/* set ll_dl for tx path to similar place as for rx */
	so->tx.ll_dl		= so->ll.tx_dl;

	so->rx.state = ISOTP_IDLE;
	so->tx.state = ISOTP_IDLE;

	hrtimer_init(&so->rxtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_SOFT);
	so->rxtimer.function = isotp_rx_timer_handler;
	hrtimer_init(&so->txtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_SOFT);
	so->txtimer.function = isotp_tx_timer_handler;

	init_waitqueue_head(&so->wait);

	so->notifier.notifier_call = isotp_notifier;
	register_netdevice_notifier(&so->notifier);

	return 0;
}

static int isotp_sock_no_ioctlcmd(struct socket *sock, unsigned int cmd,
				  unsigned long arg)
{
	/* no ioctls for socket layer -> hand it down to NIC layer */
	return -ENOIOCTLCMD;
}

static const struct proto_ops isotp_ops = {
	.family		= PF_CAN,
	.release	= isotp_release,
	.bind		= isotp_bind,
	.connect	= sock_no_connect,
	.socketpair	= sock_no_socketpair,
	.accept		= sock_no_accept,
	.getname	= isotp_getname,
	.poll		= datagram_poll,
	.ioctl		= isotp_sock_no_ioctlcmd,
	.gettstamp	= sock_gettstamp,
	.listen		= sock_no_listen,
	.shutdown	= sock_no_shutdown,
	.setsockopt	= isotp_setsockopt,
	.getsockopt	= isotp_getsockopt,
	.sendmsg	= isotp_sendmsg,
	.recvmsg	= isotp_recvmsg,
	.mmap		= sock_no_mmap,
	.sendpage	= sock_no_sendpage,
};

static struct proto isotp_proto __read_mostly = {
	.name		= "CAN_ISOTP",
	.owner		= THIS_MODULE,
	.obj_size	= sizeof(struct isotp_sock),
	.init		= isotp_init,
};

static const struct can_proto isotp_can_proto = {
	.type		= SOCK_DGRAM,
	.protocol	= CAN_ISOTP,
	.ops		= &isotp_ops,
	.prot		= &isotp_proto,
};

static __init int isotp_module_init(void)
{
	int err;

	printk(banner);

	err = can_proto_register(&isotp_can_proto);
	if (err < 0)
		printk(KERN_ERR "can: registration of isotp protocol failed\n");

	return err;
}

static __exit void isotp_module_exit(void)
{
	can_proto_unregister(&isotp_can_proto);
}

module_init(isotp_module_init);
module_exit(isotp_module_exit);
