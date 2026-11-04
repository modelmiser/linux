// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/base/devres.c - device resource management
 *
 * Copyright (c) 2006  SUSE Linux Products GmbH
 * Copyright (c) 2006  Tejun Heo <teheo@suse.de>
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/percpu.h>
#include <linux/dcache.h>
#include <linux/debugfs.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/sched.h>
#include <linux/sched/task.h>

#include <asm/sections.h>

#include "base.h"
#include "trace.h"

struct devres {
	struct devres_node		node;
	dr_release_t			release;
	/*
	 * Some archs want to perform DMA into kmalloc caches
	 * and need a guaranteed alignment larger than
	 * the alignment of a 64-bit integer.
	 * Thus we use ARCH_DMA_MINALIGN for data[] which will force the same
	 * alignment for struct devres when allocated by kmalloc().
	 */
	u8 __aligned(ARCH_DMA_MINALIGN) data[];
};

struct devres_group {
	struct devres_node		node[2];
	void				*id;
	int				color;
	/* -- 8 pointers */
};

void devres_node_init(struct devres_node *node,
		      dr_node_release_t release,
		      dr_node_free_t free_node)
{
	INIT_LIST_HEAD(&node->entry);
	node->release = release;
	node->free_node = free_node;
}

static inline void free_node(struct devres_node *node)
{
	node->free_node(node);
}

void devres_set_node_dbginfo(struct devres_node *node, const char *name,
			     size_t size)
{
	node->name = name;
	node->size = size;
}

#ifdef CONFIG_DEBUG_DEVRES
static int log_devres = 0;
module_param_named(log, log_devres, int, S_IRUGO | S_IWUSR);

static void devres_dbg(struct device *dev, struct devres_node *node,
		       const char *op)
{
	if (unlikely(log_devres))
		dev_err(dev, "DEVRES %3s %p %s (%zu bytes)\n",
			op, node, node->name, node->size);
}
#else /* CONFIG_DEBUG_DEVRES */
#define devres_dbg(dev, node, op)	do {} while (0)
#endif /* CONFIG_DEBUG_DEVRES */

static void devres_log(struct device *dev, struct devres_node *node,
		       const char *op)
{
	trace_devres_log(dev, op, node, node->name, node->size);
	devres_dbg(dev, node, op);
}

/*
 * Release functions for devres group.  These callbacks are used only
 * for identification.
 */
static void group_open_release(struct device *dev, struct devres_node *node)
{
	/* noop */
}

static void group_close_release(struct device *dev, struct devres_node *node)
{
	/* noop */
}

static struct devres_group *node_to_group(struct devres_node *node)
{
	if (node->release == &group_open_release)
		return container_of(node, struct devres_group, node[0]);
	if (node->release == &group_close_release)
		return container_of(node, struct devres_group, node[1]);
	return NULL;
}

static bool check_dr_size(size_t size, size_t *tot_size)
{
	/* We must catch any near-SIZE_MAX cases that could overflow. */
	if (unlikely(check_add_overflow(sizeof(struct devres),
					size, tot_size)))
		return false;

	/* Actually allocate the full kmalloc bucket size. */
	*tot_size = kmalloc_size_roundup(*tot_size);

	return true;
}

static void dr_node_release(struct device *dev, struct devres_node *node)
{
	struct devres *dr = container_of(node, struct devres, node);

	dr->release(dev, dr->data);
}

static void dr_node_free(struct devres_node *node)
{
	struct devres *dr = container_of(node, struct devres, node);

	kfree(dr);
}

static __always_inline struct devres *alloc_dr(dr_release_t release,
					       size_t size, gfp_t gfp, int nid)
{
	size_t tot_size;
	struct devres *dr;

	if (!check_dr_size(size, &tot_size))
		return NULL;

	dr = kmalloc_node_track_caller(tot_size, gfp, nid);
	if (unlikely(!dr))
		return NULL;

	/* No need to clear memory twice */
	if (!(gfp & __GFP_ZERO))
		memset(dr, 0, offsetof(struct devres, data));

	devres_node_init(&dr->node, dr_node_release, dr_node_free);
	dr->release = release;
	return dr;
}

static void add_dr(struct device *dev, struct devres_node *node)
{
	devres_log(dev, node, "ADD");
	BUG_ON(!list_empty(&node->entry));
	list_add_tail(&node->entry, &dev->devres_head);
}

static void replace_dr(struct device *dev,
		       struct devres_node *old, struct devres_node *new)
{
	devres_log(dev, old, "REPLACE");
	BUG_ON(!list_empty(&new->entry));
	list_replace(&old->entry, &new->entry);
}

/**
 * __devres_alloc_node - Allocate device resource data
 * @release: Release function devres will be associated with
 * @size: Allocation size
 * @gfp: Allocation flags
 * @nid: NUMA node
 * @name: Name of the resource
 *
 * Allocate devres of @size bytes.  The allocated area is zeroed, then
 * associated with @release.  The returned pointer can be passed to
 * other devres_*() functions.
 *
 * RETURNS:
 * Pointer to allocated devres on success, NULL on failure.
 */
void *__devres_alloc_node(dr_release_t release, size_t size, gfp_t gfp, int nid,
			  const char *name)
{
	struct devres *dr;

	dr = alloc_dr(release, size, gfp | __GFP_ZERO, nid);
	if (unlikely(!dr))
		return NULL;
	devres_set_node_dbginfo(&dr->node, name, size);
	return dr->data;
}
EXPORT_SYMBOL_GPL(__devres_alloc_node);

/**
 * devres_for_each_res - Resource iterator
 * @dev: Device to iterate resource from
 * @release: Look for resources associated with this release function
 * @match: Match function (optional)
 * @match_data: Data for the match function
 * @fn: Function to be called for each matched resource.
 * @data: Data for @fn, the 3rd parameter of @fn
 *
 * Call @fn for each devres of @dev which is associated with @release
 * and for which @match returns 1.
 *
 * RETURNS:
 * 	void
 */
void devres_for_each_res(struct device *dev, dr_release_t release,
			dr_match_t match, void *match_data,
			void (*fn)(struct device *, void *, void *),
			void *data)
{
	struct devres_node *node;
	struct devres_node *tmp;

	if (!fn)
		return;

	guard(spinlock_irqsave)(&dev->devres_lock);
	list_for_each_entry_safe_reverse(node, tmp,
			&dev->devres_head, entry) {
		struct devres *dr = container_of(node, struct devres, node);

		if (node->release != dr_node_release)
			continue;
		if (dr->release != release)
			continue;
		if (match && !match(dev, dr->data, match_data))
			continue;
		fn(dev, dr->data, data);
	}
}
EXPORT_SYMBOL_GPL(devres_for_each_res);

static inline void free_dr(struct devres *dr)
{
	free_node(&dr->node);
}

/**
 * devres_free - Free device resource data
 * @res: Pointer to devres data to free
 *
 * Free devres created with devres_alloc().
 */
void devres_free(void *res)
{
	if (res) {
		struct devres *dr = container_of(res, struct devres, data);

		BUG_ON(!list_empty(&dr->node.entry));
		free_dr(dr);
	}
}
EXPORT_SYMBOL_GPL(devres_free);

void devres_node_add(struct device *dev, struct devres_node *node)
{
	guard(spinlock_irqsave)(&dev->devres_lock);

	add_dr(dev, node);
}

/**
 * devres_add - Register device resource
 * @dev: Device to add resource to
 * @res: Resource to register
 *
 * Register devres @res to @dev.  @res should have been allocated
 * using devres_alloc().  On driver detach, the associated release
 * function will be invoked and devres will be freed automatically.
 */
void devres_add(struct device *dev, void *res)
{
	struct devres *dr = container_of(res, struct devres, data);

	devres_node_add(dev, &dr->node);
}
EXPORT_SYMBOL_GPL(devres_add);

static struct devres *find_dr(struct device *dev, dr_release_t release,
			      dr_match_t match, void *match_data)
{
	struct devres_node *node;

	list_for_each_entry_reverse(node, &dev->devres_head, entry) {
		struct devres *dr = container_of(node, struct devres, node);

		if (node->release != dr_node_release)
			continue;
		if (dr->release != release)
			continue;
		if (match && !match(dev, dr->data, match_data))
			continue;
		return dr;
	}

	return NULL;
}

/**
 * devres_find - Find device resource
 * @dev: Device to lookup resource from
 * @release: Look for resources associated with this release function
 * @match: Match function (optional)
 * @match_data: Data for the match function
 *
 * Find the latest devres of @dev which is associated with @release
 * and for which @match returns 1.  If @match is NULL, it's considered
 * to match all.
 *
 * RETURNS:
 * Pointer to found devres, NULL if not found.
 */
void *devres_find(struct device *dev, dr_release_t release,
		  dr_match_t match, void *match_data)
{
	struct devres *dr;

	guard(spinlock_irqsave)(&dev->devres_lock);
	dr = find_dr(dev, release, match, match_data);
	if (dr)
		return dr->data;

	return NULL;
}
EXPORT_SYMBOL_GPL(devres_find);

/**
 * devres_get - Find devres, if non-existent, add one atomically
 * @dev: Device to lookup or add devres for
 * @new_res: Pointer to new initialized devres to add if not found
 * @match: Match function (optional)
 * @match_data: Data for the match function
 *
 * Find the latest devres of @dev which has the same release function
 * as @new_res and for which @match return 1.  If found, @new_res is
 * freed; otherwise, @new_res is added atomically.
 *
 * RETURNS:
 * Pointer to found or added devres.
 */
void *devres_get(struct device *dev, void *new_res,
		 dr_match_t match, void *match_data)
{
	struct devres *new_dr = container_of(new_res, struct devres, data);
	struct devres *dr;
	unsigned long flags;

	spin_lock_irqsave(&dev->devres_lock, flags);
	dr = find_dr(dev, new_dr->release, match, match_data);
	if (!dr) {
		add_dr(dev, &new_dr->node);
		dr = new_dr;
		new_res = NULL;
	}
	spin_unlock_irqrestore(&dev->devres_lock, flags);
	devres_free(new_res);

	return dr->data;
}
EXPORT_SYMBOL_GPL(devres_get);

bool devres_node_remove(struct device *dev, struct devres_node *node)
{
	struct devres_node *__node;

	guard(spinlock_irqsave)(&dev->devres_lock);
	list_for_each_entry_reverse(__node, &dev->devres_head, entry) {
		if (__node == node) {
			list_del_init(&node->entry);
			devres_log(dev, node, "REM");
			return true;
		}
	}

	return false;
}

/**
 * devres_remove - Find a device resource and remove it
 * @dev: Device to find resource from
 * @release: Look for resources associated with this release function
 * @match: Match function (optional)
 * @match_data: Data for the match function
 *
 * Find the latest devres of @dev associated with @release and for
 * which @match returns 1.  If @match is NULL, it's considered to
 * match all.  If found, the resource is removed atomically and
 * returned.
 *
 * RETURNS:
 * Pointer to removed devres on success, NULL if not found.
 */
void *devres_remove(struct device *dev, dr_release_t release,
		    dr_match_t match, void *match_data)
{
	struct devres *dr;

	guard(spinlock_irqsave)(&dev->devres_lock);
	dr = find_dr(dev, release, match, match_data);
	if (dr) {
		list_del_init(&dr->node.entry);
		devres_log(dev, &dr->node, "REM");
		return dr->data;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(devres_remove);

/**
 * devres_destroy - Find a device resource and destroy it
 * @dev: Device to find resource from
 * @release: Look for resources associated with this release function
 * @match: Match function (optional)
 * @match_data: Data for the match function
 *
 * Find the latest devres of @dev associated with @release and for
 * which @match returns 1.  If @match is NULL, it's considered to
 * match all.  If found, the resource is removed atomically and freed.
 *
 * Note that the release function for the resource will not be called,
 * only the devres-allocated data will be freed.  The caller becomes
 * responsible for freeing any other data.
 *
 * RETURNS:
 * 0 if devres is found and freed, -ENOENT if not found.
 */
int devres_destroy(struct device *dev, dr_release_t release,
		   dr_match_t match, void *match_data)
{
	void *res;

	res = devres_remove(dev, release, match, match_data);
	if (unlikely(!res))
		return -ENOENT;

	devres_free(res);
	return 0;
}
EXPORT_SYMBOL_GPL(devres_destroy);


/**
 * devres_release - Find a device resource and destroy it, calling release
 * @dev: Device to find resource from
 * @release: Look for resources associated with this release function
 * @match: Match function (optional)
 * @match_data: Data for the match function
 *
 * Find the latest devres of @dev associated with @release and for
 * which @match returns 1.  If @match is NULL, it's considered to
 * match all.  If found, the resource is removed atomically, the
 * release function called and the resource freed.
 *
 * RETURNS:
 * 0 if devres is found and freed, -ENOENT if not found.
 */
int devres_release(struct device *dev, dr_release_t release,
		   dr_match_t match, void *match_data)
{
	void *res;

	res = devres_remove(dev, release, match, match_data);
	if (unlikely(!res))
		return -ENOENT;

	(*release)(dev, res);
	devres_free(res);
	return 0;
}
EXPORT_SYMBOL_GPL(devres_release);

static int remove_nodes(struct device *dev,
			struct list_head *first, struct list_head *end,
			struct list_head *todo)
{
	struct devres_node *node, *n;
	int cnt = 0, nr_groups = 0;

	/* First pass - move normal devres entries to @todo and clear
	 * devres_group colors.
	 */
	node = list_entry(first, struct devres_node, entry);
	list_for_each_entry_safe_from(node, n, end, entry) {
		struct devres_group *grp;

		grp = node_to_group(node);
		if (grp) {
			/* clear color of group markers in the first pass */
			grp->color = 0;
			nr_groups++;
		} else {
			/* regular devres entry */
			if (&node->entry == first)
				first = first->next;
			list_move_tail(&node->entry, todo);
			cnt++;
		}
	}

	if (!nr_groups)
		return cnt;

	/* Second pass - Scan groups and color them.  A group gets
	 * color value of two iff the group is wholly contained in
	 * [current node, end). That is, for a closed group, both opening
	 * and closing markers should be in the range, while just the
	 * opening marker is enough for an open group.
	 */
	node = list_entry(first, struct devres_node, entry);
	list_for_each_entry_safe_from(node, n, end, entry) {
		struct devres_group *grp;

		grp = node_to_group(node);
		BUG_ON(!grp || list_empty(&grp->node[0].entry));

		grp->color++;
		if (list_empty(&grp->node[1].entry))
			grp->color++;

		BUG_ON(grp->color <= 0 || grp->color > 2);
		if (grp->color == 2) {
			/* No need to update current node or end. The removed
			 * nodes are always before both.
			 */
			list_move_tail(&grp->node[0].entry, todo);
			list_del_init(&grp->node[1].entry);
		}
	}

	return cnt;
}

#ifdef CONFIG_DEBUG_DEVRES_REFS
/*
 * CONFIG_DEBUG_DEVRES_REFS: before a driver's managed memory is freed, find
 * open files that still point into it.
 *
 * Two phases around devres_release_all(), both from device_unbind_cleanup()
 * only, i.e. on driver unbind and after a failed probe, in process context
 * with device_lock held. It is deliberately not run from
 * devres_release_all() itself, which is also reached from device_release()
 * and so from any context in which a final put_device() is legal.
 *
 * Phase 1 (devres_check_open_files) snapshots the regions and walks every
 * fd table. A hit on an ordinary file is a WARN at once. A hit on a debugfs
 * file is decided by whether debugfs has revoked the file: if already
 * removed and guarded by debugfs, info now; otherwise the verdict waits,
 * because many drivers remove their debugfs files through a devres action
 * that runs inside devres_release_all(). Phase 2
 * (devres_finish_open_file_check) revisits those after release: removed
 * and guarded by then is info; removed but with unguarded file_operations,
 * or still live, is a WARN.
 */
static LIST_HEAD(devres_open_file_hooks);
/* Lock order: device_lock(dev) -> devres_open_file_hooks_lock -> task_lock(). */
static DEFINE_MUTEX(devres_open_file_hooks_lock);

/**
 * devres_register_open_file_hook - tell the open-file check how to reach a
 *                                  subsystem's object from an open file
 * @hook: static or zero-initialised hook with @fops, @name and @holder set
 *
 * Return: 0, -EINVAL if a field is missing, -EEXIST if @fops already has a
 * hook.
 */
int devres_register_open_file_hook(struct devres_open_file_hook *hook)
{
	struct devres_open_file_hook *h;
	int ret = 0;

	if (!hook->fops || !hook->holder || !hook->name)
		return -EINVAL;

	mutex_lock(&devres_open_file_hooks_lock);
	list_for_each_entry(h, &devres_open_file_hooks, list) {
		if (h->fops == hook->fops) {
			ret = -EEXIST;
			goto out;
		}
	}
	list_add_tail(&hook->list, &devres_open_file_hooks);
out:
	mutex_unlock(&devres_open_file_hooks_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(devres_register_open_file_hook);

/**
 * devres_unregister_open_file_hook - remove a hook; a no-op if it was never
 *                                    registered (static or zero-initialised)
 * @hook: the hook passed to devres_register_open_file_hook()
 */
void devres_unregister_open_file_hook(struct devres_open_file_hook *hook)
{
	/* Never registered (static zero) or already unregistered: nothing to do. */
	if (!hook->list.next || list_empty(&hook->list))
		return;

	mutex_lock(&devres_open_file_hooks_lock);
	list_del_init(&hook->list);
	mutex_unlock(&devres_open_file_hooks_lock);
}
EXPORT_SYMBOL_GPL(devres_unregister_open_file_hook);

struct devres_region {
	const void *start;
	size_t size;
	const char *name;
	bool warned;
};

/* Beyond this many distinct fd tables, later sharers are walked again. */
#define DEVRES_SEEN_TABLES_MAX	1048576U

/* A hit on a live debugfs file, decided after devres_release_all(). */
struct devres_pending_ref {
	struct list_head list;
	struct file *file;
	struct devres_region *region;
	const char *what;
	char comm[TASK_COMM_LEN];
	pid_t pnr;
	unsigned int fd;
};

/*
 * Copy the data regions of @dev's managed allocations. Taken before
 * devres_release_all() so the walk never holds devres_lock and never looks
 * at a node that is being freed.
 */
static unsigned int devres_snapshot_regions(struct device *dev,
					    struct devres_region **out)
{
	struct devres_region *regions;
	struct devres_node *node;
	unsigned int n = 0, i = 0;
	unsigned long flags;

	spin_lock_irqsave(&dev->devres_lock, flags);
	list_for_each_entry(node, &dev->devres_head, entry)
		if (node->release == dr_node_release && node->size)
			n++;
	spin_unlock_irqrestore(&dev->devres_lock, flags);

	if (!n)
		return 0;

	regions = kcalloc(n, sizeof(*regions), GFP_KERNEL);
	if (!regions)
		return 0;

	/*
	 * The list may have changed between the two passes (a driver still
	 * calling devm_* after remove() returned, which is its own bug).
	 * Growth is bounded by @n (extra nodes are not checked); shrinkage
	 * leaves @i below @n, and only [0, @i) is returned.
	 */
	spin_lock_irqsave(&dev->devres_lock, flags);
	list_for_each_entry(node, &dev->devres_head, entry) {
		struct devres *dr;

		if (node->release != dr_node_release || !node->size)
			continue;
		if (i == n)
			break;
		dr = container_of(node, struct devres, node);
		regions[i].start = dr->data;
		regions[i].size = node->size;
		regions[i].name = node->name;
		i++;
	}
	spin_unlock_irqrestore(&dev->devres_lock, flags);

	if (!i) {
		kfree(regions);
		return 0;
	}
	*out = regions;
	return i;
}

static struct devres_region *devres_region_containing(struct devres_region *r,
						      unsigned int nr,
						      const void *p)
{
	unsigned int i;

	if (!p)
		return NULL;
	for (i = 0; i < nr; i++)
		if (p >= r[i].start && p < r[i].start + r[i].size)
			return &r[i];
	return NULL;
}

#define DEVRES_REF_FMT "DEVRES %s (%zu bytes) still referenced by %s of fd %u (%pD3) in %s[%d]"

/* One backtrace per allocation; further holders get one line each. */
static void devres_warn_ref(struct device *dev, struct devres_region *region,
			    const char *what, unsigned int fd,
			    struct file *file, const char *comm, pid_t pnr,
			    const char *why)
{
	if (!region->warned) {
		region->warned = true;
		dev_WARN(dev, DEVRES_REF_FMT " at unbind%s\n", region->name,
			 region->size, what, fd, file, comm, pnr, why);
	} else {
		dev_warn(dev, DEVRES_REF_FMT " at unbind%s\n", region->name,
			 region->size, what, fd, file, comm, pnr, why);
	}
}

static void devres_report_revoked(struct device *dev,
				  struct devres_region *region,
				  const char *what, unsigned int fd,
				  struct file *file, const char *comm,
				  pid_t pnr, const struct file_operations *fops,
				  const char *when)
{
	if (fops && fops->release)
		dev_info(dev, DEVRES_REF_FMT " at unbind: debugfs file removed %s, ops refused by debugfs, close will still call %ps\n",
			 region->name, region->size, what, fd, file, comm,
			 pnr, when, fops->release);
	else
		dev_info(dev, DEVRES_REF_FMT " at unbind: debugfs file removed %s, ops refused by debugfs, no release callback\n",
			 region->name, region->size, what, fd, file, comm,
			 pnr, when);
}

static void devres_report_open_file(struct device *dev,
				    struct devres_open_file_check *chk,
				    struct devres_region *region,
				    struct task_struct *task, pid_t pnr,
				    unsigned int fd, struct file *file,
				    const char *what)
{
	const struct file_operations *real_fops;
	struct devres_pending_ref *pend;
	char comm[TASK_COMM_LEN];

	get_task_comm(comm, task);

	/* Already removed and guarded by debugfs: info now. */
	if (debugfs_file_revoked(file, &real_fops)) {
		devres_report_revoked(dev, region, what, fd, file, comm, pnr,
				      real_fops, "in remove()");
		return;
	}

	/* Live debugfs file: a devres action may still remove it. Defer. */
	if (file_inode(file)->i_sb->s_magic == DEBUGFS_MAGIC) {
		pend = kzalloc_obj(*pend);
		if (pend) {
			pend->file = get_file(file);
			pend->region = region;
			pend->what = what;
			strscpy(pend->comm, comm, sizeof(pend->comm));
			pend->pnr = pnr;
			pend->fd = fd;
			list_add_tail(&pend->list, &chk->pending);
			return;
		}
		/* No memory to defer: undecided, so a line without a backtrace. */
		dev_warn(dev, DEVRES_REF_FMT " at unbind: live debugfs file, verdict not deferred (no memory)\n",
			 region->name, region->size, what, fd, file, comm, pnr);
		return;
	}

	devres_warn_ref(dev, region, what, fd, file, comm, pnr, "");
}

static void devres_check_one_file(struct device *dev,
				  struct devres_open_file_check *chk,
				  struct task_struct *task, pid_t pnr,
				  unsigned int fd, struct file *file)
{
	struct devres_open_file_hook *hook;
	struct devres_region *region;
	const void *pd, *ip;

	/* O_PATH files have no operations and no release: nothing can use them. */
	if (file->f_mode & FMODE_PATH)
		return;

	pd = READ_ONCE(file->private_data);
	region = devres_region_containing(chk->regions, chk->nr, pd);
	if (region)
		devres_report_open_file(dev, chk, region, task, pnr, fd, file,
					"private_data");

	ip = READ_ONCE(file_inode(file)->i_private);
	if (ip != pd) {
		region = devres_region_containing(chk->regions, chk->nr, ip);
		if (region)
			devres_report_open_file(dev, chk, region, task, pnr,
						fd, file, "i_private");
	}

	/*
	 * A character device file holds a reference on its cdev's kobject
	 * (chrdev_open()), and inode->i_cdev is cleared only when that
	 * kobject is released, so for an open non-O_PATH file the pointer
	 * is stable here even after cdev_del(). A struct cdev embedded in
	 * managed memory therefore shows up directly. Compared, never
	 * dereferenced. i_cdev shares a union; only valid for S_ISCHR.
	 */
	if (S_ISCHR(file_inode(file)->i_mode)) {
		const void *cdev = READ_ONCE(file_inode(file)->i_cdev);

		region = devres_region_containing(chk->regions, chk->nr, cdev);
		if (region)
			devres_report_open_file(dev, chk, region, task, pnr,
						fd, file, "i_cdev");
	}

	list_for_each_entry(hook, &devres_open_file_hooks, list) {
		if (file->f_op != hook->fops)
			continue;
		region = devres_region_containing(chk->regions, chk->nr,
						  hook->holder(file));
		if (region)
			devres_report_open_file(dev, chk, region, task, pnr,
						fd, file, hook->name);
	}
}

/*
 * Each fd table is walked once. A table already walked is remembered as
 * (owning task, table) with a reference on the task only: a table counts as
 * seen while that task still points at it, which proves it was not freed
 * and reused by another task (the recorded task could only reach the same
 * address again through two table switches, and what it opens after the
 * walk visited it is not promised anyway). Nothing takes a reference on the
 * table itself, so the walk never ends up closing another task's files. If
 * the set cannot grow, later sharers are walked again, costing duplicate
 * lines. The set is scanned linearly: O(threads x tables) per walk.
 */
static bool devres_fd_table_seen(struct devres_open_file_check *chk,
				 struct files_struct *files)
{
	unsigned int i;

	for (i = 0; i < chk->nseen; i++)
		if (chk->seen[i].files == files &&
		    READ_ONCE(chk->seen[i].task->files) == files)
			return true;
	return false;
}

static void devres_fd_table_mark(struct devres_open_file_check *chk,
				 struct task_struct *task)
{
	struct devres_seen_table *seen;
	struct files_struct *files;

	if (chk->nseen == chk->maxseen) {
		unsigned int max = chk->maxseen ? chk->maxseen * 2 : 64;

		if (max > DEVRES_SEEN_TABLES_MAX)
			return;
		seen = kvrealloc(chk->seen, array_size(max, sizeof(*seen)),
				 GFP_KERNEL | __GFP_NOWARN);
		if (!seen)
			return;
		chk->seen = seen;
		chk->maxseen = max;
	}

	files = READ_ONCE(task->files);
	if (files && !devres_fd_table_seen(chk, files)) {
		chk->seen[chk->nseen].task = get_task_struct(task);
		chk->seen[chk->nseen].files = files;
		chk->nseen++;
	}
}

/**
 * devres_check_open_files - find open files into memory about to be freed
 * @dev: device being unbound; its managed resources are still allocated
 * @chk: zero-initialised state, passed on to devres_finish_open_file_check()
 *
 * Walk every thread's open files and report any that still point into one
 * of @dev's managed allocations, which devres_release_all() is about to
 * free. Must be called in process context with no spinlocks held; the walk
 * can sleep, reschedules between tasks, and holds only one task reference
 * and one file reference at a time, plus task references for the fd tables
 * already walked (dropped at the end of the walk) and references on
 * deferred debugfs files (dropped in devres_finish_open_file_check()).
 *
 * A deferred debugfs file keeps a file reference across
 * devres_release_all(); debugfs_remove() does not wait for file references,
 * only for operations in flight, so no in-tree remover blocks on it. A
 * devres action that waited for all opens of its file to drain would.
 *
 * Holding a file reference means that if the owner closes the file
 * meanwhile, the final fput() (and so the driver's release callback) runs
 * from this context's deferred fput (task work, or the delayed-fput work
 * item when called from a kthread such as the deferred-probe worker)
 * instead of the owner's close(). The file was reported either way; only
 * where its release runs moves.
 */
void devres_check_open_files(struct device *dev,
			     struct devres_open_file_check *chk)
{
	unsigned int tid = 0;

	might_sleep();

	INIT_LIST_HEAD(&chk->pending);
	chk->nr = devres_snapshot_regions(dev, &chk->regions);
	if (!chk->nr)
		return;

	mutex_lock(&devres_open_file_hooks_lock);
	for (;;) {
		struct task_struct *task;
		struct file *file;
		unsigned int fd = 0;
		struct pid *pid;
		pid_t pnr;

		rcu_read_lock();
		pid = find_ge_pid(tid, &init_pid_ns);
		if (!pid) {
			rcu_read_unlock();
			break;
		}
		pnr = pid_nr(pid);
		tid = pnr + 1;
		task = pid_task(pid, PIDTYPE_PID);
		if (task && READ_ONCE(task->files) &&
		    !devres_fd_table_seen(chk, READ_ONCE(task->files)))
			get_task_struct(task);
		else
			task = NULL;
		rcu_read_unlock();
		if (!task)
			continue;

		devres_fd_table_mark(chk, task);
		while ((file = fget_task_next(task, &fd))) {
			devres_check_one_file(dev, chk, task, pnr, fd, file);
			fput(file);
			fd++;
		}
		put_task_struct(task);
		cond_resched();
	}
	mutex_unlock(&devres_open_file_hooks_lock);

	while (chk->nseen)
		put_task_struct(chk->seen[--chk->nseen].task);
	kvfree(chk->seen);
	chk->seen = NULL;
	chk->maxseen = 0;
}

/**
 * devres_finish_open_file_check - decide the deferred debugfs hits
 * @dev: the device, after devres_release_all()
 * @chk: state from devres_check_open_files()
 *
 * A debugfs file that was live during the walk may have been removed by a
 * devres action since; if debugfs guards it, the hit is informational. One
 * removed but with unguarded file_operations, or one still live, points at
 * freed memory.
 */
void devres_finish_open_file_check(struct device *dev,
				   struct devres_open_file_check *chk)
{
	struct devres_pending_ref *pend, *tmp;
	const struct file_operations *real_fops;

	if (!chk->nr)
		return;

	list_for_each_entry_safe(pend, tmp, &chk->pending, list) {
		if (debugfs_file_revoked(pend->file, &real_fops))
			devres_report_revoked(dev, pend->region, pend->what,
					      pend->fd, pend->file, pend->comm,
					      pend->pnr, real_fops,
					      "by a devres action");
		else if (d_unlinked(pend->file->f_path.dentry))
			devres_warn_ref(dev, pend->region, pend->what, pend->fd,
					pend->file, pend->comm, pend->pnr,
					": debugfs file removed, but its file_operations are not guarded by debugfs");
		else
			devres_warn_ref(dev, pend->region, pend->what, pend->fd,
					pend->file, pend->comm, pend->pnr,
					": debugfs file still registered after its data was freed");
		list_del(&pend->list);
		fput(pend->file);
		kfree(pend);
	}

	kfree(chk->regions);
	chk->regions = NULL;
	chk->nr = 0;
}
#endif /* CONFIG_DEBUG_DEVRES_REFS */

static void release_nodes(struct device *dev, struct list_head *todo)
{
	struct devres_node *node, *tmp;

	list_for_each_entry_safe_reverse(node, tmp, todo, entry) {
		devres_log(dev, node, "REL");
		node->release(dev, node);
		free_node(node);
	}
}

/**
 * devres_release_all - Release all managed resources
 * @dev: Device to release resources for
 *
 * Release all resources associated with @dev.  This function is
 * called on driver detach.
 */
int devres_release_all(struct device *dev)
{
	unsigned long flags;
	LIST_HEAD(todo);
	int cnt;

	/* Looks like an uninitialized device structure */
	if (WARN_ON(dev->devres_head.next == NULL))
		return -ENODEV;

	/* Nothing to release if list is empty */
	if (list_empty(&dev->devres_head))
		return 0;

	spin_lock_irqsave(&dev->devres_lock, flags);
	cnt = remove_nodes(dev, dev->devres_head.next, &dev->devres_head, &todo);
	spin_unlock_irqrestore(&dev->devres_lock, flags);

	release_nodes(dev, &todo);
	return cnt;
}

static void devres_group_free(struct devres_node *node)
{
	struct devres_group *grp = container_of(node, struct devres_group, node[0]);

	kfree(grp);
}

/**
 * devres_open_group - Open a new devres group
 * @dev: Device to open devres group for
 * @id: Separator ID
 * @gfp: Allocation flags
 *
 * Open a new devres group for @dev with @id.  For @id, using a
 * pointer to an object which won't be used for another group is
 * recommended.  If @id is NULL, address-wise unique ID is created.
 *
 * RETURNS:
 * ID of the new group, NULL on failure.
 */
void *devres_open_group(struct device *dev, void *id, gfp_t gfp)
{
	struct devres_group *grp;

	grp = kmalloc_obj(*grp, gfp);
	if (unlikely(!grp))
		return NULL;

	devres_node_init(&grp->node[0], &group_open_release, devres_group_free);
	devres_node_init(&grp->node[1], &group_close_release, NULL);
	devres_set_node_dbginfo(&grp->node[0], "grp<", 0);
	devres_set_node_dbginfo(&grp->node[1], "grp>", 0);
	grp->id = grp;
	if (id)
		grp->id = id;
	grp->color = 0;

	devres_node_add(dev, &grp->node[0]);
	return grp->id;
}
EXPORT_SYMBOL_GPL(devres_open_group);

/*
 * Find devres group with ID @id.  If @id is NULL, look for the latest open
 * group.
 */
static struct devres_group *find_group(struct device *dev, void *id)
{
	struct devres_node *node;

	list_for_each_entry_reverse(node, &dev->devres_head, entry) {
		struct devres_group *grp;

		if (node->release != &group_open_release)
			continue;

		grp = container_of(node, struct devres_group, node[0]);

		if (id) {
			if (grp->id == id)
				return grp;
		} else if (list_empty(&grp->node[1].entry))
			return grp;
	}

	return NULL;
}

/**
 * devres_close_group - Close a devres group
 * @dev: Device to close devres group for
 * @id: ID of target group, can be NULL
 *
 * Close the group identified by @id.  If @id is NULL, the latest open
 * group is selected.
 */
void devres_close_group(struct device *dev, void *id)
{
	struct devres_group *grp;

	guard(spinlock_irqsave)(&dev->devres_lock);
	grp = find_group(dev, id);
	if (grp)
		add_dr(dev, &grp->node[1]);
	else
		WARN_ON(1);
}
EXPORT_SYMBOL_GPL(devres_close_group);

/**
 * devres_remove_group - Remove a devres group
 * @dev: Device to remove group for
 * @id: ID of target group, can be NULL
 *
 * Remove the group identified by @id.  If @id is NULL, the latest
 * open group is selected.  Note that removing a group doesn't affect
 * any other resources.
 */
void devres_remove_group(struct device *dev, void *id)
{
	struct devres_group *grp;
	unsigned long flags;

	spin_lock_irqsave(&dev->devres_lock, flags);

	grp = find_group(dev, id);
	if (grp) {
		list_del_init(&grp->node[0].entry);
		list_del_init(&grp->node[1].entry);
		devres_log(dev, &grp->node[0], "REM");
	} else
		WARN_ON(1);

	spin_unlock_irqrestore(&dev->devres_lock, flags);

	kfree(grp);
}
EXPORT_SYMBOL_GPL(devres_remove_group);

/**
 * devres_release_group - Release resources in a devres group
 * @dev: Device to release group for
 * @id: ID of target group, can be NULL
 *
 * Release all resources in the group identified by @id.  If @id is
 * NULL, the latest open group is selected.  The selected group and
 * groups properly nested inside the selected group are removed.
 *
 * RETURNS:
 * The number of released non-group resources.
 */
int devres_release_group(struct device *dev, void *id)
{
	struct devres_group *grp;
	unsigned long flags;
	LIST_HEAD(todo);
	int cnt = 0;

	spin_lock_irqsave(&dev->devres_lock, flags);
	grp = find_group(dev, id);
	if (grp) {
		struct list_head *first = &grp->node[0].entry;
		struct list_head *end = &dev->devres_head;

		if (!list_empty(&grp->node[1].entry))
			end = grp->node[1].entry.next;

		cnt = remove_nodes(dev, first, end, &todo);
	} else if (list_empty(&dev->devres_head)) {
		/*
		 * dev is probably dying via devres_release_all(): groups
		 * have already been removed and are on the process of
		 * being released - don't touch and don't warn.
		 */
	} else {
		WARN_ON(1);
	}
	spin_unlock_irqrestore(&dev->devres_lock, flags);

	release_nodes(dev, &todo);

	return cnt;
}
EXPORT_SYMBOL_GPL(devres_release_group);

/*
 * Custom devres actions allow inserting a simple function call
 * into the teardown sequence.
 */

struct action_devres {
	void *data;
	void (*action)(void *);
};

struct devres_action {
	struct devres_node node;
	struct action_devres action;
};

static int devm_action_match(struct devres_action *devres, struct action_devres *target)
{
	return devres->action.action == target->action &&
	       devres->action.data == target->data;
}

static void devm_action_release(struct device *dev, struct devres_node *node)
{
	struct devres_action *devres = container_of(node, struct devres_action, node);

	devres->action.action(devres->action.data);
}

static void devm_action_free(struct devres_node *node)
{
	struct devres_action *action = container_of(node, struct devres_action, node);

	kfree(action);
}

/**
 * __devm_add_action() - add a custom action to list of managed resources
 * @dev: Device that owns the action
 * @action: Function that should be called
 * @data: Pointer to data passed to @action implementation
 * @name: Name of the resource (for debugging purposes)
 *
 * This adds a custom action to the list of managed resources so that
 * it gets executed as part of standard resource unwinding.
 */
int __devm_add_action(struct device *dev, void (*action)(void *), void *data, const char *name)
{
	struct devres_action *devres;

	devres = kzalloc_obj(*devres);
	if (!devres)
		return -ENOMEM;

	devres_node_init(&devres->node, devm_action_release, devm_action_free);
	devres_set_node_dbginfo(&devres->node, name, sizeof(*devres));

	devres->action.data = data;
	devres->action.action = action;

	devres_node_add(dev, &devres->node);
	return 0;
}
EXPORT_SYMBOL_GPL(__devm_add_action);

static struct devres_action *devres_action_find(struct device *dev,
						void (*action)(void *),
						void *data)
{
	struct devres_node *node;
	struct action_devres target = {
		.data = data,
		.action = action,
	};

	list_for_each_entry_reverse(node, &dev->devres_head, entry) {
		struct devres_action *dr = container_of(node, struct devres_action, node);

		if (node->release != devm_action_release)
			continue;
		if (devm_action_match(dr, &target))
			return dr;
	}

	return NULL;
}

bool devm_is_action_added(struct device *dev, void (*action)(void *), void *data)
{
	guard(spinlock_irqsave)(&dev->devres_lock);

	return !!devres_action_find(dev, action, data);
}
EXPORT_SYMBOL_GPL(devm_is_action_added);

static struct devres_action *remove_action(struct device *dev,
					   void (*action)(void *),
					   void *data)
{
	struct devres_action *dr;

	guard(spinlock_irqsave)(&dev->devres_lock);

	dr = devres_action_find(dev, action, data);
	if (!dr)
		return ERR_PTR(-ENOENT);

	list_del_init(&dr->node.entry);
	devres_log(dev, &dr->node, "REM");

	return dr;
}

/**
 * devm_remove_action_nowarn() - removes previously added custom action
 * @dev: Device that owns the action
 * @action: Function implementing the action
 * @data: Pointer to data passed to @action implementation
 *
 * Removes instance of @action previously added by devm_add_action().
 * Both action and data should match one of the existing entries.
 *
 * In contrast to devm_remove_action(), this function does not WARN() if no
 * entry could have been found.
 *
 * This should only be used if the action is contained in an object with
 * independent lifetime management, e.g. the Devres rust abstraction.
 *
 * Causing the warning from regular driver code most likely indicates an abuse
 * of the devres API.
 *
 * Returns: 0 on success, -ENOENT if no entry could have been found.
 */
int devm_remove_action_nowarn(struct device *dev,
			      void (*action)(void *),
			      void *data)
{
	struct devres_action *dr;

	dr = remove_action(dev, action, data);
	if (IS_ERR(dr))
		return PTR_ERR(dr);

	kfree(dr);

	return 0;
}
EXPORT_SYMBOL_GPL(devm_remove_action_nowarn);

/**
 * devm_release_action() - release previously added custom action
 * @dev: Device that owns the action
 * @action: Function implementing the action
 * @data: Pointer to data passed to @action implementation
 *
 * Releases and removes instance of @action previously added by
 * devm_add_action().  Both action and data should match one of the
 * existing entries.
 */
void devm_release_action(struct device *dev, void (*action)(void *), void *data)
{
	struct devres_action *dr;

	dr = remove_action(dev, action, data);
	if (WARN_ON(IS_ERR(dr)))
		return;

	dr->action.action(dr->action.data);

	kfree(dr);
}
EXPORT_SYMBOL_GPL(devm_release_action);

/*
 * Managed kmalloc/kfree
 */
static void devm_kmalloc_release(struct device *dev, void *res)
{
	/* noop */
}

static int devm_kmalloc_match(struct device *dev, void *res, void *data)
{
	return res == data;
}

/**
 * devm_kmalloc - Resource-managed kmalloc
 * @dev: Device to allocate memory for
 * @size: Allocation size
 * @gfp: Allocation gfp flags
 *
 * Managed kmalloc.  Memory allocated with this function is
 * automatically freed on driver detach.  Like all other devres
 * resources, guaranteed alignment is unsigned long long.
 *
 * RETURNS:
 * Pointer to allocated memory on success, NULL on failure.
 */
void *devm_kmalloc(struct device *dev, size_t size, gfp_t gfp)
{
	struct devres *dr;

	if (unlikely(!size))
		return ZERO_SIZE_PTR;

	/* use raw alloc_dr for kmalloc caller tracing */
	dr = alloc_dr(devm_kmalloc_release, size, gfp, dev_to_node(dev));
	if (unlikely(!dr))
		return NULL;

	/*
	 * This is named devm_kzalloc_release for historical reasons
	 * The initial implementation did not support kmalloc, only kzalloc
	 */
	devres_set_node_dbginfo(&dr->node, "devm_kzalloc_release", size);
	devres_add(dev, dr->data);
	return dr->data;
}
EXPORT_SYMBOL_GPL(devm_kmalloc);

/**
 * devm_krealloc - Resource-managed krealloc()
 * @dev: Device to re-allocate memory for
 * @ptr: Pointer to the memory chunk to re-allocate
 * @new_size: New allocation size
 * @gfp: Allocation gfp flags
 *
 * Managed krealloc(). Resizes the memory chunk allocated with devm_kmalloc().
 * Behaves similarly to regular krealloc(): if @ptr is NULL or ZERO_SIZE_PTR,
 * it's the equivalent of devm_kmalloc(). If new_size is zero, it frees the
 * previously allocated memory and returns ZERO_SIZE_PTR. This function doesn't
 * change the order in which the release callback for the re-alloc'ed devres
 * will be called (except when falling back to devm_kmalloc() or when freeing
 * resources when new_size is zero). The contents of the memory are preserved
 * up to the lesser of new and old sizes.
 */
void *devm_krealloc(struct device *dev, void *ptr, size_t new_size, gfp_t gfp)
{
	size_t total_new_size, total_old_size;
	struct devres *old_dr, *new_dr;
	unsigned long flags;

	if (unlikely(!new_size)) {
		devm_kfree(dev, ptr);
		return ZERO_SIZE_PTR;
	}

	if (unlikely(ZERO_OR_NULL_PTR(ptr)))
		return devm_kmalloc(dev, new_size, gfp);

	if (WARN_ON(is_kernel_rodata((unsigned long)ptr)))
		/*
		 * We cannot reliably realloc a const string returned by
		 * devm_kstrdup_const().
		 */
		return NULL;

	if (!check_dr_size(new_size, &total_new_size))
		return NULL;

	total_old_size = ksize(container_of(ptr, struct devres, data));
	if (total_old_size == 0) {
		WARN(1, "Pointer doesn't point to dynamically allocated memory.");
		return NULL;
	}

	/*
	 * If new size is smaller or equal to the actual number of bytes
	 * allocated previously - just return the same pointer.
	 */
	if (total_new_size <= total_old_size)
		return ptr;

	/*
	 * Otherwise: allocate new, larger chunk. We need to allocate before
	 * taking the lock as most probably the caller uses GFP_KERNEL.
	 * alloc_dr() will call check_dr_size() to reserve extra memory
	 * for struct devres automatically, so size @new_size user request
	 * is delivered to it directly as devm_kmalloc() does.
	 */
	new_dr = alloc_dr(devm_kmalloc_release,
			  new_size, gfp, dev_to_node(dev));
	if (!new_dr)
		return NULL;

	devres_set_node_dbginfo(&new_dr->node, "devm_krealloc_release", new_size);

	/*
	 * The spinlock protects the linked list against concurrent
	 * modifications but not the resource itself.
	 */
	spin_lock_irqsave(&dev->devres_lock, flags);

	old_dr = find_dr(dev, devm_kmalloc_release, devm_kmalloc_match, ptr);
	if (!old_dr) {
		spin_unlock_irqrestore(&dev->devres_lock, flags);
		free_dr(new_dr);
		WARN(1, "Memory chunk not managed or managed by a different device.");
		return NULL;
	}

	replace_dr(dev, &old_dr->node, &new_dr->node);

	spin_unlock_irqrestore(&dev->devres_lock, flags);

	/*
	 * We can copy the memory contents after releasing the lock as we're
	 * no longer modifying the list links.
	 */
	memcpy(new_dr->data, old_dr->data,
	       total_old_size - offsetof(struct devres, data));
	/*
	 * Same for releasing the old devres - it's now been removed from the
	 * list. This is also the reason why we must not use devm_kfree() - the
	 * links are no longer valid.
	 */
	free_dr(old_dr);

	return new_dr->data;
}
EXPORT_SYMBOL_GPL(devm_krealloc);

/**
 * devm_kstrdup - Allocate resource managed space and
 *                copy an existing string into that.
 * @dev: Device to allocate memory for
 * @s: the string to duplicate
 * @gfp: the GFP mask used in the devm_kmalloc() call when
 *       allocating memory
 * RETURNS:
 * Pointer to allocated string on success, NULL on failure.
 */
char *devm_kstrdup(struct device *dev, const char *s, gfp_t gfp)
{
	if (!s)
		return NULL;

	return devm_kmemdup(dev, s, strlen(s) + 1, gfp);
}
EXPORT_SYMBOL_GPL(devm_kstrdup);

/**
 * devm_kstrdup_const - resource managed conditional string duplication
 * @dev: device for which to duplicate the string
 * @s: the string to duplicate
 * @gfp: the GFP mask used in the kmalloc() call when allocating memory
 *
 * Strings allocated by devm_kstrdup_const will be automatically freed when
 * the associated device is detached.
 *
 * RETURNS:
 * Source string if it is in .rodata section otherwise it falls back to
 * devm_kstrdup.
 */
const char *devm_kstrdup_const(struct device *dev, const char *s, gfp_t gfp)
{
	if (is_kernel_rodata((unsigned long)s))
		return s;

	return devm_kstrdup(dev, s, gfp);
}
EXPORT_SYMBOL_GPL(devm_kstrdup_const);

/**
 * devm_kvasprintf - Allocate resource managed space and format a string
 *		     into that.
 * @dev: Device to allocate memory for
 * @gfp: the GFP mask used in the devm_kmalloc() call when
 *       allocating memory
 * @fmt: The printf()-style format string
 * @ap: Arguments for the format string
 * RETURNS:
 * Pointer to allocated string on success, NULL on failure.
 */
char *devm_kvasprintf(struct device *dev, gfp_t gfp, const char *fmt,
		      va_list ap)
{
	unsigned int len;
	char *p;
	va_list aq;

	va_copy(aq, ap);
	len = vsnprintf(NULL, 0, fmt, aq);
	va_end(aq);

	p = devm_kmalloc(dev, len+1, gfp);
	if (!p)
		return NULL;

	vsnprintf(p, len+1, fmt, ap);

	return p;
}
EXPORT_SYMBOL(devm_kvasprintf);

/**
 * devm_kasprintf - Allocate resource managed space and format a string
 *		    into that.
 * @dev: Device to allocate memory for
 * @gfp: the GFP mask used in the devm_kmalloc() call when
 *       allocating memory
 * @fmt: The printf()-style format string
 * @...: Arguments for the format string
 * RETURNS:
 * Pointer to allocated string on success, NULL on failure.
 */
char *devm_kasprintf(struct device *dev, gfp_t gfp, const char *fmt, ...)
{
	va_list ap;
	char *p;

	va_start(ap, fmt);
	p = devm_kvasprintf(dev, gfp, fmt, ap);
	va_end(ap);

	return p;
}
EXPORT_SYMBOL_GPL(devm_kasprintf);

/**
 * devm_kfree - Resource-managed kfree
 * @dev: Device this memory belongs to
 * @p: Memory to free
 *
 * Free memory allocated with devm_kmalloc().
 */
void devm_kfree(struct device *dev, const void *p)
{
	int rc;

	/*
	 * Special cases: pointer to a string in .rodata returned by
	 * devm_kstrdup_const() or NULL/ZERO ptr.
	 */
	if (unlikely(is_kernel_rodata((unsigned long)p) || ZERO_OR_NULL_PTR(p)))
		return;

	rc = devres_destroy(dev, devm_kmalloc_release,
			    devm_kmalloc_match, (void *)p);
	WARN_ON(rc);
}
EXPORT_SYMBOL_GPL(devm_kfree);

/**
 * devm_kmemdup - Resource-managed kmemdup
 * @dev: Device this memory belongs to
 * @src: Memory region to duplicate
 * @len: Memory region length
 * @gfp: GFP mask to use
 *
 * Duplicate region of a memory using resource managed kmalloc
 */
void *devm_kmemdup(struct device *dev, const void *src, size_t len, gfp_t gfp)
{
	void *p;

	p = devm_kmalloc(dev, len, gfp);
	if (p)
		memcpy(p, src, len);

	return p;
}
EXPORT_SYMBOL_GPL(devm_kmemdup);

/**
 * devm_kmemdup_const - conditionally duplicate and manage a region of memory
 *
 * @dev: Device this memory belongs to
 * @src: memory region to duplicate
 * @len: memory region length,
 * @gfp: GFP mask to use
 *
 * Return: source address if it is in .rodata or the return value of kmemdup()
 * to which the function falls back otherwise.
 */
const void *
devm_kmemdup_const(struct device *dev, const void *src, size_t len, gfp_t gfp)
{
	if (is_kernel_rodata((unsigned long)src))
		return src;

	return devm_kmemdup(dev, src, len, gfp);
}
EXPORT_SYMBOL_GPL(devm_kmemdup_const);

struct pages_devres {
	unsigned long addr;
	unsigned int order;
};

static int devm_pages_match(struct device *dev, void *res, void *p)
{
	struct pages_devres *devres = res;
	struct pages_devres *target = p;

	return devres->addr == target->addr;
}

static void devm_pages_release(struct device *dev, void *res)
{
	struct pages_devres *devres = res;

	free_pages(devres->addr, devres->order);
}

/**
 * devm_get_free_pages - Resource-managed __get_free_pages
 * @dev: Device to allocate memory for
 * @gfp_mask: Allocation gfp flags
 * @order: Allocation size is (1 << order) pages
 *
 * Managed get_free_pages.  Memory allocated with this function is
 * automatically freed on driver detach.
 *
 * RETURNS:
 * Address of allocated memory on success, 0 on failure.
 */

unsigned long devm_get_free_pages(struct device *dev,
				  gfp_t gfp_mask, unsigned int order)
{
	struct pages_devres *devres;
	unsigned long addr;

	addr = __get_free_pages(gfp_mask, order);

	if (unlikely(!addr))
		return 0;

	devres = devres_alloc(devm_pages_release,
			      sizeof(struct pages_devres), GFP_KERNEL);
	if (unlikely(!devres)) {
		free_pages(addr, order);
		return 0;
	}

	devres->addr = addr;
	devres->order = order;

	devres_add(dev, devres);
	return addr;
}
EXPORT_SYMBOL_GPL(devm_get_free_pages);

/**
 * devm_free_pages - Resource-managed free_pages
 * @dev: Device this memory belongs to
 * @addr: Memory to free
 *
 * Free memory allocated with devm_get_free_pages(). Unlike free_pages,
 * there is no need to supply the @order.
 */
void devm_free_pages(struct device *dev, unsigned long addr)
{
	struct pages_devres devres = { .addr = addr };

	WARN_ON(devres_release(dev, devm_pages_release, devm_pages_match,
			       &devres));
}
EXPORT_SYMBOL_GPL(devm_free_pages);

static void devm_percpu_release(struct device *dev, void *pdata)
{
	void __percpu *p;

	p = *(void __percpu **)pdata;
	free_percpu(p);
}

/**
 * __devm_alloc_percpu - Resource-managed alloc_percpu
 * @dev: Device to allocate per-cpu memory for
 * @size: Size of per-cpu memory to allocate
 * @align: Alignment of per-cpu memory to allocate
 *
 * Managed alloc_percpu. Per-cpu memory allocated with this function is
 * automatically freed on driver detach.
 *
 * RETURNS:
 * Pointer to allocated memory on success, NULL on failure.
 */
void __percpu *__devm_alloc_percpu(struct device *dev, size_t size,
		size_t align)
{
	void *p;
	void __percpu *pcpu;

	pcpu = __alloc_percpu(size, align);
	if (!pcpu)
		return NULL;

	p = devres_alloc(devm_percpu_release, sizeof(void *), GFP_KERNEL);
	if (!p) {
		free_percpu(pcpu);
		return NULL;
	}

	*(void __percpu **)p = pcpu;

	devres_add(dev, p);

	return pcpu;
}
EXPORT_SYMBOL_GPL(__devm_alloc_percpu);
