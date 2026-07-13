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
#include <linux/vmstat.h>
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
/* Highest order alloc_pages accepts on this kernel: MAX_ORDER was an EXCLUSIVE
 * bound until 6.4 (23baf831a32c made it inclusive), and 6.8 renamed it
 * MAX_PAGE_ORDER. Used by the CMA first-block verification span, which wants
 * one allocation of pageblock_order + 1 when the kernel allows it. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
#define GH_MAX_ALLOC_ORDER	MAX_PAGE_ORDER
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
#define GH_MAX_ALLOC_ORDER	MAX_ORDER
#else
#define GH_MAX_ALLOC_ORDER	(MAX_ORDER - 1)
#endif
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

/* ---- CMA reservoir parameters (v10) ----
 *
 * The reservoir is the elastic half of the dual-pool model: pageblocks we
 * labeled MIGRATE_CMA whose pages sit FREE in buddy. While a VM is down, app
 * movable allocations use them like ordinary memory; unmovable allocations can
 * never enter a CMA block, so when a VM is about to start, a targeted CMA-mode
 * alloc_contig_range reassembles each block into 2MB pages in seconds instead
 * of fighting the fragmentation wall (measured: 8GB in ~10s vs a sweep stalling
 * at ~30%). pool_want_with_cma=0 (default) keeps all of this off: v9 behavior.
 *
 * Both preflight values below come from userspace (post-fs-data.sh) because the
 * running kernel exposes neither: MIGRATE_CMA's value is config/vendor-dependent
 * (read from /sys/kernel/btf/vmlinux by kapi_check), and pageblock_order is a
 * pure macro - no kernel variable, absent from kallsyms and BTF (read from
 * /proc/pagetypeinfo "Page block order"). Either missing -> feature off.
 */

static int migrate_cma_val = -1;
module_param(migrate_cma_val, int, 0400);
MODULE_PARM_DESC(migrate_cma_val,
	"MIGRATE_CMA enumerator value on the running kernel (from kapi_check BTF preflight); -1 = CMA reservoir off");

static int pageblock_order_val = -1;
module_param(pageblock_order_val, int, 0400);
MODULE_PARM_DESC(pageblock_order_val,
	"pageblock_order of the running kernel (from /proc/pagetypeinfo preflight); -1 = CMA reservoir off");

/*
 * Headroom floor for FLIPPING blocks to CMA: every flip moves a pageblock from
 * the everyone-budget to the movable-only budget, and the kernel's unmovable
 * working set (~3.5G resident on these devices) cannot follow it there.
 * Flip too far and unmovable allocations starve while "MemAvailable" still looks
 * fine. So: refuse a flip once (si_mem_available - free CMA pages) would drop
 * under this. This is a DIFFERENT brake from acquire_mem_floor_mb (which stops
 * the sweep before reclaim livelocks); both apply, each guarding its own path.
 */
static unsigned int cma_reservoir_floor_mb = 1024;
module_param(cma_reservoir_floor_mb, uint, 0600);
MODULE_PARM_DESC(cma_reservoir_floor_mb,
	"Refuse to flip more pageblocks to CMA when non-CMA available memory would drop below this many MB (default 1024)");

/*
 * ---- moveable_to_cma: two userspace levers + one status ----
 *
 * Plain MOVABLE allocations that carry NO __GFP_CMA - page cache and mTHP anon,
 * the app's actual working set - cannot reach the CMA reservoir under a stock
 * restrict_cma_redirect=true kernel, so the reservoir just sits there unconsumed.
 * Two independent levers open that path; a status reports when the vendor kernel
 * already opens it globally (making both levers redundant). Full ABI in the
 * sysfs block near pool_want_with_cma.
 */

/* Free-system-CMA floor (MB) below which the gfp_cma_hook stops granting
 * ALLOC_CMA to non-__GFP_CMA movable allocations - an absolute brake so the
 * bypass can never drain CMA to exhaustion. */
static unsigned int cma_bypass_floor_mb = 256;
module_param(cma_bypass_floor_mb, uint, 0600);
MODULE_PARM_DESC(cma_bypass_floor_mb,
	"Free system CMA floor (MB) below which gfp_cma_hook stops granting ALLOC_CMA to non-anon movable allocations (default 256)");

/* Live arm state of the gfp bypass hook, driven by the moveable_to_cma_gfp_cma_hook
 * knob. Soft flag: the tracepoint probe stays registered; cma_bypass_wants()
 * bails on its first line when this is false (a disabled no-op probe writes
 * nothing, so it never perturbs any other probe on the same shared vendor hook). */
static bool cma_bypass_enabled;

/* Settled ONCE in cma_boot_build(): 1 iff restrict_cma_redirect resolved AND is
 * currently false (vendor already redirects all movable -> CMA), else 0. When 1,
 * writes to BOTH levers below are accepted-but-no-op (nothing to do). */
static int mtc_vender_already_allowed;

/* insmod-time desires for the two levers, applied by cma_boot_build() once
 * cma_capable + mtc_vender_already_allowed are known (runtime writes apply
 * immediately instead). restrict: -1 = untouched, 0/1 = desired knob value
 * (1 = redirect enabled / restrict disabled, i.e. movable CAN migrate into CMA).
 * hook: 0/1 arm request. */
static int mtc_restrict_want = -1;
static int mtc_gfp_hook_want;

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
static int pool_want_with_cma;	/* v10 TOTAL guardianship target in 2MB pages:
				 * held pool (pool_want of it) + CMA reservoir
				 * (the rest). 0 (default) = reservoir feature
				 * off, exact v9 behavior. Invariant everywhere:
				 * pool_want <= pool_want_with_cma <= pool_size_max
				 * (0 is the disable sentinel, exempt). Set at
				 * insmod and at runtime via its sysfs. */
static int pool_size_max = POOL_SIZE_MAX;	/* effective cap on pool_want/capacity,
						 * computed from system RAM at insmod
						 * (see hugepage_reserve_init); every
						 * pool_want write site clamps to it. */
module_param(pool_size_max, int, 0400);
MODULE_PARM_DESC(pool_size_max,
	"Effective pool cap in 2MB pages: min(ram - min(ram/2, 6G), 24G); read-only");
static bool pool_ready;		/* true once module_init has built the pool */
static DEFINE_RAW_SPINLOCK(pool_lock);

/* ---- CMA reservoir state (v10) ----
 *
 * cma_blocks[] is the ONE persistent piece of reservoir state: the base pfn of
 * every pageblock that is currently ours-and-CMA. Everything else about the
 * feature is either a parameter, derived on demand, or rebuildable - no origin
 * tracking, no history masks. stage-in removes an entry, flip/build adds one,
 * write-small demolition and exit restore-and-remove; that is the full
 * lifecycle. Each entry is >= 2MB, so the count is bounded by POOL_SIZE_MAX.
 *
 * All mutation happens in process context under cma_mutex (build, demolition,
 * verification, exit). Readers that only want the count (stats) read
 * cma_blocks_n unlocked. The free hook NEVER touches this state: migratetype
 * flips and frees of reservoir pages are process-context-only by design (S9
 * invariant), so the hook's atomic context stays exactly as cheap as v9.
 */
static unsigned long cma_blocks[POOL_SIZE_MAX];
static int cma_blocks_n;
static u16 cma_est[POOL_SIZE_MAX];	/* per-block free-page estimates, parallel
					 * to cma_blocks[]; scratch for demolition
					 * and stage-in ordering (cma_mutex) */
static DEFINE_MUTEX(cma_mutex);
static int cma_pb_order = -1;	/* validated runtime pageblock order */
static bool cma_capable;	/* settled ONCE at init: symbols + preflight
				 * params all present AND the first-block
				 * verification passed on live memory. A boot
				 * constant - there is deliberately no lazy /
				 * first-run runtime state (v10 rev.). */
/* C4 tripwire: pages whose pageblock reads CMA that tried to enter the pool.
 * Normally constant 0 - the process-context-only flip rule plus the free-hook
 * gate guarantee no reservoir page ever reaches a pool entry point. Nonzero
 * means a logic bug upstream; the page itself is refused (it drains to buddy
 * and lands on the CMA freelist, which is its correct home either way). */
static atomic_long_t dbg_cma_leak;

/* Pages per pageblock / 2MB sub-blocks per pageblock, at the RUNTIME order.
 * Only meaningful once cma_capable; order-9 devices give SUBBLKS = 1 and every
 * pairing/exchange mechanism degenerates to a no-op. */
#define CMA_PB_NR	(1UL << cma_pb_order)
#define CMA_SUBBLKS	(1 << (cma_pb_order - PAGE_ORDER))

/* Reservoir size in 2MB-page equivalents (the unit every other pool number
 * uses), derived from the block count - never stored separately. Gated on the
 * validated order, NOT on cma_capable: a systemic verification failure clears
 * cma_capable but may leave committed blocks behind, and those must keep
 * counting (for the GUI and for demolition targets) until demolished. */
static int cma_pool_cma_2mb(void)
{
	if (READ_ONCE(cma_pb_order) < PAGE_ORDER)
		return 0;
	return READ_ONCE(cma_blocks_n) * CMA_SUBBLKS;
}

/* Round a 2MB-page target UP to whole pageblocks (§2: both wants are aligned
 * while the feature is on; no-op on order-9 devices), stepping back DOWN if
 * the round-up crossed the pool cap. */
static int cma_align_2mb(int v)
{
	int sub;

	if (!cma_capable)
		return v;
	sub = CMA_SUBBLKS;
	if (sub <= 1)
		return v;
	v = roundup(v, sub);
	if (v > pool_size_max)
		v = rounddown(pool_size_max, sub);
	return v;
}

/* pool_want raised past pool_want_with_cma: the total target follows (§5) -
 * legacy management apps only write pool_want, and this keeps that one knob
 * driving the whole elastic loop (grow -> with_cma follows -> acquire fills;
 * shrink -> excess flips back to reservoir). Only while enabled: 0 is the
 * disable sentinel and must never be pulled up. */
static void cma_want_follow(int want)
{
	int wc = READ_ONCE(pool_want_with_cma);

	if (wc > 0 && want > wc)
		WRITE_ONCE(pool_want_with_cma, cma_align_2mb(want));
}

/* External-source gate - defined with the reservoir engine below; used by
 * every external pool filler (refill worker, acquire loops). */
static bool cma_external_ok(void);


/* ------------------------------------------------------------------ *
 * Unity build: the body lives in the parts/ files, textually #included below
 * in original top-to-bottom order (one translation unit; every static stays
 * file-local, no cross-file externs). docker_exec.sh copies parts/ into the
 * build dir; only this file is in obj-m.
 * ------------------------------------------------------------------ */
#include "parts/gh_data.c.inc"
#include "parts/gh_hooks.c.inc"
#include "parts/gh_sysfs.c.inc"
#include "parts/gh_cma.c.inc"

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

	/*
	 * v10: resolve the sweep zone early (the reservoir floor guard reads
	 * CMA-free counts off its pgdat), then build the CMA reservoir BEFORE
	 * the pool prefill - whole pageblocks want the cleanest memory this
	 * boot will ever offer, and the prefill can't draw from committed
	 * blocks anyway (GFP_KERNEL never allocates from CMA freelists). The
	 * zone is re-resolved after the prefill as before; a pool page's zone
	 * is the more authoritative answer once one exists.
	 */
	acq_zone = acq_resolve_zone();
	cma_boot_build();

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
	for (i = 0; i < pool_total; i++)	/* v10: index the prefill */
		pb_track(page_to_pfn(page_pool[i]), PB_AVAIL, 0);

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

	/*
	 * moveable_to_cma: register the gfp adjust hook so the gfp_cma_hook lever can
	 * arm it at runtime (the soft flag cma_bypass_enabled, pre-set by
	 * cma_boot_build from any insmod desire, gates actual effect). Only when the
	 * reservoir feature is usable and the vendor is not already redirecting; a
	 * missing hook just logs and leaves the lever unavailable (-ENOSYS on write).
	 */
	if (cma_capable && !mtc_vender_already_allowed) {
		int hret = cma_adjust_hook_register();

		if (hret == 0)
			pr_info("moveable_to_cma: gfp adjust hook ready (%s), gfp_cma_hook=%d\n",
				cma_adjust_is_calc ? "android_vh_calc_alloc_flags"
						   : "android_vh_alloc_flags_cma_adjust",
				READ_ONCE(cma_bypass_enabled));
		else
			pr_info("moveable_to_cma: no gfp adjust hook on this kernel (ret=%d), gfp_cma_hook lever unavailable\n",
				hret);
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
	 * moveable_to_cma: detach the gfp adjust hook before teardown so no bypass
	 * grant races the reservoir restore below (tracepoint_synchronize_unregister
	 * inside guarantees no probe is in flight afterward). The restrict_cma_redirect
	 * key is deliberately NOT restored: its state is a user policy choice, not our
	 * resource, and there is no safe "previous value" to roll back to.
	 */
	cma_adjust_hook_unregister();

	/*
	 * v10: restore the CMA reservoir first (§10) - grab each block back,
	 * flip it MOVABLE, free it - while the module's bookkeeping is fully
	 * alive. Retried a few times: a block can be transiently ungrabbable
	 * (racing movable allocation mid-migration). A block that still
	 * resists is left CMA-labeled: its label and freelist stay mutually
	 * consistent, the memory stays app-usable (movable), it merely
	 * refuses unmovable allocations until reboot - noisy, never harmful.
	 */
	mutex_lock(&cma_mutex);
	{
		int pass;

		for (pass = 0; pass < 3 && cma_blocks_n; pass++) {
			cma_reservoir_demolish(0, true);
			if (cma_blocks_n)
				msleep(100);
		}
	}
	if (cma_blocks_n)
		pr_err("cma: %d reservoir block(s) left CMA-labeled at exit\n",
		       cma_blocks_n);
	mutex_unlock(&cma_mutex);

	/* v10: free the limbo strays (held order-9 compounds). */
	{
		struct page *lp;

		while ((lp = limbo_del_idx(0)))
			__free_pages(lp, PAGE_ORDER);
	}

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
