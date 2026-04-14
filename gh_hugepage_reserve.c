// SPDX-License-Identifier: GPL-2.0
/*
 * gh_hugepage_reserve
 *
 * Pre-allocates 2MB compound pages at boot and serves them to Gunyah VM
 * memory allocations via kretprobe on __alloc_pages, ensuring VMs get
 * contiguous THP-backed memory even when the system is fragmented.
 *
 * Architecture:
 *
 *   1. Pool allocation (insmod):
 *      Pre-allocate order-9 (2MB) compound pages into a static pool.
 *
 *   2. Always-on kretprobe (__alloc_pages):
 *      Registered at init, intercepts order-9 allocations from tracked
 *      VM owner processes. Replaces the return value with a pool page
 *      and frees the original buddy page (if any) back to avoid double
 *      memory consumption. entry_handler filters by order, pool count,
 *      and mm - overhead is negligible for non-matching calls.
 *
 *   3. VM lifecycle tracking (kprobes):
 *      - Creation:    kprobe on gunyah_dev_vm_mgr_ioctl (GH_CREATE_VM)
 *                     records the caller's mm_struct for filtering.
 *      - Destruction: kprobe on gunyah_vm_release / gh_vm_free
 *                     schedules delayed pool refill after VM exits.
 *
 *   4. Pool refill (delayed work):
 *      After VM shutdown + configurable delay, refills pool from buddy
 *      allocator with compaction/reclaim. Retries on fragmentation.
 *
 * Prerequisites:
 *   echo advise > /sys/kernel/mm/transparent_hugepage/shmem_enabled
 *
 * Usage:
 *   insmod gh_hugepage_reserve.ko pool_target=1024 refill_delay_ms=5000
 */

#define pr_fmt(fmt) "gh_hugepage_reserve: " fmt
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/ioctl.h>
#include <linux/delay.h>
#include <linux/compaction.h>
#include <linux/sched/mm.h>

#define POOL_SIZE_MAX		4096
#define PAGE_ORDER		9	/* 2^9 * 4KB = 2MB */
#define REFILL_DELAY_MS_DEFAULT	5000
#define REFILL_RETRY_MAX	3
#define REFILL_RETRY_INTERVAL_MS 3000

/* GH_CREATE_VM = _IO('G', 0x0) - same across */
#define GH_IOCTL_TYPE	'G'
#define GH_CREATE_VM	_IO(GH_IOCTL_TYPE, 0x0)

/* ---- Module parameters ---- */

static int pool_target = 1024;
module_param(pool_target, int, 0444);
MODULE_PARM_DESC(pool_target, "Number of 2MB pages to pre-allocate (default 1024)");

static int refill_delay_ms = REFILL_DELAY_MS_DEFAULT;
module_param(refill_delay_ms, int, 0644);
MODULE_PARM_DESC(refill_delay_ms,
	"Delay in ms before refilling pool after VM shutdown (default 5000)");

/* ---- Pool ---- */

static struct page *page_pool[POOL_SIZE_MAX];
static atomic_t pool_count = ATOMIC_INIT(0);
static int pool_total;
static DEFINE_RAW_SPINLOCK(pool_lock);

/* Per-CPU guard: prevent free_pages_kp from reclaiming pages freed by ret_handler */
static DEFINE_PER_CPU(int, in_ret_handler);

/* ---- Probe state ---- */

static bool hook_active;
static bool detect_active;
static bool vm_destroy_active;
static bool free_intercept_active;
static DEFINE_MUTEX(hook_mutex);

/* ---- Statistics and VM tracking ---- */

enum refill_state { REFILL_IDLE = 0, REFILL_WAITING, REFILL_RUNNING };

static atomic_t refill_status = ATOMIC_INIT(REFILL_IDLE);
static atomic_t vm_active_count = ATOMIC_INIT(0);
static atomic_t total_served = ATOMIC_INIT(0);
static atomic_t total_refilled = ATOMIC_INIT(0);

/* ---- VM owner mm tracking ---- */

/*
 * Track mm_structs of processes that create Gunyah VMs.
 * The kprobe on the VM creation ioctl guarantees only actual Gunyah
 * users are tracked - no process name filtering needed.
 *
 * The kretprobe on __alloc_pages checks current->mm against this set
 * to avoid serving pool pages to unrelated order-9 allocations.
 */
#define VM_OWNER_MAX	8
static struct mm_struct *vm_owner_mms[VM_OWNER_MAX];
static atomic_t vm_owner_count = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(vm_owner_lock);

static void vm_owner_add(struct mm_struct *mm)
{
	unsigned long flags;
	int i, count;

	spin_lock_irqsave(&vm_owner_lock, flags);
	count = atomic_read(&vm_owner_count);

	for (i = 0; i < count; i++) {
		if (vm_owner_mms[i] == mm)
			goto out;
	}
	if (count < VM_OWNER_MAX) {
		mmgrab(mm);
		vm_owner_mms[count] = mm;
		smp_store_release(&vm_owner_count.counter, count + 1);
		pr_info("tracking VM owner mm=%px (comm=%s)\n",
			mm, current->comm);
	} else {
		pr_warn("too many VM owners (%d), ignoring %s\n",
			VM_OWNER_MAX, current->comm);
	}
out:
	spin_unlock_irqrestore(&vm_owner_lock, flags);
}

static bool vm_owner_contains(struct mm_struct *mm)
{
	int i, count;

	count = smp_load_acquire(&vm_owner_count.counter);
	for (i = 0; i < count; i++) {
		if (READ_ONCE(vm_owner_mms[i]) == mm)
			return true;
	}
	return false;
}

/* ---- Work and probe structs ---- */

static struct work_struct vm_owner_release_work;
static struct delayed_work refill_work;
static struct kretprobe kretp;
static struct kprobe vm_detect_kp;
static struct kprobe vm_destroy_kp;
static struct kprobe free_pages_kp;

/* ================================================================== */
/*  Pool push/pop (protected by pool_lock)                            */
/* ================================================================== */

static struct page *pool_pop(void)
{
	struct page *page = NULL;
	unsigned long flags;
	int idx;

	raw_spin_lock_irqsave(&pool_lock, flags);
	idx = atomic_read(&pool_count);
	if (idx > 0) {
		page = page_pool[--idx];
		page_pool[idx] = NULL;
		atomic_set(&pool_count, idx);
	}
	raw_spin_unlock_irqrestore(&pool_lock, flags);
	return page;
}

static bool pool_push(struct page *page)
{
	unsigned long flags;
	int idx;
	bool ok;

	raw_spin_lock_irqsave(&pool_lock, flags);
	idx = atomic_read(&pool_count);
	ok = (idx < pool_total);
	if (ok) {
		page_pool[idx] = page;
		atomic_set(&pool_count, idx + 1);
	}
	raw_spin_unlock_irqrestore(&pool_lock, flags);
	return ok;
}

/* ================================================================== */
/*  kprobe on __free_pages - page reclamation                         */
/* ================================================================== */

static int free_pages_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	struct page *page;
	unsigned int order;

	order = (unsigned int)regs_get_kernel_argument(regs, 1);
	if (order != PAGE_ORDER)
		return 0;
	if (atomic_read(&pool_count) >= pool_total)
		return 0;
	/* Avoid reclaiming pages freed by our ret_handler */
	if (this_cpu_read(in_ret_handler))
		return 0;

	page = (struct page *)regs_get_kernel_argument(regs, 0);
	if (!page || !PageCompound(page))
		return 0;

	/*
	 * Bump refcount: __free_pages calls put_page_testzero which
	 * decrements 2->1, returns false, page is NOT freed to buddy.
	 * Page stays with refcount 1 in our pool.
	 */
	get_page(page);

	if (pool_push(page)) {
		atomic_inc(&total_refilled);
		pr_info_ratelimited("reclaimed page from free, pool %d/%d\n",
				    atomic_read(&pool_count), pool_total);
	} else {
		/* Pool raced to full - undo, let __free_pages free it */
		put_page(page);
	}

	return 0;
}

/* ================================================================== */
/*  kretprobe on __alloc_pages - page replacement                     */
/* ================================================================== */

static int entry_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	if ((unsigned int)regs_get_kernel_argument(regs, 1) != PAGE_ORDER)
		return 1;
	if (atomic_read(&pool_count) <= 0)
		return 1;
	/* Only intercept allocations from tracked Gunyah VM owners */
	if (!current->mm || !vm_owner_contains(current->mm))
		return 1;
	return 0;
}

static int ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct page *orig, *pool_page;

	pool_page = pool_pop();
	if (!pool_page)
		return 0;

	orig = (struct page *)regs_return_value(regs);
	regs_set_return_value(regs, (unsigned long)pool_page);
	atomic_inc(&total_served);
	pr_info_ratelimited("served page, %d left\n",
			    atomic_read(&pool_count));

	if (orig) {
		/* Guard: prevent free_pages_kp from reclaiming this page */
		this_cpu_write(in_ret_handler, 1);
		__free_pages(orig, PAGE_ORDER);
		this_cpu_write(in_ret_handler, 0);
	}
	return 0;
}

/* ================================================================== */
/*  VM creation detection                                             */
/* ================================================================== */

static int vm_detect_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	unsigned int cmd = (unsigned int)regs_get_kernel_argument(regs, 1);

	if (cmd != GH_CREATE_VM)
		return 0;

	atomic_inc(&vm_active_count);

	if (current->mm)
		vm_owner_add(current->mm);

	pr_info("VM creation detected (active=%d, comm=%s)\n",
		atomic_read(&vm_active_count), current->comm);
	return 0;
}

/* ================================================================== */
/*  VM destruction detection                                          */
/* ================================================================== */

static int vm_destroy_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	int active;

	/*
	 * Filter: only handle tracked VM owners.
	 */
	if (current->mm && !vm_owner_contains(current->mm))
		return 0;
	if (!current->mm && atomic_read(&vm_owner_count) == 0)
		return 0;

	active = atomic_dec_return(&vm_active_count);
	if (active < 0)
		atomic_set(&vm_active_count, 0);

	pr_info("VM destruction detected (active=%d, pool=%d/%d, comm=%s)\n",
		max(active, 0), atomic_read(&pool_count), pool_total,
		current->comm);

	/* Schedule refill immediately - worker polls mm_users before allocating */
	if (atomic_read(&pool_count) < pool_total &&
	    atomic_cmpxchg(&refill_status, REFILL_IDLE, REFILL_WAITING) == REFILL_IDLE) {
		schedule_delayed_work(&refill_work, 0);
		pr_info("refill scheduled\n");
	} else {
		/* Pool full or refill already running - release mms now */
		schedule_work(&vm_owner_release_work);
	}

	return 0;
}

/* ================================================================== */
/*  Helpers                                                           */
/* ================================================================== */

static int register_kretp_with_fallback(void)
{
	static const char * const sym_names[] = {
		"__alloc_pages_noprof",
		"__alloc_pages",
		"__alloc_pages_nodemask",
		NULL,
	};
	int i, ret = -ENOENT;

	for (i = 0; sym_names[i]; i++) {
		kretp.kp.symbol_name = sym_names[i];
		ret = register_kretprobe(&kretp);
		if (ret == 0) {
			pr_info("hooked %s\n",
				sym_names[i]);
			return 0;
		}
	}
	pr_err("no suitable __alloc_pages symbol\n");
	return ret;
}

/* Release all tracked VM owner mm_structs (deferred - mmdrop may sleep) */
static void vm_owner_release_worker(struct work_struct *work)
{
	unsigned long flags;
	struct mm_struct *to_drop[VM_OWNER_MAX];
	int i, count;

	spin_lock_irqsave(&vm_owner_lock, flags);
	count = atomic_read(&vm_owner_count);
	for (i = 0; i < count; i++) {
		to_drop[i] = vm_owner_mms[i];
		vm_owner_mms[i] = NULL;
	}
	atomic_set(&vm_owner_count, 0);
	spin_unlock_irqrestore(&vm_owner_lock, flags);

	for (i = 0; i < count; i++) {
		pr_info("releasing VM owner mm=%px\n",
			to_drop[i]);
		mmdrop(to_drop[i]);
	}
}

/*
 * Refill pool from buddy allocator after VM shutdown.
 * The free_pages kprobe reclaims most pages directly from the free path;
 * this worker handles any remaining shortfall via alloc_pages + compaction.
 * Runs in kworker context (mm=NULL), safe from kretprobe interception.
 */
static void refill_worker(struct work_struct *work)
{
	int current_count, target, allocated, retry;
	int total_allocated = 0;

	atomic_set(&refill_status, REFILL_RUNNING);

	/*
	 * Phase 1: Wait for VM owner process to exit (mm_users == 0).
	 * Pages return to buddy only after exit_mmap() completes.
	 * We hold mm_count via mmgrab(), so mm_struct stays valid.
	 */
	{
		int wait_ms;

		for (wait_ms = 0; wait_ms < refill_delay_ms; wait_ms += 50) {
			bool exited = false;
			int i, count;

			if (atomic_read(&pool_count) >= pool_total)
				break;

			count = smp_load_acquire(&vm_owner_count.counter);
			if (count == 0) {
				exited = true;
			} else {
				for (i = 0; i < count; i++) {
					struct mm_struct *mm = READ_ONCE(vm_owner_mms[i]);

					if (mm && atomic_read(&mm->mm_users) == 0) {
						exited = true;
						break;
					}
				}
			}
			if (exited)
				break;
			msleep(50);
		}
		pr_info("process exit wait: %d ms, pool %d/%d\n",
			wait_ms, atomic_read(&pool_count), pool_total);
	}

	/* Phase 2: Brief pause for free-intercept to catch initial burst */
	if (atomic_read(&pool_count) < pool_total)
		msleep(200);

	for (retry = 0; retry <= REFILL_RETRY_MAX; retry++) {
		if (retry > 0) {
			pr_info("refill retry %d/%d, waiting %d ms\n",
				retry, REFILL_RETRY_MAX, REFILL_RETRY_INTERVAL_MS);
			msleep(REFILL_RETRY_INTERVAL_MS);
		}

		current_count = atomic_read(&pool_count);
		target = pool_total - current_count;

		if (target <= 0) {
			pr_info("pool already full (%d/%d)\n",
				current_count, pool_total);
			break;
		}

		pr_info("refill attempt %d: need %d pages (have %d/%d)\n",
			retry + 1, target, current_count, pool_total);

		allocated = 0;
		while (atomic_read(&pool_count) < pool_total) {
			struct page *p;

			p = alloc_pages(GFP_KERNEL | __GFP_COMP |
					__GFP_NOWARN | __GFP_RETRY_MAYFAIL,
					PAGE_ORDER);
			if (!p)
				break;

			if (!pool_push(p)) {
				__free_pages(p, PAGE_ORDER);
				break;
			}
			allocated++;
			atomic_inc(&total_refilled);

			if (allocated % 50 == 0)
				cond_resched();
		}

		total_allocated += allocated;
		pr_info("refill attempt %d: got %d pages (pool now %d/%d)\n",
			retry + 1, allocated, atomic_read(&pool_count), pool_total);

		if (atomic_read(&pool_count) >= pool_total)
			break;
	}

	pr_info("refill complete: %d pages recovered, pool %d/%d\n",
		total_allocated, atomic_read(&pool_count), pool_total);

	/* Release tracked mms - clean state for next VM cycle */
	schedule_work(&vm_owner_release_work);

	atomic_set(&refill_status, REFILL_IDLE);
}

/* ================================================================== */
/*  sysfs parameters                                                  */
/* ================================================================== */

/* hook_enable: manual override for kretprobe */
static int hook_enable_set(const char *val, const struct kernel_param *kp)
{
	int enable, ret = 0;

	if (kstrtoint(val, 10, &enable))
		return -EINVAL;

	mutex_lock(&hook_mutex);

	if (enable && !hook_active) {
		if (atomic_read(&pool_count) <= 0) {
			pr_err("pool empty, cannot enable\n");
			ret = -ENODATA;
		} else {
			ret = register_kretp_with_fallback();
			if (ret == 0)
				hook_active = true;
		}
	} else if (!enable && hook_active) {
		unregister_kretprobe(&kretp);
		hook_active = false;
		pr_info("hook manually disabled\n");
	}

	mutex_unlock(&hook_mutex);
	return ret;
}

static int hook_enable_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", hook_active ? 1 : 0);
}

static const struct kernel_param_ops hook_enable_ops = {
	.set = hook_enable_set,
	.get = hook_enable_get,
};
module_param_cb(hook_enable, &hook_enable_ops, NULL, 0644);
MODULE_PARM_DESC(hook_enable, "Manual override: 1=activate 0=deactivate");

/* pool_avail: read-only */
static int pool_avail_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&pool_count));
}

static const struct kernel_param_ops pool_avail_ops = {
	.get = pool_avail_get,
};
module_param_cb(pool_avail, &pool_avail_ops, NULL, 0444);
MODULE_PARM_DESC(pool_avail, "Pages available in pool (read-only)");

/* refill_stat: read-only status */
static int refill_stat_get(char *buf, const struct kernel_param *kp)
{
	static const char * const state_names[] = {
		"idle", "waiting", "running",
	};
	int state = atomic_read(&refill_status);
	int n = 0;

	n += sysfs_emit_at(buf, n, "state=%s\n", state_names[state]);
	n += sysfs_emit_at(buf, n, "pool_avail=%d\n", atomic_read(&pool_count));
	n += sysfs_emit_at(buf, n, "pool_total=%d\n", pool_total);
	n += sysfs_emit_at(buf, n, "total_served=%d\n", atomic_read(&total_served));
	n += sysfs_emit_at(buf, n, "total_refilled=%d\n", atomic_read(&total_refilled));
	n += sysfs_emit_at(buf, n, "active_vms=%d\n", max(atomic_read(&vm_active_count), 0));

	return n;
}

static const struct kernel_param_ops refill_stat_ops = {
	.get = refill_stat_get,
};
module_param_cb(refill_stat, &refill_stat_ops, NULL, 0444);
MODULE_PARM_DESC(refill_stat, "Refill status and statistics (read-only)");

/* manual_refill: write 1 to trigger manual refill */
static int manual_refill_set(const char *val, const struct kernel_param *kp)
{
	int trigger;

	if (kstrtoint(val, 10, &trigger) || trigger != 1)
		return -EINVAL;

	if (atomic_read(&pool_count) >= pool_total) {
		pr_info("pool already full\n");
		return 0;
	}

	if (atomic_cmpxchg(&refill_status, REFILL_IDLE, REFILL_WAITING) != REFILL_IDLE) {
		pr_warn("refill already in progress\n");
		return -EBUSY;
	}

	schedule_delayed_work(&refill_work, msecs_to_jiffies(100));
	pr_info("manual refill triggered\n");
	return 0;
}

static const struct kernel_param_ops manual_refill_ops = {
	.set = manual_refill_set,
};
module_param_cb(manual_refill, &manual_refill_ops, NULL, 0200);
MODULE_PARM_DESC(manual_refill, "Write 1 to trigger manual pool refill");

/* ================================================================== */
/*  Module init / exit                                                */
/* ================================================================== */

static int __init hugepage_reserve_init(void)
{
	int i, ret;

	if (pool_target < 1 || pool_target > POOL_SIZE_MAX)
		pool_target = 1024;

	if (refill_delay_ms < 1000)
		refill_delay_ms = 1000;

	pr_info("allocating %d x 2MB = %d MB\n",
		pool_target, pool_target * 2);

	INIT_WORK(&vm_owner_release_work, vm_owner_release_worker);
	INIT_DELAYED_WORK(&refill_work, refill_worker);

	/* Prepare kretprobe struct */
	kretp.handler       = ret_handler;
	kretp.entry_handler = entry_handler;
	kretp.data_size     = 0;
	kretp.maxactive     = 20;

	/* Pre-allocate order-9 compound pages */
	for (i = 0; i < pool_target; i++) {
		page_pool[i] = alloc_pages(GFP_KERNEL | __GFP_COMP |
					   __GFP_NOWARN | __GFP_RETRY_MAYFAIL,
					   PAGE_ORDER);
		if (!page_pool[i]) {
			pr_info("stopped at %d/%d\n",
				i, pool_target);
			break;
		}
		if ((i + 1) % 50 == 0) {
			cond_resched();
			if ((i + 1) % 100 == 0)
				pr_info("allocated %d/%d ...\n",
					i + 1, pool_target);
		}
	}

	pool_total = i;
	atomic_set(&pool_count, pool_total);

	if (pool_total == 0) {
		pr_err("no pages allocated, aborting\n");
		return -ENOMEM;
	}

	pr_info("pool ready: %d x 2MB = %d MB\n",
		pool_total, pool_total * 2);

	/*
	 * Register kretprobe at init - must be active before any VM ioctl.
	 * entry_handler filters efficiently: order, pool count, then mm.
	 */
	if (register_kretp_with_fallback() == 0) {
		hook_active = true;
	} else {
		pr_warn("kretprobe registration failed\n");
	}

	/* Register VM creation kprobe */
	{
		static const char * const syms[] = {
			"gh_dev_vm_mgr_ioctl",
			"gunyah_dev_vm_mgr_ioctl",
			NULL,
		};
		int j;

		vm_detect_kp.pre_handler = vm_detect_pre_handler;
		for (j = 0; syms[j]; j++) {
			vm_detect_kp.symbol_name = syms[j];
			ret = register_kprobe(&vm_detect_kp);
			if (ret == 0) {
				detect_active = true;
				pr_info("watching for VM creation (%s)\n",
					syms[j]);
				break;
			}
		}
		if (!detect_active)
			pr_warn("VM creation symbol not found (ret=%d)\n", ret);
	}

	/* Register VM destruction kprobe */
	{
		static const char * const syms[] = {
			"gh_vm_free",
			"gunyah_vm_release",
			NULL,
		};
		int j;

		vm_destroy_kp.pre_handler = vm_destroy_pre_handler;
		for (j = 0; syms[j]; j++) {
			vm_destroy_kp.symbol_name = syms[j];
			ret = register_kprobe(&vm_destroy_kp);
			if (ret == 0) {
				vm_destroy_active = true;
				pr_info("watching for VM destruction (%s)\n",
					syms[j]);
				break;
			}
		}
		if (!vm_destroy_active)
			pr_warn("VM destruction symbol not found (ret=%d), no auto-refill\n", ret);
	}

	/* Register __free_pages kprobe for direct page reclamation */
	{
		static const char * const syms[] = {
			"__free_pages",
			NULL,
		};
		int j;

		free_pages_kp.pre_handler = free_pages_pre_handler;
		for (j = 0; syms[j]; j++) {
			free_pages_kp.symbol_name = syms[j];
			ret = register_kprobe(&free_pages_kp);
			if (ret == 0) {
				free_intercept_active = true;
				pr_info("free-path reclaim active (%s)\n",
					syms[j]);
				break;
			}
		}
		if (!free_intercept_active)
			pr_warn("__free_pages hook failed (ret=%d), using alloc-based refill only\n", ret);
	}

	return 0;
}

static void __exit hugepage_reserve_exit(void)
{
	int i, remaining;

	/* Cancel all pending work */
	cancel_work_sync(&vm_owner_release_work);
	cancel_delayed_work_sync(&refill_work);

	/* Remove all probes */
	mutex_lock(&hook_mutex);
	if (hook_active) {
		unregister_kretprobe(&kretp);
		hook_active = false;
	}
	mutex_unlock(&hook_mutex);

	if (detect_active)
		unregister_kprobe(&vm_detect_kp);
	if (vm_destroy_active)
		unregister_kprobe(&vm_destroy_kp);
	if (free_intercept_active)
		unregister_kprobe(&free_pages_kp);

	/* Release tracked VM owner mms */
	{
		int j, count = atomic_read(&vm_owner_count);

		for (j = 0; j < count; j++) {
			if (vm_owner_mms[j])
				mmdrop(vm_owner_mms[j]);
		}
		atomic_set(&vm_owner_count, 0);
	}

	/* Free remaining pool pages */
	remaining = atomic_read(&pool_count);
	for (i = 0; i < remaining; i++)
		__free_pages(page_pool[i], PAGE_ORDER);

	pr_info("freed %d/%d pages (served=%d, refilled=%d)\n",
		remaining, pool_total,
		atomic_read(&total_served), atomic_read(&total_refilled));
}

module_init(hugepage_reserve_init);
module_exit(hugepage_reserve_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("wasdwasd0105");
MODULE_AUTHOR("lateautumn");
MODULE_AUTHOR("BigfootACA");
MODULE_DESCRIPTION("Pre-allocate pages for Gunyah with VM lifecycle-aware pool recycling");
