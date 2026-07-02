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
 *   insmod gh_hugepage_reserve.ko pool_want=1024 refill_delay_ms=5000
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
#include <linux/page_ref.h>
#include <linux/topology.h>
#include <linux/nodemask.h>

#define POOL_SIZE_MAX		4096
#define ACQUIRE_MAX_FAILS	8	/* stop once the cumulative fail score reaches
					 * this (a failure +1, a success -3, floored
					 * at 0) */
#define ACQUIRE_FAIL_DECAY	3	/* a success drops the fail score by this */
#define ACQUIRE_DELAY_MS	10	/* breathe between migrations */
#define PAGE_ORDER		9	/* 2^9 * 4KB = 2MB */
#define REFILL_DELAY_MS_DEFAULT	5000
#define REFILL_RETRY_MAX	3
#define REFILL_RETRY_INTERVAL_MS 3000

/* GH_CREATE_VM = _IO('G', 0x0) - same across */
#define GH_IOCTL_TYPE	'G'
#define GH_CREATE_VM	_IO(GH_IOCTL_TYPE, 0x0)

/* ---- Module parameters ---- */

/* pool_want is the single target knob (declared below): set at insmod and at
 * runtime via the same sysfs file. */

static int refill_delay_ms = REFILL_DELAY_MS_DEFAULT;
module_param(refill_delay_ms, int, 0644);
MODULE_PARM_DESC(refill_delay_ms,
	"Delay in ms before refilling pool after VM shutdown (default 5000)");

/* ---- Pool ---- */

static struct page *page_pool[POOL_SIZE_MAX];
static atomic_t pool_count = ATOMIC_INIT(0);
static int pool_total;	/* current CAPACITY: pages we actually hold now.
			 * The free-hook/refill only ever chase this, so it must
			 * never exceed what we've actually obtained - otherwise
			 * the hook would perpetually steal system order-9 frees. */
static int pool_want = 1024;	/* the single TARGET knob: set at insmod and at
				 * runtime via the pool_want sysfs. init allocates
				 * toward it; acquire raises capacity toward it. */
static bool pool_ready;		/* true once module_init has built the pool */
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

/*
 * One tracked Gunyah VM owner. tgid/comm are captured at GH_CREATE_VM time
 * so userspace can attribute pool pages to a concrete process (and, in the
 * app, to a VM). served counts the pool pages actually handed to this owner.
 */
struct vm_owner {
	struct mm_struct *mm;
	pid_t		  tgid;
	char		  comm[TASK_COMM_LEN];
	atomic_t	  served;
};

static struct vm_owner vm_owners[VM_OWNER_MAX];
static atomic_t vm_owner_count = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(vm_owner_lock);

static void vm_owner_add(struct mm_struct *mm)
{
	unsigned long flags;
	int i, count;

	spin_lock_irqsave(&vm_owner_lock, flags);
	count = atomic_read(&vm_owner_count);

	for (i = 0; i < count; i++) {
		if (vm_owners[i].mm == mm)
			goto out;
	}
	if (count < VM_OWNER_MAX) {
		mmgrab(mm);
		vm_owners[count].mm = mm;
		vm_owners[count].tgid = current->tgid;
		strscpy(vm_owners[count].comm, current->comm,
			sizeof(vm_owners[count].comm));
		atomic_set(&vm_owners[count].served, 0);
		smp_store_release(&vm_owner_count.counter, count + 1);
		pr_info("tracking VM owner mm=%px pid=%d (comm=%s)\n",
			mm, current->tgid, current->comm);
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
		if (READ_ONCE(vm_owners[i].mm) == mm)
			return true;
	}
	return false;
}

/* Charge a served pool page to the owner matching @mm (best-effort). */
static void vm_owner_served_inc(struct mm_struct *mm)
{
	int i, count;

	count = smp_load_acquire(&vm_owner_count.counter);
	for (i = 0; i < count; i++) {
		if (READ_ONCE(vm_owners[i].mm) == mm) {
			atomic_inc(&vm_owners[i].served);
			return;
		}
	}
}

/* ================================================================== */
/*  Served-page table                                                  */
/*                                                                     */
/*  Records every pool page handed to a guest (pfn -> owner tgid) so   */
/*  outstanding pages can be *located/attributed*, not just counted.   */
/*  Maintained from atomic probe context -> fully preallocated, no     */
/*  allocation on the hot path (chaining hash over a node freelist).   */
/*  Bounded: outstanding pages <= pool_total, so SERVED_MAX is plenty. */
/* ================================================================== */

#define SERVED_MAX	8192U		/* power of two */
#define SERVED_NULL	0xFFFFU

struct served_node {
	unsigned long	pfn;
	pid_t		tgid;
	u16		next;		/* freelist or bucket chain index */
};

static struct served_node served_nodes[SERVED_MAX];
static u16  served_bucket[SERVED_MAX];
static u16  served_free_head;
static int  served_count;
static int  served_overflow;
static DEFINE_RAW_SPINLOCK(served_lock);

/* Last reconcile result + per-owner live tallies (guarded by recon_mutex). */
static DEFINE_MUTEX(recon_mutex);
static int   recon_live, recon_orphan_freed, recon_orphan_inuse;
static int   recon_owner_n;
static pid_t recon_owner_tgid[VM_OWNER_MAX];
static char  recon_owner_comm[VM_OWNER_MAX][TASK_COMM_LEN];
static long  recon_owner_pages[VM_OWNER_MAX];

static void served_init(void)
{
	unsigned int i;

	for (i = 0; i < SERVED_MAX; i++) {
		served_nodes[i].next = (i + 1 < SERVED_MAX) ? (u16)(i + 1) : SERVED_NULL;
		served_bucket[i] = SERVED_NULL;
	}
	served_free_head = 0;
	served_count = 0;
	served_overflow = 0;
}

static inline unsigned int served_hash(unsigned long pfn)
{
	return (unsigned int)(pfn >> PAGE_ORDER) & (SERVED_MAX - 1);
}

static void served_add(unsigned long pfn, pid_t tgid)
{
	unsigned long flags;
	unsigned int b;
	u16 n, idx;

	raw_spin_lock_irqsave(&served_lock, flags);
	b = served_hash(pfn);
	for (n = served_bucket[b]; n != SERVED_NULL; n = served_nodes[n].next) {
		if (served_nodes[n].pfn == pfn) {
			served_nodes[n].tgid = tgid;	/* re-served: update owner */
			goto out;
		}
	}
	if (served_free_head == SERVED_NULL) {
		served_overflow++;
		goto out;
	}
	idx = served_free_head;
	served_free_head = served_nodes[idx].next;
	served_nodes[idx].pfn = pfn;
	served_nodes[idx].tgid = tgid;
	served_nodes[idx].next = served_bucket[b];
	served_bucket[b] = idx;
	served_count++;
out:
	raw_spin_unlock_irqrestore(&served_lock, flags);
}

static void served_del(unsigned long pfn)
{
	unsigned long flags;
	unsigned int b;
	u16 n, prev = SERVED_NULL;

	raw_spin_lock_irqsave(&served_lock, flags);
	b = served_hash(pfn);
	for (n = served_bucket[b]; n != SERVED_NULL;
	     prev = n, n = served_nodes[n].next) {
		if (served_nodes[n].pfn == pfn) {
			if (prev == SERVED_NULL)
				served_bucket[b] = served_nodes[n].next;
			else
				served_nodes[prev].next = served_nodes[n].next;
			served_nodes[n].next = served_free_head;
			served_free_head = n;
			served_count--;
			break;
		}
	}
	raw_spin_unlock_irqrestore(&served_lock, flags);
}

/*
 * Reconcile (process context only): drop every tracked page whose owner is no
 * longer a currently-tracked VM owner. Each dropped (orphan) page is classified
 * by whether its refcount is now 0 (returned to the system) or not (reused
 * elsewhere). Surviving entries are tallied per live owner.
 */
static void served_do_reconcile(void)
{
	unsigned long flags;
	pid_t live_tgid[VM_OWNER_MAX];
	char  live_comm[VM_OWNER_MAX][TASK_COMM_LEN];
	long  live_pages[VM_OWNER_MAX];
	int   live_n, i, b;
	int   live = 0, of = 0, oi = 0;

	spin_lock_irqsave(&vm_owner_lock, flags);
	live_n = atomic_read(&vm_owner_count);
	if (live_n > VM_OWNER_MAX)
		live_n = VM_OWNER_MAX;
	for (i = 0; i < live_n; i++) {
		live_tgid[i] = vm_owners[i].tgid;
		strscpy(live_comm[i], vm_owners[i].comm, TASK_COMM_LEN);
		live_pages[i] = 0;
	}
	spin_unlock_irqrestore(&vm_owner_lock, flags);

	raw_spin_lock_irqsave(&served_lock, flags);
	for (b = 0; b < SERVED_MAX; b++) {
		u16 prev = SERVED_NULL, n = served_bucket[b];

		while (n != SERVED_NULL) {
			int owner = -1;
			u16 nx;

			for (i = 0; i < live_n; i++) {
				if (live_tgid[i] == served_nodes[n].tgid) {
					owner = i;
					break;
				}
			}
			if (owner >= 0) {
				live++;
				live_pages[owner]++;
				prev = n;
				n = served_nodes[n].next;
				continue;
			}
			/* orphan: owner is no longer a tracked VM */
			{
				unsigned long pfn = served_nodes[n].pfn;
				struct page *pg = pfn_valid(pfn) ? pfn_to_page(pfn) : NULL;

				if (pg && page_count(pg) == 0)
					of++;
				else
					oi++;
			}
			nx = served_nodes[n].next;
			if (prev == SERVED_NULL)
				served_bucket[b] = nx;
			else
				served_nodes[prev].next = nx;
			served_nodes[n].next = served_free_head;
			served_free_head = n;
			served_count--;
			n = nx;
		}
	}
	raw_spin_unlock_irqrestore(&served_lock, flags);

	recon_live = live;
	recon_orphan_freed = of;
	recon_orphan_inuse = oi;
	recon_owner_n = live_n;
	for (i = 0; i < live_n; i++) {
		recon_owner_tgid[i] = live_tgid[i];
		strscpy(recon_owner_comm[i], live_comm[i], TASK_COMM_LEN);
		recon_owner_pages[i] = live_pages[i];
	}
}

/* ---- Work and probe structs ---- */

static struct work_struct vm_owner_release_work;
static struct delayed_work refill_work;
static struct kretprobe kretp;
static struct kprobe vm_detect_kp;
static struct kprobe vm_destroy_kp;
static struct kprobe free_pages_kp;

/* ---- Aggressive acquire (GUI-only): alloc_contig_pages migrates to build
 *      2MB blocks even under fragmentation. Neither helper is reliably exported:
 *      alloc_contig_pages is never exported, and prep_compound_page is only
 *      EXPORT_SYMBOL_GPL on some vendor trees (e.g. this 6.6 GKI) while merely
 *      declared in the private mm/internal.h on others (e.g. ACK 6.1). Both are
 *      non-static (hence in kallsyms), so resolve both via kprobe at init - a
 *      missing export must not fail module load, only disable acquire. */
static struct work_struct acquire_work;
static atomic_t acquire_running = ATOMIC_INIT(0);
static struct page *(*p_alloc_contig_pages)(unsigned long nr_pages,
		gfp_t gfp_mask, int nid, nodemask_t *nodemask);
static void (*p_prep_compound_page)(struct page *page, unsigned int order);

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

/*
 * Append a freshly *acquired* page and grow CAPACITY (pool_total) up to it.
 * Used only by the acquire path (real new allocations proven to succeed),
 * never by the free-hook - so capacity only rises by what we genuinely got.
 */
static bool pool_push_grow(struct page *page)
{
	unsigned long flags;
	int idx;
	bool ok;

	raw_spin_lock_irqsave(&pool_lock, flags);
	idx = atomic_read(&pool_count);
	ok = (idx < POOL_SIZE_MAX);
	if (ok) {
		page_pool[idx] = page;
		atomic_set(&pool_count, idx + 1);
		if (idx + 1 > pool_total)
			WRITE_ONCE(pool_total, idx + 1);
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

	/* A served pool page is being freed: drop it from the table regardless
	 * of whether we end up reclaiming it back into the pool below. */
	page = (struct page *)regs_get_kernel_argument(regs, 0);
	if (page)
		served_del(page_to_pfn(page));

	if (atomic_read(&pool_count) >= pool_total)
		return 0;
	/* Avoid reclaiming pages freed by our ret_handler */
	if (this_cpu_read(in_ret_handler))
		return 0;

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
	if (current->mm)
		vm_owner_served_inc(current->mm);
	served_add(page_to_pfn(pool_page), current->tgid);
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
		to_drop[i] = vm_owners[i].mm;
		vm_owners[i].mm = NULL;
		vm_owners[i].tgid = 0;
		vm_owners[i].comm[0] = '\0';
		atomic_set(&vm_owners[i].served, 0);
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
					struct mm_struct *mm = READ_ONCE(vm_owners[i].mm);

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
	/* Live served-out (lent to VMs) count. owned+traced = pool_avail + served;
	 * this can exceed pool_total after a shrink that left served pages out. */
	n += sysfs_emit_at(buf, n, "served=%d\n", served_count);
	n += sysfs_emit_at(buf, n, "pool_want=%d\n", READ_ONCE(pool_want));
	n += sysfs_emit_at(buf, n, "total_served=%d\n", atomic_read(&total_served));
	n += sysfs_emit_at(buf, n, "total_refilled=%d\n", atomic_read(&total_refilled));
	n += sysfs_emit_at(buf, n, "active_vms=%d\n", max(atomic_read(&vm_active_count), 0));
	n += sysfs_emit_at(buf, n, "acquire_active=%d\n", atomic_read(&acquire_running));

	return n;
}

static const struct kernel_param_ops refill_stat_ops = {
	.get = refill_stat_get,
};
module_param_cb(refill_stat, &refill_stat_ops, NULL, 0444);
MODULE_PARM_DESC(refill_stat, "Refill status and statistics (read-only)");

/* vm_owners: read-only per-VM-owner attribution */
static int vm_owners_get(char *buf, const struct kernel_param *kp)
{
	unsigned long flags;
	int i, count, n = 0;

	spin_lock_irqsave(&vm_owner_lock, flags);
	count = atomic_read(&vm_owner_count);
	for (i = 0; i < count; i++) {
		if (!vm_owners[i].mm)
			continue;
		/* comm is emitted last so userspace can keep spaces in it */
		n += sysfs_emit_at(buf, n, "pid=%d served=%d comm=%s\n",
				   vm_owners[i].tgid,
				   atomic_read(&vm_owners[i].served),
				   vm_owners[i].comm);
	}
	spin_unlock_irqrestore(&vm_owner_lock, flags);
	return n;
}

static const struct kernel_param_ops vm_owners_ops = {
	.get = vm_owners_get,
};
module_param_cb(vm_owners, &vm_owners_ops, NULL, 0444);
MODULE_PARM_DESC(vm_owners,
	"Per-VM-owner attribution: pid/served-pages/comm (read-only)");

/* reconcile: write 1 to reconcile the served-page table (process context) */
static int reconcile_set(const char *val, const struct kernel_param *kp)
{
	int v;

	if (kstrtoint(val, 10, &v) || v != 1)
		return -EINVAL;
	mutex_lock(&recon_mutex);
	served_do_reconcile();
	mutex_unlock(&recon_mutex);
	return 0;
}

static const struct kernel_param_ops reconcile_ops = {
	.set = reconcile_set,
};
module_param_cb(reconcile, &reconcile_ops, NULL, 0200);
MODULE_PARM_DESC(reconcile, "Write 1 to reconcile the served-page table");

/* served_summary: read-only reconciled view of the served-page table */
static int served_summary_get(char *buf, const struct kernel_param *kp)
{
	int n = 0, i;

	mutex_lock(&recon_mutex);
	n += sysfs_emit_at(buf, n, "tracked=%d\n", READ_ONCE(served_count));
	n += sysfs_emit_at(buf, n, "overflow=%d\n", READ_ONCE(served_overflow));
	n += sysfs_emit_at(buf, n, "live=%d\n", recon_live);
	n += sysfs_emit_at(buf, n, "orphan_freed=%d\n", recon_orphan_freed);
	n += sysfs_emit_at(buf, n, "orphan_inuse=%d\n", recon_orphan_inuse);
	for (i = 0; i < recon_owner_n; i++)
		n += sysfs_emit_at(buf, n, "owner pid=%d pages=%ld comm=%s\n",
				   recon_owner_tgid[i], recon_owner_pages[i],
				   recon_owner_comm[i]);
	mutex_unlock(&recon_mutex);
	return n;
}

static const struct kernel_param_ops served_summary_ops = {
	.get = served_summary_get,
};
module_param_cb(served_summary, &served_summary_ops, NULL, 0444);
MODULE_PARM_DESC(served_summary,
	"Reconciled served-page table summary (read-only)");

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

/*
 * pool_do_resize: change the target (pool_want).
 *   shrink (new < capacity): free the excess pooled pages immediately.
 *   grow   (new > capacity): raise the target only; new pages are NOT
 *                            allocated here - use acquire to fill.
 */
static void pool_do_resize(int newt)
{
	struct page *batch[32];
	int target_avail;

	/*
	 * Clamp only to the array bounds. A target below the currently lent-out
	 * count is allowed and safe: shrink frees only pooled pages, and when the
	 * lent pages are later returned the free-hook sees pool_count >= pool_total
	 * and lets them go to buddy instead of re-pooling - no overflow, no leak.
	 */
	if (newt < 1)
		newt = 1;
	if (newt > POOL_SIZE_MAX)
		newt = POOL_SIZE_MAX;

	WRITE_ONCE(pool_want, newt);

	/*
	 * The real reserve held is owned+traced = pool_count(avail) + served. GROW
	 * stops here: nothing to free, pool_total is left for acquire/free-hook to
	 * raise as pages come in.
	 */
	if (atomic_read(&pool_count) + served_count <= newt)
		return;

	/*
	 * SHRINK: free available pages so owned+traced drops to newt. Served pages
	 * can't be freed (VMs hold them), so the floor is target_avail =
	 * max(0, newt - served). Set pool_total to that floor *first* so the
	 * free-hook (which reclaims a freed order-9 page whenever
	 * pool_count < pool_total) skips the pages we drain here - pool_count stays
	 * >= target_avail throughout. Restore pool_total = newt afterwards so the
	 * hook can re-pool VM-returned pages back up to the new target.
	 */
	target_avail = newt - served_count;
	if (target_avail < 0)
		target_avail = 0;
	WRITE_ONCE(pool_total, target_avail);

	/* free outside the lock to avoid the page allocator under it. */
	for (;;) {
		unsigned long flags;
		int n = 0, i;

		raw_spin_lock_irqsave(&pool_lock, flags);
		while (n < 32 && atomic_read(&pool_count) > target_avail) {
			int idx = atomic_read(&pool_count) - 1;

			batch[n++] = page_pool[idx];
			page_pool[idx] = NULL;
			atomic_set(&pool_count, idx);
		}
		raw_spin_unlock_irqrestore(&pool_lock, flags);

		for (i = 0; i < n; i++)
			__free_pages(batch[i], PAGE_ORDER);
		if (n == 0)
			break;
	}

	WRITE_ONCE(pool_total, newt);
	pr_info("pool shrink: want=%d avail=%d served=%d\n",
		newt, atomic_read(&pool_count), served_count);
}

/*
 * pool_want: the single target knob. Settable at insmod and at runtime via the
 * same file. At insmod (before the pool is built) we just record the value;
 * after init, a write resizes live (shrink frees now, grow raises target).
 */
static int pool_want_set(const char *val, const struct kernel_param *kp)
{
	int v;

	if (kstrtoint(val, 10, &v))
		return -EINVAL;
	if (v < 1)
		v = 1;
	if (v > POOL_SIZE_MAX)
		v = POOL_SIZE_MAX;

	if (!READ_ONCE(pool_ready)) {
		WRITE_ONCE(pool_want, v);	/* boot: init will allocate toward it */
	} else if (atomic_read(&acquire_running)) {
		/*
		 * Refuse to resize while the acquire worker runs: it is migrating
		 * pages *into* the pool, and a concurrent resize would race it over
		 * pool_total/pool_count/page_pool[]. Keep it simple and lock-free by
		 * requiring the module to be quiescent first - write 0 to 'acquire'
		 * to interrupt the worker, then set pool_want once it has stopped.
		 */
		return -EBUSY;
	} else {
		pool_do_resize(v);		/* runtime: live resize */
	}
	return 0;
}

static int pool_want_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(pool_want));
}

static const struct kernel_param_ops pool_want_ops = {
	.set = pool_want_set,
	.get = pool_want_get,
};
module_param_cb(pool_want, &pool_want_ops, NULL, 0644);
MODULE_PARM_DESC(pool_want,
	"Target pages (insmod + runtime): grow raises target, shrink frees now");

/* ================================================================== */
/*  Aggressive acquire (GUI-only)                                      */
/* ================================================================== */

/* Resolve a non-exported symbol via a throwaway kprobe (module already uses
 * kprobes). Returns NULL if the symbol is absent. */
static void *resolve_kfunc(const char *name)
{
	struct kprobe kp = { .symbol_name = name };
	void *addr = NULL;

	if (register_kprobe(&kp) == 0) {
		addr = (void *)kp.addr;
		unregister_kprobe(&kp);
	}
	return addr;
}

/*
 * Fill the pool toward pool_want using alloc_contig_pages(), which *migrates*
 * movable pages to assemble 2MB blocks even when the buddy allocator has none.
 * Heavy (migration) but only ever run from the GUI button, never at boot, so a
 * worst-case stall/abort is reboot-recoverable and can't bootloop.
 *
 * alloc_contig_pages returns nr_pages individual refcount-1 pages; turn the
 * range into one order-9 compound page (prep_compound_page sets structure,
 * then zero the tail refcounts) so it is indistinguishable from an
 * alloc_pages(__GFP_COMP) page for serving and for __free_pages on release.
 */
static void acquire_worker(struct work_struct *w)
{
	const int nr = 1 << PAGE_ORDER;
	int got = 0, fail_score = 0, i;

	if (!p_alloc_contig_pages || !p_prep_compound_page)
		goto out;

	/*
	 * Fill until the reserve (available + served-out) reaches pool_want, so
	 * pages already lent to VMs count toward the target. Exit when:
	 *   - acquire_running is cleared (GUI wrote 0 to interrupt - pool_want is
	 *     left intact so the remaining deficit still shows as "waiting"), or
	 *   - the cumulative fail score reaches ACQUIRE_MAX_FAILS (each migration
	 *     failure adds 1, each success subtracts ACQUIRE_FAIL_DECAY, floored
	 *     at 0), the normal "system can't migrate any more" give-up.
	 * msleep throttles CPU.
	 */
	while (atomic_read(&acquire_running) &&
	       atomic_read(&pool_count) + served_count < READ_ONCE(pool_want) &&
	       fail_score < ACQUIRE_MAX_FAILS) {
		struct page *p = p_alloc_contig_pages(nr,
				GFP_KERNEL | __GFP_NOWARN, numa_node_id(), NULL);

		if (!p) {
			fail_score++;
			msleep(ACQUIRE_DELAY_MS);
			continue;
		}
		fail_score -= ACQUIRE_FAIL_DECAY;
		if (fail_score < 0)
			fail_score = 0;

		p_prep_compound_page(p, PAGE_ORDER);
		for (i = 1; i < nr; i++)
			set_page_count(p + i, 0);	/* tails: 1 -> 0 */

		if (!pool_push_grow(p)) {
			__free_pages(p, PAGE_ORDER);
			break;			/* pool array full */
		}
		atomic_inc(&total_refilled);
		got++;
		msleep(ACQUIRE_DELAY_MS);	/* let the system breathe */
	}

	pr_info("acquire done: +%d pages, avail=%d capacity=%d want=%d\n",
		got, atomic_read(&pool_count), pool_total, READ_ONCE(pool_want));
out:
	atomic_set(&acquire_running, 0);
}

/* acquire: write 1 to start filling toward pool_want, 0 to interrupt. */
static int acquire_set(const char *val, const struct kernel_param *kp)
{
	int v;

	if (kstrtoint(val, 10, &v) || (v != 0 && v != 1))
		return -EINVAL;
	if (v == 0) {
		/* Interrupt: the worker exits at its next check; pool_want stays. */
		atomic_set(&acquire_running, 0);
		return 0;
	}
	if (!p_alloc_contig_pages || !p_prep_compound_page)
		return -ENOSYS;
	/* Nothing to do if the reserve (available + served) already meets want. */
	if (atomic_read(&pool_count) + served_count >= READ_ONCE(pool_want))
		return 0;
	if (atomic_cmpxchg(&acquire_running, 0, 1) != 0)
		return -EBUSY;
	/*
	 * Fire-and-return: the heavy migration runs in the kworker and the write
	 * comes back immediately. Blocking here would hold the per-module param
	 * mutex for the whole acquire, stalling every other get/set (refill_stat
	 * polling, etc.) and the GUI. Instead the worker sets acquire_running=0
	 * when done; userspace polls refill_stat's acquire_active to watch live
	 * progress and detect completion.
	 */
	schedule_work(&acquire_work);
	return 0;
}

static const struct kernel_param_ops acquire_ops = {
	.set = acquire_set,
};
module_param_cb(acquire, &acquire_ops, NULL, 0200);
MODULE_PARM_DESC(acquire,
	"Write 1 to acquire (migrate) pages toward pool_want, 0 to interrupt");

/* ================================================================== */
/*  Module init / exit                                                */
/* ================================================================== */

static int __init hugepage_reserve_init(void)
{
	int i, ret;

	if (pool_want < 1 || pool_want > POOL_SIZE_MAX)
		pool_want = 1024;

	if (refill_delay_ms < 1000)
		refill_delay_ms = 1000;

	pr_info("allocating up to %d x 2MB = %d MB\n",
		pool_want, pool_want * 2);

	INIT_WORK(&vm_owner_release_work, vm_owner_release_worker);
	INIT_DELAYED_WORK(&refill_work, refill_worker);
	INIT_WORK(&acquire_work, acquire_worker);
	served_init();

	/* Resolve the helpers the aggressive acquire path needs (optional feature).
	 * Both are non-static but not reliably exported across kernels, so resolve
	 * via kprobe rather than extern-linking - that way a missing/unexported
	 * symbol only disables acquire instead of failing the whole module load. */
	p_alloc_contig_pages = resolve_kfunc("alloc_contig_pages");
	p_prep_compound_page = resolve_kfunc("prep_compound_page");
	if (!p_alloc_contig_pages || !p_prep_compound_page)
		pr_warn("aggressive acquire disabled (alloc_contig_pages=%d prep_compound_page=%d)\n",
			!!p_alloc_contig_pages, !!p_prep_compound_page);

	/* Prepare kretprobe struct */
	kretp.handler       = ret_handler;
	kretp.entry_handler = entry_handler;
	kretp.data_size     = 0;
	kretp.maxactive     = 20;

	/* Pre-allocate order-9 compound pages toward the target (give up per-page
	 * when even compaction/reclaim can't produce one; never OOM-kills). */
	for (i = 0; i < pool_want; i++) {
		page_pool[i] = alloc_pages(GFP_KERNEL | __GFP_COMP |
					   __GFP_NOWARN | __GFP_RETRY_MAYFAIL,
					   PAGE_ORDER);
		if (!page_pool[i]) {
			pr_info("stopped at %d/%d\n",
				i, pool_want);
			break;
		}
		if ((i + 1) % 50 == 0) {
			cond_resched();
			if ((i + 1) % 100 == 0)
				pr_info("allocated %d/%d ...\n",
					i + 1, pool_want);
		}
	}

	pool_total = i;			/* capacity = what we actually got */
	/* pool_want stays = the requested target (may exceed capacity) */
	atomic_set(&pool_count, pool_total);

	if (pool_total == 0) {
		pr_err("no pages allocated, aborting\n");
		return -ENOMEM;
	}

	pr_info("pool ready: %d x 2MB = %d MB (target %d)\n",
		pool_total, pool_total * 2, pool_want);

	/* From here, pool_want writes resize live instead of just recording. */
	WRITE_ONCE(pool_ready, true);

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

	/* Stop live resizes racing with teardown */
	WRITE_ONCE(pool_ready, false);

	/* Cancel all pending work */
	cancel_work_sync(&vm_owner_release_work);
	cancel_work_sync(&acquire_work);
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
			if (vm_owners[j].mm)
				mmdrop(vm_owners[j].mm);
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
