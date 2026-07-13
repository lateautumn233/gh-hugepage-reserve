/* SPDX: part of gh_hugepage_reserve.ko - textually #included by
 * gh_hugepage_reserve.c (unity build, single translation unit). NOT compiled
 * separately and needs no include guard; all statics stay file-local.
 * Content: free-path reclaim hook, movable->CMA levers, serve kretprobe, VM create/destroy detect, helpers */

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

	if (cma_pool_entry_reject(page))	/* C4 tripwire (atomic-safe: bitmap read) */
		return false;
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
	if (ok)
		pb_track(page_to_pfn(page), PB_AVAIL, 0);
	return ok;
}

/*
 * §8 exchange path (SUBBLKS > 1 only, and only when the pool_want gate just
 * refused a returning page - the "want was shrunk mid-flight" corner): losing
 * this page to buddy would orphan its guarded siblings into a never-complete
 * block, so instead ACCEPT it and demote one low-completeness avail page to
 * the limbo pool - pool size unchanged, completeness strictly improved. If
 * nothing is demotable, the new block itself parks in limbo. Everything here
 * is list/bit work under raw locks (pool_lock -> pb_lock/limbo_lock, both
 * leaf): no free, no flip, atomic-legal (§8). A page is only MUTATED
 * (rebuild_order9_compound) after its destination is secured - a refused page
 * must continue into buddy pristine.
 */
static bool pool_take_frozen_exchange(struct page *page)
{
	unsigned long flags, pfn = page_to_pfn(page), vpfn = 0;
	unsigned int vcomp = ~0U;
	int i, scan, vslot = -1, idx;
	u8 av, sv, lb;
	bool ok = false, to_limbo = false;

	if (!pb_enabled() || !kapi.k_prep_compound_page)
		return false;
	pb_peek(pfn, &av, &sv, &lb);
	if (!((av | sv | lb) & (u8)~pb_bit_of(pfn)))
		return false;		/* no guarded sibling: drain as today */
	if (cma_pool_entry_reject(page))
		return false;		/* C4 applies here too */

	raw_spin_lock_irqsave(&pool_lock, flags);
	idx = atomic_read(&pool_count);
	if (idx < READ_ONCE(pool_want)) {
		/* raced: a seat opened between the gate and here - plain take */
		rebuild_order9_compound(page);
		page->mapping = NULL;
		page_pool[idx] = page;
		atomic_set(&pool_count, idx + 1);
		if (idx + 1 > pool_total)
			WRITE_ONCE(pool_total, idx + 1);
		ok = true;
		goto unlock;
	}
	/* bounded victim hunt from the stack top: lowest guardianship
	 * completeness wins; complete blocks are never victims */
	scan = min(idx, 32);
	for (i = 0; i < scan; i++) {
		struct page *cand = page_pool[idx - 1 - i];
		unsigned int comp;
		u8 cav, csv, clb;

		if (!cand)
			continue;
		pb_peek(page_to_pfn(cand), &cav, &csv, &clb);
		comp = pb_popcount8(cav | csv | clb);
		if (comp >= (unsigned int)CMA_SUBBLKS)
			continue;
		if (comp < vcomp) {
			vcomp = comp;
			vslot = idx - 1 - i;
		}
	}
	if (vslot >= 0) {
		struct page *victim = page_pool[vslot];

		if (limbo_add(victim)) {	/* destination secured first */
			rebuild_order9_compound(page);
			page->mapping = NULL;
			page_pool[vslot] = page;
			vpfn = page_to_pfn(victim);
			ok = true;
		}
		/* limbo full: fall through, drain as today */
	} else if (limbo_add(page)) {
		/* nothing demotable: the new block itself waits in limbo */
		rebuild_order9_compound(page);
		page->mapping = NULL;
		to_limbo = true;
	}
unlock:
	raw_spin_unlock_irqrestore(&pool_lock, flags);
	if (ok) {
		pb_track(pfn, PB_AVAIL, 0);
		if (vpfn)
			pb_track(vpfn, PB_LIMBO, PB_AVAIL);
	} else if (to_limbo) {
		pb_track(pfn, PB_LIMBO, 0);
	}
	return ok || to_limbo;
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
	} else if (pool_take_frozen_exchange(page)) {
		/* §8: gate-refused, but siblings are guarded - the page was
		 * exchanged into the pool (or parked in limbo) instead of
		 * orphaning its block. */
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
	return sysfs_emit(buf, "o9_seen=%ld\ndel_hit=%ld\ndel_miss=%ld\ntake_fail=%ld\ncma_leak=%ld\n",
			  atomic_long_read(&dbg_o9_seen),
			  atomic_long_read(&dbg_del_hit),
			  atomic_long_read(&dbg_del_miss),
			  atomic_long_read(&dbg_take_fail),
			  atomic_long_read(&dbg_cma_leak));
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
/*  moveable_to_cma: gfp bypass hook + restrict_cma_redirect flip      */
/* ================================================================== */

/* Free system CMA pages on the pool's pgdat - defined later, next to the CMA
 * reservoir engine's floor guard that shares it. Forward-declared here for the
 * bypass hook's own floor check below. */
static unsigned long cma_free_pages(void);

/*
 * ALLOC_CMA (mm/internal.h): the bit gfp_to_alloc_flags_cma() ORs into
 * alloc_flags to admit a request to the CMA freelist. A plain #define, not an
 * enum - carries no BTF type, cannot be verified at preflight. Confirmed 0x80 on
 * every stock GKI KMI 5.10..mainline (aosp-mirror/kernel_common); the low bits
 * are structurally fixed (0-2 watermark, 0x40 CPUSET, 0x80 CMA) across the 6.4
 * reserve-flag rename. Gated to [6.1,6.16) with the rest of the cma feature.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0) && \
    LINUX_VERSION_CODE <  KERNEL_VERSION(6, 16, 0)
#define GH_ALLOC_CMA	0x80u
#endif

/* Which of the two ABI-era hook variants cma_adjust_hook_register() found.
 * Declared unconditionally so the init log line compiles either way. */
static bool cma_adjust_is_calc;

#ifdef GH_ALLOC_CMA
/*
 * The bypass decision, shared by both hook variants. Cheapest checks first.
 * Widens ALLOC_CMA past its normal __GFP_CMA gate for plain MOVABLE requests
 * (page cache, mTHP anon) - but only while it costs the reservoir and the system
 * nothing:
 *   0. the gfp_cma_hook lever is armed (moveable_to_cma_gfp_cma_hook=1);
 *   1. a reservoir exists AND gathering is quiescent - not the boot build /
 *      prefill (pool_ready set) and no acquire worker running - else app
 *      consumption races the gatherer for the free pages it is assembling into
 *      CMA. Lends whatever is built, even if acquire stalled below target;
 *   2. free system CMA stays above cma_bypass_floor_mb after granting this.
 *
 * Testing __GFP_MOVABLE set and __GFP_RECLAIMABLE clear is exactly
 * MIGRATE_MOVABLE in this encoding, and sidesteps gfp_migratetype()'s read of
 * the un-exported page_group_by_mobility_disabled. It names the public __GFP_*
 * macros, so it recompiles correctly against any target kernel's bit values.
 */
static bool cma_bypass_wants(gfp_t gfp_mask)
{
	unsigned long floor_pages;
	int wc;

	if (!READ_ONCE(cma_bypass_enabled))
		return false;
	if ((gfp_mask & (__GFP_MOVABLE | __GFP_RECLAIMABLE)) != __GFP_MOVABLE)
		return false;
	if (!cma_capable)
		return false;

	wc = READ_ONCE(pool_want_with_cma);
	if (wc <= 0 || cma_pool_cma_2mb() <= 0)	/* feature off / no reservoir yet */
		return false;
	/* Don't lend while the reservoir is still being GATHERED: the boot build +
	 * prefill (pool_ready not yet set) or any acquire worker in flight. App
	 * consumption would race the gatherer for the very free pages / pageblocks
	 * it is trying to assemble into CMA. Once both are quiescent the reservoir
	 * is as built as it will get, so lend whatever assembled - including when
	 * acquire stalled below target on fragmentation instead of reaching it. */
	if (!READ_ONCE(pool_ready) || atomic_read(&acquire_running))
		return false;

	floor_pages = (unsigned long)READ_ONCE(cma_bypass_floor_mb) << (20 - PAGE_SHIFT);
	if (cma_free_pages() < floor_pages)
		return false;

	return true;
}

/* <6.12-era hook: android_vh_alloc_flags_cma_adjust(gfp_mask, alloc_flags*),
 * called AFTER the built-in ALLOC_CMA decision. */
static void gh_alloc_flags_cma_adjust(void *data, gfp_t gfp_mask,
				       unsigned int *alloc_flags)
{
	if (cma_bypass_wants(gfp_mask))
		*alloc_flags |= GH_ALLOC_CMA;
}

/* 6.12+-era hook: android_vh_calc_alloc_flags(gfp_mask, alloc_flags*, bypass*),
 * called BEFORE the built-in decision. *bypass is left untouched so that
 * decision still runs afterward unmodified - we only ever OR the same bit it
 * would, so which runs first is irrelevant. */
static void gh_calc_alloc_flags_adjust(void *data, gfp_t gfp_mask,
					unsigned int *alloc_flags, bool *bypass)
{
	if (cma_bypass_wants(gfp_mask))
		*alloc_flags |= GH_ALLOC_CMA;
}

/* Both located by name (no exported symbol; no btf_trace typedef in GKI vmlinux
 * BTF, so never disabled by the preflight - kapi_abi.tsv keeps them for docs +
 * the manual disable_kapi path). At most one name exists on any kernel. */
static struct tracepoint *cma_adjust_tp;
static bool cma_adjust_active;

static void find_cma_adjust_tp(struct tracepoint *tp, void *priv)
{
	if (!cma_adjust_tp && !strcmp(tp->name, "android_vh_alloc_flags_cma_adjust"))
		cma_adjust_tp = tp;
}

static void find_calc_alloc_flags_tp(struct tracepoint *tp, void *priv)
{
	if (!cma_adjust_tp && !strcmp(tp->name, "android_vh_calc_alloc_flags"))
		cma_adjust_tp = tp;
}

static void *cma_adjust_probe_fn(void)
{
	return cma_adjust_is_calc ? (void *)gh_calc_alloc_flags_adjust
				  : (void *)gh_alloc_flags_cma_adjust;
}

static int cma_adjust_hook_register(void)
{
	int ret;

	if (cma_adjust_active)
		return 0;
	if (!cma_adjust_tp) {
		if (!kapi_sym_disabled("android_vh_alloc_flags_cma_adjust")) {
			for_each_kernel_tracepoint(find_cma_adjust_tp, NULL);
			if (cma_adjust_tp)
				cma_adjust_is_calc = false;
		}
		if (!cma_adjust_tp && !kapi_sym_disabled("android_vh_calc_alloc_flags")) {
			for_each_kernel_tracepoint(find_calc_alloc_flags_tp, NULL);
			if (cma_adjust_tp)
				cma_adjust_is_calc = true;
		}
		if (!cma_adjust_tp)
			return -ENOENT;
	}
	ret = tracepoint_probe_register(cma_adjust_tp, cma_adjust_probe_fn(), NULL);
	if (ret == 0)
		cma_adjust_active = true;
	return ret;
}

static void cma_adjust_hook_unregister(void)
{
	if (!cma_adjust_active || !cma_adjust_tp)
		return;
	tracepoint_probe_unregister(cma_adjust_tp, cma_adjust_probe_fn(), NULL);
	tracepoint_synchronize_unregister();
	cma_adjust_active = false;
}

/* True iff this kernel exposes one of the two gfp adjust hooks AND we can OR the
 * ALLOC_CMA bit (GH_ALLOC_CMA defined = in-range build). */
static bool kapi_can_gfp_hook(void)
{
	return cma_adjust_tp != NULL;
}
#else
static int cma_adjust_hook_register(void) { return -ENOENT; }
static void cma_adjust_hook_unregister(void) {}
static bool kapi_can_gfp_hook(void) { return false; }
#endif /* GH_ALLOC_CMA */

/*
 * restrict_cma_redirect flip. The static key is a GLOBAL switch: false makes
 * every movable allocation eligible for CMA (and turns on the "prefer CMA when
 * CMA-free > half" balancer), affecting vendor carveouts too - heavier than the
 * gfp hook, which only widens our reservoir's intake. Both togglers sleep
 * (jump_label_mutex + cpus_read_lock), so PROCESS CONTEXT only (sysfs store /
 * init). On 6.6/6.12 the same key backs cma_has_pcplist(): a live flip can
 * strand CMA pages on the per-cpu CMA list, so drain pcp right after (6.1 has no
 * such overload; production DEBUG_VM is off so order_to_pindex's VM_BUG_ON is a
 * no-op regardless).
 */
static int mtc_restrict_get_state(void)		/* -1 unavailable, else 0/1 */
{
	if (!kapi.k_restrict_cma_redirect)
		return -1;
	return static_key_enabled(kapi.k_restrict_cma_redirect) ? 1 : 0;
}

static int mtc_restrict_set_state(bool restricted)
{
	if (!kapi.k_restrict_cma_redirect ||
	    !kapi.k_static_key_enable || !kapi.k_static_key_disable)
		return -ENOSYS;
	if (restricted == (mtc_restrict_get_state() == 1))
		return 0;			/* already in target state */
	if (restricted)
		kapi.k_static_key_enable(kapi.k_restrict_cma_redirect);
	else
		kapi.k_static_key_disable(kapi.k_restrict_cma_redirect);
	if (kapi.k_drain_all_pages)
		kapi.k_drain_all_pages(NULL);	/* flush stale CMA pcp entries */
	return 0;
}

static bool kapi_can_restrict_flip(void)
{
	return kapi.k_restrict_cma_redirect &&
	       kapi.k_static_key_enable && kapi.k_static_key_disable;
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
	    atomic_read(&pool_count) + READ_ONCE(served_count) < pool_total &&
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

			if (atomic_read(&pool_count) + READ_ONCE(served_count) >=
			    pool_total)
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
	if (atomic_read(&pool_count) + READ_ONCE(served_count) < pool_total)
		msleep(200);

	for (retry = 0; retry <= REFILL_RETRY_MAX; retry++) {
		if (retry > 0) {
			pr_info("refill retry %d/%d, waiting %d ms\n",
				retry, REFILL_RETRY_MAX, REFILL_RETRY_INTERVAL_MS);
			msleep(REFILL_RETRY_INTERVAL_MS);
		}

		current_count = atomic_read(&pool_count);
		/*
		 * v10 §9 seat fix: lent-out pages OWN their seats. The v9
		 * target (pool_total - avail) ignored served, so this worker
		 * allocated fresh buddy pages into seats whose real pages were
		 * still coming home through the free hook - a replacement
		 * engine that evicted every returning VM page to buddy. Only
		 * the shortfall the hook could not recover (avail + served <
		 * capacity) is refilled from outside; cma_external_ok also
		 * caps it under the with_cma total like every external filler.
		 */
		target = pool_total - current_count - READ_ONCE(served_count);

		if (target <= 0) {
			pr_info("pool already full (%d+%d/%d)\n",
				current_count, READ_ONCE(served_count), pool_total);
			break;
		}

		pr_info("refill attempt %d: need %d pages (have %d+%d/%d)\n",
			retry + 1, target, current_count,
			READ_ONCE(served_count), pool_total);

		allocated = 0;
		while (atomic_read(&pool_count) + READ_ONCE(served_count) <
		       pool_total && cma_external_ok()) {
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

		if (atomic_read(&pool_count) + READ_ONCE(served_count) >= pool_total)
			break;
	}

	pr_info("refill complete: %d pages recovered, pool %d/%d\n",
		total_allocated, atomic_read(&pool_count), pool_total);

	/* Sweep owners whose process/VMs are gone - survivors keep tracking */
	schedule_work(&vm_owner_sweep_work);

	atomic_set(&refill_status, REFILL_IDLE);
}

