// SPDX-License-Identifier: GPL-2.0
/*
 * Cadence USBSS DRD Driver - gadget side.
 *
 * Copyright (C) 2018-2019 Cadence Design Systems.
 * Copyright (C) 2017-2019 NXP
 *
 * Authors: Pawel Jez <pjez@cadence.com>,
 *          Pawel Laszczak <pawell@cadence.com>
 *	    Peter Chen <peter.chen@nxp.com>
 */

/*
 * Work around 1:
 * At some situations, the controller may get stale data address in TRB
 * at below sequences:
 * 1. Controller read TRB includes data address
 * 2. Software updates TRBs includes data address and Cycle bit
 * 3. Controller read TRB which includes Cycle bit
 * 4. DMA run with stale data address
 *
 * To fix this problem, driver needs to make the first TRB in TD as invalid.
 * After preparing all TRBs driver needs to check the position of DMA and
 * if the DMA point to the first just added TRB and doorbell is 1,
 * then driver must defer making this TRB as valid. This TRB will be make
 * as valid during adding next TRB only if DMA is stopped or at TRBERR
 * interrupt.
 *
 * Work around 2:
 * Controller for OUT endpoints has shared on-chip buffers for all incoming
 * packets, including ep0out. It's FIFO buffer, so packets must be handle by DMA
 * in correct order. If the first packet in the buffer will not be handled,
 * then the following packets directed for other endpoints and  functions
 * will be blocked.
 * Additionally the packets directed to one endpoint can block entire on-chip
 * buffers. In this case transfer to other endpoints also will blocked.
 *
 * To resolve this issue after raising the descriptor missing interrupt
 * driver prepares internal usb_request object and use it to arm DMA transfer.
 *
 * The problematic situation was observed in case when endpoint has been enabled
 * but no usb_request were queued. Driver try detects such endpoints and will
 * use this workaround only for these endpoint.
 *
 * Driver use limited number of buffer. This number can be set by macro
 * CDNS_WA2_NUM_BUFFERS.
 *
 * Such blocking situation was observed on ACM gadget. For this function
 * host send OUT data packet but ACM function is not prepared for this packet.
 * It's cause that buffer placed in on chip memory block transfer to other
 * endpoints.
 *
 * It's limitation of controller but maybe this issues should be fixed in
 * function driver.
 *
 * This work around can be disabled/enabled by means of quirk_internal_buffer
 * module parameter. By default feature is enabled. It can has impact to
 * transfer performance and in most case this feature can be disabled.
 */

#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/usb/gadget.h>
#include <linux/pm_runtime.h>
#include <linux/module.h>

#include "core.h"
#include "gadget-export.h"
#include "gadget.h"
#include "trace.h"

static int __cdns3_gadget_ep_queue(struct usb_ep *ep,
				   struct usb_request *request,
				   gfp_t gfp_flags);

/*
 * Parameter allows to disable/enable handling of work around 2 feature.
 * By default this value is enabled.
 */
static bool quirk_internal_buffer = 1;
module_param(quirk_internal_buffer, bool, 0644);
MODULE_PARM_DESC(quirk_internal_buffer, "Disable/enable WA2 algorithm");

/**
 * cdns3_set_register_bit - set bit in given register.
 * @ptr: address of device controller register to be read and changed
 * @mask: bits requested to set
 */
void cdns3_set_register_bit(void __iomem *ptr, u32 mask)
{
	mask = readl(ptr) | mask;
	writel(mask, ptr);
}

/**
 * cdns3_ep_addr_to_index - Macro converts endpoint address to
 * index of endpoint object in cdns3_device.eps[] container
 * @ep_addr: endpoint address for which endpoint object is required
 *
 */
u8 cdns3_ep_addr_to_index(u8 ep_addr)
{
	return (((ep_addr & 0x7F)) + ((ep_addr & USB_DIR_IN) ? 16 : 0));
}

/**
 * cdns3_next_request - returns next request from list
 * @list: list containing requests
 *
 * Returns request or NULL if no requests in list
 */
struct usb_request *cdns3_next_request(struct list_head *list)
{
	return list_first_entry_or_null(list, struct usb_request, list);
}

/**
 * cdns3_aligned_buf - returns next buffer from list
 * @list: list containing buffers
 *
 * Returns buffer or NULL if no buffers in list
 */
struct cdns3_aligned_buf *cdns3_next_align_buf(struct list_head *list)
{
	return list_first_entry_or_null(list, struct cdns3_aligned_buf, list);
}

/**
 * cdns3_next_priv_request - returns next request from list
 * @list: list containing requests
 *
 * Returns request or NULL if no requests in list
 */
struct cdns3_request *cdns3_next_priv_request(struct list_head *list)
{
	return list_first_entry_or_null(list, struct cdns3_request, list);
}

/**
 * select_ep - selects endpoint
 * @priv_dev:  extended gadget object
 * @ep: endpoint address
 */
void cdns3_select_ep(struct cdns3_device *priv_dev, u32 ep)
{
	if (priv_dev->selected_ep == ep)
		return;

	priv_dev->selected_ep = ep;
	writel(ep, &priv_dev->regs->ep_sel);
}

dma_addr_t cdns3_trb_virt_to_dma(struct cdns3_endpoint *priv_ep,
				 struct cdns3_trb *trb)
{
	u32 offset = (char *)trb - (char *)priv_ep->trb_pool;

	return priv_ep->trb_pool_dma + offset;
}

int cdns3_ring_size(struct cdns3_endpoint *priv_ep)
{
	switch (priv_ep->type) {
	case USB_ENDPOINT_XFER_ISOC:
		return TRB_ISO_RING_SIZE;
	case USB_ENDPOINT_XFER_CONTROL:
		return TRB_CTRL_RING_SIZE;
	default:
		return TRB_RING_SIZE;
	}
}

/**
 * cdns3_allocate_trb_pool - Allocates TRB's pool for selected endpoint
 * @priv_ep:  endpoint object
 *
 * Function will return 0 on success or -ENOMEM on allocation error
 */
int cdns3_allocate_trb_pool(struct cdns3_endpoint *priv_ep)
{
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;
	int ring_size = cdns3_ring_size(priv_ep);
	struct cdns3_trb *link_trb;

	if (!priv_ep->trb_pool) {
		priv_ep->trb_pool = dma_zalloc_coherent(priv_dev->sysdev,
							ring_size,
							&priv_ep->trb_pool_dma,
							GFP_DMA);
		if (!priv_ep->trb_pool)
			return -ENOMEM;
	} else {
		memset(priv_ep->trb_pool, 0, ring_size);
	}

	if (!priv_ep->num)
		return 0;

	priv_ep->num_trbs = ring_size / TRB_SIZE;
	/* Initialize the last TRB as Link TRB */
	link_trb = (priv_ep->trb_pool + (priv_ep->num_trbs - 1));
	link_trb->buffer = TRB_BUFFER(priv_ep->trb_pool_dma);
	link_trb->control = TRB_CYCLE | TRB_TYPE(TRB_LINK) |
			    TRB_CHAIN | TRB_TOGGLE;

	return 0;
}

static void cdns3_free_trb_pool(struct cdns3_endpoint *priv_ep)
{
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;

	if (priv_ep->trb_pool) {
		dma_free_coherent(priv_dev->sysdev,
				  cdns3_ring_size(priv_ep),
				  priv_ep->trb_pool, priv_ep->trb_pool_dma);
		priv_ep->trb_pool = NULL;
	}
}

/**
 * cdns3_ep_stall_flush - Stalls and flushes selected endpoint
 * @priv_ep: endpoint object
 *
 * Endpoint must be selected before call to this function
 */
static void cdns3_ep_stall_flush(struct cdns3_endpoint *priv_ep)
{
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;

	cdns3_dbg(priv_ep->cdns3_dev, "Stall & flush endpoint %s\n",
		  priv_ep->name);

	writel(EP_CMD_DFLUSH | EP_CMD_ERDY | EP_CMD_SSTALL,
	       &priv_dev->regs->ep_cmd);

	/* wait for DFLUSH cleared */
	cdns3_handshake(&priv_dev->regs->ep_cmd, EP_CMD_DFLUSH, 0, 1000);
	priv_ep->flags |= EP_STALL;
}

/**
 * cdns3_hw_reset_eps_config - reset endpoints configuration kept by controller.
 * @priv_dev: extended gadget object
 */
void cdns3_hw_reset_eps_config(struct cdns3_device *priv_dev)
{
	writel(USB_CONF_CFGRST, &priv_dev->regs->usb_conf);

	cdns3_allow_enable_l1(priv_dev, 0);
	priv_dev->hw_configured_flag = 0;
	priv_dev->onchip_mem_allocated_size = 0;
	priv_dev->out_mem_is_allocated = 0;
}

/**
 * cdns3_ep_inc_trb - increment a trb index.
 * @index: Pointer to the TRB index to increment.
 * @cs: Cycle state
 * @trb_in_seg: number of TRBs in segment
 *
 * The index should never point to the link TRB. After incrementing,
 * if it is point to the link TRB, wrap around to the beginning and revert
 * cycle state bit The
 * link TRB is always at the last TRB entry.
 */
static void cdns3_ep_inc_trb(int *index, u8 *cs, int trb_in_seg)
{
	(*index)++;
	if (*index == (trb_in_seg - 1)) {
		*index = 0;
		*cs ^=  1;
	}
}

/**
 * cdns3_ep_inc_enq - increment endpoint's enqueue pointer
 * @priv_ep: The endpoint whose enqueue pointer we're incrementing
 */
static void cdns3_ep_inc_enq(struct cdns3_endpoint *priv_ep)
{
	priv_ep->free_trbs--;
	cdns3_ep_inc_trb(&priv_ep->enqueue, &priv_ep->pcs, priv_ep->num_trbs);
}

/**
 * cdns3_ep_inc_deq - increment endpoint's dequeue pointer
 * @priv_ep: The endpoint whose dequeue pointer we're incrementing
 */
static void cdns3_ep_inc_deq(struct cdns3_endpoint *priv_ep)
{
	priv_ep->free_trbs++;
	cdns3_ep_inc_trb(&priv_ep->dequeue, &priv_ep->ccs, priv_ep->num_trbs);
}

void cdns3_move_deq_to_next_trb(struct cdns3_request *priv_req)
{
	struct cdns3_endpoint *priv_ep = priv_req->priv_ep;
	int current_trb = priv_req->start_trb;

	while (current_trb != priv_req->end_trb) {
		cdns3_ep_inc_deq(priv_ep);
		current_trb = priv_ep->dequeue;
	}

	cdns3_ep_inc_deq(priv_ep);
}

/**
 * cdns3_allow_enable_l1 - enable/disable permits to transition to L1.
 * @priv_dev: Extended gadget object
 * @enable: Enable/disable permit to transition to L1.
 *
 * If bit USB_CONF_L1EN is set and device receive Extended Token packet,
 * then controller answer with ACK handshake.
 * If bit USB_CONF_L1DS is set and device receive Extended Token packet,
 * then controller answer with NYET handshake.
 */
void cdns3_allow_enable_l1(struct cdns3_device *priv_dev, int enable)
{
	if (enable)
		writel(USB_CONF_L1EN, &priv_dev->regs->usb_conf);
	else
		writel(USB_CONF_L1DS, &priv_dev->regs->usb_conf);
}

enum usb_device_speed cdns3_get_speed(struct cdns3_device *priv_dev)
{
	u32 reg;

	reg = readl(&priv_dev->regs->usb_sts);

	if (DEV_SUPERSPEED(reg))
		return USB_SPEED_SUPER;
	else if (DEV_HIGHSPEED(reg))
		return USB_SPEED_HIGH;
	else if (DEV_FULLSPEED(reg))
		return USB_SPEED_FULL;
	else if (DEV_LOWSPEED(reg))
		return USB_SPEED_LOW;
	return USB_SPEED_UNKNOWN;
}

/**
 * cdns3_start_all_request - add to ring all request not started
 * @priv_dev: Extended gadget object
 * @priv_ep: The endpoint for whom request will be started.
 *
 * Returns return ENOMEM if transfer ring i not enough TRBs to start
 *         all requests.
 */
static int cdns3_start_all_request(struct cdns3_device *priv_dev,
				   struct cdns3_endpoint *priv_ep)
{
	struct cdns3_request *priv_req;
	struct usb_request *request;
	int ret = 0;

	while (!list_empty(&priv_ep->deferred_req_list)) {
		request = cdns3_next_request(&priv_ep->deferred_req_list);
		priv_req = to_cdns3_request(request);

		ret = cdns3_ep_run_transfer(priv_ep, request);
		if (ret)
			return ret;

		list_del(&request->list);
		list_add_tail(&request->list,
			      &priv_ep->pending_req_list);
	}

	priv_ep->flags &= ~EP_RING_FULL;
	return ret;
}

/**
 * cdns3_descmiss_copy_data copy data from internal requests to request queued
 * by class driver.
 * @priv_ep: extended endpoint object
 * @request: request object
 */
static void cdns3_descmiss_copy_data(struct cdns3_endpoint *priv_ep,
				     struct usb_request *request)
{
	struct usb_request *descmiss_req;
	struct cdns3_request *descmiss_priv_req;

	while (!list_empty(&priv_ep->descmiss_req_list)) {
		int chunk_end;
		int length;

		descmiss_priv_req =
			cdns3_next_priv_request(&priv_ep->descmiss_req_list);
		descmiss_req = &descmiss_priv_req->request;

		/* driver can't touch pending request */
		if (descmiss_priv_req->flags & REQUEST_PENDING)
			break;

		chunk_end = descmiss_priv_req->flags & REQUEST_INTERNAL_CH;
		length = request->actual + descmiss_req->actual;

		if (length <= request->length) {
			memcpy(&((u8 *)request->buf)[request->actual],
			       descmiss_req->buf,
			       descmiss_req->actual);
			request->actual = length;
		} else {
			/* It should never occures */
			request->status = -ENOMEM;
		}

		list_del_init(&descmiss_priv_req->list);

		kfree(descmiss_req->buf);
		cdns3_gadget_ep_free_request(&priv_ep->endpoint, descmiss_req);

		if (!chunk_end)
			break;
	}
}

/**
 * cdns3_gadget_giveback - call struct usb_request's ->complete callback
 * @priv_ep: The endpoint to whom the request belongs to
 * @priv_req: The request we're giving back
 * @status: completion code for the request
 *
 * Must be called with controller's lock held and interrupts disabled. This
 * function will unmap @req and call its ->complete() callback to notify upper
 * layers that it has completed.
 */
void cdns3_gadget_giveback(struct cdns3_endpoint *priv_ep,
			   struct cdns3_request *priv_req,
			   int status)
{
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;
	struct usb_request *request = &priv_req->request;

	list_del_init(&request->list);

	if (request->status == -EINPROGRESS)
		request->status = status;

	usb_gadget_unmap_request_by_dev(priv_dev->sysdev, request,
					priv_ep->dir);

	if ((priv_req->flags & REQUEST_UNALIGNED) &&
	    priv_ep->dir == USB_DIR_OUT && !request->status)
		memcpy(request->buf, priv_req->aligned_buf->buf,
		       request->length);

	priv_req->flags &= ~(REQUEST_PENDING | REQUEST_UNALIGNED);
	trace_cdns3_gadget_giveback(priv_req);

	/* WA2: */
	if (priv_ep->flags & EP_QUIRK_EXTRA_BUF_EN &&
	    priv_req->flags & REQUEST_INTERNAL) {
		struct usb_request *req;

		req = cdns3_next_request(&priv_ep->deferred_req_list);
		request = req;
		priv_ep->descmis_req = NULL;

		if (!req)
			return;

		cdns3_descmiss_copy_data(priv_ep, req);
		if (!(priv_ep->flags & EP_QUIRK_END_TRANSFER) &&
		    req->length != req->actual) {
			/* wait for next part of transfer */
			return;
		}

		if (req->status == -EINPROGRESS)
			req->status = 0;

		list_del_init(&req->list);
		cdns3_start_all_request(priv_dev, priv_ep);
	}

	/* Start all not pending request */
	if (priv_ep->flags & EP_RING_FULL)
		cdns3_start_all_request(priv_dev, priv_ep);

	if (request->complete) {
		spin_unlock(&priv_dev->lock);
		usb_gadget_giveback_request(&priv_ep->endpoint,
					    request);
		spin_lock(&priv_dev->lock);
	}

	if (request->buf == priv_dev->zlp_buf)
		cdns3_gadget_ep_free_request(&priv_ep->endpoint, request);
}

void cdns3_wa1_restore_cycle_bit(struct cdns3_endpoint *priv_ep)
{
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;

	/* Work around for stale data address in TRB*/
	if (priv_ep->wa1_set) {
		cdns3_dbg(priv_dev, "WA1: update cycle bit\n");
		priv_ep->wa1_set = 0;
		priv_ep->wa1_trb_index = 0xFFFF;
		if (priv_ep->wa1_cycle_bit) {
			priv_ep->wa1_trb->control =
				priv_ep->wa1_trb->control | 0x1;
		} else {
			priv_ep->wa1_trb->control =
				priv_ep->wa1_trb->control & ~0x1;
		}
	}
}

static int cdns3_prepare_aligned_request_buf(struct cdns3_request *priv_req)
{
	struct cdns3_endpoint *priv_ep = priv_req->priv_ep;
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;
	struct cdns3_aligned_buf *buf;

	/* check if buffer is aligned to 8. */
	if (!((uintptr_t)priv_req->request.buf & 0x7))
		return 0;

	buf = priv_req->aligned_buf;

	if (!buf || priv_req->request.length > buf->size) {
		buf = kzalloc(sizeof(*buf), GFP_ATOMIC);
		if (!buf)
			return -ENOMEM;

		buf->size = priv_req->request.length;

		buf->buf = dma_alloc_coherent(priv_dev->sysdev,
					      buf->size,
					      &buf->dma,
					      GFP_ATOMIC);
		if (!buf->buf) {
			kfree(buf);
			return -ENOMEM;
		}

		if (priv_req->aligned_buf) {
			trace_cdns3_free_aligned_request(priv_req);
			priv_req->aligned_buf->in_use = 0;
			priv_dev->run_garbage_colector = 1;
		}

		buf->in_use = 1;
		priv_req->aligned_buf = buf;

		list_add_tail(&buf->list,
			      &priv_dev->aligned_buf_list);
	}

	if (priv_ep->dir == USB_DIR_IN) {
		memcpy(buf->buf, priv_req->request.buf,
		       priv_req->request.length);
	}

	priv_req->flags |= REQUEST_UNALIGNED;
	trace_cdns3_prepare_aligned_request(priv_req);

	return 0;
}

/**
 * cdns3_ep_run_transfer - start transfer on no-default endpoint hardware
 * @priv_ep: endpoint object
 *
 * Returns zero on success or negative value on failure
 */
int cdns3_ep_run_transfer(struct cdns3_endpoint *priv_ep,
			  struct usb_request *request)
{
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;
	struct cdns3_request *priv_req;
	struct cdns3_trb *trb;
	dma_addr_t trb_dma;
	int prev_enqueue;
	u32 togle_pcs = 1;
	int sg_iter = 0;
	int dma_index;
	u32 doorbell;
	int num_trb;
	int address;
	u32 control;
	int pcs;

	if (priv_ep->type == USB_ENDPOINT_XFER_ISOC)
		num_trb = priv_ep->interval;
	else
		num_trb = request->num_sgs ? request->num_sgs : 1;

	if (num_trb > priv_ep->free_trbs) {
		priv_ep->flags |= EP_RING_FULL;
		return -ENOBUFS;
	}

	priv_req = to_cdns3_request(request);
	address = priv_ep->endpoint.desc->bEndpointAddress;

	priv_ep->flags |= EP_PENDING_REQUEST;

	/* must allocate buffer aligned to 8 */
	if (priv_req->flags & REQUEST_UNALIGNED)
		trb_dma = priv_req->aligned_buf->dma;
	else
		trb_dma = request->dma;

	trb = priv_ep->trb_pool + priv_ep->enqueue;
	priv_req->start_trb = priv_ep->enqueue;
	priv_req->trb = trb;
	prev_enqueue = priv_ep->enqueue;

	/* prepare ring */
	if ((priv_ep->enqueue + num_trb)  >= (priv_ep->num_trbs - 1)) {
		/*updating C bt in  Link TRB before starting DMA*/
		struct cdns3_trb *link_trb = priv_ep->trb_pool +
					     (priv_ep->num_trbs - 1);
		link_trb->control = ((priv_ep->pcs) ? TRB_CYCLE : 0) |
				    TRB_TYPE(TRB_LINK) | TRB_CHAIN |
				    TRB_TOGGLE;
	}

	/* arm transfer on selected endpoint */
	cdns3_select_ep(priv_ep->cdns3_dev, address);

	doorbell = !!(readl(&priv_dev->regs->ep_cmd) & EP_CMD_DRDY);

	if (!priv_ep->wa1_set) {
		if (doorbell) {
			priv_ep->wa1_cycle_bit = priv_ep->pcs ? TRB_CYCLE : 0;
			priv_ep->wa1_set = 1;
			priv_ep->wa1_trb = trb;
			priv_ep->wa1_trb_index = priv_ep->enqueue;
			togle_pcs = 0;
			cdns3_dbg(priv_dev, "WA1 set guard\n");
		}
	}

	/* set incorrect Cycle Bit for first trb*/
	control = priv_ep->pcs ? 0 : TRB_CYCLE;
	do {
		/* fill TRB */
		control |= TRB_TYPE(TRB_NORMAL);
		trb->buffer = TRB_BUFFER(request->num_sgs == 0
				? trb_dma : request->sg[sg_iter].dma_address);

		trb->length = TRB_BURST_LEN(16/*priv_ep->trb_burst_size*/) |
		    TRB_LEN(request->num_sgs == 0 ?
				request->length : request->sg[sg_iter].length);

		pcs = priv_ep->pcs ? TRB_CYCLE : 0;

		/*
		 * first trb should be prepared as last to avoid processing
		 *  transfer to early
		 */
		if (sg_iter != 0)
			control |= pcs;

		if (priv_ep->type == USB_ENDPOINT_XFER_ISOC  && !priv_ep->dir) {
			control |= TRB_IOC | TRB_ISP;
		} else {
			/* for last element in TD or in SG list */
			if (sg_iter == (num_trb - 1) && sg_iter != 0)
				control |= pcs | TRB_IOC | TRB_ISP;
		}

		if (sg_iter) {
			trb->control = control;
			control = 0;
		}

		++sg_iter;
		priv_req->end_trb = priv_ep->enqueue;
		cdns3_ep_inc_enq(priv_ep);
		trb = priv_ep->trb_pool + priv_ep->enqueue;
	} while (sg_iter < num_trb);

	trb = priv_req->trb;

	priv_req->flags |= REQUEST_PENDING;

	/* give the TD to the consumer*/
	if (sg_iter == 1)
		control |= TRB_IOC | TRB_ISP;

	/*
	 * Memory barrier - cycle bit must be set before other filds in trb.
	 */
	wmb();

	if (togle_pcs)
		trb->control = control ^ 1;
	else
		trb->control = control;

	doorbell = !!(readl(&priv_dev->regs->ep_cmd) & EP_CMD_DRDY);
	dma_index = (readl(&priv_dev->regs->ep_traddr) -
			 priv_ep->trb_pool_dma) / TRB_SIZE;

	cdns3_dbg(priv_dev, "dorbel %d, dma_index %d, prev_enqueu %d",
		  doorbell, dma_index, prev_enqueue);

	if (!doorbell || dma_index != priv_ep->wa1_trb_index)
		cdns3_wa1_restore_cycle_bit(priv_ep);

	trace_cdns3_prepare_trb(priv_ep, priv_req->trb);

	/*
	 * Memory barrier - Cycle Bit must be set before trb->length  and
	 * trb->buffer fields.
	 */
	wmb();

	/*
	 * For DMULT mode we can set address to transfer ring only once after
	 * enabling endpoint.
	 */
	if (priv_ep->flags & EP_UPDATE_EP_TRBADDR) {
		writel(EP_TRADDR_TRADDR(priv_ep->trb_pool_dma +
					priv_req->start_trb * TRB_SIZE),
		       &priv_dev->regs->ep_traddr);

		cdns3_dbg(priv_ep->cdns3_dev, "Update ep_trbaddr for %s to %08x\n",
			  priv_ep->name, readl(&priv_dev->regs->ep_traddr));

		priv_ep->flags &= ~EP_UPDATE_EP_TRBADDR;
	}

	if (!priv_ep->wa1_set && !(priv_ep->flags & EP_STALL)) {
		trace_cdns3_ring(priv_ep);
		/*clearing TRBERR and EP_STS_DESCMIS before seting DRDY*/
		writel(EP_STS_TRBERR | EP_STS_DESCMIS, &priv_dev->regs->ep_sts);
		writel(EP_CMD_DRDY, &priv_dev->regs->ep_cmd);
		trace_cdns3_doorbell_epx(priv_ep->name,
					 readl(&priv_dev->regs->ep_traddr));
	}

	return 0;
}

void cdns3_set_hw_configuration(struct cdns3_device *priv_dev)
{
	struct cdns3_endpoint *priv_ep;
	struct usb_ep *ep;
	int result = 0;

	if (priv_dev->hw_configured_flag)
		return;

	writel(USB_CONF_CFGSET, &priv_dev->regs->usb_conf);
	writel(EP_CMD_ERDY | EP_CMD_REQ_CMPL, &priv_dev->regs->ep_cmd);

	cdns3_set_register_bit(&priv_dev->regs->usb_conf,
			       USB_CONF_U1EN | USB_CONF_U2EN);

	/* wait until configuration set */
	result = cdns3_handshake(&priv_dev->regs->usb_sts,
				 USB_STS_CFGSTS_MASK, 1, 100);

	priv_dev->hw_configured_flag = 1;
//	cdns3_allow_enable_l1(priv_dev, 1);

	list_for_each_entry(ep, &priv_dev->gadget.ep_list, ep_list) {
		if (ep->enabled) {
			priv_ep = ep_to_cdns3_ep(ep);
			cdns3_start_all_request(priv_dev, priv_ep);
		}
	}
}

/**
 * cdns3_request_handled - check whether request has been handled by DMA
 *
 * @priv_ep: extended endpoint object.
 * @priv_req: request object for checking
 *
 * Endpoint must be selected before invoking this function.
 *
 * Returns false if request has not been handled by DMA, else returns true.
 *
 * SR - start ring
 * ER -  end ring
 * DQ = priv_ep->dequeue - dequeue position
 * EQ = priv_ep->enqueue -  enqueue position
 * ST = priv_req->start_trb - index of first TRB in transfer ring
 * ET = priv_req->end_trb - index of last TRB in transfer ring
 * CI = current_index - index of processed TRB by DMA.
 *
 * As first step, function checks if cycle bit for priv_req->start_trb is
 * correct.
 *
 * some rules:
 * 1. priv_ep->dequeue never exceed current_index.
 * 2  priv_ep->enqueue never exceed priv_ep->dequeue
 *
 * Then We can split recognition into two parts:
 * Case 1 - priv_ep->dequeue < current_index
 *      SR ... EQ ... DQ ... CI ... ER
 *      SR ... DQ ... CI ... EQ ... ER
 *
 *      Request has been handled by DMA if ST and ET is between DQ and CI.
 *
 * Case 2 - priv_ep->dequeue > current_index
 * This situation take place when CI go through the LINK TRB at the end of
 * transfer ring.
 *      SR ... CI ... EQ ... DQ ... ER
 *
 *      Request has been handled by DMA if ET is less then CI or
 *      ET is greater or equal DQ.
 */
static bool cdns3_request_handled(struct cdns3_endpoint *priv_ep,
				  struct cdns3_request *priv_req)
{
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;
	struct cdns3_trb *trb = priv_req->trb;
	int current_index = 0;
	int handled = 0;

	current_index = (readl(&priv_dev->regs->ep_traddr) -
			 priv_ep->trb_pool_dma) / TRB_SIZE;

	trb = &priv_ep->trb_pool[priv_req->start_trb];

	if ((trb->control  & TRB_CYCLE) != priv_ep->ccs)
		goto finish;

	if (priv_ep->dequeue < current_index) {
		if ((current_index == (priv_ep->num_trbs - 1)) &&
		    !priv_ep->dequeue)
			goto finish;

		if (priv_req->end_trb >= priv_ep->dequeue &&
		    priv_req->end_trb < current_index)
			handled = 1;
	} else if (priv_ep->dequeue  > current_index) {
		if (priv_req->end_trb  < current_index ||
		    priv_req->end_trb >= priv_ep->dequeue)
			handled = 1;
	}

finish:
	trace_cdns3_request_handled(priv_req, current_index, handled);

	return handled;
}

static void cdns3_transfer_completed(struct cdns3_device *priv_dev,
				     struct cdns3_endpoint *priv_ep)
{
	struct cdns3_request *priv_req;
	struct usb_request *request;
	struct cdns3_trb *trb;

	while (!list_empty(&priv_ep->pending_req_list)) {
		request = cdns3_next_request(&priv_ep->pending_req_list);
		priv_req = to_cdns3_request(request);

		/* Re-select endpoint. It could be changed by other CPU during
		 * handling usb_gadget_giveback_request.
		 */
		cdns3_select_ep(priv_dev, priv_ep->endpoint.address);

		if (!cdns3_request_handled(priv_ep, priv_req))
			return;

		trb = priv_ep->trb_pool + priv_ep->dequeue;
		trace_cdns3_complete_trb(priv_ep, trb);

		if (trb != priv_req->trb)
			dev_warn(priv_dev->dev,
				 "request_trb=0x%p, queue_trb=0x%p\n",
				 priv_req->trb, trb);

		request->actual = TRB_LEN(le32_to_cpu(trb->length));
		cdns3_move_deq_to_next_trb(priv_req);
		cdns3_gadget_giveback(priv_ep, priv_req, 0);
	}
	priv_ep->flags &= ~EP_PENDING_REQUEST;
}

void cdns3_rearm_transfer(struct cdns3_endpoint *priv_ep, u8 rearm)
{
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;

	cdns3_wa1_restore_cycle_bit(priv_ep);

	if (rearm) {
		trace_cdns3_ring(priv_ep);

		/* Cycle Bit must be updated before arming DMA. */
		wmb();
		writel(EP_CMD_DRDY, &priv_dev->regs->ep_cmd);

		trace_cdns3_doorbell_epx(priv_ep->name,
					 readl(&priv_dev->regs->ep_traddr));
	}
}

/**
 * cdns3_descmissing_packet - handles descriptor missing event.
 * @priv_dev: extended gadget object
 *
 * This function is used only for WA2. For more information see Work around 2
 * description.
 */
static int cdns3_descmissing_packet(struct cdns3_endpoint *priv_ep)
{
	struct cdns3_request *priv_req;
	struct usb_request *request;

	if (priv_ep->flags & EP_QUIRK_EXTRA_BUF_DET) {
		priv_ep->flags &= ~EP_QUIRK_EXTRA_BUF_DET;
		priv_ep->flags |= EP_QUIRK_EXTRA_BUF_EN;
	}

	cdns3_dbg(priv_ep->cdns3_dev, "WA2: Description Missing detected\n");

	request = cdns3_gadget_ep_alloc_request(&priv_ep->endpoint,
						GFP_ATOMIC);
	if (!request)
		return -ENOMEM;

	priv_req = to_cdns3_request(request);
	priv_req->flags |= REQUEST_INTERNAL;

	/* if this field is still assigned it indicate that transfer related
	 * with this request has not been finished yet. Driver in this
	 * case simply allocate next request and assign flag REQUEST_INTERNAL_CH
	 * flag to previous one. It will indicate that current request is
	 * part of the previous one.
	 */
	if (priv_ep->descmis_req)
		priv_ep->descmis_req->flags |= REQUEST_INTERNAL_CH;

	priv_req->request.buf = kzalloc(CDNS3_DESCMIS_BUF_SIZE,
					GFP_ATOMIC);
	if (!priv_req->request.buf) {
		cdns3_gadget_ep_free_request(&priv_ep->endpoint, request);
		return -ENOMEM;
	}

	priv_req->request.length = CDNS3_DESCMIS_BUF_SIZE;
	priv_ep->descmis_req = priv_req;

	__cdns3_gadget_ep_queue(&priv_ep->endpoint,
				&priv_ep->descmis_req->request,
				GFP_ATOMIC);

	return 0;
}

/**
 * cdns3_check_ep_interrupt_proceed - Processes interrupt related to endpoint
 * @priv_ep: endpoint object
 *
 * Returns 0
 */
static int cdns3_check_ep_interrupt_proceed(struct cdns3_endpoint *priv_ep)
{
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;
	u32 ep_sts_reg;

	cdns3_select_ep(priv_dev, priv_ep->endpoint.address);

	trace_cdns3_epx_irq(priv_dev, priv_ep);

	ep_sts_reg = readl(&priv_dev->regs->ep_sts);
	writel(ep_sts_reg, &priv_dev->regs->ep_sts);

	if (ep_sts_reg & EP_STS_TRBERR) {
		/*
		 * For isochronous transfer driver completes request on
		 * IOC or on TRBERR. IOC appears only when device receive
		 * OUT data packet. If host disable stream or lost some packet
		 * then the only way to finish all queued transfer is to do it
		 * on TRBERR event.
		 */
		if (priv_ep->type == USB_ENDPOINT_XFER_ISOC &&
		    !priv_ep->wa1_set)
			cdns3_transfer_completed(priv_dev, priv_ep);
		else
			cdns3_rearm_transfer(priv_ep, priv_ep->wa1_set);
	}

	if ((ep_sts_reg & EP_STS_IOC) || (ep_sts_reg & EP_STS_ISP)) {
		if (priv_ep->flags & EP_QUIRK_EXTRA_BUF_EN) {
			if (ep_sts_reg & EP_STS_ISP)
				priv_ep->flags |= EP_QUIRK_END_TRANSFER;
			else
				priv_ep->flags &= ~EP_QUIRK_END_TRANSFER;
		}

		cdns3_transfer_completed(priv_dev, priv_ep);
	}

	/*
	 * WA2: this condition should only be meet when
	 * priv_ep->flags & EP_QUIRK_EXTRA_BUF_DET or
	 * priv_ep->flags & EP_QUIRK_EXTRA_BUF_EN.
	 * In other cases this interrupt will be disabled/
	 */
	if (ep_sts_reg & EP_STS_DESCMIS) {
		int err;

		err = cdns3_descmissing_packet(priv_ep);
		if (err)
			dev_err(priv_dev->dev,
				"Failed: No sufficient memory for DESCMIS\n");
	}

	return 0;
}

/**
 * cdns3_check_usb_interrupt_proceed - Processes interrupt related to device
 * @priv_dev: extended gadget object
 * @usb_ists: bitmap representation of device's reported interrupts
 * (usb_ists register value)
 */
static void cdns3_check_usb_interrupt_proceed(struct cdns3_device *priv_dev,
					      u32 usb_ists)
{
	int speed = 0;

	trace_cdns3_usb_irq(priv_dev, usb_ists);
	/* Connection detected */
	if (usb_ists & (USB_ISTS_CON2I | USB_ISTS_CONI)) {
		speed = cdns3_get_speed(priv_dev);
		priv_dev->gadget.speed = speed;
		usb_gadget_set_state(&priv_dev->gadget, USB_STATE_POWERED);
		cdns3_ep0_config(priv_dev);
	}

	/* Disconnection detected */
	if (usb_ists & (USB_ISTS_DIS2I | USB_ISTS_DISI)) {
		if (priv_dev->gadget_driver &&
		    priv_dev->gadget_driver->disconnect) {
			spin_unlock(&priv_dev->lock);
			priv_dev->gadget_driver->disconnect(&priv_dev->gadget);
			spin_lock(&priv_dev->lock);
		}

		priv_dev->gadget.speed = USB_SPEED_UNKNOWN;
		usb_gadget_set_state(&priv_dev->gadget, USB_STATE_NOTATTACHED);
		cdns3_hw_reset_eps_config(priv_dev);
	}

	/* reset*/
	if (usb_ists & (USB_ISTS_UWRESI | USB_ISTS_UHRESI | USB_ISTS_U2RESI)) {
		if (priv_dev->gadget_driver &&
		    priv_dev->gadget_driver->reset) {
			spin_unlock(&priv_dev->lock);
			priv_dev->gadget_driver->reset(&priv_dev->gadget);
			spin_lock(&priv_dev->lock);
		}

		/*read again to check the actual speed*/
		speed = cdns3_get_speed(priv_dev);
		usb_gadget_set_state(&priv_dev->gadget, USB_STATE_DEFAULT);
		priv_dev->gadget.speed = speed;
		cdns3_hw_reset_eps_config(priv_dev);
		cdns3_ep0_config(priv_dev);
	}
}

/**
 * cdns3_device_irq_handler- interrupt handler for device part of controller
 *
 * @cdns: structure of cdns3
 *
 * Returns IRQ_HANDLED or IRQ_NONE
 */
static irqreturn_t cdns3_device_irq_handler(struct cdns3 *cdns)
{
	struct cdns3_device *priv_dev;
	irqreturn_t ret = IRQ_NONE;
	unsigned long flags;
	u32 reg;

	priv_dev = cdns->gadget_dev;
	spin_lock_irqsave(&priv_dev->lock, flags);

	/* check USB device interrupt */
	reg = readl(&priv_dev->regs->usb_ists);
	writel(reg, &priv_dev->regs->usb_ists);

	if (reg) {
		cdns3_check_usb_interrupt_proceed(priv_dev, reg);
		ret = IRQ_HANDLED;
	}

	/* check endpoint interrupt */
	reg = readl(&priv_dev->regs->ep_ists);

	if (reg) {
		priv_dev->shadow_ep_en |= reg;
		reg = ~reg & readl(&priv_dev->regs->ep_ien);
		/* mask deferred interrupt. */
		writel(reg, &priv_dev->regs->ep_ien);
		ret = IRQ_WAKE_THREAD;
	}

	spin_unlock_irqrestore(&priv_dev->lock, flags);
	return ret;
}

/**
 * cdns3_device_thread_irq_handler- interrupt handler for device part
 * of controller
 *
 * @irq: irq number for cdns3 core device
 * @data: structure of cdns3
 *
 * Returns IRQ_HANDLED or IRQ_NONE
 */
static irqreturn_t cdns3_device_thread_irq_handler(struct cdns3 *cdns)
{
	struct cdns3_device *priv_dev;
	irqreturn_t ret = IRQ_NONE;
	unsigned long flags;
	u32 ep_ien;
	int bit;
	u32 reg;

	priv_dev = cdns->gadget_dev;
	spin_lock_irqsave(&priv_dev->lock, flags);

	reg = readl(&priv_dev->regs->ep_ists);

	/* handle default endpoint OUT */
	if (reg & EP_ISTS_EP_OUT0) {
		cdns3_check_ep0_interrupt_proceed(priv_dev, USB_DIR_OUT);
		ret = IRQ_HANDLED;
	}

	/* handle default endpoint IN */
	if (reg & EP_ISTS_EP_IN0) {
		cdns3_check_ep0_interrupt_proceed(priv_dev, USB_DIR_IN);
		ret = IRQ_HANDLED;
	}

	/* check if interrupt from non default endpoint, if no exit */
	reg &= ~(EP_ISTS_EP_OUT0 | EP_ISTS_EP_IN0);
	if (!reg)
		goto irqend;

	for_each_set_bit(bit, (unsigned long *)&reg,
			 sizeof(u32) * BITS_PER_BYTE) {
		priv_dev->shadow_ep_en |= BIT(bit);
		cdns3_check_ep_interrupt_proceed(priv_dev->eps[bit]);
		ret = IRQ_HANDLED;
	}

	if (priv_dev->run_garbage_colector) {
		struct cdns3_aligned_buf *buf, *tmp;

		list_for_each_entry_safe(buf, tmp, &priv_dev->aligned_buf_list,
					 list) {
			if (!buf->in_use) {
				list_del(&buf->list);

				spin_unlock_irqrestore(&priv_dev->lock, flags);
				dma_free_coherent(priv_dev->sysdev, buf->size,
						  buf->buf,
						  buf->dma);
				spin_lock_irqsave(&priv_dev->lock, flags);

				kfree(buf);
			}
		}

		priv_dev->run_garbage_colector = 0;
	}

irqend:
	ep_ien = readl(&priv_dev->regs->ep_ien) | priv_dev->shadow_ep_en;
	priv_dev->shadow_ep_en = 0;
	/* Unmask all handled EP interrupts */
	writel(ep_ien, &priv_dev->regs->ep_ien);
	spin_unlock_irqrestore(&priv_dev->lock, flags);
	return ret;
}

/**
 * cdns3_ep_onchip_buffer_reserve - Try to reserve onchip buf for EP
 *
 * The real reservation will occur during write to EP_CFG register,
 * this function is used to check if the 'size' reservation is allowed.
 *
 * @priv_dev: extended gadget object
 * @size: the size (KB) for EP would like to allocate
 *
 * Return 0 if the required size can met or negative value on failure
 */
#define CDNS3_ONCHIP_BUF_SIZE 16
static int cdns3_ep_onchip_buffer_reserve(struct cdns3_device *priv_dev,
					  int size, int is_in)
{
	if (is_in) {
		priv_dev->onchip_mem_allocated_size += size;
	} else if (!priv_dev->out_mem_is_allocated) {
		 /* ALL OUT EPs are shared the same chunk onchip memory */
		priv_dev->onchip_mem_allocated_size += size;
		priv_dev->out_mem_is_allocated = 1;
	}

	if (priv_dev->onchip_mem_allocated_size > CDNS3_ONCHIP_BUF_SIZE) {
		priv_dev->onchip_mem_allocated_size -= size;
		return -EPERM;
	} else {
		return 0;
	}
}

/**
 * cdns3_ep_config Configure hardware endpoint
 * @priv_ep: extended endpoint object
 */
void cdns3_ep_config(struct cdns3_endpoint *priv_ep)
{
	bool is_iso_ep = (priv_ep->type == USB_ENDPOINT_XFER_ISOC);
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;
	u32 bEndpointAddress = priv_ep->num | priv_ep->dir;
	u32 max_packet_size = 0;
	u8 buffering;
	u8 maxburst = 0;
	u32 ep_cfg = 0;
	u8 mult = 0;
	int ret;

	buffering = CDNS3_EP_BUF_SIZE - 1;

	switch (priv_ep->type) {
	case USB_ENDPOINT_XFER_INT:
		ep_cfg = EP_CFG_EPTYPE(USB_ENDPOINT_XFER_INT);
		break;
	case USB_ENDPOINT_XFER_BULK:
		ep_cfg = EP_CFG_EPTYPE(USB_ENDPOINT_XFER_BULK);
		break;
	default:
		ep_cfg = EP_CFG_EPTYPE(USB_ENDPOINT_XFER_ISOC);
		mult = CDNS3_EP_ISO_HS_MULT - 1;
		buffering = mult + 1;
	}

	switch (priv_dev->gadget.speed) {
	case USB_SPEED_FULL:
		max_packet_size = is_iso_ep ? 1023 : 64;
		break;
	case USB_SPEED_HIGH:
		max_packet_size = is_iso_ep ? 1024 : 512;
		break;
	case USB_SPEED_SUPER:
		/* It's limitation that driver assumes in driver. */
		mult = 0;
		max_packet_size = 1024;
		if (priv_ep->type == USB_ENDPOINT_XFER_ISOC) {
			maxburst = CDNS3_EP_ISO_SS_BURST - 1;
			buffering = (mult + 1) *
				    (maxburst + 1);

			if (priv_ep->interval > 1)
				buffering++;
		} else {
			maxburst = CDNS3_EP_BUF_SIZE - 1;
		}
		break;
	default:
		/* all other speed are not supported */
		return;
	}

	if (max_packet_size == 1024)
		priv_ep->trb_burst_size = 128;
	else if (max_packet_size >= 512)
		priv_ep->trb_burst_size = 64;
	else
		priv_ep->trb_burst_size = 16;

	ret = cdns3_ep_onchip_buffer_reserve(priv_dev, buffering,
		!!priv_ep->dir);
	if (ret) {
		dev_err(priv_dev->dev, "onchip mem is full, ep is invalid\n");
		return;
	}

	ep_cfg |= EP_CFG_MAXPKTSIZE(max_packet_size) |
		  EP_CFG_MULT(mult) |
		  EP_CFG_BUFFERING(buffering) |
		  EP_CFG_MAXBURST(maxburst);

	cdns3_select_ep(priv_dev, bEndpointAddress);
	writel(ep_cfg, &priv_dev->regs->ep_cfg);

	dev_dbg(priv_dev->dev, "Configure %s: with val %08x\n",
		priv_ep->name, ep_cfg);
}

/* Find correct direction for HW endpoint according to description */
static int cdns3_ep_dir_is_correct(struct usb_endpoint_descriptor *desc,
				   struct cdns3_endpoint *priv_ep)
{
	return (priv_ep->endpoint.caps.dir_in && usb_endpoint_dir_in(desc)) ||
	       (priv_ep->endpoint.caps.dir_out && usb_endpoint_dir_out(desc));
}

static struct
cdns3_endpoint *cdns3_find_available_ep(struct cdns3_device *priv_dev,
					struct usb_endpoint_descriptor *desc)
{
	struct usb_ep *ep;
	struct cdns3_endpoint *priv_ep;

	list_for_each_entry(ep, &priv_dev->gadget.ep_list, ep_list) {
		unsigned long num;
		int ret;
		/* ep name pattern likes epXin or epXout */
		char c[2] = {ep->name[2], '\0'};

		ret = kstrtoul(c, 10, &num);
		if (ret)
			return ERR_PTR(ret);

		priv_ep = ep_to_cdns3_ep(ep);
		if (cdns3_ep_dir_is_correct(desc, priv_ep)) {
			if (!(priv_ep->flags & EP_CLAIMED)) {
				priv_ep->num  = num;
				return priv_ep;
			}
		}
	}

	return ERR_PTR(-ENOENT);
}

/*
 *  Cadence IP has one limitation that all endpoints must be configured
 * (Type & MaxPacketSize) before setting configuration through hardware
 * register, it means we can't change endpoints configuration after
 * set_configuration.
 *
 * This function set EP_CLAIMED flag which is added when the gadget driver
 * uses usb_ep_autoconfig to configure specific endpoint;
 * When the udc driver receives set_configurion request,
 * it goes through all claimed endpoints, and configure all endpoints
 * accordingly.
 *
 * At usb_ep_ops.enable/disable, we only enable and disable endpoint through
 * ep_cfg register which can be changed after set_configuration, and do
 * some software operation accordingly.
 */
static struct
usb_ep *cdns3_gadget_match_ep(struct usb_gadget *gadget,
			      struct usb_endpoint_descriptor *desc,
			      struct usb_ss_ep_comp_descriptor *comp_desc)
{
	struct cdns3_device *priv_dev = gadget_to_cdns3_device(gadget);
	struct cdns3_endpoint *priv_ep;
	unsigned long flags;

	priv_ep = cdns3_find_available_ep(priv_dev, desc);
	if (IS_ERR(priv_ep)) {
		dev_err(priv_dev->dev, "no available ep\n");
		return NULL;
	}

	dev_dbg(priv_dev->dev, "match endpoint: %s\n", priv_ep->name);

	spin_lock_irqsave(&priv_dev->lock, flags);
	priv_ep->endpoint.desc = desc;
	priv_ep->dir  = usb_endpoint_dir_in(desc) ? USB_DIR_IN : USB_DIR_OUT;
	priv_ep->type = usb_endpoint_type(desc);
	priv_ep->flags |= EP_CLAIMED;
	priv_ep->interval = desc->bInterval ? BIT(desc->bInterval - 1) : 0;

	spin_unlock_irqrestore(&priv_dev->lock, flags);
	return &priv_ep->endpoint;
}

/**
 * cdns3_gadget_ep_alloc_request Allocates request
 * @ep: endpoint object associated with request
 * @gfp_flags: gfp flags
 *
 * Returns allocated request address, NULL on allocation error
 */
struct usb_request *cdns3_gadget_ep_alloc_request(struct usb_ep *ep,
						  gfp_t gfp_flags)
{
	struct cdns3_endpoint *priv_ep = ep_to_cdns3_ep(ep);
	struct cdns3_request *priv_req;

	priv_req = kzalloc(sizeof(*priv_req), gfp_flags);
	if (!priv_req)
		return NULL;

	priv_req->priv_ep = priv_ep;

	trace_cdns3_alloc_request(priv_req);
	return &priv_req->request;
}

/**
 * cdns3_gadget_ep_free_request Free memory occupied by request
 * @ep: endpoint object associated with request
 * @request: request to free memory
 */
void cdns3_gadget_ep_free_request(struct usb_ep *ep,
				  struct usb_request *request)
{
	struct cdns3_request *priv_req = to_cdns3_request(request);

	if (priv_req->aligned_buf)
		priv_req->aligned_buf->in_use = 0;

	trace_cdns3_free_request(priv_req);
	kfree(priv_req);
}

/**
 * cdns3_gadget_ep_enable Enable endpoint
 * @ep: endpoint object
 * @desc: endpoint descriptor
 *
 * Returns 0 on success, error code elsewhere
 */
static int cdns3_gadget_ep_enable(struct usb_ep *ep,
				  const struct usb_endpoint_descriptor *desc)
{
	struct cdns3_endpoint *priv_ep;
	struct cdns3_device *priv_dev;
	u32 reg = EP_STS_EN_TRBERREN;
	u32 bEndpointAddress;
	unsigned long flags;
	int ret;

	priv_ep = ep_to_cdns3_ep(ep);
	priv_dev = priv_ep->cdns3_dev;

	if (!ep || !desc || desc->bDescriptorType != USB_DT_ENDPOINT) {
		dev_dbg(priv_dev->dev, "usbss: invalid parameters\n");
		return -EINVAL;
	}

	if (!desc->wMaxPacketSize) {
		dev_err(priv_dev->dev, "usbss: missing wMaxPacketSize\n");
		return -EINVAL;
	}

	if (dev_WARN_ONCE(priv_dev->dev, priv_ep->flags & EP_ENABLED,
			  "%s is already enabled\n", priv_ep->name))
		return 0;

	spin_lock_irqsave(&priv_dev->lock, flags);

	priv_ep->endpoint.desc = desc;
	priv_ep->type = usb_endpoint_type(desc);
	priv_ep->interval = desc->bInterval ? BIT(desc->bInterval - 1) : 0;

	if (priv_ep->interval > ISO_MAX_INTERVAL &&
	    priv_ep->type == USB_ENDPOINT_XFER_ISOC) {
		dev_err(priv_dev->dev, "Driver is limited to %d period\n",
			ISO_MAX_INTERVAL);

		ret =  -EINVAL;
		goto exit;
	}

	ret = cdns3_allocate_trb_pool(priv_ep);

	if (ret)
		goto exit;

	bEndpointAddress = priv_ep->num | priv_ep->dir;
	cdns3_select_ep(priv_dev, bEndpointAddress);

	trace_cdns3_gadget_ep_enable(priv_ep);

	writel(EP_CMD_EPRST, &priv_dev->regs->ep_cmd);

	ret = cdns3_handshake(&priv_dev->regs->ep_cmd,
			      EP_CMD_CSTALL | EP_CMD_EPRST, 0, 1000);

	/* enable interrupt for selected endpoint */
	cdns3_set_register_bit(&priv_dev->regs->ep_ien,
			       BIT(cdns3_ep_addr_to_index(bEndpointAddress)));
	/*
	 * WA2: Set flag for all not ISOC OUT endpoints. If this flag is set
	 * driver try to detect whether endpoint need additional internal
	 * buffer for unblocking on-chip FIFO buffer. This flag will be cleared
	 * if before first DESCMISS interrupt the DMA will be armed.
	 */
	if (quirk_internal_buffer) {
		if (!priv_ep->dir && priv_ep->type != USB_ENDPOINT_XFER_ISOC) {
			priv_ep->flags |= EP_QUIRK_EXTRA_BUF_DET;
			reg |= EP_STS_EN_DESCMISEN;
		}
	}

	writel(reg, &priv_dev->regs->ep_sts_en);

	cdns3_set_register_bit(&priv_dev->regs->ep_cfg, EP_CFG_ENABLE);

	ep->desc = desc;
	priv_ep->flags &= ~(EP_PENDING_REQUEST | EP_STALL |
			    EP_QUIRK_EXTRA_BUF_EN);
	priv_ep->flags |= EP_ENABLED | EP_UPDATE_EP_TRBADDR;
	priv_ep->wa1_set = 0;
	priv_ep->enqueue = 0;
	priv_ep->dequeue = 0;
	reg = readl(&priv_dev->regs->ep_sts);
	priv_ep->pcs = !!EP_STS_CCS(reg);
	priv_ep->ccs = !!EP_STS_CCS(reg);
	/* one TRB is reserved for link TRB used in DMULT mode*/
	priv_ep->free_trbs = priv_ep->num_trbs - 1;
exit:
	spin_unlock_irqrestore(&priv_dev->lock, flags);

	return ret;
}

/**
 * cdns3_gadget_ep_disable Disable endpoint
 * @ep: endpoint object
 *
 * Returns 0 on success, error code elsewhere
 */
static int cdns3_gadget_ep_disable(struct usb_ep *ep)
{
	struct cdns3_endpoint *priv_ep;
	struct cdns3_request *priv_req;
	struct cdns3_device *priv_dev;
	struct usb_request *request;
	unsigned long flags;
	int ret = 0;
	u32 ep_cfg;

	if (!ep) {
		pr_err("usbss: invalid parameters\n");
		return -EINVAL;
	}

	priv_ep = ep_to_cdns3_ep(ep);
	priv_dev = priv_ep->cdns3_dev;

	if (dev_WARN_ONCE(priv_dev->dev, !(priv_ep->flags & EP_ENABLED),
			  "%s is already disabled\n", priv_ep->name))
		return 0;

	pm_runtime_get_sync(priv_dev->dev);
	spin_lock_irqsave(&priv_dev->lock, flags);

	trace_cdns3_gadget_ep_disable(priv_ep);

	cdns3_select_ep(priv_dev, ep->desc->bEndpointAddress);

	ep_cfg = readl(&priv_dev->regs->ep_cfg);
	ep_cfg &= ~EP_CFG_ENABLE;
	writel(ep_cfg, &priv_dev->regs->ep_cfg);

	writel(EP_CMD_EPRST, &priv_dev->regs->ep_cmd);

	ret = cdns3_handshake(&priv_dev->regs->ep_cmd,
			      EP_CMD_CSTALL | EP_CMD_EPRST, 0, 1000);
	while (!list_empty(&priv_ep->pending_req_list)) {
		request = cdns3_next_request(&priv_ep->pending_req_list);

		cdns3_gadget_giveback(priv_ep, to_cdns3_request(request),
				      -ESHUTDOWN);
	}

	while (!list_empty(&priv_ep->descmiss_req_list)) {
		priv_req = cdns3_next_priv_request(&priv_ep->descmiss_req_list);

		kfree(priv_req->request.buf);
		cdns3_gadget_ep_free_request(&priv_ep->endpoint,
					     &priv_req->request);
		list_del_init(&priv_req->list);
	}

	while (!list_empty(&priv_ep->deferred_req_list)) {
		request = cdns3_next_request(&priv_ep->deferred_req_list);

		cdns3_gadget_giveback(priv_ep, to_cdns3_request(request),
				      -ESHUTDOWN);
	}

	priv_ep->descmis_req = NULL;

	ep->desc = NULL;
	priv_ep->flags &= ~EP_ENABLED;

	spin_unlock_irqrestore(&priv_dev->lock, flags);
	pm_runtime_put_sync(priv_dev->dev);

	return ret;
}

/**
 * cdns3_gadget_ep_queue Transfer data on endpoint
 * @ep: endpoint object
 * @request: request object
 * @gfp_flags: gfp flags
 *
 * Returns 0 on success, error code elsewhere
 */
static int __cdns3_gadget_ep_queue(struct usb_ep *ep,
				   struct usb_request *request,
				   gfp_t gfp_flags)
{
	struct cdns3_endpoint *priv_ep = ep_to_cdns3_ep(ep);
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;
	struct cdns3_request *priv_req;
	int deferred = 0;
	int ret = 0;

	request->actual = 0;
	request->status = -EINPROGRESS;
	priv_req = to_cdns3_request(request);
	trace_cdns3_ep_queue(priv_req);

	/*
	 * WA2: if transfer was queued before DESCMISS appear than we
	 * can disable handling of DESCMISS interrupt. Driver assumes that it
	 * can disable special treatment for this endpoint.
	 */
	if (priv_ep->flags & EP_QUIRK_EXTRA_BUF_DET) {
		u32 reg;

		cdns3_select_ep(priv_dev, priv_ep->num | priv_ep->dir);
		priv_ep->flags &= ~EP_QUIRK_EXTRA_BUF_DET;
		reg = readl(&priv_dev->regs->ep_sts_en);
		reg &= ~EP_STS_EN_DESCMISEN;
		writel(reg, &priv_dev->regs->ep_sts_en);
	}

	/* WA2 */
	if (priv_ep->flags & EP_QUIRK_EXTRA_BUF_EN) {
		u8 pending_empty = list_empty(&priv_ep->pending_req_list);
		u8 descmiss_empty = list_empty(&priv_ep->descmiss_req_list);

		/*
		 *  DESCMISS transfer has been finished, so data will be
		 *  directly copied from internal allocated usb_request
		 *  objects.
		 */
		if (pending_empty && !descmiss_empty &&
		    !(priv_req->flags & REQUEST_INTERNAL)) {
			cdns3_descmiss_copy_data(priv_ep, request);
			list_add_tail(&request->list,
				      &priv_ep->pending_req_list);
			cdns3_gadget_giveback(priv_ep, priv_req,
					      request->status);
			return ret;
		}

		/*
		 * WA2 driver will wait for completion DESCMISS transfer,
		 * before starts new, not DESCMISS transfer.
		 */
		if (!pending_empty && !descmiss_empty)
			deferred = 1;

		if (priv_req->flags & REQUEST_INTERNAL)
			list_add_tail(&priv_req->list,
				      &priv_ep->descmiss_req_list);
	}

	ret = cdns3_prepare_aligned_request_buf(priv_req);
	if (ret < 0)
		return ret;

	ret = usb_gadget_map_request_by_dev(priv_dev->sysdev, request,
					    usb_endpoint_dir_in(ep->desc));
	if (ret)
		return ret;

	/*
	 * If hardware endpoint configuration has not been set yet then
	 * just queue request in deferred list. Transfer will be started in
	 * cdns3_set_hw_configuration.
	 */
	if (!priv_dev->hw_configured_flag)
		deferred = 1;
	else
		ret = cdns3_ep_run_transfer(priv_ep, request);

	if (ret || deferred)
		list_add_tail(&request->list, &priv_ep->deferred_req_list);
	else
		list_add_tail(&request->list, &priv_ep->pending_req_list);

	return ret;
}

static int cdns3_gadget_ep_queue(struct usb_ep *ep, struct usb_request *request,
				 gfp_t gfp_flags)
{
	struct usb_request *zlp_request;
	struct cdns3_endpoint *priv_ep;
	struct cdns3_device *priv_dev;
	unsigned long flags;
	int ret;

	if (!request || !ep)
		return -EINVAL;

	priv_ep = ep_to_cdns3_ep(ep);
	priv_dev = priv_ep->cdns3_dev;

	spin_lock_irqsave(&priv_dev->lock, flags);

	ret = __cdns3_gadget_ep_queue(ep, request, gfp_flags);

	if (ret == 0 && request->zero && request->length &&
	    (request->length % ep->maxpacket == 0)) {
		struct cdns3_request *priv_req;

		zlp_request = cdns3_gadget_ep_alloc_request(ep, GFP_ATOMIC);
		zlp_request->buf = priv_dev->zlp_buf;
		zlp_request->length = 0;

		priv_req = to_cdns3_request(zlp_request);
		priv_req->flags |= REQUEST_ZLP;

		dev_dbg(priv_dev->dev, "Queuing ZLP for endpoint: %s\n",
			priv_ep->name);
		ret = __cdns3_gadget_ep_queue(ep, zlp_request, gfp_flags);
	}

	spin_unlock_irqrestore(&priv_dev->lock, flags);
	return ret;
}

/**
 * cdns3_gadget_ep_dequeue Remove request from transfer queue
 * @ep: endpoint object associated with request
 * @request: request object
 *
 * Returns 0 on success, error code elsewhere
 */
int cdns3_gadget_ep_dequeue(struct usb_ep *ep,
			    struct usb_request *request)
{
	struct cdns3_endpoint *priv_ep = ep_to_cdns3_ep(ep);
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;
	struct usb_request *req, *req_temp;
	struct cdns3_request *priv_req;
	struct cdns3_trb *link_trb;
	unsigned long flags;
	int ret = 0;

	if (!ep || !request || !ep->desc)
		return -EINVAL;

	spin_lock_irqsave(&priv_dev->lock, flags);

	priv_req = to_cdns3_request(request);

	trace_cdns3_ep_dequeue(priv_req);

	cdns3_select_ep(priv_dev, ep->desc->bEndpointAddress);

	list_for_each_entry_safe(req, req_temp, &priv_ep->pending_req_list,
				 list) {
		if (request == req)
			goto found;
	}

	list_for_each_entry_safe(req, req_temp, &priv_ep->deferred_req_list,
				 list) {
		if (request == req)
			goto found;
	}

	goto not_found;

found:

	if (priv_ep->wa1_trb == priv_req->trb)
		cdns3_wa1_restore_cycle_bit(priv_ep);

	link_trb = priv_req->trb;
	cdns3_move_deq_to_next_trb(priv_req);
	cdns3_gadget_giveback(priv_ep, priv_req, -ECONNRESET);

	/* Update ring */
	request = cdns3_next_request(&priv_ep->deferred_req_list);
	if (request) {
		priv_req = to_cdns3_request(request);

		link_trb->buffer = TRB_BUFFER(priv_ep->trb_pool_dma +
					      (priv_req->start_trb * TRB_SIZE));
		link_trb->control = (link_trb->control & TRB_CYCLE) |
				    TRB_TYPE(TRB_LINK) | TRB_CHAIN | TRB_TOGGLE;
	} else {
		priv_ep->flags |= EP_UPDATE_EP_TRBADDR;
	}

not_found:
	spin_unlock_irqrestore(&priv_dev->lock, flags);
	return ret;
}

/**
 * cdns3_gadget_ep_set_halt Sets/clears stall on selected endpoint
 * @ep: endpoint object to set/clear stall on
 * @value: 1 for set stall, 0 for clear stall
 *
 * Returns 0 on success, error code elsewhere
 */
int cdns3_gadget_ep_set_halt(struct usb_ep *ep, int value)
{
	struct cdns3_endpoint *priv_ep = ep_to_cdns3_ep(ep);
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;
	unsigned long flags;
	int ret = 0;

	if (!(priv_ep->flags & EP_ENABLED))
		return -EPERM;

	spin_lock_irqsave(&priv_dev->lock, flags);

	cdns3_select_ep(priv_dev, ep->desc->bEndpointAddress);
	if (value) {
		cdns3_ep_stall_flush(priv_ep);
	} else {
		priv_ep->flags &= ~EP_WEDGE;

		cdns3_dbg(priv_ep->cdns3_dev, "Clear stalled endpoint %s\n",
			  priv_ep->name);

		writel(EP_CMD_CSTALL | EP_CMD_EPRST, &priv_dev->regs->ep_cmd);

		/* wait for EPRST cleared */
		ret = cdns3_handshake(&priv_dev->regs->ep_cmd,
				      EP_CMD_EPRST, 0, 100);
		if (unlikely(ret)) {
			dev_err(priv_dev->dev,
				"Clearing halt condition failed for %s\n",
				priv_ep->name);
			goto finish;

		} else {
			priv_ep->flags &= ~EP_STALL;
		}
	}

	priv_ep->flags &= ~EP_PENDING_REQUEST;
finish:
	spin_unlock_irqrestore(&priv_dev->lock, flags);

	return ret;
}

extern const struct usb_ep_ops cdns3_gadget_ep0_ops;

static const struct usb_ep_ops cdns3_gadget_ep_ops = {
	.enable = cdns3_gadget_ep_enable,
	.disable = cdns3_gadget_ep_disable,
	.alloc_request = cdns3_gadget_ep_alloc_request,
	.free_request = cdns3_gadget_ep_free_request,
	.queue = cdns3_gadget_ep_queue,
	.dequeue = cdns3_gadget_ep_dequeue,
	.set_halt = cdns3_gadget_ep_set_halt,
	.set_wedge = cdns3_gadget_ep_set_wedge,
};

/**
 * cdns3_gadget_get_frame Returns number of actual ITP frame
 * @gadget: gadget object
 *
 * Returns number of actual ITP frame
 */
static int cdns3_gadget_get_frame(struct usb_gadget *gadget)
{
	struct cdns3_device *priv_dev = gadget_to_cdns3_device(gadget);

	return readl(&priv_dev->regs->usb_itpn);
}

static int cdns3_gadget_wakeup(struct usb_gadget *gadget)
{
	return 0;
}

static int cdns3_gadget_set_selfpowered(struct usb_gadget *gadget,
					int is_selfpowered)
{
	struct cdns3_device *priv_dev = gadget_to_cdns3_device(gadget);
	unsigned long flags;

	spin_lock_irqsave(&priv_dev->lock, flags);
	priv_dev->is_selfpowered = !!is_selfpowered;
	spin_unlock_irqrestore(&priv_dev->lock, flags);
	return 0;
}

static int cdns3_gadget_pullup(struct usb_gadget *gadget, int is_on)
{
	struct cdns3_device *priv_dev = gadget_to_cdns3_device(gadget);

	if (!priv_dev->start_gadget)
		return 0;

	if (is_on)
		writel(USB_CONF_DEVEN, &priv_dev->regs->usb_conf);
	else
		writel(USB_CONF_DEVDS, &priv_dev->regs->usb_conf);

	return 0;
}

static void cdns3_gadget_config(struct cdns3_device *priv_dev)
{
	struct cdns3_usb_regs __iomem *regs = priv_dev->regs;

	cdns3_ep0_config(priv_dev);

	/* enable interrupts for endpoint 0 (in and out) */
	writel(EP_IEN_EP_OUT0 | EP_IEN_EP_IN0, &regs->ep_ien);

	/*
	 *Driver need modify LFPS minimal U1 Exit time for 0x00024505 revision
	 * of controller
	 */
	if (priv_dev->dev_ver == 0x00024505) {
		u32 reg = readl(&regs->dbg_link1);

		reg &= ~DBG_LINK1_LFPS_MIN_GEN_U1_EXIT_MASK;
		reg |= DBG_LINK1_LFPS_MIN_GEN_U1_EXIT(0x55) |
		       DBG_LINK1_LFPS_MIN_GEN_U1_EXIT_SET;
		writel(reg, &regs->dbg_link1);
	}

	/* enable generic interrupt*/
	writel(USB_IEN_INIT, &regs->usb_ien);
	writel(USB_CONF_CLK2OFFDS | USB_CONF_L1DS, &regs->usb_conf);
	writel(USB_CONF_DMULT, &regs->usb_conf);
	cdns3_gadget_pullup(&priv_dev->gadget, 1);
}

/**
 * cdns3_gadget_udc_start Gadget start
 * @gadget: gadget object
 * @driver: driver which operates on this gadget
 *
 * Returns 0 on success, error code elsewhere
 */
static int cdns3_gadget_udc_start(struct usb_gadget *gadget,
				  struct usb_gadget_driver *driver)
{
	struct cdns3_device *priv_dev = gadget_to_cdns3_device(gadget);
	unsigned long flags;

	dev_dbg(priv_dev->dev, "%s begins\n", __func__);
	spin_lock_irqsave(&priv_dev->lock, flags);
	priv_dev->gadget_driver = driver;
	if (!priv_dev->start_gadget) {
		spin_unlock_irqrestore(&priv_dev->lock, flags);
		return 0;
	}

	cdns3_gadget_config(priv_dev);
	spin_unlock_irqrestore(&priv_dev->lock, flags);
	return 0;
}

/**
 * cdns3_gadget_udc_stop Stops gadget
 * @gadget: gadget object
 *
 * Returns 0
 */
static int cdns3_gadget_udc_stop(struct usb_gadget *gadget)
{
	struct cdns3_device *priv_dev = gadget_to_cdns3_device(gadget);
	struct cdns3_endpoint *priv_ep;
	u32 bEndpointAddress;
	struct usb_ep *ep;
	int ret = 0;

	priv_dev->gadget_driver = NULL;

	priv_dev->onchip_mem_allocated_size = 0;
	priv_dev->out_mem_is_allocated = 0;
	priv_dev->gadget.speed = USB_SPEED_UNKNOWN;

	list_for_each_entry(ep, &priv_dev->gadget.ep_list, ep_list) {
		priv_ep = ep_to_cdns3_ep(ep);
		bEndpointAddress = priv_ep->num | priv_ep->dir;
		cdns3_select_ep(priv_dev, bEndpointAddress);
		writel(EP_CMD_EPRST, &priv_dev->regs->ep_cmd);
		ret = cdns3_handshake(&priv_dev->regs->ep_cmd,
				      EP_CMD_EPRST, 0, 100);
		cdns3_free_trb_pool(priv_ep);
	}

	/* disable interrupt for device */
	writel(0, &priv_dev->regs->usb_ien);
	writel(USB_CONF_DEVDS, &priv_dev->regs->usb_conf);

	return ret;
}

static const struct usb_gadget_ops cdns3_gadget_ops = {
	.get_frame = cdns3_gadget_get_frame,
	.wakeup = cdns3_gadget_wakeup,
	.set_selfpowered = cdns3_gadget_set_selfpowered,
	.pullup = cdns3_gadget_pullup,
	.udc_start = cdns3_gadget_udc_start,
	.udc_stop = cdns3_gadget_udc_stop,
	.match_ep = cdns3_gadget_match_ep,
};

static void cdns3_free_all_eps(struct cdns3_device *priv_dev)
{
	int i;

	/*ep0 OUT point to ep0 IN*/
	priv_dev->eps[16] = NULL;

	cdns3_free_trb_pool(priv_dev->eps[0]);

	for (i = 0; i < CDNS3_ENDPOINTS_MAX_COUNT; i++)
		if (priv_dev->eps[i])
			devm_kfree(priv_dev->dev, priv_dev->eps[i]);
}

/**
 * cdns3_init_eps Initializes software endpoints of gadget
 * @cdns3: extended gadget object
 *
 * Returns 0 on success, error code elsewhere
 */
static int cdns3_init_eps(struct cdns3_device *priv_dev)
{
	u32 ep_enabled_reg, iso_ep_reg;
	struct cdns3_endpoint *priv_ep;
	int ep_dir, ep_number;
	u32 ep_mask;
	int ret = 0;
	int i;

	/* Read it from USB_CAP3 and USB_CAP4 */
	ep_enabled_reg = 0x00ff00ff;
	iso_ep_reg = 0x00fe00fe;

	dev_dbg(priv_dev->dev, "Initializing non-zero endpoints\n");

	for (i = 0; i < CDNS3_ENDPOINTS_MAX_COUNT; i++) {
		ep_dir = i >> 4;	/* i div 16 */
		ep_number = i & 0xF;	/* i % 16 */
		ep_mask = BIT(i);

		if (!(ep_enabled_reg & ep_mask))
			continue;

		if (ep_dir && !ep_number) {
			priv_dev->eps[i] = priv_dev->eps[0];
			continue;
		}

		priv_ep = devm_kzalloc(priv_dev->dev, sizeof(*priv_ep),
				       GFP_KERNEL);
		if (!priv_ep) {
			ret = -ENOMEM;
			goto err;
		}

		/* set parent of endpoint object */
		priv_ep->cdns3_dev = priv_dev;
		priv_dev->eps[i] = priv_ep;
		priv_ep->num = ep_number;
		priv_ep->dir = ep_dir ? USB_DIR_IN : USB_DIR_OUT;

		if (!ep_number) {
			ret = cdns3_init_ep0(priv_dev, priv_ep);
			if (ret) {
				dev_err(priv_dev->dev, "Failed to init ep0\n");
				goto err;
			}
		} else {
			snprintf(priv_ep->name, sizeof(priv_ep->name), "ep%d%s",
				 ep_number, !!ep_dir ? "in" : "out");
			priv_ep->endpoint.name = priv_ep->name;

			usb_ep_set_maxpacket_limit(&priv_ep->endpoint,
						   CDNS3_EP_MAX_PACKET_LIMIT);
			priv_ep->endpoint.max_streams = CDNS3_EP_MAX_STREAMS;
			priv_ep->endpoint.ops = &cdns3_gadget_ep_ops;
			if (ep_dir)
				priv_ep->endpoint.caps.dir_in = 1;
			else
				priv_ep->endpoint.caps.dir_out = 1;

			if (iso_ep_reg & ep_mask)
				priv_ep->endpoint.caps.type_iso = 1;

			priv_ep->endpoint.caps.type_bulk = 1;
			priv_ep->endpoint.caps.type_int = 1;

			list_add_tail(&priv_ep->endpoint.ep_list,
				      &priv_dev->gadget.ep_list);
		}

		priv_ep->flags = 0;

		dev_dbg(priv_dev->dev, "Initialized  %s support: %s %s\n",
			 priv_ep->name,
			 priv_ep->endpoint.caps.type_bulk ? "BULK, INT" : "",
			 priv_ep->endpoint.caps.type_iso ? "ISO" : "");

		INIT_LIST_HEAD(&priv_ep->pending_req_list);
		INIT_LIST_HEAD(&priv_ep->deferred_req_list);
		INIT_LIST_HEAD(&priv_ep->descmiss_req_list);
	}

	return 0;
err:
	cdns3_free_all_eps(priv_dev);
	return -ENOMEM;
}

static void cdns3_gadget_disable(struct cdns3 *cdns)
{
	struct cdns3_device *priv_dev;

	priv_dev = cdns->gadget_dev;

	if (priv_dev->gadget_driver)
		priv_dev->gadget_driver->disconnect(&priv_dev->gadget);

	usb_gadget_disconnect(&priv_dev->gadget);
	priv_dev->gadget.speed = USB_SPEED_UNKNOWN;
}

void cdns3_gadget_exit(struct cdns3 *cdns)
{
	struct cdns3_device *priv_dev;

	priv_dev = cdns->gadget_dev;

	cdns3_gadget_disable(cdns);

	pm_runtime_mark_last_busy(cdns->dev);
	pm_runtime_put_autosuspend(cdns->dev);

	usb_del_gadget_udc(&priv_dev->gadget);

	cdns3_free_all_eps(priv_dev);

	while (!list_empty(&priv_dev->aligned_buf_list)) {
		struct cdns3_aligned_buf *buf;

		buf = cdns3_next_align_buf(&priv_dev->aligned_buf_list);

		dma_free_coherent(priv_dev->sysdev, buf->size,
				  buf->buf,
				  buf->dma);

		list_del(&buf->list);
		kfree(buf);
	}

	dma_free_coherent(priv_dev->sysdev, 8, priv_dev->setup_buf,
			  priv_dev->setup_dma);

	kfree(priv_dev->zlp_buf);
	kfree(priv_dev);
	cdns->gadget_dev = NULL;
}

static int cdns3_gadget_start(struct cdns3 *cdns)
{
	struct cdns3_device *priv_dev = cdns->gadget_dev;
	unsigned long flags;

	pm_runtime_get_sync(cdns->dev);
	spin_lock_irqsave(&priv_dev->lock, flags);
	priv_dev->start_gadget = 1;
	if (!priv_dev->gadget_driver) {
		spin_unlock_irqrestore(&priv_dev->lock, flags);
		return 0;
	}

	cdns3_gadget_config(priv_dev);
	spin_unlock_irqrestore(&priv_dev->lock, flags);

	return 0;
}

static int __cdns3_gadget_init(struct cdns3 *cdns)
{
	struct cdns3_device *priv_dev;
	u32 max_speed;
	int ret = 0;

	priv_dev = kzalloc(sizeof(*priv_dev), GFP_KERNEL);
	if (!priv_dev)
		return -ENOMEM;

	cdns->gadget_dev = priv_dev;
	priv_dev->sysdev = cdns->dev;
	priv_dev->dev = cdns->dev;
	priv_dev->regs = cdns->dev_regs;

	max_speed = usb_get_maximum_speed(cdns->dev);

	/* Check the maximum_speed parameter */
	switch (max_speed) {
	case USB_SPEED_FULL:
	case USB_SPEED_HIGH:
	case USB_SPEED_SUPER:
		break;
	default:
		dev_err(cdns->dev, "invalid maximum_speed parameter %d\n",
			max_speed);
		/* fall through */
	case USB_SPEED_UNKNOWN:
		/* default to superspeed */
		max_speed = USB_SPEED_SUPER;
		break;
	}

	/* fill gadget fields */
	priv_dev->gadget.max_speed = max_speed;
	priv_dev->gadget.speed = USB_SPEED_UNKNOWN;
	priv_dev->gadget.ops = &cdns3_gadget_ops;
	priv_dev->gadget.name = "usb-ss-gadget";
	priv_dev->gadget.sg_supported = 1;
	priv_dev->gadget.quirk_avoids_skb_reserve = 1;

	spin_lock_init(&priv_dev->lock);
	INIT_WORK(&priv_dev->pending_status_wq,
		  cdns3_pending_setup_status_handler);

	/* initialize endpoint container */
	INIT_LIST_HEAD(&priv_dev->gadget.ep_list);
	INIT_LIST_HEAD(&priv_dev->aligned_buf_list);

	pm_runtime_get_sync(cdns->dev);
	ret = cdns3_init_eps(priv_dev);
	if (ret) {
		dev_err(priv_dev->dev, "Failed to create endpoints\n");
		goto err1;
	}

	/* allocate memory for setup packet buffer */
	priv_dev->setup_buf = dma_alloc_coherent(priv_dev->sysdev, 8,
						 &priv_dev->setup_dma, GFP_DMA);
	if (!priv_dev->setup_buf) {
		dev_err(priv_dev->dev, "Failed to allocate memory for SETUP buffer\n");
		ret = -ENOMEM;
		goto err2;
	}

	priv_dev->zlp_buf = kzalloc(CDNS3_EP_ZLP_BUF_SIZE, GFP_KERNEL);
	if (!priv_dev->zlp_buf) {
		ret = -ENOMEM;
		goto err3;
	}

	/* add USB gadget device */
	ret = usb_add_gadget_udc(priv_dev->dev, &priv_dev->gadget);
	if (ret < 0) {
		dev_err(priv_dev->dev,
			"Failed to register USB device controller\n");
		goto err4;
	}

	if (ret)
		goto err4;


	pm_runtime_put_sync(cdns->dev);
	return 0;
err4:
	kfree(priv_dev->zlp_buf);
err3:
	dma_free_coherent(priv_dev->sysdev, 8, priv_dev->setup_buf,
			  priv_dev->setup_dma);
err2:
	cdns3_free_all_eps(priv_dev);
err1:
	cdns->gadget_dev = NULL;
	pm_runtime_put_sync(cdns->dev);
	return ret;
}

static void __cdns3_gadget_stop(struct cdns3 *cdns)
{
	struct cdns3_device *priv_dev = cdns->gadget_dev;
	unsigned long flags;

	cdns3_gadget_disable(cdns);
	spin_lock_irqsave(&priv_dev->lock, flags);
	usb_gadget_set_state(&priv_dev->gadget, USB_STATE_NOTATTACHED);
	/* disable interrupt for device */
	writel(0, &priv_dev->regs->usb_ien);
	writel(USB_CONF_DEVDS, &priv_dev->regs->usb_conf);
	priv_dev->start_gadget = 0;
	spin_unlock_irqrestore(&priv_dev->lock, flags);
}

static void cdns3_gadget_stop(struct cdns3 *cdns)
{
	if (cdns->role == CDNS3_ROLE_GADGET)
		__cdns3_gadget_stop(cdns);
	pm_runtime_mark_last_busy(cdns->dev);
	pm_runtime_put_autosuspend(cdns->dev);
}

static int cdns3_gadget_suspend(struct cdns3 *cdns, bool do_wakeup)
{
	__cdns3_gadget_stop(cdns);
	return 0;
}

static int cdns3_gadget_resume(struct cdns3 *cdns, bool hibernated)
{
	struct cdns3_device *priv_dev = cdns->gadget_dev;
	unsigned long flags;

	spin_lock_irqsave(&priv_dev->lock, flags);
	priv_dev->start_gadget = 1;

	if (!priv_dev->gadget_driver) {
		spin_unlock_irqrestore(&priv_dev->lock, flags);
		return 0;
	}

	cdns3_gadget_config(priv_dev);
	spin_unlock_irqrestore(&priv_dev->lock, flags);
	return 0;
}

/**
 * cdns3_gadget_init - initialize device structure
 *
 * cdns: cdns3 instance
 *
 * This function initializes the gadget.
 */
int cdns3_gadget_init(struct cdns3 *cdns)
{
	struct cdns3_role_driver *rdrv;

	rdrv = devm_kzalloc(cdns->dev, sizeof(*rdrv), GFP_KERNEL);
	if (!rdrv)
		return -ENOMEM;

	rdrv->start	= cdns3_gadget_start;
	rdrv->stop	= cdns3_gadget_stop;
	rdrv->suspend	= cdns3_gadget_suspend;
	rdrv->resume	= cdns3_gadget_resume;
	rdrv->irq	= cdns3_device_irq_handler;
	rdrv->thread_irq = cdns3_device_thread_irq_handler;
	rdrv->name	= "gadget";
	cdns->roles[CDNS3_ROLE_GADGET] = rdrv;

	return __cdns3_gadget_init(cdns);
}
