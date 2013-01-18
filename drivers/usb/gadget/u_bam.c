/* Copyright (c) 2011, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/termios.h>
#include <mach/msm_smd.h>
#include <linux/netdevice.h>
#include <mach/bam_dmux.h>
#include <linux/debugfs.h>
#include <linux/bitops.h>
#include <linux/termios.h>

#include "u_rmnet.h"

#define N_PORTS	1

static struct workqueue_struct *gbam_wq;
static int n_ports;
static unsigned bam_ch_ids[] = { 8 };

#define TX_PKT_DROP_THRESHOLD			1000
#define RX_PKT_FLOW_CTRL_EN_THRESHOLD		1000
#define RX_PKT_FLOW_CTRL_DISABLE		500
#define RX_PKT_FLOW_CTRL_SUPPORT		1

#define BAM_MUX_HDR				8

#define RX_Q_SIZE				16
#define TX_Q_SIZE				200
#define RX_REQ_SIZE				(2048 - BAM_MUX_HDR)

unsigned int tx_pkt_drop_thld = TX_PKT_DROP_THRESHOLD;
module_param(tx_pkt_drop_thld, uint, S_IRUGO | S_IWUSR);

unsigned int rx_fctrl_en_thld = RX_PKT_FLOW_CTRL_EN_THRESHOLD;
module_param(rx_fctrl_en_thld, uint, S_IRUGO | S_IWUSR);

unsigned int rx_fctrl_support = RX_PKT_FLOW_CTRL_SUPPORT;
module_param(rx_fctrl_support, uint, S_IRUGO | S_IWUSR);

unsigned int rx_fctrl_dis_thld = RX_PKT_FLOW_CTRL_DISABLE;
module_param(rx_fctrl_dis_thld, uint, S_IRUGO | S_IWUSR);

unsigned int tx_q_size = TX_Q_SIZE;
module_param(tx_q_size, uint, S_IRUGO | S_IWUSR);

unsigned int rx_q_size = RX_Q_SIZE;
module_param(rx_q_size, uint, S_IRUGO | S_IWUSR);

unsigned int rx_req_size = RX_REQ_SIZE;
module_param(rx_req_size, uint, S_IRUGO | S_IWUSR);

struct bam_ch_info {
	atomic_t		opened;
	unsigned		id;

	struct list_head        tx_idle;
	struct sk_buff_head	tx_skb_q;

	struct list_head        rx_idle;
	struct sk_buff_head	rx_skb_q;

	struct gbam_port	*port;
	struct work_struct	write_tobam_w;

	/* stats */
	unsigned int		pending_with_bam;
	unsigned int		tohost_drp_cnt;
	unsigned int		tomodem_drp_cnt;
	unsigned int		tx_len;
	unsigned int		rx_len;
	unsigned long		to_modem;
	unsigned long		to_host;
};

struct gbam_port {
	unsigned		port_num;
	spinlock_t		port_lock;

	struct grmnet		*port_usb;

	struct bam_ch_info	data_ch;

	struct work_struct	connect_w;
};

static struct portmaster {
	struct gbam_port *port;
} ports[N_PORTS];

static void gbam_start_rx(struct gbam_port *port);

/*---------------misc functions---------------- */
static void gbam_free_requests(struct usb_ep *ep, struct list_head *head)
{
	struct usb_request	*req;

	while (!list_empty(head)) {
		req = list_entry(head->next, struct usb_request, list);
		list_del(&req->list);
		usb_ep_free_request(ep, req);
	}
}

static int gbam_alloc_requests(struct usb_ep *ep, struct list_head *head,
		int num,
		void (*cb)(struct usb_ep *ep, struct usb_request *),
		gfp_t flags)
{
	int i;
	struct usb_request *req;

	pr_debug("%s: ep:%p head:%p num:%d cb:%p", __func__,
			ep, head, num, cb);

	for (i = 0; i < num; i++) {
		req = usb_ep_alloc_request(ep, flags);
		if (!req) {
			pr_debug("%s: req allocated:%d\n", __func__, i);
			return list_empty(head) ? -ENOMEM : 0;
		}
		req->complete = cb;
		list_add(&req->list, head);
	}

	return 0;
}
/*--------------------------------------------- */

/*------------data_path----------------------------*/
static void gbam_write_data_tohost(struct gbam_port *port)
{
	unsigned long			flags;
	struct bam_ch_info		*d = &port->data_ch;
	struct sk_buff			*skb;
	int				ret;
	struct usb_request		*req;
	struct usb_ep			*ep;

	spin_lock_irqsave(&port->port_lock, flags);
	if (!port->port_usb) {
		spin_unlock_irqrestore(&port->port_lock, flags);
		return;
	}

	ep = port->port_usb->in;

	while (!list_empty(&d->tx_idle)) {
		skb = __skb_dequeue(&d->tx_skb_q);
		if (!skb) {
			spin_unlock_irqrestore(&port->port_lock, flags);
			return;
		}
		req = list_first_entry(&d->tx_idle,
				struct usb_request,
				list);
		req->context = skb;
		req->buf = skb->data;
		req->length = skb->len;

		list_del(&req->list);

		spin_unlock(&port->port_lock);
		ret = usb_ep_queue(ep, req, GFP_ATOMIC);
		spin_lock(&port->port_lock);
		if (ret) {
			pr_err("%s: usb epIn failed\n", __func__);
			list_add(&req->list, &d->tx_idle);
			dev_kfree_skb_any(skb);
			break;
		}
		d->to_host++;
	}
	spin_unlock_irqrestore(&port->port_lock, flags);
}

void gbam_data_recv_cb(void *p, struct sk_buff *skb)
{
	struct gbam_port	*port = p;
	struct bam_ch_info	*d = &port->data_ch;
	unsigned long		flags;

	if (!skb)
		return;

	pr_debug("%s: p:%p#%d d:%p skb_len:%d\n", __func__,
			port, port->port_num, d, skb->len);

	spin_lock_irqsave(&port->port_lock, flags);
	if (!port->port_usb) {
		spin_unlock_irqrestore(&port->port_lock, flags);
		dev_kfree_skb_any(skb);
		return;
	}

	if (d->tx_skb_q.qlen > tx_pkt_drop_thld) {
		d->tohost_drp_cnt++;
		if (printk_ratelimit())
			pr_err("%s: tx pkt dropped: tx_drop_cnt:%u\n",
					__func__, d->tohost_drp_cnt);
		spin_unlock_irqrestore(&port->port_lock, flags);
		dev_kfree_skb_any(skb);
		return;
	}

	__skb_queue_tail(&d->tx_skb_q, skb);
	spin_unlock_irqrestore(&port->port_lock, flags);

	gbam_write_data_tohost(port);
}

void gbam_data_write_done(void *p, struct sk_buff *skb)
{
	struct gbam_port	*port = p;
	struct bam_ch_info	*d = &port->data_ch;
	unsigned long		flags;

	if (!skb)
		return;

	dev_kfree_skb_any(skb);

	spin_lock_irqsave(&port->port_lock, flags);

	d->pending_with_bam--;

	pr_debug("%s: port:%p d:%p tom:%lu pbam:%u, pno:%d\n", __func__,
			port, d, d->to_modem,
			d->pending_with_bam, port->port_num);

	if (rx_fctrl_support &&
			d->pending_with_bam >= rx_fctrl_dis_thld) {

		spin_unlock_irqrestore(&port->port_lock, flags);
		return;
	}
	spin_unlock_irqrestore(&port->port_lock, flags);

	gbam_start_rx(port);
}

static void gbam_data_write_tobam(struct work_struct *w)
{
	struct gbam_port	*port;
	struct bam_ch_info	*d;
	struct sk_buff		*skb;
	unsigned long		flags;
	int			ret;

	d = container_of(w, struct bam_ch_info, write_tobam_w);
	port = d->port;

	spin_lock_irqsave(&port->port_lock, flags);
	if (!port->port_usb) {
		spin_unlock_irqrestore(&port->port_lock, flags);
		return;
	}

	while ((skb = __skb_dequeue(&d->rx_skb_q))) {
		d->pending_with_bam++;
		d->to_modem++;

		pr_debug("%s: port:%p d:%p tom:%lu pbam:%u pno:%d\n", __func__,
				port, d, d->to_modem, d->pending_with_bam,
				port->port_num);

		spin_unlock_irqrestore(&port->port_lock, flags);
		ret = msm_bam_dmux_write(d->id, skb);
		spin_lock_irqsave(&port->port_lock, flags);
		if (ret) {
			pr_debug("%s: write error:%d\n", __func__, ret);
			d->pending_with_bam--;
			d->to_modem--;
			d->tomodem_drp_cnt++;
			dev_kfree_skb_any(skb);
			break;
		}
	}
	spin_unlock_irqrestore(&port->port_lock, flags);
}
/*-------------------------------------------------------------*/

static void gbam_epin_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct gbam_port	*port = ep->driver_data;
	struct bam_ch_info	*d;
	struct sk_buff		*skb = req->context;
	int			status = req->status;

	switch (status) {
	case 0:
		/* successful completion */
	case -ECONNRESET:
	case -ESHUTDOWN:
		/* connection gone */
		break;
	default:
		pr_err("%s: data tx ep error %d\n",
				__func__, status);
		break;
	}

	dev_kfree_skb_any(skb);

	if (!port)
		return;

	spin_lock(&port->port_lock);
	d = &port->data_ch;
	list_add_tail(&req->list, &d->tx_idle);
	spin_unlock(&port->port_lock);

	gbam_write_data_tohost(port);
}

static void
gbam_epout_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct gbam_port	*port = ep->driver_data;
	struct bam_ch_info	*d = &port->data_ch;
	struct sk_buff		*skb = req->context;
	int			status = req->status;
	int			queue = 0;

	switch (status) {
	case 0:
		skb_put(skb, req->actual);
		queue = 1;
		break;
	case -ECONNRESET:
	case -ESHUTDOWN:
		/* cable disconnection */
		dev_kfree_skb_any(skb);
		req->buf = 0;
		usb_ep_free_request(ep, req);
		return;
	default:
		if (printk_ratelimit())
			pr_err("%s: %s response error %d, %d/%d\n",
				__func__, ep->name, status,
				req->actual, req->length);
		dev_kfree_skb_any(skb);
		break;
	}

	spin_lock(&port->port_lock);
	if (queue) {
		__skb_queue_tail(&d->rx_skb_q, skb);
		queue_work(gbam_wq, &d->write_tobam_w);
	}

	/* TODO: Handle flow control gracefully by having
	 * having call back mechanism from bam driver
	 */
	if (rx_fctrl_support &&
		d->pending_with_bam >= rx_fctrl_en_thld) {

		list_add_tail(&req->list, &d->rx_idle);
		spin_unlock(&port->port_lock);
		return;
	}
	spin_unlock(&port->port_lock);

	skb = alloc_skb(rx_req_size + BAM_MUX_HDR, GFP_ATOMIC);
	if (!skb) {
		spin_lock(&port->port_lock);
		list_add_tail(&req->list, &d->rx_idle);
		spin_unlock(&port->port_lock);
		return;
	}
	skb_reserve(skb, BAM_MUX_HDR);

	req->buf = skb->data;
	req->length = rx_req_size;
	req->context = skb;

	status = usb_ep_queue(ep, req, GFP_ATOMIC);
	if (status) {
		dev_kfree_skb_any(skb);

		if (printk_ratelimit())
			pr_err("%s: data rx enqueue err %d\n",
					__func__, status);

		spin_lock(&port->port_lock);
		list_add_tail(&req->list, &d->rx_idle);
		spin_unlock(&port->port_lock);
	}
}

static void gbam_start_rx(struct gbam_port *port)
{
	struct usb_request		*req;
	struct bam_ch_info		*d;
	struct usb_ep			*ep;
	unsigned long			flags;
	int				ret;
	struct sk_buff			*skb;

	spin_lock_irqsave(&port->port_lock, flags);
	if (!port->port_usb) {
		spin_unlock_irqrestore(&port->port_lock, flags);
		return;
	}

	d = &port->data_ch;
	ep = port->port_usb->out;

	while (port->port_usb && !list_empty(&d->rx_idle)) {
		req = list_first_entry(&d->rx_idle, struct usb_request, list);

		skb = alloc_skb(rx_req_size + BAM_MUX_HDR, GFP_ATOMIC);
		if (!skb)
			break;
		skb_reserve(skb, BAM_MUX_HDR);

		list_del(&req->list);
		req->buf = skb->data;
		req->length = rx_req_size;
		req->context = skb;

		spin_unlock_irqrestore(&port->port_lock, flags);
		ret = usb_ep_queue(ep, req, GFP_ATOMIC);
		spin_lock_irqsave(&port->port_lock, flags);
		if (ret) {
			dev_kfree_skb_any(skb);

			if (printk_ratelimit())
				pr_err("%s: rx queue failed\n", __func__);

			if (port->port_usb)
				list_add(&req->list, &d->rx_idle);
			else
				usb_ep_free_request(ep, req);
			break;
		}
	}
	spin_unlock_irqrestore(&port->port_lock, flags);
}

static void gbam_start_io(struct gbam_port *port)
{
	unsigned long		flags;
	struct usb_ep		*ep;
	int			ret;
	struct bam_ch_info	*d;

	pr_debug("%s: port:%p\n", __func__, port);

	spin_lock_irqsave(&port->port_lock, flags);
	if (!port->port_usb) {
		spin_unlock_irqrestore(&port->port_lock, flags);
		return;
	}

	d = &port->data_ch;
	ep = port->port_usb->out;
	ret = gbam_alloc_requests(ep, &d->rx_idle, rx_q_size,
			gbam_epout_complete, GFP_ATOMIC);
	if (ret) {
		pr_err("%s: rx req allocation failed\n", __func__);
		return;
	}

	ep = port->port_usb->in;
	ret = gbam_alloc_requests(ep, &d->tx_idle, tx_q_size,
			gbam_epin_complete, GFP_ATOMIC);
	if (ret) {
		pr_err("%s: tx req allocation failed\n", __func__);
		gbam_free_requests(ep, &d->rx_idle);
		return;
	}

	spin_unlock_irqrestore(&port->port_lock, flags);

	/* queue out requests */
	gbam_start_rx(port);
}

static void gbam_connect_work(struct work_struct *w)
{
	struct gbam_port *port = container_of(w, struct gbam_port, connect_w);
	struct bam_ch_info *d = &port->data_ch;
	int ret;

	ret = msm_bam_dmux_open(d->id, port,
				gbam_data_recv_cb,
				gbam_data_write_done);
	if (ret) {
		pr_err("%s: unable open bam ch:%d err:%d\n",
				__func__, d->id, ret);
		return;
	}
	atomic_set(&d->opened, 1);

	gbam_start_io(port);

	pr_debug("%s: done\n", __func__);
}

static void gbam_port_free(int portno)
{
	struct gbam_port *port = ports[portno].port;

	if (!port)
		kfree(port);
}

static int gbam_port_alloc(int portno)
{
	struct gbam_port	*port;
	struct bam_ch_info	*d;

	port = kzalloc(sizeof(struct gbam_port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	port->port_num = portno;

	/* port initialization */
	spin_lock_init(&port->port_lock);
	INIT_WORK(&port->connect_w, gbam_connect_work);

	/* data ch */
	d = &port->data_ch;
	d->port = port;
	INIT_LIST_HEAD(&d->tx_idle);
	INIT_LIST_HEAD(&d->rx_idle);
	INIT_WORK(&d->write_tobam_w, gbam_data_write_tobam);
	skb_queue_head_init(&d->tx_skb_q);
	skb_queue_head_init(&d->rx_skb_q);
	d->id = bam_ch_ids[portno];

	ports[portno].port = port;

	pr_debug("%s: port:%p portno:%d\n", __func__, port, portno);

	return 0;
}

#if defined(CONFIG_DEBUG_FS)
#define DEBUG_BUF_SIZE	1024
static ssize_t gbam_read_stats(struct file *file, char __user *ubuf,
		size_t count, loff_t *ppos)
{
	struct gbam_port	*port;
	struct bam_ch_info	*d;
	char			*buf;
	unsigned long		flags;
	int			ret;
	int			i;
	int			temp = 0;

	buf = kzalloc(sizeof(char) * DEBUG_BUF_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (i = 0; i < n_ports; i++) {
		port = ports[i].port;
		if (!port)
			continue;
		spin_lock_irqsave(&port->port_lock, flags);

		d = &port->data_ch;

		temp += scnprintf(buf + temp, DEBUG_BUF_SIZE - temp,
				"#PORT:%d port:%p data_ch:%p#\n"
				"dpkts_to_usbhost: %lu\n"
				"dpkts_to_modem:  %lu\n"
				"dpkts_pwith_bam: %u\n"
				"to_usbhost_dcnt:  %u\n"
				"tomodem__dcnt:  %u\n"
				"tx_buf_len:	 %u\n"
				"data_ch_opened: %d\n",
				i, port, &port->data_ch,
				d->to_host, d->to_modem,
				d->pending_with_bam,
				d->tohost_drp_cnt, d->tomodem_drp_cnt,
				d->tx_skb_q.qlen, atomic_read(&d->opened));

		spin_unlock_irqrestore(&port->port_lock, flags);
	}

	ret = simple_read_from_buffer(ubuf, count, ppos, buf, temp);

	kfree(buf);

	return ret;
}

static ssize_t gbam_reset_stats(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct gbam_port	*port;
	struct bam_ch_info	*d;
	int			i;
	unsigned long		flags;

	for (i = 0; i < n_ports; i++) {
		port = ports[i].port;
		if (!port)
			continue;

		spin_lock_irqsave(&port->port_lock, flags);

		d = &port->data_ch;

		d->to_host = 0;
		d->to_modem = 0;
		d->pending_with_bam = 0;
		d->tohost_drp_cnt = 0;
		d->tomodem_drp_cnt = 0;

		spin_unlock_irqrestore(&port->port_lock, flags);
	}
	return count;
}

const struct file_operations gbam_stats_ops = {
	.read = gbam_read_stats,
	.write = gbam_reset_stats,
};

static void gbam_debugfs_init(void)
{
	struct dentry *dent;
	struct dentry *dfile;

	dent = debugfs_create_dir("usb_rmnet", 0);
	if (IS_ERR(dent))
		return;

	/* TODO: Implement cleanup function to remove created file */
	dfile = debugfs_create_file("status", 0444, dent, 0, &gbam_stats_ops);
	if (!dfile || IS_ERR(dfile))
		debugfs_remove(dent);
}
#else
static void gam_debugfs_init(void) { }
#endif

static void gbam_free_buffers(struct gbam_port *port)
{
	struct sk_buff		*skb;
	unsigned long		flags;
	struct bam_ch_info	*d;

	spin_lock_irqsave(&port->port_lock, flags);

	if (!port || !port->port_usb)
		goto free_buf_out;

	d = &port->data_ch;

	gbam_free_requests(port->port_usb->in, &d->tx_idle);
	gbam_free_requests(port->port_usb->out, &d->rx_idle);

	while ((skb = __skb_dequeue(&d->tx_skb_q)))
		dev_kfree_skb_any(skb);

	while ((skb = __skb_dequeue(&d->rx_skb_q)))
		dev_kfree_skb_any(skb);

free_buf_out:
	spin_unlock_irqrestore(&port->port_lock, flags);
}

void gbam_disconnect(struct grmnet *gr, u8 port_num)
{
	struct gbam_port	*port;
	unsigned long		flags;
	struct bam_ch_info	*d;

	pr_debug("%s: grmnet:%p port#%d\n", __func__, gr, port_num);

	if (port_num >= n_ports) {
		pr_err("%s: invalid portno#%d\n", __func__, port_num);
		return;
	}

	if (!gr) {
		pr_err("%s: grmnet port is null\n", __func__);
		return;
	}

	port = ports[port_num].port;
	d = &port->data_ch;

	gbam_free_buffers(port);

	spin_lock_irqsave(&port->port_lock, flags);
	port->port_usb = 0;
	spin_unlock_irqrestore(&port->port_lock, flags);

	/* disable endpoints */
	usb_ep_disable(gr->out);
	usb_ep_disable(gr->in);

	if (atomic_read(&d->opened))
		msm_bam_dmux_close(d->id);

	atomic_set(&d->opened, 0);
}

int gbam_connect(struct grmnet *gr, u8 port_num)
{
	struct gbam_port	*port;
	struct bam_ch_info	*d;
	int			ret;
	unsigned long		flags;

	pr_debug("%s: grmnet:%p port#%d\n", __func__, gr, port_num);

	if (port_num >= n_ports) {
		pr_err("%s: invalid portno#%d\n", __func__, port_num);
		return -ENODEV;
	}

	if (!gr) {
		pr_err("%s: grmnet port is null\n", __func__);
		return -ENODEV;
	}

	port = ports[port_num].port;
	d = &port->data_ch;

	ret = usb_ep_enable(gr->in, gr->in_desc);
	if (ret) {
		pr_err("%s: usb_ep_enable failed eptype:IN ep:%p",
				__func__, gr->in);
		return ret;
	}
	gr->in->driver_data = port;

	ret = usb_ep_enable(gr->out, gr->out_desc);
	if (ret) {
		pr_err("%s: usb_ep_enable failed eptype:OUT ep:%p",
				__func__, gr->out);
		gr->in->driver_data = 0;
		return ret;
	}
	gr->out->driver_data = port;

	spin_lock_irqsave(&port->port_lock, flags);
	port->port_usb = gr;

	d->to_host = 0;
	d->to_modem = 0;
	d->pending_with_bam = 0;
	d->tohost_drp_cnt = 0;
	d->tomodem_drp_cnt = 0;
	spin_unlock_irqrestore(&port->port_lock, flags);


	queue_work(gbam_wq, &port->connect_w);

	return 0;
}

int gbam_setup(unsigned int count)
{
	int	i;
	int	ret;

	pr_debug("%s: requested ports:%d\n", __func__, count);

	if (!count || count > N_PORTS) {
		pr_err("%s: Invalid num of ports count:%d\n",
				__func__, count);
		return -EINVAL;
	}

	gbam_wq = alloc_workqueue("k_gbam", WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!gbam_wq) {
		pr_err("%s: Unable to create workqueue gbam_wq\n",
				__func__);
		return -ENOMEM;
	}

	for (i = 0; i < count; i++) {
		ret = gbam_port_alloc(i);
		if (ret) {
			pr_err("%s: Unable to alloc port:%d\n", __func__, i);
			goto free_ports;
		}
		n_ports++;
	}

	gbam_debugfs_init();

	return 0;
free_ports:
	for (i = 0; i < n_ports; i++)
		gbam_port_free(i);

	destroy_workqueue(gbam_wq);

	return ret;
}
