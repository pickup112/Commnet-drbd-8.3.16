/*
 * 	connector.c
 * 
 * 2004-2005 Copyright (c) Evgeniy Polyakov <johnpol@2ka.mipt.ru>
 * All rights reserved.
 * 
 * Modified by Philipp Reiser to work on older 2.6.x kernels.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/skbuff.h>
#include <linux/netlink.h>
#include <linux/moduleparam.h>
#include <linux/connector.h>
#include <linux/capability.h>

#ifndef DRBD_CONNECTOR_BACKPORT_HEADER
#error "drbd backported connector.c compiled against kernel connector.h will not work"
#error "enable CONFIG_CONNECTOR in your kernel and try again"
#endif

#include <net/sock.h>

#ifdef DRBD_NL_DST_GROUPS
   /* pre 2.6.16 */
#  define NETLINK_GROUP(skb) NETLINK_CB(skb).dst_groups
#else
#  define NETLINK_GROUP(skb) NETLINK_CB(skb).dst_group
#endif

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Evgeniy Polyakov <johnpol@2ka.mipt.ru>");
MODULE_DESCRIPTION("Generic userspace <-> kernelspace connector.");

static u32 cn_idx = CN_IDX_CONNECTOR;
static u32 cn_val = CN_VAL_CONNECTOR;

module_param(cn_idx, uint, 0);
module_param(cn_val, uint, 0);
MODULE_PARM_DESC(cn_idx, "Connector's main device idx.");
MODULE_PARM_DESC(cn_val, "Connector's main device val.");

static DECLARE_MUTEX(notify_lock);
static LIST_HEAD(notify_list);

static struct cn_dev cdev;

int cn_already_initialized = 0;

/*
 * msg->seq and msg->ack are used to determine message genealogy.
 * When someone sends message it puts there locally unique sequence
 * and random acknowledge numbers.  Sequence number may be copied into
 * nlmsghdr->nlmsg_seq too.
 *
 * Sequence number is incremented with each message to be sent.
 *
 * If we expect reply to our message then the sequence number in
 * received message MUST be the same as in original message, and
 * acknowledge number MUST be the same + 1.
 *
 * If we receive a message and its sequence number is not equal to the
 * one we are expecting then it is a new message.
 *
 * If we receive a message and its sequence number is the same as one
 * we are expecting but it's acknowledgement number is not equal to
 * the acknowledgement number in the original message + 1, then it is
 * a new message.
 *
 */
int cn_netlink_send(struct cn_msg *msg, u32 __group, gfp_t gfp_mask)
{
	struct cn_callback_entry *__cbq;
	unsigned int size;
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	struct cn_msg *data;
	struct cn_dev *dev = &cdev;
	u32 group = 0;
	int found = 0;

	if (!__group) {
		spin_lock_bh(&dev->cbdev->queue_lock);
		list_for_each_entry(__cbq, &dev->cbdev->queue_list,
				    callback_entry) {
			if (cn_cb_equal(&__cbq->id.id, &msg->id)) {
				found = 1;
				group = __cbq->group;
			}
		}
		spin_unlock_bh(&dev->cbdev->queue_lock);

		if (!found)
			return -ENODEV;
	} else {
		group = __group;
	}

	size = NLMSG_SPACE(sizeof(*msg) + msg->len);

	skb = alloc_skb(size, gfp_mask);
	if (!skb)
		return -ENOMEM;

	nlh = NLMSG_PUT(skb, 0, msg->seq, NLMSG_DONE, size - sizeof(*nlh));

	data = NLMSG_DATA(nlh);

	memcpy(data, msg, sizeof(*data) + msg->len);

	NETLINK_GROUP(skb) = group;
	/* NETLINKメッセージ応答を送信 */
	netlink_broadcast(dev->nls, skb, 0, group, gfp_mask);

	return 0;

nlmsg_failure:
	kfree_skb(skb);
	return -EINVAL;
}

/*
 * Callback helper - queues work and setup destructor for given data.
 */
static int cn_call_callback(struct cn_msg *msg, void (*destruct_data)(void *), void *data)
{
	struct cn_callback_entry *__cbq;
	struct cn_dev *dev = &cdev;
	int err = -ENODEV;

	spin_lock_bh(&dev->cbdev->queue_lock);
	list_for_each_entry(__cbq, &dev->cbdev->queue_list, callback_entry) {
		if (cn_cb_equal(&__cbq->id.id, &msg->id)) {
			if (likely(!test_bit(0, &__cbq->work.pending) &&
					__cbq->data.ddata == NULL)) {
				__cbq->data.callback_priv = msg;

				__cbq->data.ddata = data;
				__cbq->data.destruct_data = destruct_data;

				if (queue_work(dev->cbdev->cn_queue,
						&__cbq->work))
					err = 0;
			} else {
				struct work_struct *w;
				struct cn_callback_data *d;
				
				w = kmalloc(sizeof(*w) + sizeof(*d), GFP_ATOMIC);
				if (w) {
					memset(w,0,sizeof(*w) + sizeof(*d));
					d = (struct cn_callback_data *)(w+1);

					d->callback_priv = msg;
					d->callback = __cbq->data.callback;
					d->ddata = data;
					d->destruct_data = destruct_data;
					d->free = w;

					INIT_LIST_HEAD(&w->entry);
					w->pending = 0;
					w->func = &cn_queue_wrapper;
					w->data = d;
					init_timer(&w->timer);
					
					if (queue_work(dev->cbdev->cn_queue, w))
						err = 0;
					else {
						kfree(w);
						err = -EINVAL;
					}
				} else
					err = -ENOMEM;
			}
			break;
		}
	}
	spin_unlock_bh(&dev->cbdev->queue_lock);

	return err;
}

/*
 * Skb receive helper - checks skb and msg size and calls callback
 * helper.
 */
static int __cn_rx_skb(struct sk_buff *skb, struct nlmsghdr *nlh)
{
	u32 pid, uid, seq, group;
	struct cn_msg *msg;

	pid = NETLINK_CREDS(skb)->pid;
	uid = NETLINK_CREDS(skb)->uid;
	seq = nlh->nlmsg_seq;
	group = NETLINK_GROUP(skb);
	msg = NLMSG_DATA(nlh);

	/* DRBD specific change: Only allow packets from ROOT */
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	return cn_call_callback(msg, (void (*)(void *))kfree_skb, skb);
}

/*
 * Main netlink receiving function.
 *
 * It checks skb and netlink header sizes and calls the skb receive
 * helper with a shared skb.
 */
static void cn_rx_skb(struct sk_buff *__skb)
{
	struct nlmsghdr *nlh;
	u32 len;
	int err;
	struct sk_buff *skb;

	skb = skb_get(__skb);

	if (skb->len >= NLMSG_SPACE(0)) {
		nlh = (struct nlmsghdr *)skb->data;

		if (nlh->nlmsg_len < sizeof(struct cn_msg) ||
		    skb->len < nlh->nlmsg_len ||
		    nlh->nlmsg_len > CONNECTOR_MAX_MSG_SIZE) {
			kfree_skb(skb);
			goto out;
		}

		len = NLMSG_ALIGN(nlh->nlmsg_len);
		if (len > skb->len)
			len = skb->len;

		err = __cn_rx_skb(skb, nlh);
		if (err < 0)
			kfree_skb(skb);
	}

out:
	kfree_skb(__skb);
}

/*
 * Netlink socket input callback - dequeues the skbs and calls the
 * main netlink receiving function.
 */
/* NETLINK受信コールバック */
static void cn_input(struct sock *sk, int len)
{
	struct sk_buff *skb;
	/* skb_dequeue. キューの 先頭からソケットバッファを抜く */
	while ((skb = skb_dequeue(&sk->sk_receive_queue)) != NULL)
		cn_rx_skb(skb);
}

/*
 * Notification routing.
 *
 * Gets id and checks if there are notification request for it's idx
 * and val.  If there are such requests notify the listeners with the
 * given notify event.
 *
 */
static void cn_notify(struct cb_id *id, u32 notify_event)
{
	struct cn_ctl_entry *ent;
	/* セマフォ待ち：確保できるまで待ち続ける。 */
	down(&notify_lock);
	list_for_each_entry(ent, &notify_list, notify_entry) {
		int i;
		struct cn_notify_req *req;
		struct cn_ctl_msg *ctl = ent->msg;
		int idx_found, val_found;

		idx_found = val_found = 0;

		req = (struct cn_notify_req *)ctl->data;
		for (i = 0; i < ctl->idx_notify_num; ++i, ++req) {
			if (id->idx >= req->first && 
					id->idx < req->first + req->range) {
				idx_found = 1;
				break;
			}
		}

		for (i = 0; i < ctl->val_notify_num; ++i, ++req) {
			if (id->val >= req->first && 
					id->val < req->first + req->range) {
				val_found = 1;
				break;
			}
		}

		if (idx_found && val_found) {
			struct cn_msg m = { .ack = notify_event, };

			memcpy(&m.id, id, sizeof(m.id));
			cn_netlink_send(&m, ctl->group, GFP_KERNEL);
		}
	}
	/* セマフォ解放 */
	up(&notify_lock);
}

/*
 * Callback add routing - adds callback with given ID and name.
 * If there is registered callback with the same ID it will not be added.
 *
 * May sleep.
 */
int cn_add_callback(struct cb_id *id, char *name, void (*callback)(void *))
{
	int err;
	struct cn_dev *dev = &cdev;

	err = cn_queue_add_callback(dev->cbdev, name, id, callback);
	if (err)
		return err;

	cn_notify(id, 0);

	return 0;
}

/*
 * Callback remove routing - removes callback
 * with given ID.
 * If there is no registered callback with given
 * ID nothing happens.
 *
 * May sleep while waiting for reference counter to become zero.
 */
void cn_del_callback(struct cb_id *id)
{
	struct cn_dev *dev = &cdev;

	cn_queue_del_callback(dev->cbdev, id);
	cn_notify(id, 1);
}

/*
 * Checks two connector's control messages to be the same.
 * Returns 1 if they are the same or if the first one is corrupted.
 */
static int cn_ctl_msg_equals(struct cn_ctl_msg *m1, struct cn_ctl_msg *m2)
{
	int i;
	struct cn_notify_req *req1, *req2;

	if (m1->idx_notify_num != m2->idx_notify_num)
		return 0;

	if (m1->val_notify_num != m2->val_notify_num)
		return 0;

	if (m1->len != m2->len)
		return 0;

	if ((m1->idx_notify_num + m1->val_notify_num) * sizeof(*req1) !=
	    m1->len)
		return 1;

	req1 = (struct cn_notify_req *)m1->data;
	req2 = (struct cn_notify_req *)m2->data;

	for (i = 0; i < m1->idx_notify_num; ++i) {
		if (req1->first != req2->first || req1->range != req2->range)
			return 0;
		req1++;
		req2++;
	}

	for (i = 0; i < m1->val_notify_num; ++i) {
		if (req1->first != req2->first || req1->range != req2->range)
			return 0;
		req1++;
		req2++;
	}

	return 1;
}

/*
 * Main connector device's callback.
 *
 * Used for notification of a request's processing.
 */
static void cn_callback(void *data)
{
	struct cn_msg *msg = data;
	struct cn_ctl_msg *ctl;
	struct cn_ctl_entry *ent;
	u32 size;

	if (msg->len < sizeof(*ctl))
		return;

	ctl = (struct cn_ctl_msg *)msg->data;

	size = (sizeof(*ctl) + ((ctl->idx_notify_num +
				 ctl->val_notify_num) *
				sizeof(struct cn_notify_req)));

	if (msg->len != size)
		return;

	if (ctl->len + sizeof(*ctl) != msg->len)
		return;

	/*
	 * Remove notification.
	 */
	if (ctl->group == 0) {
		struct cn_ctl_entry *n;
		/* セマフォ待ち：確保できるまで待ち続ける。 */
		down(&notify_lock);
		list_for_each_entry_safe(ent, n, &notify_list, notify_entry) {
			if (cn_ctl_msg_equals(ent->msg, ctl)) {
				list_del(&ent->notify_entry);
				kfree(ent);
			}
		}
		/* セマフォ解放 */
		up(&notify_lock);

		return;
	}

	size += sizeof(*ent);

	ent = kmalloc(size, GFP_KERNEL);
	if (!ent)
		return;

	memset(ent,0,size);
	ent->msg = (struct cn_ctl_msg *)(ent + 1);

	memcpy(ent->msg, ctl, size - sizeof(*ent));
	/* セマフォ待ち：確保できるまで待ち続ける。 */
	down(&notify_lock);
	list_add(&ent->notify_entry, &notify_list);
	/* 確保しているセマフォ構造体のセマフォ資源を1つ返す */
	up(&notify_lock);
}
/* Connector(NETLINK)初期化 */
int __init cn_init(void)
{
	struct cn_dev *dev = &cdev;
	int err;

	dev->input = cn_input;	/* NETLINK受信処理コールバック情報セット */
	dev->id.idx = cn_idx;
	dev->id.val = cn_val;

#ifdef DRBD_NL_DST_GROUPS
	/* history of upstream commits between kernel.org 2.6.13 and 2.6.14-rc1:
	 * 4fdb3bb723db469717c6d38fda667d8b0fa86ebd 2005-08-10 adds module parameter
	 * d629b836d151d43332492651dd841d32e57ebe3b 2005-08-15 renames dst_groups to dst_group
	 * 066286071d3542243baa68166acb779187c848b3 2005-08-15 adds groups parameter
	 * so it is not exactly correct to trigger on the rename dst_groups to dst_group,
	 * but sufficiently close.
	 */
	/* NETLINK受信コールバックセット */
	dev->nls = netlink_kernel_create(NETLINK_CONNECTOR,dev->input);
#else
	/* NETLINK受信コールバックセット */
	dev->nls = netlink_kernel_create(NETLINK_CONNECTOR,
					 CN_NETLINK_USERS + 0xf,
					 dev->input, THIS_MODULE);
#endif
	if (!dev->nls)
		return -EIO;

	dev->cbdev = cn_queue_alloc_dev("cqueue", dev->nls);
	if (!dev->cbdev) {
		if (dev->nls->sk_socket)
			sock_release(dev->nls->sk_socket);
		return -EINVAL;
	}

	err = cn_add_callback(&dev->id, "connector", &cn_callback);
	if (err) {
		cn_queue_free_dev(dev->cbdev);
		if (dev->nls->sk_socket)
			sock_release(dev->nls->sk_socket);
		return -EINVAL;
	}

	cn_already_initialized = 1;

	return 0;
}

void __exit cn_fini(void)
{
	struct cn_dev *dev = &cdev;

	cn_already_initialized = 0;

	cn_del_callback(&dev->id);
	cn_queue_free_dev(dev->cbdev);
	if (dev->nls->sk_socket)
		sock_release(dev->nls->sk_socket);
}
