/* SPDX: part of gh_hugepage_reserve.ko - textually #included by
 * gh_hugepage_reserve.c (unity build, single translation unit). NOT compiled
 * separately and needs no include guard; all statics stay file-local.
 * Content: sysfs parameters (module_param_cb) */

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
	/* v10 CMA reservoir block ('='-split like everything above, GUI-safe). */
	n += sysfs_emit_at(buf, n, "pool_want_with_cma=%d\n", READ_ONCE(pool_want_with_cma));
	n += sysfs_emit_at(buf, n, "pool_cma=%d\n", cma_pool_cma_2mb());
	n += sysfs_emit_at(buf, n, "pool_avail_cma_able=%d\n", cma_avail_cma_able_2mb());
	n += sysfs_emit_at(buf, n, "cma_pb_order=%d\n", cma_capable ? cma_pb_order : -1);

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

	if (atomic_read(&pool_count) + READ_ONCE(served_count) >= pool_total) {
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

/* Headroom floor for CMA flips - defined with the reservoir engine below,
 * together with the §7 class-ordered shrink pieces. */
static bool cma_floor_ok(int nblocks);
static int pool_extract_block(unsigned long pb, struct page **subs, int max);
static struct page *pool_extract_pfn(unsigned long pfn);
static unsigned long pb_find_full_avail(void);
static unsigned long pb_find_class_c_avail(void);
static int cma_commit_compound_block(unsigned long base, struct page **subs);
static void cma_limbo_process(void);
static int cma_verify_first_span(void);

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
	bool flip = false;
	int flip_budget = 0, flipped = 0;

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

	/*
	 * v10 §7 class-A slice (SUBBLKS==1 devices): a drained avail page IS a
	 * whole, aligned pageblock, so instead of losing it to buddy it can
	 * flip straight into the CMA reservoir - this is the return leg the
	 * one-way "-f fork" never had (shrink used to disown the memory; now
	 * the guardianship just changes form, capped at pool_want_with_cma -
	 * newt). Class B (served siblings) / class C (partial) and SUBBLKS>1
	 * pairing need the pb-hash and arrive with commits 2/3; with
	 * SUBBLKS==1 every avail page is class A. The flip happens while the
	 * page is still OUR fully-held compound (label change is safe), then
	 * the ordinary order-9 compound free carries it - via a short pcp
	 * transit whose stored pcppage migratetype preserves CMA - onto its
	 * block's CMA freelist.
	 */
	if (cma_capable &&
	    READ_ONCE(pool_want_with_cma) > newt && CMA_SUBBLKS == 1) {
		mutex_lock(&cma_mutex);
		flip = true;
		flip_budget = READ_ONCE(pool_want_with_cma) - newt -
			      cma_pool_cma_2mb();
	}

	/*
	 * v10 §7 class order (SUBBLKS > 1): drop class-C strays to buddy
	 * first (their blocks can never complete from what we hold), flip
	 * class-A groups whole into the reservoir second, and only then let
	 * the generic drain below take what remains - by then that is class B
	 * (avail members whose lent-out siblings may still return and turn
	 * the block into class A), deliberately dropped last.
	 */
	if (cma_capable && CMA_SUBBLKS > 1) {
		int budget;

		mutex_lock(&cma_mutex);
		while (atomic_read(&pool_count) > target_avail) {	/* pass C */
			unsigned long vpfn = pb_find_class_c_avail();
			struct page *p;

			if (!vpfn)
				break;
			p = pool_extract_pfn(vpfn);
			if (!p) {
				pb_track(vpfn, 0, PB_AVAIL);	/* stale bit */
				continue;
			}
			__free_pages(p, PAGE_ORDER);
			cond_resched();
		}
		budget = READ_ONCE(pool_want_with_cma) - newt - cma_pool_cma_2mb();
		while (atomic_read(&pool_count) > target_avail) {	/* pass A */
			unsigned long key = pb_find_full_avail();
			struct page *subs[8];
			int got, j, ret;

			if (!key)
				break;
			got = pool_extract_block(key, subs, CMA_SUBBLKS);
			if (!got)
				break;	/* index/pool disagree: generic drain mops up */
			ret = -EAGAIN;
			if (got == CMA_SUBBLKS && budget >= CMA_SUBBLKS &&
			    READ_ONCE(pool_want_with_cma) > newt &&
			    cma_floor_ok(1))
				ret = cma_commit_compound_block(
					key << cma_pb_order, subs);
			if (ret == 0) {
				budget -= CMA_SUBBLKS;
				flipped += CMA_SUBBLKS;
				continue;
			}
			if (ret == -EIO)
				cma_capable = false;
			for (j = 0; j < got; j++)	/* over budget/floor: buddy */
				__free_pages(subs[j], PAGE_ORDER);
			cond_resched();
		}
		mutex_unlock(&cma_mutex);
	}

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

		for (i = 0; i < n; i++) {
			struct page *pg = batch[i];

			pb_track(page_to_pfn(pg), 0, PB_AVAIL);	/* leaving avail */
			if (flip && flip_budget > 0) {
				unsigned long pfn = page_to_pfn(pg);
				int mt = cma_pb_mt(pfn);

				/* §9 whitelist + headroom floor, like every flip */
				if (cma_mt_flippable(mt) &&
				    cma_blocks_n < POOL_SIZE_MAX &&
				    cma_floor_ok(1)) {
					kapi.k_set_pageblock_migratetype(pg,
							migrate_cma_val);
					if (cma_pb_mt(pfn) == migrate_cma_val) {
						__free_pages(pg, PAGE_ORDER);
						cma_blocks[cma_blocks_n++] = pfn;
						flip_budget--;
						flipped++;
						continue;
					}
					/* readback failed: unflip, fall through */
					kapi.k_set_pageblock_migratetype(pg,
							MIGRATE_MOVABLE);
				}
			}
			__free_pages(pg, PAGE_ORDER);
		}
		if (n == 0)
			break;
		cond_resched();
	}

	if (flip) {
		/* Flush the pcp transit so CmaFree/pool_cma agree right away
		 * (a following stage-in grab drains internally anyway; this is
		 * for the accounting the GUI and the floor guard read). */
		if (flipped && kapi.k_drain_all_pages)
			kapi.k_drain_all_pages(NULL);
		mutex_unlock(&cma_mutex);
	}

	WRITE_ONCE(pool_total, newt);
	if (pb_enabled()) {	/* §3: resize is a limbo trigger point too */
		mutex_lock(&cma_mutex);
		cma_limbo_process();
		mutex_unlock(&cma_mutex);
	}
	pr_info("pool shrink: want=%d avail=%d served=%d flipped_to_cma=%d pool_cma=%d\n",
		newt, atomic_read(&pool_count), served_count, flipped,
		cma_pool_cma_2mb());
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
	if (READ_ONCE(pool_want_with_cma) > 0)
		v = cma_align_2mb(v);	/* whole-pageblock accounting while the
					 * reservoir is enabled (§2) */

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
		cma_want_follow(v);		/* grow past with_cma: total follows (§5) */
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

