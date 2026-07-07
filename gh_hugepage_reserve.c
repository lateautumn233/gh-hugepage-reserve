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
 *                     Each owner carries a vm_count (VMs created by that mm).
 *      - Destruction: kprobe on gunyah_vm_release / gh_vm_free
 *                     schedules delayed pool refill after VM exits, then
 *                     sweeps owners per-entry: only those whose process is
 *                     dead (mm_users == 0) or whose last VM was closed are
 *                     released. Sibling VMs keep their tracking - one VM
 *                     dying (e.g. OOM kill of one crosvm) must never drop
 *                     the others.
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
#include <linux/tracepoint.h>
#include <linux/mmzone.h>
#include <linux/version.h>
#include <linux/string.h>
#include <linux/swap.h>
#include <linux/mm_inline.h>
/* Single source of truth for the resolvable kapi symbol names (KAPI_SYMBOLS),
 * generated from abi/kapi_abi.tsv by abi/gen_kapi.awk (also drives the userspace
 * ABI preflight). We only pull the name list here (not KAPI_ABI_WANT_TABLE). */
#include "kapi_abi.gen.h"
/*
 * The free-path reclaim rides the android_vh_free_one_page_bypass vendor hook.
 * It is NOT present on every KMI we build for (absent on some 5.10/5.15 vendor
 * hook sets, present on 5.15-android13, 6.1, 6.6, 6.12), and its availability is
 * not monotonic by version - so we do NOT reference register_trace_* (which
 * fails to compile where the DECLARE_HOOK is missing). Instead we locate the
 * tracepoint by name at runtime via for_each_kernel_tracepoint() and attach with
 * tracepoint_probe_register(); a missing hook simply disables free reclaim.
 */

#define POOL_SIZE_MAX_RAM	(24UL << 30)	/* hard cap on the pool, in bytes;
						 * sizes the static arrays */
#define POOL_SIZE_MAX	((int)(POOL_SIZE_MAX_RAM >> (PAGE_SHIFT + PAGE_ORDER)))
					/* the same cap in 2MB pages (12288). The
					 * effective cap is pool_size_max, computed
					 * from system RAM at insmod:
					 * min(ram - min(ram/2, SYSTEM_RESERVE),
					 *     POOL_SIZE_MAX_RAM). */
#define SYSTEM_RESERVE		(6UL << 30)	/* RAM never counted into pool_size_max:
						 * the pool leaves at least this much
						 * (or half of RAM, whichever is less)
						 * to the rest of the system. 6G fits
						 * a heavy Android resident set:
						 * ~2-3G unswappable kernel+dmabuf/GPU
						 * plus ~2.7G core services. */
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
module_param(refill_delay_ms, int, 0600);
MODULE_PARM_DESC(refill_delay_ms,
	"Delay in ms before refilling pool after VM shutdown (default 5000)");

/*
 * refill_enable gates the OLD detect->alloc-back recovery path (the VM-destroy
 * kprobe scheduling refill_worker, and manual_refill). Set 0 to measure the new
 * free-path reclaim (android_vh_free_one_page_bypass) in isolation, so the pool
 * only refills from pages a VM actually returns - not from fresh alloc_pages.
 * Default 1 keeps normal behaviour. The vm-owner sweep still runs when disabled.
 */
static int refill_enable = 1;
module_param(refill_enable, int, 0600);
MODULE_PARM_DESC(refill_enable,
	"1=auto alloc-back refill after VM shutdown (default), 0=disable (test free-hook reclaim alone)");

/* ---- Pool ---- */

static struct page *page_pool[POOL_SIZE_MAX];
static atomic_t pool_count = ATOMIC_INIT(0);
static int pool_total;	/* current CAPACITY: pages we actually hold (avail +
			 * lent-out). Refill only ever chases this, so it must
			 * never exceed what we've actually obtained. The free
			 * hook re-pools against pool_want (the CURRENT target)
			 * instead and raises this high-water as pages return -
			 * so shrinking the target never permanently disowns
			 * pages that are lent out at the time. */
static int pool_want = 1024;	/* the single TARGET knob: set at insmod and at
				 * runtime via the pool_want sysfs. init allocates
				 * toward it; acquire raises capacity toward it. */
static int pool_size_max = POOL_SIZE_MAX;	/* effective cap on pool_want/capacity,
						 * computed from system RAM at insmod
						 * (see hugepage_reserve_init); every
						 * pool_want write site clamps to it. */
module_param(pool_size_max, int, 0400);
MODULE_PARM_DESC(pool_size_max,
	"Effective pool cap in 2MB pages: min(ram - min(ram/2, 6G), 24G); read-only");
static bool pool_ready;		/* true once module_init has built the pool */
static DEFINE_RAW_SPINLOCK(pool_lock);

/* ---- Probe state ---- */

static bool hook_active;
static bool detect_active;
static bool vm_destroy_active;
static bool free_intercept_active;
static int reclaim_want = 1;	/* desired free-hook state (insmod + runtime) */
static DEFINE_MUTEX(hook_mutex);

/* ---- Statistics and VM tracking ---- */

enum refill_state { REFILL_IDLE = 0, REFILL_WAITING, REFILL_RUNNING };

static atomic_t refill_status = ATOMIC_INIT(REFILL_IDLE);
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
 *
 * Owners are released one at a time by the sweep worker, and only when
 * they are actually gone: process dead (mm_users == 0) or last VM closed
 * (vm_count == 0). Never clear the whole table on a destroy event - with
 * several VM processes, one dying (e.g. OOM kill) must not drop the
 * tracking of the survivors.
 *
 * Slots are reused in place (mm == NULL marks a free slot) so the
 * lockless readers never see entries move; vm_owner_count is the
 * high-water slot count.
 */
#define VM_OWNER_MAX	8

/*
 * One tracked Gunyah VM owner. tgid/comm are captured at GH_CREATE_VM time
 * so userspace can attribute pool pages to a concrete process (and, in the
 * app, to a VM). served counts the pool pages actually handed to this owner;
 * vm_count is the number of VMs this mm has created and not yet destroyed.
 */
struct vm_owner {
	struct mm_struct *mm;
	pid_t		  tgid;
	char		  comm[TASK_COMM_LEN];
	atomic_t	  served;
	int		  vm_count;
};

static struct vm_owner vm_owners[VM_OWNER_MAX];
static atomic_t vm_owner_count = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(vm_owner_lock);
static struct work_struct vm_owner_sweep_work;

static void vm_owner_add(struct mm_struct *mm)
{
	unsigned long flags;
	int i, count, slot = -1;

	spin_lock_irqsave(&vm_owner_lock, flags);
	count = atomic_read(&vm_owner_count);

	for (i = 0; i < count; i++) {
		if (vm_owners[i].mm == mm) {
			vm_owners[i].vm_count++;
			goto out;
		}
		if (!vm_owners[i].mm && slot < 0)
			slot = i;	/* swept slot: reusable */
	}
	if (slot < 0 && count < VM_OWNER_MAX)
		slot = count;
	if (slot >= 0) {
		mmgrab(mm);
		vm_owners[slot].tgid = current->tgid;
		strscpy(vm_owners[slot].comm, current->comm,
			sizeof(vm_owners[slot].comm));
		atomic_set(&vm_owners[slot].served, 0);
		vm_owners[slot].vm_count = 1;
		/* mm gates the entry for lockless readers: publish it last */
		smp_store_release(&vm_owners[slot].mm, mm);
		if (slot == count)
			smp_store_release(&vm_owner_count.counter, count + 1);
		pr_info("tracking VM owner mm=%px pid=%d (comm=%s)\n",
			mm, current->tgid, current->comm);
	} else {
		pr_warn("too many VM owners (%d), ignoring %s\n",
			VM_OWNER_MAX, current->comm);
		schedule_work(&vm_owner_sweep_work);
	}
out:
	spin_unlock_irqrestore(&vm_owner_lock, flags);
}

/* Drop one VM from the owner matching @mm (explicit close by a live process). */
static void vm_owner_vm_dec(struct mm_struct *mm)
{
	unsigned long flags;
	int i, count;

	spin_lock_irqsave(&vm_owner_lock, flags);
	count = atomic_read(&vm_owner_count);
	for (i = 0; i < count; i++) {
		if (vm_owners[i].mm == mm) {
			if (vm_owners[i].vm_count > 0)
				vm_owners[i].vm_count--;
			break;
		}
	}
	spin_unlock_irqrestore(&vm_owner_lock, flags);
}

/*
 * Drop one VM from the SOLE live owner - for gunyah-vm releases that arrive
 * without an mm to attribute by (delayed_fput from a kworker, late exit path).
 * Without this, a release the mm-path can't attribute leaves the owner's
 * vm_count inflated forever on a still-live process: the sweep only releases
 * owners with vm_count == 0 or mm_users == 0, so the slot stays pinned until
 * the process dies, and 8 such owners exhaust the table (new VMs untracked).
 * With exactly one live owner the attribution is unambiguous; with several,
 * decrementing a guess could drop a live VM's tracking, so we skip and return
 * false - the caller logs it and the mm_users sweep remains the GC of last
 * resort.
 */
static bool vm_owner_vm_dec_sole(void)
{
	unsigned long flags;
	int i, count, live = 0, idx = -1;
	bool ok = false;

	spin_lock_irqsave(&vm_owner_lock, flags);
	count = atomic_read(&vm_owner_count);
	for (i = 0; i < count; i++) {
		if (vm_owners[i].mm) {
			live++;
			idx = i;
		}
	}
	if (live == 1) {
		if (vm_owners[idx].vm_count > 0)
			vm_owners[idx].vm_count--;
		ok = true;
	}
	spin_unlock_irqrestore(&vm_owner_lock, flags);
	return ok;
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

/* Live VMs = sum of per-owner vm_count (lockless, for stats/logs). */
static int vm_active_total(void)
{
	int i, count, total = 0;

	count = smp_load_acquire(&vm_owner_count.counter);
	for (i = 0; i < count; i++) {
		if (READ_ONCE(vm_owners[i].mm))
			total += READ_ONCE(vm_owners[i].vm_count);
	}
	return total;
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

#define SERVED_MAX	16384U		/* power of two, >= POOL_SIZE_MAX; u16
					 * node indexes cap this at 32768 */
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

/* Jiffies of the most recent VM-destroy detection. reconcile refuses to PURGE
 * orphan entries inside the grace window that follows: the dead VM's frees are
 * still in flight (including order-9 folios parked in pcp lists), and purging
 * an entry before its free arrives makes the free hook miss it - the page then
 * drains to buddy instead of returning to the pool. This is exactly the race
 * the DroidVM panel's 2s reconcile cadence kept hitting on SIGKILLed crosvm
 * (owner released 46ms after the kill -> whole table counted as orphans ->
 * mid-teardown reconcile discarded entries whose frees arrived moments later).
 * Init sets this to "grace already expired" so boot-time reconciles purge. */
static unsigned long last_destroy_jiffies;
#define RECONCILE_GRACE_MS	10000

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

/* served_lock must be held. Records @pfn's current holder @tgid (updates it if
 * the entry already exists). Only reached from pool_serve() after it has already
 * verified a free slot exists, so the overflow branch is a belt-and-suspenders. */
static void served_add_locked(unsigned long pfn, pid_t tgid)
{
	unsigned int b = served_hash(pfn);
	u16 n, idx;

	for (n = served_bucket[b]; n != SERVED_NULL; n = served_nodes[n].next) {
		if (served_nodes[n].pfn == pfn) {
			served_nodes[n].tgid = tgid;	/* re-served: update owner */
			return;
		}
	}
	if (served_free_head == SERVED_NULL) {
		served_overflow++;
		return;
	}
	idx = served_free_head;
	served_free_head = served_nodes[idx].next;
	served_nodes[idx].pfn = pfn;
	served_nodes[idx].tgid = tgid;
	served_nodes[idx].next = served_bucket[b];
	served_bucket[b] = idx;
	served_count++;
}

/* Returns true if @pfn was found in the served table and removed. */
static bool served_del(unsigned long pfn)
{
	unsigned long flags;
	unsigned int b;
	u16 n, prev = SERVED_NULL;
	bool found = false;

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
			found = true;
			break;
		}
	}
	raw_spin_unlock_irqrestore(&served_lock, flags);
	return found;
}

/* Physical scavenger for hook-missed frees - defined after the pool helpers
 * it needs; called from the pcp-drain worker and from reconcile below. */
static int served_reacquire_free_orphans(void);

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
	/* Purge orphans only once the post-destroy grace window has passed -
	 * inside it their frees are still arriving and entries must survive so
	 * the free hook can match them (see last_destroy_jiffies). */
	bool  purge = time_after(jiffies, READ_ONCE(last_destroy_jiffies) +
				 msecs_to_jiffies(RECONCILE_GRACE_MS));

	/* Physically recover hook-missed frees FIRST, so a recoverable page is
	 * neither tallied as an orphan nor purged below once grace is over. */
	served_reacquire_free_orphans();

	spin_lock_irqsave(&vm_owner_lock, flags);
	{
		int count = atomic_read(&vm_owner_count);

		if (count > VM_OWNER_MAX)
			count = VM_OWNER_MAX;
		live_n = 0;
		for (i = 0; i < count; i++) {
			if (!vm_owners[i].mm)
				continue;	/* swept slot */
			live_tgid[live_n] = vm_owners[i].tgid;
			strscpy(live_comm[live_n], vm_owners[i].comm, TASK_COMM_LEN);
			live_pages[live_n] = 0;
			live_n++;
		}
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
			if (!purge) {		/* grace window: count, keep entry */
				prev = n;
				n = served_nodes[n].next;
				continue;
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

	/* Log whenever orphans were seen: a PURGING reconcile discards their
	 * pending frees (the hook can no longer match them), so who ran it, and
	 * when, matters when auditing lost pages. The all-live case stays quiet -
	 * the DroidVM usage panel reconciles every ~2s and would spam dmesg. */
	if (of || oi)
		pr_info("reconcile by %s[%d]: live=%d orphan_freed=%d orphan_inuse=%d%s\n",
			current->comm, task_pid_nr(current), live, of, oi,
			purge ? "" : " (grace: kept)");
}

/* ---- Work and probe structs ---- */

static struct delayed_work refill_work;
static struct delayed_work pcp_drain_work;
#define PCP_DRAIN_DELAY_MS	3000	/* after the teardown free burst settles */
static struct kretprobe kretp;
static struct kprobe vm_detect_kp;
static struct kprobe vm_destroy_kp;

/* ---- Aggressive acquire (GUI-only): alloc_contig_pages migrates to build
 *      2MB blocks even under fragmentation. Neither helper is reliably exported:
 *      alloc_contig_pages is never exported, and prep_compound_page is only
 *      EXPORT_SYMBOL_GPL on some vendor trees (e.g. this 6.6 GKI) while merely
 *      declared in the private mm/internal.h on others (e.g. ACK 6.1). Both are
 *      non-static (hence in kallsyms), so resolve both via kprobe at init - a
 *      missing export must not fail module load, only disable acquire. */
static struct work_struct acquire_work;
static atomic_t acquire_running = ATOMIC_INIT(0);
static int acquire_mode;	/* algorithm the running worker uses: 1=original, 2=smart */
/* Why the last user-triggered acquire stopped - surfaced in refill_stat as
 * acquire_stop_reason= so the GUI can tell the user why the button's work ended
 * (target met, ran out of assemblable memory, hit the floor, interrupted...)
 * instead of them digging through dmesg. Points at a static string literal, so
 * the pointer read/write needs no lock. Set ONLY by the acquire path, never the
 * periodic refill worker. */
static const char *acquire_stop_reason = "idle";
/*
 * Unified kernel-API object - the module's OWN stable ABI over the kernel.
 * Its purpose is to hide every kernel-version difference (symbol names,
 * signatures, argument and return-value SEMANTICS) behind high-level
 * operations that mean the same thing on every supported kernel: call sites
 * just do kapi.op(...) and never see the raw kernel function. Every field is
 * named k_<kernel symbol> - the suffix keeps the backing API recognizable at
 * a glance, and the k_ does two jobs: it marks that the field's prototype and
 * semantics are the module-NORMALIZED contract (the per-field comments below),
 * not necessarily the running kernel's, and it keeps the identifier from ever
 * colliding with a kernel function-like macro (on 6.10+ alloc_contig_{range,
 * pages} are alloc_hooks() macros that would hijack a bare-named field call).
 * Where the kernel contract diverges across versions a shim settles it, e.g.
 * kapi.k_folio_isolate_lru returns bool true=isolated on EVERY kernel even
 * though the 6.1 symbol returns int 0=isolated, and kapi.k_alloc_contig_range
 * takes no migratetype/acr_flags argument at all.
 *
 * Resolution happens once at init (kapi_init, via kprobe). Symbols whose
 * contract is version-stable are stored directly. The four whose contract
 * diverges across GKI bases resolve into the private `kraw` struct instead
 * (prototypes version-gated to match the running kernel - kCFI checks the
 * indirect-call type id) and the kapi field gets a shim that settles the
 * divergence in ONE place:
 *   folio_isolate_lru: 6.1/6.2 return int (0 = isolated); 6.3+
 *     (be2d5756382a) return bool (true = isolated) - a bare truth test would
 *     invert on 6.1, leaking isolated folios off-LRU with the isolation ref
 *     held and list_add-ing folios that are still on the LRU
 *   reclaim_pages: 6.6..6.12 take (list, ignore_refs); others (list)
 *   try_to_free_mem_cgroup_pages: 5.10/5.15 (memcg,nr,gfp,bool may_swap);
 *     6.1/6.6 (…,uint reclaim_options); 6.12+ (…,+int *swappiness)
 *   alloc_contig_range: same C signature everywhere (no CFI difference), but
 *     arg 3 changed meaning: migratetype (<=6.12: pass MIGRATE_MOVABLE) became
 *     acr_flags_t (upstream ~6.16, next GKI base 6.18: pass ACR_FLAGS_NONE=0;
 *     MIGRATE_MOVABLE=1 would alias ACR_FLAGS_CMA and silently run the sweep
 *     in CMA isolation mode), so the shim owns arg 3 and callers never pass it
 *
 * A missing or guard-disabled symbol leaves BOTH the kraw and kapi pointers
 * NULL; callers then either refuse a whole feature (required op - see the
 * kapi_can_* checklists) or degrade (optional enhancer - guarded at the call
 * site).
 */
struct mem_cgroup;
struct folio;
struct kapi {
	/* --- assembly primitives (required for the acquire mode that uses each) --- */
	struct page *(*k_alloc_contig_pages)(unsigned long nr_pages, gfp_t gfp_mask,
			int nid, nodemask_t *nodemask);	/* id=1: contig run, anywhere */
	int (*k_alloc_contig_range)(unsigned long start, unsigned long end,
			gfp_t gfp_mask);	/* id=2/3 sweep: movable, non-CMA */
	void (*k_prep_compound_page)(struct page *page, unsigned int order);
	/* --- per-block evict B: required for id=3 (madvise(MADV_PAGEOUT) path) --- */
	bool (*k_folio_isolate_lru)(struct folio *folio);	/* true = isolated off-LRU */
	unsigned long (*k_reclaim_pages)(struct list_head *list);
	/* --- optional enhancers: NULL just degrades the feature, never disables --- */
	void (*k_lru_add_drain_all)(void);	/* accurate PageLRU for the gate */
	void (*k_drop_slab)(void);		/* unpoison slab-held windows */
	void (*k_drain_all_pages)(struct zone *);	/* flush pcp so parked order-9
						 * frees reach the reclaim hook */
	struct mem_cgroup *(*k_mem_cgroup_from_task)(struct task_struct *p);
	unsigned long (*k_try_to_free_mem_cgroup_pages)(struct mem_cgroup *memcg,
			unsigned long nr);	/* GFP_KERNEL, swap allowed */
};
static struct kapi kapi;

static struct zone *acq_zone;		/* zone the pool pages live in (cached at init) */
static unsigned long scan_cursor;	/* persistent pfn cursor for the smart scan (id=2) */

/* Raw kernel symbols behind the shims: version-TRUE prototypes (kCFI matches
 * the indirect call against the kernel function's type id), resolved by
 * kapi_init, and never called from anywhere except the kapi_*_shim functions
 * directly below - call sites use only the normalized kapi.op pointers. */
static struct {
	int (*k_alloc_contig_range)(unsigned long start, unsigned long end,
			unsigned int migratetype_or_acr, gfp_t gfp_mask);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
	int (*k_folio_isolate_lru)(struct folio *folio);		/* 0 = isolated */
#else
	bool (*k_folio_isolate_lru)(struct folio *folio);		/* true = isolated */
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0) && \
    LINUX_VERSION_CODE <  KERNEL_VERSION(6, 12, 0)
	unsigned long (*k_reclaim_pages)(struct list_head *list, bool ignore_refs);
#else
	unsigned long (*k_reclaim_pages)(struct list_head *list);
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
	unsigned long (*k_try_to_free_mem_cgroup_pages)(struct mem_cgroup *memcg,
			unsigned long nr, gfp_t gfp, bool may_swap);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
	unsigned long (*k_try_to_free_mem_cgroup_pages)(struct mem_cgroup *memcg,
			unsigned long nr, gfp_t gfp, unsigned int opts);
#else
	unsigned long (*k_try_to_free_mem_cgroup_pages)(struct mem_cgroup *memcg,
			unsigned long nr, gfp_t gfp, unsigned int opts, int *swappiness);
#endif
} kraw;

static int kapi_alloc_contig_range_shim(unsigned long start, unsigned long end,
					gfp_t gfp_mask)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
	return kraw.k_alloc_contig_range(start, end, 0 /* ACR_FLAGS_NONE */, gfp_mask);
#else
	return kraw.k_alloc_contig_range(start, end, MIGRATE_MOVABLE, gfp_mask);
#endif
}

static bool kapi_folio_isolate_lru_shim(struct folio *folio)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
	return kraw.k_folio_isolate_lru(folio) == 0;
#else
	return kraw.k_folio_isolate_lru(folio);
#endif
}

static unsigned long kapi_reclaim_pages_shim(struct list_head *list)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0) && \
    LINUX_VERSION_CODE <  KERNEL_VERSION(6, 12, 0)
	return kraw.k_reclaim_pages(list, true);
#else
	return kraw.k_reclaim_pages(list);
#endif
}

static unsigned long kapi_try_to_free_memcg_shim(struct mem_cgroup *memcg,
					     unsigned long nr)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
	return kraw.k_try_to_free_mem_cgroup_pages(memcg, nr, GFP_KERNEL, true);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
	return kraw.k_try_to_free_mem_cgroup_pages(memcg, nr, GFP_KERNEL,
				      MEMCG_RECLAIM_MAY_SWAP);
#else
	return kraw.k_try_to_free_mem_cgroup_pages(memcg, nr, GFP_KERNEL,
				      MEMCG_RECLAIM_MAY_SWAP, NULL);
#endif
}

/*
 * ABI guard (boot-time): a comma/space-separated list of kapi logical symbol
 * names that the userspace preflight (abi/kapi_check, via /sys/kernel/btf) found
 * signature-INCOMPATIBLE on THIS running kernel. kapi_init leaves each listed
 * symbol NULL - resolved-but-disabled - so the feature that needs it is refused
 * by its kapi_can_* checklist (-ENOSYS) instead of being called through a
 * mistyped pointer (a kCFI type-id mismatch = panic). Empty (the normal case)
 * trusts the compile-time version gates; the guard only catches a signature the
 * gates did not anticipate (vendor divergence, an unforeseen future kernel).
 * Names are the canonical kernel symbols, e.g. "folio_isolate_lru,reclaim_pages"
 * (the vendor hook's id is "android_vh_free_one_page_bypass"). See abi/kapi_abi.tsv.
 */
static char *disable_kapi;
module_param(disable_kapi, charp, 0400);
MODULE_PARM_DESC(disable_kapi,
	"ABI guard: comma/space list of kapi symbols to leave unresolved (from kapi_check)");

/* Whole-token (not substring) match of @name against the disable_kapi list, so
 * "reclaim_pages" never matches "reclaim_pages_something". */
static bool kapi_sym_disabled(const char *name)
{
	const char *p = disable_kapi;
	size_t nlen = strlen(name);

	if (!p)
		return false;
	while (*p) {
		size_t tlen;

		p += strspn(p, ", \t");		/* skip separators */
		tlen = strcspn(p, ", \t");	/* length of this token */
		if (tlen == nlen && !strncmp(p, name, nlen))
			return true;
		p += tlen;
	}
	return false;
}

/*
 * Capability checklists (option 1): required-symbol sets. A feature whose
 * checklist is incomplete is refused up front (acquire_set -> -ENOSYS) rather
 * than half-run, because for these symbols "degrade" == "does nothing".
 * kapi_has_sys_reclaim() is the one enhancer worth naming (option 2): id=2 works
 * without it, just migration-only.
 */
static inline bool kapi_can_v1(void)		/* id=1 */
{
	return kapi.k_alloc_contig_pages && kapi.k_prep_compound_page;
}
static inline bool kapi_can_sweep(void)		/* id=2 core */
{
	return kapi.k_alloc_contig_range && kapi.k_prep_compound_page;
}
static inline bool kapi_can_evict_b(void)	/* id=3 = sweep + B */
{
	return kapi_can_sweep() && kapi.k_folio_isolate_lru && kapi.k_reclaim_pages;
}
static inline bool kapi_has_sys_reclaim(void)	/* A: optional enhancer for id=2 */
{
	return kapi.k_try_to_free_mem_cgroup_pages && kapi.k_mem_cgroup_from_task;
}

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

/*
 * Hand out one pool page AND record its holder in a single step: a page never
 * leaves the pool without a served-table entry (pfn -> current holder @tgid), and
 * never gets an entry without leaving the pool. This is what keeps
 * pool_avail + served == pool_total intact - in particular we refuse to hand out
 * a page we cannot track (served table full), because an untracked served page
 * can never be reclaimed by the free hook (which matches by pfn) and would leak.
 * Returns NULL when the pool is empty or the table is full; the caller then just
 * leaves the original buddy page in place.
 *
 * Holds served_lock across the pool_pop() (nesting pool_lock inside). Safe: the
 * free path takes served_lock and pool_lock sequentially, never nested the other
 * way, and no pool_* path takes served_lock - so there is no ABBA ordering.
 */
static struct page *pool_serve(pid_t tgid)
{
	unsigned long flags;
	struct page *page;

	raw_spin_lock_irqsave(&served_lock, flags);
	if (served_free_head == SERVED_NULL) {	/* no room to track -> don't serve */
		served_overflow++;
		raw_spin_unlock_irqrestore(&served_lock, flags);
		return NULL;
	}
	page = pool_pop();
	if (page)
		served_add_locked(page_to_pfn(page), tgid);
	raw_spin_unlock_irqrestore(&served_lock, flags);
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
	ok = (idx < READ_ONCE(pool_size_max));
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
/*  Free-path reclaim: android_vh_free_one_page_bypass                */
/*                                                                    */
/*  A served pool page returns to the host when its VM frees it. That */
/*  free travels the folio path (__folio_put_large / free_unref_folios*/
/*  on memfd truncate at crosvm exit), which NEVER passes through     */
/*  __free_pages - so the old __free_pages kprobe recovered almost    */
/*  nothing and the pool bled out. Every order-9 page that reaches the*/
/*  buddy allocator, by ANY path (single, batch, isolate, pcp drain), */
/*  passes through __free_one_page, whose top carries this exported   */
/*  vendor hook (present + EXPORT_TRACEPOINT_SYMBOL_GPL on GKI 6.1 -  */
/*  6.18). Setting *bypass=true removes the page before it enters the */
/*  freelist (before account_freepages), so we recover it verbatim.  */
/* ================================================================== */

/*
 * Rebuild @head as a pool-grade order-9 compound: head refcount 1, structure
 * from prep_compound_page, tail refcounts 0 - indistinguishable from an
 * alloc_pages(__GFP_COMP) page for serving and for __free_pages on release.
 * The ONE copy of the refcount protocol (a kernel-version-sensitive sequence):
 * used by the free-hook take, both acquire paths and the orphan re-acquire.
 * Non-sleeping (callers may hold raw locks). Requires kapi.k_prep_compound_page.
 * Setting the head count to 1 is idempotent for callers whose head already
 * holds a reference (alloc_contig_pages returns refcount-1 pages).
 */
static void rebuild_order9_compound(struct page *head)
{
	int i;

	set_page_count(head, 1);
	kapi.k_prep_compound_page(head, PAGE_ORDER);
	for (i = 1; i < (1 << PAGE_ORDER); i++)
		set_page_count(head + i, 0);	/* tails: rebuild as 0 */
}

/*
 * Re-pool a page the free-path is about to hand to the buddy allocator.
 * free_pages_prepare has already run: the compound is torn down, refcount is
 * frozen at 0, page->mapping cleared. If the pool holds fewer than the CURRENT
 * target (pool_want) we rebuild the order-9 compound and stash it - all
 * non-sleeping, under pool_lock, so it is safe from the hook's atomic
 * (zone->lock held, IRQs off) context. Gating on pool_want at return time (not
 * on the pool_total snapshot a resize left behind) is what makes a shrink
 * reversible: pages lent out across a pool_want=0 soft-disable are recovered
 * normally once the target is raised again - there is no "disowned" state.
 * The caller only gets here for pfns it just removed from the served table,
 * so the only page an unrelated free can lose here is one whose pfn WE served
 * and whose earlier free the hook missed (a stale entry) - see the reconcile
 * scavenger, which retires such entries by physically re-acquiring them.
 * pool_total (what we hold) is raised along, keeping avail+served ==
 * pool_total and refill consistent. The page is only mutated once we have
 * committed to taking it, so a failed take leaves it pristine to continue
 * into the buddy allocator. Caller must set *bypass=true iff this returns
 * true. Requires kapi.k_prep_compound_page (checked by caller). pool_want is
 * clamped to pool_size_max (itself at most POOL_SIZE_MAX) at every write
 * site, so idx < pool_want also bounds the array index.
 */
static bool pool_take_frozen(struct page *page)
{
	unsigned long flags;
	int idx;
	bool ok;

	raw_spin_lock_irqsave(&pool_lock, flags);
	idx = atomic_read(&pool_count);
	ok = (idx < READ_ONCE(pool_want));
	if (ok) {
		rebuild_order9_compound(page);
		page->mapping = NULL;
		page_pool[idx] = page;
		atomic_set(&pool_count, idx + 1);
		if (idx + 1 > pool_total)
			WRITE_ONCE(pool_total, idx + 1);
	}
	raw_spin_unlock_irqrestore(&pool_lock, flags);
	return ok;
}

/* Cap on re-acquire attempts per scavenge pass: bounds the worker's runtime;
 * leftovers are retried on the next pass (drain worker or any reconcile). */
#define REACQUIRE_BATCH	64

/*
 * Physically recover served pages whose free the hook MISSED - a THP split
 * frees 512 order-0 pages the order-9 gate never sees, and a free that lands
 * while the hook is unregistered escapes the same way. Such an entry lingers
 * with its page back in buddy (page_count == 0); left alone it either rots
 * until a purge writes the page off, or worse, matches the pfn's NEXT order-9
 * free and annexes it from whoever owns the page by then. Since the escaped
 * fragments coalesce back in the freelists, a targeted alloc_contig_range on
 * exactly that window usually recovers the SAME physical block almost for
 * free - so retire stale entries by taking their page back, not by
 * forgetting it.
 *
 * Racing the free hook is safe: if the page is flowing through
 * __free_one_page while we run, the hook consumes the entry and the range is
 * not free - alloc_contig_range fails and we skip. If alloc_contig_range succeeds, the
 * whole window was in buddy, the hook cannot have matched, and the entry is
 * still ours to remove: served_del AFTER the successful grab, never before.
 * Process context only (alloc_contig_range sleeps; it also drains pcp internally,
 * so a fragment parked there does not fake a "reused" page).
 * Returns the number of pages re-pooled.
 */
static int served_reacquire_free_orphans(void)
{
	unsigned long cand[REACQUIRE_BATCH];
	unsigned long flags;
	int b, i, n = 0, got = 0;

	if (!kapi.k_alloc_contig_range || !kapi.k_prep_compound_page)
		return 0;
	if (READ_ONCE(served_count) == 0)
		return 0;

	/* Collect candidates under the lock; act on them outside it. */
	raw_spin_lock_irqsave(&served_lock, flags);
	for (b = 0; b < SERVED_MAX && n < REACQUIRE_BATCH; b++) {
		u16 nd;

		for (nd = served_bucket[b];
		     nd != SERVED_NULL && n < REACQUIRE_BATCH;
		     nd = served_nodes[nd].next) {
			unsigned long pfn = served_nodes[nd].pfn;
			struct page *pg = pfn_valid(pfn) ? pfn_to_page(pfn) : NULL;

			if (pg && page_count(pg) == 0)
				cand[n++] = pfn;
		}
	}
	raw_spin_unlock_irqrestore(&served_lock, flags);

	for (i = 0; i < n; i++) {
		unsigned long pfn = cand[i];
		struct page *head;

		if (kapi.k_alloc_contig_range(pfn, pfn + (1UL << PAGE_ORDER),
				      GFP_KERNEL | __GFP_NOWARN | __GFP_NORETRY))
			continue;	/* window no longer whole: entry stays */

		served_del(pfn);
		head = pfn_to_page(pfn);
		rebuild_order9_compound(head);
		if (atomic_read(&pool_count) < READ_ONCE(pool_want) &&
		    pool_push_grow(head)) {
			atomic_inc(&total_refilled);
			got++;
		} else {
			/* Target already met (shrink since the serve): the
			 * entry is retired either way; let the page go back
			 * whole - its order-9 free re-enters the hook but no
			 * longer matches anything. */
			__free_pages(head, PAGE_ORDER);
		}
		cond_resched();
	}
	if (got)
		pr_info("re-acquired %d hook-missed page(s), avail=%d served=%d\n",
			got, atomic_read(&pool_count), READ_ONCE(served_count));
	return got;
}

/* Reclaim-path forensic counters (reclaim_debug, 0444). Cheap enough to keep
 * in production: one atomic inc per order-9 free system-wide, a few more per
 * matched free. dbg_take_fail should stay 0 - a nonzero value means a served
 * page's free arrived while the pool had no room for it (see the pr_warn). */
static atomic_long_t dbg_o9_seen;	/* order-9 frees the hook inspected */
static atomic_long_t dbg_del_hit;	/* ... that matched the served table */
static atomic_long_t dbg_del_miss;	/* ... order-9, but not ours */
static atomic_long_t dbg_take_fail;	/* matched but pool_take_frozen refused */

static void gh_free_one_page_cb(void *data, struct page *page, struct zone *zone,
				int order, int migratetype, int fpi_flags,
				bool *bypass)
{
	unsigned long pfn;

	/* Fires for every buddy free system-wide: the cheap gates come first. */
	if (order != PAGE_ORDER || !page)
		return;
	atomic_long_inc(&dbg_o9_seen);
	if (READ_ONCE(served_count) == 0)	/* nothing lent out -> not ours */
		return;

	pfn = page_to_pfn(page);
	/*
	 * Only reclaim pages WE served to a VM (present in the served table).
	 * Never steal unrelated order-9 frees. served_del removes the entry
	 * (the page is leaving the VM either way); the pool_want check inside
	 * pool_take_frozen lets it fall through to buddy when the CURRENT
	 * target says we already hold enough (e.g. after a shrink).
	 */
	if (!served_del(pfn)) {
		atomic_long_inc(&dbg_del_miss);
		return;
	}
	atomic_long_inc(&dbg_del_hit);

	if (kapi.k_prep_compound_page && pool_take_frozen(page)) {
		*bypass = true;
		atomic_inc(&total_refilled);
	} else {
		atomic_long_inc(&dbg_take_fail);
		pr_warn_ratelimited("reclaim take FAILED pfn=%lx avail=%d want=%d total=%d\n",
				    pfn, atomic_read(&pool_count),
				    READ_ONCE(pool_want), pool_total);
	}
}

static int reclaim_debug_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "o9_seen=%ld\ndel_hit=%ld\ndel_miss=%ld\ntake_fail=%ld\n",
			  atomic_long_read(&dbg_o9_seen),
			  atomic_long_read(&dbg_del_hit),
			  atomic_long_read(&dbg_del_miss),
			  atomic_long_read(&dbg_take_fail));
}

static const struct kernel_param_ops reclaim_debug_ops = {
	.get = reclaim_debug_get,
};
module_param_cb(reclaim_debug, &reclaim_debug_ops, NULL, 0400);
MODULE_PARM_DESC(reclaim_debug, "Free-path reclaim forensic counters (read-only)");

/* The android_vh_free_one_page_bypass tracepoint, located by name at runtime. */
static struct tracepoint *free_one_page_tp;

static void find_free_one_page_tp(struct tracepoint *tp, void *priv)
{
	if (!free_one_page_tp &&
	    !strcmp(tp->name, "android_vh_free_one_page_bypass"))
		free_one_page_tp = tp;
}

/* Attach/detach the free-path reclaim hook. Callers hold hook_mutex (except the
 * single-threaded init/exit). Returns 0, or -ENODEV (prep_compound_page
 * unresolved) / -ENOENT (hook absent on this kernel) / register errno. */
static int free_hook_register(void)
{
	int ret;

	if (free_intercept_active)
		return 0;
	/* ABI guard: skip the hook if the preflight flagged the vendor tracepoint's
	 * TP_PROTO incompatible (the kernel would call our probe through a mistyped
	 * pointer = kCFI panic). Treated like an absent hook. */
	if (kapi_sym_disabled("android_vh_free_one_page_bypass"))
		return -ENOENT;
	if (!kapi.k_prep_compound_page)
		return -ENODEV;
	if (!free_one_page_tp) {
		for_each_kernel_tracepoint(find_free_one_page_tp, NULL);
		if (!free_one_page_tp)
			return -ENOENT;
	}
	ret = tracepoint_probe_register(free_one_page_tp,
					(void *)gh_free_one_page_cb, NULL);
	if (ret == 0)
		free_intercept_active = true;
	return ret;
}

static void free_hook_unregister(void)
{
	if (!free_intercept_active || !free_one_page_tp)
		return;
	tracepoint_probe_unregister(free_one_page_tp,
				    (void *)gh_free_one_page_cb, NULL);
	tracepoint_synchronize_unregister();	/* no callback in flight after this */
	free_intercept_active = false;
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

	/* Hand out + record the holder as one step: never serve a page we can't
	 * track (would leak), never track one we didn't hand out. */
	pool_page = pool_serve(current->tgid);
	if (!pool_page)
		return 0;

	orig = (struct page *)regs_return_value(regs);
	regs_set_return_value(regs, (unsigned long)pool_page);
	atomic_inc(&total_served);
	if (current->mm)
		vm_owner_served_inc(current->mm);
	pr_info_ratelimited("served page, %d left\n",
			    atomic_read(&pool_count));

	if (orig) {
		/*
		 * Free the buddy page the allocator originally returned. It is
		 * NOT in the served table (only pool_page is), so the free-path
		 * hook won't re-grab it - no guard needed. It correctly returns
		 * to the buddy allocator.
		 */
		__free_pages(orig, PAGE_ORDER);
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

	if (current->mm)
		vm_owner_add(current->mm);

	pr_info("VM creation detected (active=%d, comm=%s)\n",
		vm_active_total(), current->comm);
	return 0;
}

/*
 * Flush per-CPU page caches a few seconds after a VM destroy. Teardown frees
 * order-9 folios through free_unref_page, which can park a few of them in pcp
 * lists indefinitely on an idle system - they never reach __free_one_page, so
 * the reclaim hook never sees them and their served entries linger until a
 * post-grace reconcile writes them off as orphan_freed (a steady ~2-page loss
 * per killed VM, observed). Draining pushes them through free_pcppages_bulk ->
 * __free_one_page, where the hook recovers them like any other VM return.
 */
static void pcp_drain_worker(struct work_struct *work)
{
	if (kapi.k_drain_all_pages)
		kapi.k_drain_all_pages(NULL);	/* NULL: every populated zone */
	/*
	 * Then scavenge: any served entry whose page reads free AFTER the
	 * drain is a hook-missed free (e.g. a split THP freed as order-0s).
	 * Re-acquire it now, while the fragments are still coalesced in buddy
	 * and before anyone re-allocates from the window - this is the module-
	 * autonomous recovery path; reconcile only mops up the long tail.
	 */
	served_reacquire_free_orphans();
	pr_info("post-destroy scavenge done (avail=%d served=%d)\n",
		atomic_read(&pool_count), READ_ONCE(served_count));
}

/* ================================================================== */
/*  VM destruction detection                                          */
/* ================================================================== */

static int vm_destroy_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	/*
	 * Filter: only handle tracked VM owners. On the process-exit path
	 * (OOM kill, crash) current->mm is already NULL by the time the fd
	 * is released (exit_mm runs before exit_files), so accept mm==NULL
	 * whenever anyone is tracked - the sweep identifies which owner
	 * actually died via mm_users, not via current.
	 */
	if (current->mm && !vm_owner_contains(current->mm))
		return 0;
	if (!current->mm && atomic_read(&vm_owner_count) == 0)
		return 0;

	/* Drop one VM from the closing owner. current->mm identifies it for an
	 * explicit close by a live process; an mm-less release (process exit,
	 * delayed_fput from a kworker) falls back to the sole live owner. Only
	 * the ambiguous mm-less case (several live owners) is left entirely to
	 * the mm_users sweep - log it, because if that closer stays alive its
	 * vm_count stays inflated and its slot is pinned until it exits. */
	if (current->mm)
		vm_owner_vm_dec(current->mm);
	else if (!vm_owner_vm_dec_sole())
		pr_info_ratelimited("unattributed VM destroy (owners=%d)\n",
				    atomic_read(&vm_owner_count));

	/* Open the reconcile grace window: teardown frees are inbound. */
	WRITE_ONCE(last_destroy_jiffies, jiffies);

	/* And flush pcp + scavenge hook-missed frees once the free burst
	 * settles (the worker skips whatever its kernel can't do).
	 * mod_delayed_work: several fds released in quick succession (one
	 * kill = many gunyah fds) push the pass back. */
	if (kapi.k_drain_all_pages || kapi.k_alloc_contig_range)
		mod_delayed_work(system_wq, &pcp_drain_work,
				 msecs_to_jiffies(PCP_DRAIN_DELAY_MS));

	pr_info("VM destruction detected (active=%d, pool=%d/%d, comm=%s)\n",
		vm_active_total(), atomic_read(&pool_count), pool_total,
		current->comm);

	/*
	 * Schedule refill immediately - worker polls mm_users before allocating.
	 * When refill_enable=0 the old alloc-back path is off (measuring free-hook
	 * reclaim alone); we still sweep dead owners so tracking stays accurate.
	 */
	if (READ_ONCE(refill_enable) &&
	    atomic_read(&pool_count) < pool_total &&
	    atomic_cmpxchg(&refill_status, REFILL_IDLE, REFILL_WAITING) == REFILL_IDLE) {
		schedule_delayed_work(&refill_work, 0);
		pr_info("refill scheduled\n");
	} else {
		/* Refill disabled/full/already running - sweep dead owners now */
		schedule_work(&vm_owner_sweep_work);
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

/*
 * Sweep tracked owners, releasing only those that are gone: process dead
 * (mm_users == 0) or last VM closed (vm_count == 0). Owners with live VMs
 * are left untouched, so one VM dying never drops the tracking of the
 * others. Deferred to a worker because mmdrop may sleep.
 */
static void vm_owner_sweep_worker(struct work_struct *work)
{
	unsigned long flags;
	struct mm_struct *to_drop[VM_OWNER_MAX];
	int i, count, ndrop = 0;

	spin_lock_irqsave(&vm_owner_lock, flags);
	count = atomic_read(&vm_owner_count);
	for (i = 0; i < count; i++) {
		struct mm_struct *mm = vm_owners[i].mm;

		if (!mm)
			continue;
		if (vm_owners[i].vm_count > 0 &&
		    atomic_read(&mm->mm_users) > 0)
			continue;	/* live owner: keep tracking */
		to_drop[ndrop++] = mm;
		WRITE_ONCE(vm_owners[i].mm, NULL);
		vm_owners[i].tgid = 0;
		vm_owners[i].comm[0] = '\0';
		atomic_set(&vm_owners[i].served, 0);
		vm_owners[i].vm_count = 0;
	}
	/* Trim trailing free slots so count==0 again means "nobody tracked" */
	while (count > 0 && !vm_owners[count - 1].mm)
		count--;
	smp_store_release(&vm_owner_count.counter, count);
	spin_unlock_irqrestore(&vm_owner_lock, flags);

	for (i = 0; i < ndrop; i++) {
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

	/* Sweep owners whose process/VMs are gone - survivors keep tracking */
	schedule_work(&vm_owner_sweep_work);

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
module_param_cb(hook_enable, &hook_enable_ops, NULL, 0600);
MODULE_PARM_DESC(hook_enable, "Alloc-side serve hook (kretprobe): 1=activate 0=deactivate");

/* reclaim_enable: toggle the NEW free-path VM-return reclaim hook
 * (android_vh_free_one_page_bypass), separate from the alloc-side hook_enable.
 * 1=attach (default), 0=detach. Read reports the ACTUAL active state. */
static int reclaim_enable_set(const char *val, const struct kernel_param *kp)
{
	int enable, ret = 0;

	if (kstrtoint(val, 10, &enable))
		return -EINVAL;
	enable = !!enable;

	mutex_lock(&hook_mutex);
	WRITE_ONCE(reclaim_want, enable);
	if (READ_ONCE(pool_ready)) {		/* runtime: apply immediately */
		if (enable)
			ret = free_hook_register();
		else
			free_hook_unregister();
	}					/* else: module_init honors the want */
	mutex_unlock(&hook_mutex);
	return ret;
}

static int reclaim_enable_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", free_intercept_active ? 1 : 0);
}

static const struct kernel_param_ops reclaim_enable_ops = {
	.set = reclaim_enable_set,
	.get = reclaim_enable_get,
};
module_param_cb(reclaim_enable, &reclaim_enable_ops, NULL, 0600);
MODULE_PARM_DESC(reclaim_enable,
	"Free-path VM-return reclaim hook: 1=activate (default) 0=deactivate");

/* pool_avail: read-only */
static int pool_avail_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&pool_count));
}

static const struct kernel_param_ops pool_avail_ops = {
	.get = pool_avail_get,
};
module_param_cb(pool_avail, &pool_avail_ops, NULL, 0400);
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
	n += sysfs_emit_at(buf, n, "active_vms=%d\n", vm_active_total());
	n += sysfs_emit_at(buf, n, "acquire_active=%d\n", atomic_read(&acquire_running));
	n += sysfs_emit_at(buf, n, "acquire_mode=%d\n", READ_ONCE(acquire_mode));
	/* Free text (may contain spaces); refill_stat is '=' -split on the Java side
	 * (parseProp: split("=",2)), so spaces are safe - no separator escaping. */
	n += sysfs_emit_at(buf, n, "acquire_stop_reason=%s\n", READ_ONCE(acquire_stop_reason));
	n += sysfs_emit_at(buf, n, "refill_enable=%d\n", READ_ONCE(refill_enable));
	n += sysfs_emit_at(buf, n, "free_reclaim=%d\n", free_intercept_active ? 1 : 0);

	return n;
}

static const struct kernel_param_ops refill_stat_ops = {
	.get = refill_stat_get,
};
module_param_cb(refill_stat, &refill_stat_ops, NULL, 0400);
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
module_param_cb(vm_owners, &vm_owners_ops, NULL, 0400);
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
module_param_cb(served_summary, &served_summary_ops, NULL, 0400);
MODULE_PARM_DESC(served_summary,
	"Reconciled served-page table summary (read-only)");

/* manual_refill: write 1 to trigger manual refill */
static int manual_refill_set(const char *val, const struct kernel_param *kp)
{
	int trigger;

	if (kstrtoint(val, 10, &trigger) || trigger != 1)
		return -EINVAL;

	if (!READ_ONCE(refill_enable)) {
		pr_info("manual_refill ignored (refill_enable=0)\n");
		return -ENODEV;
	}

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
 *   shrink (new < reserve): free available pages immediately, best effort
 *                           toward the target (served pages can't be freed).
 *   grow   (new > reserve): raise the target only; new pages are NOT
 *                           allocated here - use acquire to fill.
 *   0 = soft disable: every available page is freed now. Served pages stay
 *   tracked; whether a returning page is re-pooled is decided by the free
 *   hook against the target AT RETURN TIME (pool_count < pool_want), so
 *   want=0 lets them drain to buddy, while raising the target back first
 *   makes them re-pool normally - a soft-disable is fully reversible.
 */
static void pool_do_resize(int newt)
{
	struct page *batch[32];
	int target_avail;

	/*
	 * Clamp to the RAM-derived cap. A target below the currently lent-out
	 * count is allowed and safe: shrink frees only pooled pages, and when the
	 * lent pages are later returned the free-hook re-pools just up to
	 * pool_want and lets the rest go to buddy - no overflow, no leak.
	 */
	if (newt < 0)
		newt = 0;
	if (newt > pool_size_max)
		newt = pool_size_max;

	WRITE_ONCE(pool_want, newt);

	/*
	 * The real reserve held is owned+traced = pool_count(avail) + served. GROW
	 * stops here: nothing to free. pool_total is not touched - the free hook
	 * (as served pages return) and acquire (as new pages land) raise it to
	 * match what we actually hold.
	 */
	if (atomic_read(&pool_count) + served_count <= newt)
		return;

	/*
	 * SHRINK: free available pages so owned+traced drops to newt. Served pages
	 * can't be freed (VMs hold them), so the floor is target_avail =
	 * max(0, newt - served). Park pool_total at that floor *first* so a
	 * concurrently-running refill worker (whose pool_push fills whenever
	 * pool_count < pool_total) can't re-add what we drain here; restore
	 * pool_total = newt afterwards. The free hook doesn't need this dance -
	 * it gates on pool_want (already = newt) and its pages can't collide with
	 * the drain: a drained pool page was never in the served table.
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
	if (v < 0)
		v = 0;
	if (v > pool_size_max)
		v = pool_size_max;

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
module_param_cb(pool_want, &pool_want_ops, NULL, 0600);
MODULE_PARM_DESC(pool_want,
	"Target pages (insmod + runtime): grow raises target, shrink frees now, 0 soft-disables");

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

/* Resolve the first name in a NULL-terminated list that exists (handles the
 * *_noprof rename dance). */
static void *resolve_first(const char * const *names)
{
	void *addr = NULL;
	int i;

	for (i = 0; names[i] && !addr; i++)
		addr = resolve_kfunc(names[i]);
	return addr;
}

/* resolve_kfunc(), but honor the disable_kapi ABI guard: a symbol the userspace
 * preflight flagged incompatible resolves to NULL, so it is never called. */
static void *resolve_gated(const char *name)
{
	if (kapi_sym_disabled(name)) {
		pr_info("kapi: %s left NULL (disable_kapi ABI guard)\n", name);
		return NULL;
	}
	return resolve_kfunc(name);
}

/* Warn on unknown disable_kapi tokens (typo guard). The known set is the
 * generated KAPI_SYMBOLS list, so it stays in lockstep with the ABI table. */
static void kapi_validate_disable(void)
{
	static const char * const known[] = {
#define X(n) #n,
		KAPI_SYMBOLS(X)
#undef X
	};
	const char *p = disable_kapi;

	if (!p)
		return;
	while (*p) {
		size_t tlen, i;
		bool hit = false;

		p += strspn(p, ", \t");
		tlen = strcspn(p, ", \t");
		if (!tlen)
			break;
		for (i = 0; i < ARRAY_SIZE(known); i++)
			if (strlen(known[i]) == tlen &&
			    !strncmp(p, known[i], tlen)) {
				hit = true;
				break;
			}
		if (!hit)
			pr_warn("kapi: disable_kapi token \"%.*s\" not a known symbol\n",
				(int)tlen, p);
		p += tlen;
	}
}

/* Populate the kapi object once at init. Missing (or guard-disabled) stay NULL. */
static void kapi_init(void)
{
	static const char * const acp[] = {	/* alloc_contig_pages (id=1) */
		"alloc_contig_pages_noprof", "alloc_contig_pages", NULL };
	static const char * const acr[] = {	/* alloc_contig_range (id=2/3) */
		"alloc_contig_range_noprof", "alloc_contig_range", NULL };

	kapi_validate_disable();

	/* alloc_contig_pages/alloc_contig_range resolve a *_noprof/base fallback pair, so gate
	 * on the logical (base) name the guard/preflight use, not the variant. */
	kapi.k_alloc_contig_pages		= kapi_sym_disabled("alloc_contig_pages") ?
						NULL : resolve_first(acp);
	kapi.k_prep_compound_page		= resolve_gated("prep_compound_page");
	kapi.k_lru_add_drain_all		= resolve_gated("lru_add_drain_all");
	kapi.k_drop_slab			= resolve_gated("drop_slab");
	kapi.k_drain_all_pages		= resolve_gated("drain_all_pages");
	kapi.k_mem_cgroup_from_task	= resolve_gated("mem_cgroup_from_task");

	/* Divergent-contract symbols: raw pointer into kraw, normalizing shim
	 * into kapi. Resolve failure/guard leaves both NULL, so the NULL checks
	 * and kapi_can_* checklists work unchanged on the kapi side. */
	kraw.k_alloc_contig_range		= kapi_sym_disabled("alloc_contig_range") ?
						NULL : resolve_first(acr);
	kapi.k_alloc_contig_range		= kraw.k_alloc_contig_range ?
						kapi_alloc_contig_range_shim : NULL;
	kraw.k_folio_isolate_lru		= resolve_gated("folio_isolate_lru");
	kapi.k_folio_isolate_lru		= kraw.k_folio_isolate_lru ?
						kapi_folio_isolate_lru_shim : NULL;
	kraw.k_reclaim_pages		= resolve_gated("reclaim_pages");
	kapi.k_reclaim_pages		= kraw.k_reclaim_pages ?
						kapi_reclaim_pages_shim : NULL;
	kraw.k_try_to_free_mem_cgroup_pages =
				resolve_gated("try_to_free_mem_cgroup_pages");
	kapi.k_try_to_free_mem_cgroup_pages = kraw.k_try_to_free_mem_cgroup_pages ?
						kapi_try_to_free_memcg_shim : NULL;

	if (disable_kapi && *disable_kapi)
		pr_info("kapi: ABI guard active, disable_kapi=\"%s\"\n", disable_kapi);
	pr_info("kapi: id1=%d id2=%d id3=%d A(sysreclaim)=%d drain=%d drop_slab=%d\n",
		kapi_can_v1(), kapi_can_sweep(), kapi_can_evict_b(),
		kapi_has_sys_reclaim(), !!kapi.k_lru_add_drain_all, !!kapi.k_drop_slab);
}

/*
 * Resolve the zone the smart acquire (id=2) sweeps. Prefer an existing pool
 * page; otherwise (empty pool) borrow the zone a GFP_KERNEL order-0 allocation
 * would use (ZONE_NORMAL, where order-9 movable memory lives). Process context
 * only. Returns NULL if it cannot be determined (id=2 then stays disabled).
 */
static struct zone *acq_resolve_zone(void)
{
	struct page *p;
	struct zone *z = NULL;

	if (atomic_read(&pool_count) > 0 && page_pool[0])
		return page_zone(page_pool[0]);

	p = alloc_pages(GFP_KERNEL | __GFP_NOWARN, 0);
	if (p) {
		z = page_zone(p);
		__free_pages(p, 0);
	}
	return z;
}

/*
 * Fast path shared by both acquire modes: grab a free order-9 block straight
 * from the buddy allocator (splitting a free order-10, plus one light
 * compaction pass). This is instant and needs no migration - crucial because
 * the migration allocators (alloc_contig_*) do NOT harvest buddy free blocks,
 * so without this the pool ignores whatever contiguous 2MB blocks are already
 * free. __GFP_NORETRY keeps it cheap: when buddy has no free order-9/10 it fails
 * fast and the caller falls back to migration. __GFP_COMP yields a ready compound
 * page (refcount 1) - no prep_compound_page needed.
 */
static bool acquire_grab_free(bool strong)
{
	/*
	 * strong=false: __GFP_NORETRY - cheap, grabs an already-free order-9/10
	 * (split), light effort. strong=true: __GFP_RETRY_MAYFAIL - runs
	 * compaction + reclaim to assemble a block from freed movable pages (the
	 * in-kernel equivalent of "compact_memory then alloc"). Never OOM-kills.
	 */
	gfp_t gfp = GFP_KERNEL | __GFP_COMP | __GFP_NOWARN |
		    (strong ? __GFP_RETRY_MAYFAIL : __GFP_NORETRY);
	struct page *p = alloc_pages(gfp, PAGE_ORDER);

	if (!p)
		return false;
	if (!pool_push_grow(p)) {
		__free_pages(p, PAGE_ORDER);
		return false;
	}
	atomic_inc(&total_refilled);
	return true;
}

/*
 * B: evict one pageblock-aligned 2MB window's own reclaimable folios to zram, so
 * the window frees up for alloc_contig without needing migration destinations.
 * Refcount protocol mirrors madvise(MADV_PAGEOUT): folio_try_get ->
 * kapi.k_folio_isolate_lru -> list_add -> folio_put; kapi.k_reclaim_pages then pages
 * them out and drops the isolation refs. A race just skips a folio or makes the later
 * alloc_contig return -EBUSY. Bounded to <=512 folios and yields every 64 pages,
 * so it cannot livelock the way system-wide reclaim can. Returns the number of
 * folios queued (>=0), or -1 if B is unavailable (folio API <5.16 or unresolved).
 */
static int reclaim_block_b(unsigned long start_pfn)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0)
	const unsigned long end = start_pfn + (1UL << PAGE_ORDER);
	unsigned long pfn = start_pfn;
	LIST_HEAD(folios);
	int queued = 0;

	if (!kapi.k_folio_isolate_lru || !kapi.k_reclaim_pages)
		return -1;

	while (pfn < end) {
		struct folio *folio;
		struct page *page;
		unsigned long fnr;

		if (!pfn_valid(pfn)) {
			pfn++;
			continue;
		}
		page = pfn_to_page(pfn);
		if (PageBuddy(page) || PageReserved(page)) {
			pfn++;
			continue;
		}
		folio = page_folio(page);
		fnr = folio_nr_pages(folio);
		if (!folio_test_lru(folio) || folio_test_unevictable(folio) ||
		    !folio_try_get(folio)) {
			pfn += fnr ? fnr : 1;
			continue;
		}
		fnr = folio_nr_pages(folio);		/* re-read under our ref */
		if (kapi.k_folio_isolate_lru(folio)) {
			list_add(&folio->lru, &folios);
			queued++;
		}
		folio_put(folio);
		pfn += fnr ? fnr : 1;
		if ((pfn & 0x3f) == 0)
			cond_resched();
	}
	if (!list_empty(&folios))
		kapi.k_reclaim_pages(&folios);		/* pages out; drops the isolation refs */
	return queued;
#else
	return -1;	/* folio API absent (<5.16): B unavailable */
#endif
}

/*
 * Reclaim ~nr pages from the kworker's (root) memcg, allowing swap to zram, so
 * compaction has room to assemble 2MB blocks. Bounded by nr. Returns pages
 * reclaimed, or 0 if unavailable. Keep nr SMALL per call and check headroom
 * before calling - driving the system to near-OOM makes reclaim livelock into
 * an RCU stall (see acquire_worker_v3).
 */
static unsigned long reclaim_system_a(unsigned long nr)
{
	struct mem_cgroup *memcg;

	if (!kapi_has_sys_reclaim())
		return 0;
	memcg = kapi.k_mem_cgroup_from_task(current);	/* kworker -> root memcg */
	if (!memcg)
		return 0;
	return kapi.k_try_to_free_mem_cgroup_pages(memcg, nr);
}

/*
 * acquire id=1 (original): fill the pool toward pool_want using
 * alloc_contig_pages(), which *migrates* movable pages to assemble 2MB blocks
 * even when the buddy allocator has none. Heavy (migration) but only ever run
 * from the GUI button, never at boot, so a worst-case stall/abort is
 * reboot-recoverable and can't bootloop.
 *
 * alloc_contig_pages returns nr_pages individual refcount-1 pages; turn the
 * range into one order-9 compound page (prep_compound_page sets structure,
 * then zero the tail refcounts) so it is indistinguishable from an
 * alloc_pages(__GFP_COMP) page for serving and for __free_pages on release.
 */
static void acquire_worker_v1(void)
{
	const int nr = 1 << PAGE_ORDER;
	const char *reason = NULL;
	int got = 0, fail_score = 0;

	if (!kapi_can_v1())
		return;

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
		struct page *p = kapi.k_alloc_contig_pages(nr,
				GFP_KERNEL | __GFP_NOWARN, numa_node_id(), NULL);

		if (!p) {
			fail_score++;
			msleep(ACQUIRE_DELAY_MS);
			continue;
		}
		fail_score -= ACQUIRE_FAIL_DECAY;
		if (fail_score < 0)
			fail_score = 0;

		rebuild_order9_compound(p);

		if (!pool_push_grow(p)) {
			__free_pages(p, PAGE_ORDER);
			reason = "pool capacity full";
			break;			/* pool array full */
		}
		atomic_inc(&total_refilled);
		got++;
		msleep(ACQUIRE_DELAY_MS);	/* let the system breathe */
	}

	/* Why the loop ended, for the GUI (see acquire_stop_reason). */
	if (!reason) {
		if (!atomic_read(&acquire_running))
			reason = "stopped by user";
		else if (atomic_read(&pool_count) + served_count >= READ_ONCE(pool_want))
			reason = "reached target";
		else
			reason = "migration exhausted";	/* fail_score hit the cap */
	}
	WRITE_ONCE(acquire_stop_reason, reason);

	pr_info("acquire(v1) done: +%d pages, avail=%d capacity=%d want=%d\n",
		got, atomic_read(&pool_count), pool_total, READ_ONCE(pool_want));
}

/*
 * Feasibility gate for one pageblock-aligned 2MB window - the authoritative
 * "will this assemble" test, run BEFORE any migration/eviction so a doomed
 * window is skipped rather than white-kicked. A window is assemblable iff every
 * page is free, on the LRU (anon/file: migratable & B-reclaimable), or
 * __PageMovable (driver-movable), and none is reserved or longterm-pinned.
 *
 * Migratetype is NOT a gate anymore: Unmovable/Reclaimable pageblocks are tried
 * too - many hold no actual straggler, just the label, and that ~7.6GB is where
 * the theoretical headroom lives. Only CMA (FOLL_LONGTERM trap), isolated, and
 * highatomic blocks are refused outright. The per-page straggler test decides
 * the rest, and it also naturally rejects our OWN pool pages (in-use, not LRU,
 * not movable) so the sweep never re-grabs the reserve. compound_head resolves
 * THP tails to the head. REQUIRES a prior lru_add_drain_all() so pagevec-resident
 * folios read as LRU; a rare transient false positive just skips a good window.
 */
static bool block_candidate(struct zone *z, unsigned long pfn)
{
	const unsigned long end = pfn + (1UL << PAGE_ORDER);
	struct page *first;
	unsigned long i;
	int mt;

	if (!pfn_valid(pfn))
		return false;
	first = pfn_to_page(pfn);
	if (page_zone(first) != z)
		return false;
	mt = get_pageblock_migratetype(first);
	if (is_migrate_cma(mt) || mt == MIGRATE_HIGHATOMIC
#ifdef CONFIG_MEMORY_ISOLATION
	    || mt == MIGRATE_ISOLATE
#endif
	   )
		return false;			/* CMA/isolated/atomic: never touch */

	for (i = pfn; i < end; i++) {
		struct page *p, *head;

		if (!pfn_valid(i))
			return false;		/* hole: can't assemble across it */
		p = pfn_to_page(i);
		head = compound_head(p);
		if (PageReserved(head))
			return false;		/* bootmem/firmware */
		if (page_count(head) == 0)
			continue;		/* free: buddy / pcp / being freed */
		if (!PageLRU(head) && !__PageMovable(head))
			return false;		/* in-use + not LRU/movable = straggler */
		if (
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
		    folio_maybe_dma_pinned(page_folio(head)))
#else
		    page_maybe_dma_pinned(head))
#endif
			return false;		/* longterm-pinned (e.g. a live VM) */
	}
	return true;
}

/* Keep at least this much memory available (MB) for the rest of the system.
 * Reclaiming past the floor is what livelocks into an RCU stall -> panic/reboot:
 * the freed pages are instantly re-faulted, every CPU spins in reclaim, and 21 s
 * without a quiescent state trips the watchdog. Staying above the floor keeps
 * reclaim making forward progress, so worst case is "gave up early", not a hang.
 * Tunable: raise it if the sweep leaves the system too tight; the historical
 * safe value was 1024. Read fresh each window, so a live write takes effect
 * mid-sweep. */
static unsigned int acquire_mem_floor_mb = 512;
module_param(acquire_mem_floor_mb, uint, 0600);
MODULE_PARM_DESC(acquire_mem_floor_mb,
	"id=2/3 sweep stops when si_mem_available() drops below this many MB (default 512)");

#define ACQUIRE_MEM_FLOOR_PAGES \
	((unsigned long)READ_ONCE(acquire_mem_floor_mb) << (20 - PAGE_SHIFT))
#define ACQUIRE_A_STRIDE	8	/* A: system reclaim once per this many failed windows */

/* Reclaim strategy the shared sweep uses when migration alone can't assemble a
 * window: B evicts that window's OWN folios (targeted, needs folio API 5.16+);
 * A does a strided bounded system-wide reclaim to create migration headroom
 * (coarse, but works wherever try_to_free_mem_cgroup_pages resolves). */
enum acquire_assist { ASSIST_B, ASSIST_A };

/* Drop reclaimable slab (dentry/inode) once at sweep start. That slab is NOT on
 * the LRU, so neither B nor A can reclaim it, yet a single dentry page poisons a
 * whole window (block_candidate sees a straggler). Shrinking it up front unpoisons
 * those windows so they become assemblable - the module doing what a userspace
 * `echo 2 > drop_caches` does. Global side-effect (filesystem re-lookups are
 * slower afterwards), so it is toggleable; default on. */
static int acquire_drop_slab = 1;
module_param(acquire_drop_slab, int, 0600);
MODULE_PARM_DESC(acquire_drop_slab,
	"id=2/3: drop reclaimable slab at sweep start to unpoison windows (default 1)");

/*
 * The shared acquire sweep (id=2 uses ASSIST_A, id=3 uses ASSIST_B): one FULL
 * sweep of the zone from the persistent cursor, wrapping once. Per window: try
 * cheap ASYNC migration; if that fails and the window has no unmovable
 * straggler, apply the reclaim assist and retry SYNC.
 *   ASSIST_B: evict the window's OWN folios to zram (reclaim_block_b) - it then
 *             frees in place, no migration destinations needed. Bounded to one
 *             2MB window; never strips the system.
 *   ASSIST_A: strided, bounded system-wide reclaim (reclaim_system_a) to free
 *             migration destinations elsewhere; the SYNC retry migrates the
 *             window into them. One batch every ACQUIRE_A_STRIDE failed windows.
 *
 * No failure cap: a window that still won't assemble (transient contention, an
 * undetectable pin) is passed over - the sweep keeps going. SPARSEMEM holes in
 * the zone span are skipped a whole section at a time, so both the runtime and
 * the reported scan count track real present memory, not the (much larger) span.
 * It exits only when (a) the whole zone span has been traversed once, (b)
 * pool_want is met, (c) the user
 * writes acquire=0, or (d) the memory floor is hit (the sole safety brake -
 * never reclaim toward OOM; this is what keeps A from the RCU-stall reboot the
 * earlier unbounded A-loop hit). The cursor is persistent and advanced every
 * window, so a user-/floor-stopped partial sweep resumes where it left off.
 * lru_add_drain_all() up front makes PageLRU (hence the straggler test) accurate.
 */
static void acquire_sweep(enum acquire_assist assist)
{
	const unsigned long nr = 1UL << PAGE_ORDER;
	struct zone *z = acq_zone;
	unsigned long zstart, zend, pfn, span_pages, present_blocks;
	unsigned long examined = 0, advanced = 0, evicted = 0, since_a = 0;
	const char *reason = NULL;
	int got = 0, i;
	bool floor_hit = false;

	if (!z || !kapi_can_sweep())
		return;

	zstart = ALIGN(z->zone_start_pfn, nr);
	zend = zone_end_pfn(z);
	if (zend <= zstart)
		return;
	if (scan_cursor < zstart || scan_cursor >= zend)
		scan_cursor = zstart;
	span_pages = zend - zstart;			/* full span, holes included */
	/* Honest denominator: real present memory in 2MB blocks (e.g. 16G RAM,
	 * not the ~48G span). Scanning/reporting against the span reads as a
	 * falsely-high "scanned 48G" when two thirds of it are holes. */
	present_blocks = READ_ONCE(z->present_pages) >> PAGE_ORDER;
	if (!present_blocks)
		present_blocks = 1;

	if (kapi.k_lru_add_drain_all)
		kapi.k_lru_add_drain_all();		/* make PageLRU accurate for the straggler test */

	while (atomic_read(&acquire_running) &&
	       atomic_read(&pool_count) + served_count < READ_ONCE(pool_want)) {
		struct page *head;
		int ret;

		cond_resched();

		if (advanced >= span_pages) {
			reason = "scanned all present memory";
			break;			/* traversed the whole span once: done */
		}
		pfn = scan_cursor;

#ifdef CONFIG_SPARSEMEM
		/* Skip SPARSEMEM holes a whole section at a time. A zone's span
		 * (zone_start_pfn..zone_end_pfn) can include large absent sections
		 * (non-contiguous DRAM, vendor carveouts); an absent section has NO
		 * valid pfn, so stepping one 2MB window at a time would burn up to
		 * PAGES_PER_SECTION/nr iterations per section on block_candidate's
		 * pfn_valid() failure for nothing. Jump the cursor to the next section
		 * and count the gap as span *advanced* only - NOT as an *examined*
		 * window - so both the runtime and the reported scan count reflect
		 * real present memory. (present_pages only *counts* present memory; it
		 * can't say WHERE the holes are, so pfn_valid drives the skip.) */
		if (!pfn_valid(pfn)) {
			unsigned long next = ALIGN(pfn + 1, PAGES_PER_SECTION);

			advanced += next - pfn;
			scan_cursor = (next >= zend) ? zstart : next;
			continue;
		}
#endif
		examined++;			/* present windows actually looked at */
		advanced += nr;
		scan_cursor = pfn + nr;		/* advance + persist BEFORE work so a */
		if (scan_cursor >= zend)	/* stop here resumes at the next window */
			scan_cursor = zstart;

		/* block_candidate is now the full feasibility gate: a window that
		 * would white-kick (straggler / reserved / pinned) is rejected here,
		 * BEFORE any migration or eviction - so we never evict for nothing.
		 * It also skips our own pool pages. */
		if (!block_candidate(z, pfn))
			continue;

		/* Cheap first: ASYNC migration (window may be mostly free already). */
		ret = kapi.k_alloc_contig_range(pfn, pfn + nr,
			GFP_KERNEL | __GFP_NOWARN | __GFP_NORETRY);

		if (ret != 0) {
			/* Straggler-free but occupied. The only brake: never reclaim
			 * the system toward OOM. */
			if (si_mem_available() < ACQUIRE_MEM_FLOOR_PAGES + nr) {
				floor_hit = true;
				reason = "low-memory floor";
				break;
			}
			if (assist == ASSIST_B) {
				/* Evict this window's own folios, then it frees in place. */
				i = reclaim_block_b(pfn);
				if (i < 0) {
					reason = "evict-B unavailable";
					break;		/* B unavailable (use id=2/A instead) */
				}
				evicted += (unsigned long)i;
			} else {		/* ASSIST_A */
				/* Strided system-wide reclaim -> migration headroom that
				 * this and the next few windows migrate into. */
				if (++since_a >= ACQUIRE_A_STRIDE) {
					since_a = 0;
					evicted += reclaim_system_a(nr * 8);
				}
			}
			ret = kapi.k_alloc_contig_range(pfn, pfn + nr,
				GFP_KERNEL | __GFP_NOWARN);	/* SYNC retry */
		}
		msleep(ACQUIRE_DELAY_MS);
		if (ret != 0)
			continue;			/* transient/pinned: pass over it */

		head = pfn_to_page(pfn);
		rebuild_order9_compound(head);
		if (!pool_push_grow(head)) {
			__free_pages(head, PAGE_ORDER);
			reason = "pool capacity full";
			break;
		}
		atomic_inc(&total_refilled);
		got++;
	}

	/* No break took a specific reason -> the while-condition ended it: the user
	 * interrupted (acquire=0) or the target was reached. Surface it for the GUI. */
	if (!reason)
		reason = atomic_read(&acquire_running) ? "reached target"
						       : "stopped by user";
	WRITE_ONCE(acquire_stop_reason, reason);

	pr_info("acquire(%s) done: +%d pages, avail=%d want=%d evicted=%luMB scanned=%lu/%lu blocks (present=%luMB span=%luMB) floor_hit=%d\n",
		assist == ASSIST_B ? "id3/B" : "id2/A", got, atomic_read(&pool_count),
		READ_ONCE(pool_want), evicted >> (20 - PAGE_SHIFT),
		examined, present_blocks,
		(present_blocks << PAGE_ORDER) >> (20 - PAGE_SHIFT),
		span_pages >> (20 - PAGE_SHIFT), floor_hit);
}

static void acquire_worker(struct work_struct *w)
{
	/*
	 * Phase 0 (all modes): drop reclaimable slab (dentry/inode) once. That slab
	 * is not on the LRU so no acquire path can reclaim it, yet a single dentry
	 * page poisons a whole window - freeing it up front unpoisons windows for
	 * every mode (including v1's alloc_contig_pages and the Phase 1 fast-path).
	 */
	if (READ_ONCE(acquire_drop_slab) && kapi.k_drop_slab)
		kapi.k_drop_slab();

	/*
	 * Phase 1 (all modes): harvest already-free order-9/10 blocks from buddy
	 * - instant, no migration. Drains what's cheaply available (now including
	 * blocks the slab drop just freed) before paying for migration in Phase 2.
	 */
	while (atomic_read(&acquire_running) &&
	       atomic_read(&pool_count) + served_count < READ_ONCE(pool_want)) {
		if (!acquire_grab_free(false))
			break;
		cond_resched();
	}

	/* Phase 2: migration for the fragmented remainder, plus a reclaim assist:
	 * id=3 = per-block evict (B), id=2 = system-wide reclaim (A), id=1 = old. */
	if (READ_ONCE(acquire_mode) == 3)
		acquire_sweep(ASSIST_B);	/* per-block evict to zram */
	else if (READ_ONCE(acquire_mode) == 2)
		acquire_sweep(ASSIST_A);	/* strided system-wide reclaim */
	else
		acquire_worker_v1();
	atomic_set(&acquire_running, 0);
}

/*
 * acquire: 0 = stop the running worker, 1 = original acquire (alloc_contig_pages
 * blind scan), 2 = shared sweep with system-wide reclaim assist (A), 3 = shared
 * sweep with per-block evict assist (B). 2 and 3 run the same acquire_sweep()
 * skeleton, differing only in how they free space when migration alone fails.
 */
static int acquire_set(const char *val, const struct kernel_param *kp)
{
	int v;

	if (kstrtoint(val, 10, &v) || v < 0 || v > 3)
		return -EINVAL;
	if (v == 0) {
		/* Interrupt: the worker exits at its next check; pool_want stays. */
		atomic_set(&acquire_running, 0);
		return 0;
	}
	/* Required-symbol checklists (option 1): a mode whose required set is
	 * incomplete is refused here rather than half-run. id=3 requires B on top of
	 * the sweep - without it id=3 would just be id=2, so point the user at id=2. */
	if (v == 1 && !kapi_can_v1())
		return -ENOSYS;
	if (v == 2 && (!kapi_can_sweep() || !acq_zone))
		return -ENOSYS;
	if (v == 3 && (!kapi_can_evict_b() || !acq_zone))
		return -ENOSYS;
	/* Nothing to do if the reserve (available + served) already meets want. */
	if (atomic_read(&pool_count) + served_count >= READ_ONCE(pool_want)) {
		WRITE_ONCE(acquire_stop_reason, "already at target");
		return 0;
	}
	if (atomic_cmpxchg(&acquire_running, 0, 1) != 0)
		return -EBUSY;
	WRITE_ONCE(acquire_mode, v);
	WRITE_ONCE(acquire_stop_reason, "acquiring");
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
	"0=stop, 1=alloc_contig_pages, 2=sweep+system-reclaim(A), 3=sweep+per-block-evict(B)");

/* ================================================================== */
/*  Module init / exit                                                */
/* ================================================================== */

static int __init hugepage_reserve_init(void)
{
	int i, ret;

	/*
	 * Effective cap: keep min(ram/2, SYSTEM_RESERVE) away from the pool -
	 * half the RAM on small systems, SYSTEM_RESERVE on big ones - and offer
	 * the rest, up to the POOL_SIZE_MAX_RAM array bound. Computed before any
	 * pool_want clamp below and before pool_ready, so every later write site
	 * sees the final value.
	 */
	{
		unsigned long ram = totalram_pages() << PAGE_SHIFT;	/* bytes */
		unsigned long keep = min(ram / 2, SYSTEM_RESERVE);
		unsigned long pool_size_max_ram = min(ram - keep, POOL_SIZE_MAX_RAM);

		pool_size_max = (int)(pool_size_max_ram >> (PAGE_SHIFT + PAGE_ORDER));
	}

	if (pool_want < 0)
		pool_want = 1024;
	if (pool_want > pool_size_max)
		pool_want = pool_size_max;

	if (refill_delay_ms < 1000)
		refill_delay_ms = 1000;

	/* "Grace already expired": jiffies wraps from INITIAL_JIFFIES, so a zero
	 * init would leave reconcile in no-purge mode for half the wrap period. */
	last_destroy_jiffies = jiffies - msecs_to_jiffies(RECONCILE_GRACE_MS) - 1;

	pr_info("allocating up to %d x 2MB = %d MB (limit %d x 2MB = %d MB)\n",
		pool_want, pool_want * 2, pool_size_max, pool_size_max * 2);

	INIT_WORK(&vm_owner_sweep_work, vm_owner_sweep_worker);
	INIT_DELAYED_WORK(&refill_work, refill_worker);
	INIT_DELAYED_WORK(&pcp_drain_work, pcp_drain_worker);
	INIT_WORK(&acquire_work, acquire_worker);
	served_init();

	/* Resolve every optional kernel symbol into the kapi object; a miss only
	 * disables/degrades a feature, never fails module load. */
	kapi_init();

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

	/*
	 * An empty pool is a valid load state (boot-time memory too fragmented,
	 * or pool_want=0 soft-disable): the hooks stay armed but idle while
	 * pool_count is 0, and capacity grows later via acquire (or a pool_want
	 * write followed by acquire).
	 */
	if (pool_total == 0 && pool_want > 0)
		pr_warn("started empty; grow via acquire toward target %d\n",
			pool_want);

	pr_info("pool ready: %d x 2MB = %d MB (target %d)\n",
		pool_total, pool_total * 2, pool_want);

	/* Cache the zone the smart acquire (id=2) sweeps. */
	acq_zone = acq_resolve_zone();
	if (acq_zone)
		scan_cursor = ALIGN(acq_zone->zone_start_pfn, 1UL << PAGE_ORDER);

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

	/*
	 * Register the free-path vendor hook for VM-shutdown reclaim. Catches
	 * every order-9 page reaching the buddy allocator (the folio/batch paths
	 * the old __free_pages kprobe missed), so served pages a VM frees are
	 * recovered into the pool instead of leaking to buddy and fragmenting.
	 */
	if (!READ_ONCE(reclaim_want)) {
		pr_info("free-path reclaim disabled by reclaim_enable=0\n");
	} else {
		ret = free_hook_register();
		if (ret == 0)
			pr_info("free-path reclaim active (android_vh_free_one_page_bypass)\n");
		else if (ret == -ENODEV)
			pr_warn("prep_compound_page unresolved, free-path reclaim disabled\n");
		else if (ret == -ENOENT)
			pr_warn("android_vh_free_one_page_bypass absent on this kernel, free-path reclaim disabled\n");
		else
			pr_warn("free_one_page hook registration failed (ret=%d), VM-return pages will leak to buddy\n",
				ret);
	}

	return 0;
}

static void __exit hugepage_reserve_exit(void)
{
	int i, remaining;

	/* Stop live resizes racing with teardown */
	WRITE_ONCE(pool_ready, false);

	/*
	 * Signal any in-flight acquire loop to stop before waiting for it: both
	 * acquire workers re-check acquire_running every iteration, so this bounds
	 * the cancel_work_sync wait to the current window instead of a whole
	 * zone sweep (v2 has no fail-score give-up).
	 */
	atomic_set(&acquire_running, 0);

	/*
	 * Remove all probes FIRST, cancel works second. The create/destroy
	 * kprobes queue works (refill, owner sweep, pcp drain), so cancelling
	 * works while the probes are still armed lets a VM event in the gap
	 * re-queue an already-cancelled work - which would then run after this
	 * function freed the pool and dropped the owner mms (use-after-free).
	 * unregister_kprobe/tracepoint_synchronize_unregister wait for in-
	 * flight handlers, so after this block no new work can appear.
	 */
	mutex_lock(&hook_mutex);
	if (hook_active) {
		unregister_kretprobe(&kretp);
		hook_active = false;
	}
	free_hook_unregister();		/* under hook_mutex, like the toggle */
	mutex_unlock(&hook_mutex);

	if (detect_active)
		unregister_kprobe(&vm_detect_kp);
	if (vm_destroy_active)
		unregister_kprobe(&vm_destroy_kp);

	/*
	 * Cancel works in dependency order: refill_worker's tail schedules
	 * vm_owner_sweep_work, so the sweep must be cancelled AFTER refill has
	 * been flushed - cancelling it first lets an in-flight refill re-arm
	 * it behind its own cancel, with the same use-after-free result.
	 */
	cancel_work_sync(&acquire_work);
	cancel_delayed_work_sync(&refill_work);
	cancel_delayed_work_sync(&pcp_drain_work);
	cancel_work_sync(&vm_owner_sweep_work);

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
MODULE_AUTHOR("HuJK");
MODULE_DESCRIPTION("Pre-allocate pages for Gunyah with VM lifecycle-aware pool recycling");
