/*
 * 	cn_queue.c
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
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/skbuff.h>
#include <linux/suspend.h>
#include <linux/connector.h>
#include <linux/delay.h>

#include <linux/drbd_config.h> /* In case kzalloc() is missing. */

#ifdef NEED_BACKPORT_OF_KZALLOC
static inline void *kzalloc(size_t size, int flags)
{
	void *rv = kmalloc(size, flags);
	if (rv)
		memset(rv, 0, size);

	return rv;
}
#endif


#ifndef KERNEL_HAS_MSLEEP
/**
 * msleep - sleep safely even with waitqueue interruptions
 * @msecs: Time in milliseconds to sleep for
 */
static inline void msleep(unsigned int msecs)
{
	unsigned long timeout = (msecs * HZ + 999) / 1000;

	while (timeout) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		timeout = schedule_timeout(timeout);
	}
}

#endif

void cn_queue_wrapper(void *data)
{
	struct cn_callback_data *d = data;

	d->callback(d->callback_priv);

	d->destruct_data(d->ddata);
	d->ddata = NULL;

	kfree(d->free);
}

static struct cn_callback_entry *cn_queue_alloc_callback_entry(char *name, struct cb_id *id, void (*callback)(void *))
{
	struct cn_callback_entry *cbq;

	cbq = kzalloc(sizeof(*cbq), GFP_KERNEL);
	if (!cbq) {
		printk(KERN_ERR "Failed to create new callback queue.\n");
		return NULL;
	}

	snprintf(cbq->id.name, sizeof(cbq->id.name), "%s", name);
	memcpy(&cbq->id.id, id, sizeof(struct cb_id));
	cbq->data.callback = callback;
	
	INIT_WORK(&cbq->work, &cn_queue_wrapper, &cbq->data);
	return cbq;
}

static void cn_queue_free_callback(struct cn_callback_entry *cbq)
{
	cancel_delayed_work(&cbq->work);
	flush_workqueue(cbq->pdev->cn_queue);

	kfree(cbq);
}

int cn_cb_equal(struct cb_id *i1, struct cb_id *i2)
{
	return ((i1->idx == i2->idx) && (i1->val == i2->val));
}

int cn_queue_add_callback(struct cn_queue_dev *dev, char *name, struct cb_id *id, void (*callback)(void *))
{
	struct cn_callback_entry *cbq, *__cbq;
	int found = 0;

	cbq = cn_queue_alloc_callback_entry(name, id, callback);
	if (!cbq)
		return -ENOMEM;

	atomic_inc(&dev->refcnt);
	cbq->pdev = dev;

	spin_lock_bh(&dev->queue_lock);
	list_for_each_entry(__cbq, &dev->queue_list, callback_entry) {
		if (cn_cb_equal(&__cbq->id.id, id)) {
			found = 1;
			break;
		}
	}
	if (!found)
		list_add_tail(&cbq->callback_entry, &dev->queue_list); 	/* dev->queue_listのリストにcbq->callback_entryのリストを追加する */
	spin_unlock_bh(&dev->queue_lock);

	if (found) {
		atomic_dec(&dev->refcnt);
		cn_queue_free_callback(cbq);
		return -EINVAL;
	}

	cbq->nls = dev->nls;
	cbq->seq = 0;
	cbq->group = cbq->id.id.idx;

	return 0;
}

void cn_queue_del_callback(struct cn_queue_dev *dev, struct cb_id *id)
{
	struct cn_callback_entry *cbq, *n;
	int found = 0;

	spin_lock_bh(&dev->queue_lock);
	list_for_each_entry_safe(cbq, n, &dev->queue_list, callback_entry) {
		if (cn_cb_equal(&cbq->id.id, id)) {
			list_del(&cbq->callback_entry);
			found = 1;
			break;
		}
	}
	spin_unlock_bh(&dev->queue_lock);

	if (found) {
		cn_queue_free_callback(cbq);
		/* refcntの参照カウントをチェック：０ならTRUE、その他はFALSE */
		atomic_dec_and_test(&dev->refcnt);
	}
}

struct cn_queue_dev *cn_queue_alloc_dev(char *name, struct sock *nls)
{
	struct cn_queue_dev *dev;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return NULL;

	snprintf(dev->name, sizeof(dev->name), "%s", name);
	atomic_set(&dev->refcnt, 0);
	INIT_LIST_HEAD(&dev->queue_list);
	spin_lock_init(&dev->queue_lock);

	dev->nls = nls;
	dev->netlink_groups = 0;

	dev->cn_queue = create_workqueue(dev->name);
	if (!dev->cn_queue) {
		kfree(dev);
		return NULL;
	}

	return dev;
}

void cn_queue_free_dev(struct cn_queue_dev *dev)
{
	struct cn_callback_entry *cbq, *n;

	flush_workqueue(dev->cn_queue);
	destroy_workqueue(dev->cn_queue);

	spin_lock_bh(&dev->queue_lock);
	list_for_each_entry_safe(cbq, n, &dev->queue_list, callback_entry)
		list_del(&cbq->callback_entry);
	spin_unlock_bh(&dev->queue_lock);

	while (atomic_read(&dev->refcnt)) {
		printk(KERN_INFO "Waiting for %s to become free: refcnt=%d.\n",
		       dev->name, atomic_read(&dev->refcnt));
		msleep(1000);
	}

	kfree(dev);
	dev = NULL;
}
