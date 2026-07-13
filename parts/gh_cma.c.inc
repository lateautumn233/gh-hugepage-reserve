/* SPDX: part of gh_hugepage_reserve.ko - textually #included by
 * gh_hugepage_reserve.c (unity build, single translation unit). NOT compiled
 * separately and needs no include guard; all statics stay file-local.
 * Content: CMA reservoir engine + aggressive acquire */

/* ================================================================== */
/*  CMA reservoir engine (v10)                                        */
/*                                                                    */
/*  Blocks live in exactly one of two states: HELD (every page ours,  */
/*  refcount-normalized to order-0 singles, label being inspected or  */
/*  flipped) or COMMITTED (label CMA, every page free in buddy on the */
/*  block's own CMA freelist, base pfn recorded in cma_blocks[]).     */
/*  Migratetype flips happen ONLY on fully-held blocks and ONLY in    */
/*  process context under cma_mutex - the free hook never flips and   */
/*  never frees (§9), which is what keeps label and freelist          */
/*  consistent at every instant.                                      */
/* ================================================================== */

/* Free CMA pages on the pgdat the pool lives on. zone_page_state reads the
 * same folded (slightly lagging) counters /proc/meminfo's CmaFree uses, but
 * through our cached zone pointer - global_zone_page_state would add a
 * vm_zone_stat dependency the symbol-safety check forbids. Phones are
 * single-node, so one pgdat is the whole story; a second node would only make
 * the floor guard conservative in the wrong direction, and none of the target
 * devices has one. */
static unsigned long cma_free_pages(void)
{
	struct pglist_data *pgdat;
	unsigned long sum = 0;
	int i;

	if (!acq_zone)
		return 0;
	pgdat = acq_zone->zone_pgdat;
	for (i = 0; i < MAX_NR_ZONES; i++)
		sum += zone_page_state(&pgdat->node_zones[i], NR_FREE_CMA_PAGES);
	return sum;
}

/* Headroom floor (§9): OK to flip @nblocks more pageblocks to CMA? CMA-free
 * memory only serves movable allocations, so what must stay above the floor is
 * available-minus-CMA - the budget kernel/unmovable allocations can still
 * reach. This is a different brake from acquire_mem_floor_mb (anti-livelock
 * for the sweep); both exist, each on its own path. */
static bool cma_floor_ok(int nblocks)
{
	unsigned long floor = (unsigned long)READ_ONCE(cma_reservoir_floor_mb)
				<< (20 - PAGE_SHIFT);
	unsigned long need = (unsigned long)nblocks << cma_pb_order;
	long noncma = (long)si_mem_available() - (long)cma_free_pages();

	return noncma > (long)(floor + need);
}

/* Guardianship total in 2MB pages: held pool (avail + lent out) + reservoir.
 * The §9 total invariant compares this against pool_want_with_cma. */
static int cma_guardianship_2mb(void)
{
	return atomic_read(&pool_count) + READ_ONCE(served_count) +
	       cma_pool_cma_2mb();
}

/* External-source gate (§6/§9): with the reservoir enabled, EXTERNAL memory
 * may only be grabbed while total guardianship is below pool_want_with_cma -
 * past that, a pool shortfall must be staged in from the reservoir, or fresh
 * grabs would push the total over the target. v9 semantics (feature off) are
 * untouched: 0/unset always allows. */
static bool cma_external_ok(void)
{
	int wc = READ_ONCE(pool_want_with_cma);

	return wc <= 0 || cma_guardianship_2mb() < wc;
}

/* FREE non-CMA pages on the pgdat (NR_FREE_PAGES - NR_FREE_CMA_PAGES): what
 * an unmovable allocation can take RIGHT NOW, without reclaim.
 * si_mem_available is the wrong ruler for "can the pool prefill still
 * allocate": it counts reclaimable pagecache that order-9 GFP_KERNEL often
 * cannot actually compact out (measured: prefill got 0/256 while MemAvailable
 * showed 3.4G non-CMA). */
static unsigned long cma_free_noncma_pages(void)
{
	struct pglist_data *pgdat;
	unsigned long freep = 0;
	int i;

	if (!acq_zone)
		return 0;
	pgdat = acq_zone->zone_pgdat;
	for (i = 0; i < MAX_NR_ZONES; i++)
		freep += zone_page_state(&pgdat->node_zones[i], NR_FREE_PAGES);
	return freep - min(freep, cma_free_pages());
}

/*
 * Normalize a raw pfn range WE fully own to independent refcount-1 order-0
 * pages, whatever mix it arrived as (alloc_pages(order) = head 1 / tails 0;
 * alloc_contig_range = singles already). The same refcount surgery
 * rebuild_order9_compound already does, pointed the other way: singles are
 * the working form for label flips and partial frees.
 */
static void cma_span_make_singles(unsigned long pfn, unsigned long nr)
{
	unsigned long i;

	for (i = 0; i < nr; i++)
		set_page_count(pfn_to_page(pfn + i), 1);
}

/*
 * Free a singles-held range into the buddy freelists as order-8 chunks.
 * Order 8 is deliberate: it is above PAGE_ALLOC_COSTLY_ORDER and is not the
 * THP order, so free_unref_page never parks it in a pcp list - every chunk
 * goes straight through __free_one_page onto the freelist of the block's
 * CURRENT label. That immediacy is load-bearing three times over: the
 * first-block +512 accounting check reads NR_FREE_CMA_PAGES right after the
 * free; the floor guard and the GUI read CmaFree and must see reservoir
 * builds/demolitions at once; and a demolition grab must find the pages IN
 * buddy, not parked in pcp. Adjacent chunks coalesce back up on their own.
 * pfn/nr are pageblock-derived so the 256-page alignment always holds. The
 * free hook ignores these frees (order != 9).
 */
static void cma_span_free_to_buddy(unsigned long pfn, unsigned long nr)
{
	const unsigned long step = 1UL << 8;
	unsigned long i, j;

	for (i = 0; i < nr; i += step) {
		struct page *head = pfn_to_page(pfn + i);

		for (j = 1; j < step; j++)
			set_page_count(head + j, 0);	/* order-8 free wants tails at 0 */
		__free_pages(head, 8);
		cond_resched();
	}
}

/*
 * Flip one FULLY-HELD, singles-normalized pageblock to CMA and free it into
 * its own (now CMA) freelist, recording it in cma_blocks[]. Label whitelist
 * first (§9): accept {UNMOVABLE(0), MOVABLE(1), RECLAIMABLE(2)} - a
 * grab/steal often leaves an UNMOVABLE residue on a block that is entirely
 * ours, and holding every page makes the flip safe regardless of label.
 * Reject everything else without needing its runtime value: CMA (someone
 * else's carveout), HIGHATOMIC (the nr_reserved_highatomic ledger would go
 * stale), ISOLATE (an isolation in flight), >= 6 (vendor CHP extensions,
 * guarded by CHP_BUG_ON panics). 0/1/2 as raw integers is safe: they are the
 * first three enumerators on every supported kernel. No steal can relabel the
 * block between check and flip - steals happen when free pages of the block
 * are allocated, and it has none while held. cma_mutex held.
 * Returns 0 = committed; -EPERM = label not flippable (caller swaps
 * candidates, block left untouched and still held); -EIO = readback after the
 * flip failed = systemic, caller must stop the feature (block unflipped,
 * still held).
 */
static int cma_block_commit(unsigned long base)
{
	int mt = cma_pb_mt(base);

	if (!cma_mt_flippable(mt))
		return -EPERM;
	if (cma_blocks_n >= POOL_SIZE_MAX)
		return -EPERM;			/* can't record: don't flip */
	kapi.k_set_pageblock_migratetype(pfn_to_page(base), migrate_cma_val);
	if (cma_pb_mt(base) != migrate_cma_val) {
		kapi.k_set_pageblock_migratetype(pfn_to_page(base), MIGRATE_MOVABLE);
		pr_err("cma: flip readback failed at pfn=%lx (wrote %d, read %d)\n",
		       base, migrate_cma_val, cma_pb_mt(base));
		return -EIO;
	}
	cma_span_free_to_buddy(base, CMA_PB_NR);
	cma_blocks[cma_blocks_n++] = base;
	return 0;
}

/*
 * Take one reservoir block back out of circulation: CMA-mode contig grab
 * (migrates app squatters out - they are movable by construction, nothing
 * unmovable can have entered), flip back to MOVABLE while every page is held,
 * free into the movable freelists. Returns 0 = restored (caller must drop the
 * cma_blocks[] entry), -errno = grab failed this time (transient pin /
 * memory pressure: entry stays, caller may retry later). cma_mutex held.
 */
static int cma_block_demolish(unsigned long base)
{
	int ret;

	if (cma_pb_mt(base) != migrate_cma_val) {
		/* Our label is gone - something relabeled a block we own the
		 * bookkeeping for. Never grab through a foreign label (the
		 * isolation undo would stamp OUR migratetype over theirs);
		 * drop the entry and shout. Should never happen. */
		pr_err("cma: reservoir block pfn=%lx lost its CMA label (mt=%d), dropping entry\n",
		       base, cma_pb_mt(base));
		return 0;
	}
	ret = kapi.k_alloc_contig_range_cma(base, base + CMA_PB_NR,
					    GFP_KERNEL | __GFP_NOWARN);
	if (ret)
		return ret;
	kapi.k_set_pageblock_migratetype(pfn_to_page(base), MIGRATE_MOVABLE);
	cma_span_free_to_buddy(base, CMA_PB_NR);
	return 0;
}

/* Racy free-page estimate for one block (no locks: page_count==0 covers buddy,
 * pcp and in-flight frees well enough to RANK blocks by grab cost - the only
 * consumer). */
static u16 cma_block_free_estimate(unsigned long base)
{
	unsigned long pfn, end = base + CMA_PB_NR;
	unsigned int freeish = 0;

	for (pfn = base; pfn < end; pfn++)
		if (page_count(pfn_to_page(pfn)) == 0)
			freeish++;
	return (u16)freeish;
}

#define CMA_EST_SKIP	0xFFFFU		/* demolition failed this pass: don't re-pick */

/*
 * Demolish reservoir blocks until guardianship fits under @target_2mb, or the
 * whole reservoir when @all. Emptiest-first (§5): the fewer squatters a block
 * has, the cheaper the grab-back - estimated once up front, then a linear
 * arg-max per round (worst case ~12k^2 int compares, fine for a rare admin
 * write). A block whose grab fails is skipped for the rest of THIS call;
 * the caller decides whether to run another pass. Returns blocks restored.
 * cma_mutex held, process context.
 */
static int cma_reservoir_demolish(int target_2mb, bool all)
{
	int i, done = 0;

	for (i = 0; i < cma_blocks_n; i++) {
		cma_est[i] = all ? 0 : cma_block_free_estimate(cma_blocks[i]);
		cond_resched();
	}

	while (cma_blocks_n > 0) {
		int pick = -1;
		u16 best = 0;

		if (!all && cma_guardianship_2mb() <= target_2mb)
			break;
		for (i = 0; i < cma_blocks_n; i++) {
			if (cma_est[i] == CMA_EST_SKIP)
				continue;
			if (pick < 0 || cma_est[i] > best) {
				pick = i;
				best = cma_est[i];
			}
		}
		if (pick < 0)
			break;			/* every remaining block resisted */
		if (cma_block_demolish(cma_blocks[pick]) == 0) {
			/* swap-with-last in BOTH arrays */
			cma_blocks[pick] = cma_blocks[cma_blocks_n - 1];
			cma_est[pick] = cma_est[cma_blocks_n - 1];
			WRITE_ONCE(cma_blocks_n, cma_blocks_n - 1);
			done++;
		} else {
			cma_est[pick] = CMA_EST_SKIP;
		}
		cond_resched();
	}
	return done;
}

/*
 * §6 stage-in (the commit-2 slice that works without the pb-hash): move
 * reservoir blocks into the held pool while the pool reserve (avail + served)
 * is short of pool_want. Whole-pageblock unit: CMA-mode grab (migrates app
 * squatters out - movable by construction), flip MOVABLE, rebuild as SUBBLKS
 * order-9 pool compounds, push into the pool, drop the cma_blocks[] entry.
 * Guardianship is conserved 1:1, so the caller's "guardianship >=
 * pool_want_with_cma" source-order check holds across the whole run.
 * Emptiest-first: fewer squatters = cheaper grab = pool fills fastest.
 * Interruptible via acquire_running like every acquire loop. A block whose
 * grab fails is skipped for this call. cma_mutex held, process context.
 * Returns blocks staged in.
 */
static int cma_stage_in(void)
{
	int i, staged = 0;

	for (i = 0; i < cma_blocks_n; i++) {
		cma_est[i] = cma_block_free_estimate(cma_blocks[i]);
		cond_resched();
	}

	while (atomic_read(&acquire_running) && cma_blocks_n > 0 &&
	       atomic_read(&pool_count) + READ_ONCE(served_count) <
			READ_ONCE(pool_want)) {
		int pick = -1;
		u16 best = 0;
		unsigned long base;

		if (atomic_read(&pool_count) + CMA_SUBBLKS > READ_ONCE(pool_size_max))
			break;			/* pool array capacity */
		for (i = 0; i < cma_blocks_n; i++) {
			if (cma_est[i] == CMA_EST_SKIP)
				continue;
			if (pick < 0 || cma_est[i] > best) {
				pick = i;
				best = cma_est[i];
			}
		}
		if (pick < 0)
			break;			/* every remaining block resisted */
		base = cma_blocks[pick];
		if (cma_pb_mt(base) != migrate_cma_val) {
			pr_err("cma: reservoir block pfn=%lx lost its CMA label (mt=%d), dropping entry\n",
			       base, cma_pb_mt(base));
			goto drop;		/* never grab through a foreign label */
		}
		if (kapi.k_alloc_contig_range_cma(base, base + CMA_PB_NR,
						  GFP_KERNEL | __GFP_NOWARN)) {
			cma_est[pick] = CMA_EST_SKIP;
			continue;
		}
		kapi.k_set_pageblock_migratetype(pfn_to_page(base), MIGRATE_MOVABLE);
		for (i = 0; i < CMA_SUBBLKS; i++) {
			struct page *head =
				pfn_to_page(base + ((unsigned long)i << PAGE_ORDER));

			rebuild_order9_compound(head);
			if (pool_push_grow(head))
				atomic_inc(&total_refilled);
			else	/* capacity race: let it go whole */
				__free_pages(head, PAGE_ORDER);
		}
		staged++;
drop:
		cma_blocks[pick] = cma_blocks[cma_blocks_n - 1];
		cma_est[pick] = cma_est[cma_blocks_n - 1];
		WRITE_ONCE(cma_blocks_n, cma_blocks_n - 1);
		cond_resched();
	}
	if (staged)
		pr_info("cma: staged %d block(s) into pool, avail=%d pool_cma=%d\n",
			staged, atomic_read(&pool_count), cma_pool_cma_2mb());
	return staged;
}

/* Reservoir deficit (§6): the feature is live and total guardianship has not
 * reached the total target - Phase R's run condition. */
static bool cma_reservoir_deficit(void)
{
	return cma_capable && READ_ONCE(pool_want_with_cma) > 0 &&
	       cma_guardianship_2mb() < READ_ONCE(pool_want_with_cma);
}


/*
 * Rebuild the pb-hash from scratch out of the pool array, the served table
 * and the limbo pool (§3: derived index, not history - THIS is the proof).
 * Run at acquire start: cheap insurance against overflow drift. Concurrent
 * serves/reclaims during the rebuild leave a bit or two stale; the next
 * rebuild corrects them, and nothing downstream treats the index as truth.
 * Process context; chunked so no raw lock is held long.
 */
static void pb_rebuild(void)
{
	unsigned long flags, pfns[64];
	int i, n, start;

	if (!pb_enabled())
		return;
	raw_spin_lock_irqsave(&pb_lock, flags);
	pb_reset_locked();
	pb_overflow = 0;
	raw_spin_unlock_irqrestore(&pb_lock, flags);

	start = 0;
	for (;;) {			/* avail pool, 64 slots per lock hold */
		raw_spin_lock_irqsave(&pool_lock, flags);
		n = 0;
		for (i = start; i < atomic_read(&pool_count) && n < 64; i++)
			if (page_pool[i])
				pfns[n++] = page_to_pfn(page_pool[i]);
		start = i;
		raw_spin_unlock_irqrestore(&pool_lock, flags);
		for (i = 0; i < n; i++)
			pb_track(pfns[i], PB_AVAIL, 0);
		if (n < 64)
			break;
		cond_resched();
	}

	for (start = 0; start < (int)SERVED_MAX; start++) {	/* served table */
		u16 nd;

		raw_spin_lock_irqsave(&served_lock, flags);
		n = 0;
		for (nd = served_bucket[start]; nd != SERVED_NULL && n < 64;
		     nd = served_nodes[nd].next)
			pfns[n++] = served_nodes[nd].pfn;
		raw_spin_unlock_irqrestore(&served_lock, flags);
		for (i = 0; i < n; i++)
			pb_track(pfns[i], PB_SERVED, 0);
		if ((start & 1023) == 0)
			cond_resched();
	}

	raw_spin_lock_irqsave(&limbo_lock, flags);		/* limbo pool */
	n = 0;
	for (i = 0; i < limbo_n && n < 64; i++)
		pfns[n++] = page_to_pfn(limbo_pages[i]);
	raw_spin_unlock_irqrestore(&limbo_lock, flags);
	for (i = 0; i < n; i++)
		pb_track(pfns[i], PB_LIMBO, 0);
}

/*
 * Remove every avail member of pageblock @pb from the pool array into @subs.
 * One pool_lock hold over the whole array (~100us worst case at 12k entries):
 * acceptable for the rare process-context callers (exchange, shrink groups),
 * and a chunked removal scan would race the stack's own push/pop reindexing.
 */
static int pool_extract_block(unsigned long pb, struct page **subs, int max)
{
	unsigned long flags;
	int i = 0, got = 0;

	raw_spin_lock_irqsave(&pool_lock, flags);
	while (i < atomic_read(&pool_count)) {
		struct page *p = page_pool[i];

		if (p && got < max &&
		    (page_to_pfn(p) >> cma_pb_order) == pb) {
			int last = atomic_read(&pool_count) - 1;

			subs[got++] = p;
			page_pool[i] = page_pool[last];
			page_pool[last] = NULL;
			atomic_set(&pool_count, last);
			continue;	/* re-examine the swapped-in slot */
		}
		i++;
	}
	raw_spin_unlock_irqrestore(&pool_lock, flags);
	for (i = 0; i < got; i++)
		pb_track(page_to_pfn(subs[i]), 0, PB_AVAIL);
	return got;
}

/* Remove ONE specific avail page from the pool (Phase Q seat swap). */
static struct page *pool_extract_pfn(unsigned long pfn)
{
	unsigned long flags;
	struct page *p = NULL;
	int i;

	raw_spin_lock_irqsave(&pool_lock, flags);
	for (i = 0; i < atomic_read(&pool_count); i++) {
		if (page_pool[i] && page_to_pfn(page_pool[i]) == pfn) {
			int last = atomic_read(&pool_count) - 1;

			p = page_pool[i];
			page_pool[i] = page_pool[last];
			page_pool[last] = NULL;
			atomic_set(&pool_count, last);
			break;
		}
	}
	raw_spin_unlock_irqrestore(&pool_lock, flags);
	if (p)
		pb_track(pfn, 0, PB_AVAIL);
	return p;
}

/*
 * Flip one whole pageblock whose SUBBLKS sub-blocks are all HELD as order-9
 * compounds (extracted from the pool, or pool + limbo) into the reservoir.
 * Same whitelist/readback contract as cma_block_commit; the compounds are
 * freed AFTER the flip through the ordinary order-9 path (the stored pcppage
 * migratetype carries CMA through the pcp transit). Floor is the caller's.
 * cma_mutex held. 0 committed; -EPERM label; -EIO systemic (block NOT freed
 * in either failure - pages stay with the caller).
 */
static int cma_commit_compound_block(unsigned long base, struct page **subs)
{
	int mt = cma_pb_mt(base), i;

	if (!cma_mt_flippable(mt))
		return -EPERM;
	if (cma_blocks_n >= POOL_SIZE_MAX)
		return -EPERM;
	kapi.k_set_pageblock_migratetype(pfn_to_page(base), migrate_cma_val);
	if (cma_pb_mt(base) != migrate_cma_val) {
		kapi.k_set_pageblock_migratetype(pfn_to_page(base), MIGRATE_MOVABLE);
		pr_err("cma: group flip readback failed at pfn=%lx\n", base);
		return -EIO;
	}
	for (i = 0; i < CMA_SUBBLKS; i++)
		__free_pages(subs[i], PAGE_ORDER);
	cma_blocks[cma_blocks_n++] = base;
	return 0;
}

/* Limbo intake cap (§6): compute BEFORE accepting - strays are only worth
 * holding while the pool has cma_able groups to exchange them against. */
static int cma_limbo_intake_cap(void)
{
	int cap = READ_ONCE(pb_full_avail) * CMA_SUBBLKS;

	return min(cap, LIMBO_MAX);
}

/* An avail member of a class-C pageblock (§7: guardianship incomplete - some
 * sibling is neither pooled, lent, nor in limbo). These are the first to go
 * on a shrink: they can never complete into a flippable block by themselves.
 * Returns the member's pfn, or 0. */
static unsigned long pb_find_class_c_avail(void)
{
	unsigned long flags, pfn = 0;
	unsigned int i;
	u8 full = pb_full_mask();

	for (i = 0; i < PB_HASH_MAX && !pfn; i++) {
		u8 av, guard;

		if ((i & 0x3ff) == 0)
			cond_resched();
		raw_spin_lock_irqsave(&pb_lock, flags);
		av = pb_nodes[i].avail_mask;
		guard = av | pb_nodes[i].served_mask | pb_nodes[i].limbo_mask;
		if (av && guard != full)
			pfn = (pb_nodes[i].pb << cma_pb_order) +
			      ((unsigned long)__ffs(av) << PAGE_ORDER);
		raw_spin_unlock_irqrestore(&pb_lock, flags);
	}
	return pfn;
}

/* Scan for a full-avail pageblock (exchange material). Returns the pb key,
 * or 0 (no pageblock 0 on these devices - DRAM starts well above it). */
static unsigned long pb_find_full_avail(void)
{
	unsigned long flags, key = 0;
	unsigned int i;
	u8 full = pb_full_mask();

	for (i = 0; i < PB_HASH_MAX && !key; i++) {
		if ((i & 0x3ff) == 0)
			cond_resched();
		raw_spin_lock_irqsave(&pb_lock, flags);
		if (pb_nodes[i].avail_mask == full)
			key = pb_nodes[i].pb;
		raw_spin_unlock_irqrestore(&pb_lock, flags);
	}
	return key;
}

/*
 * §6 Phase R exchange: pull one full-avail group out of the pool, flip it to
 * CMA, and refill the vacated seats with limbo strays 1:1 - net effect: the
 * reservoir gains a whole block, the pool trades clean group members for
 * strays without changing size, and limbo drains. Loops while there is
 * deficit, material and floor headroom. cma_mutex held.
 */
static void cma_limbo_exchange(void)
{
	int guard = LIMBO_MAX + 8;

	if (!pb_enabled())
		return;
	while (guard-- > 0 && cma_reservoir_deficit() &&
	       READ_ONCE(limbo_n) > 0 && READ_ONCE(pb_full_avail) > 0 &&
	       cma_floor_ok(1)) {
		struct page *subs[8];
		unsigned long key = pb_find_full_avail();
		int got, i, ret;

		if (!key)
			break;
		got = pool_extract_block(key, subs, CMA_SUBBLKS);
		if (got < CMA_SUBBLKS) {
			/* stale entry (raced a serve): give the pages back */
			for (i = 0; i < got; i++)
				if (!pool_push(subs[i]))
					__free_pages(subs[i], PAGE_ORDER);
			continue;
		}
		ret = cma_commit_compound_block(key << cma_pb_order, subs);
		if (ret) {
			for (i = 0; i < got; i++)
				if (!pool_push(subs[i]))
					__free_pages(subs[i], PAGE_ORDER);
			if (ret == -EIO) {
				cma_capable = false;
				break;
			}
			continue;	/* -EPERM: label not ours to flip */
		}
		for (i = 0; i < CMA_SUBBLKS; i++) {	/* refill seats 1:1 */
			struct page *lp = limbo_del_idx(0);

			if (!lp)
				break;
			pb_track(page_to_pfn(lp), 0, PB_LIMBO);
			if (!pool_push(lp))
				__free_pages(lp, PAGE_ORDER);
		}
		cond_resched();
	}
}

/* Bump the age of limbo entry @idx; returns the new age or -1 if gone. */
static int limbo_age_bump(int idx)
{
	unsigned long flags;
	int age = -1;

	raw_spin_lock_irqsave(&limbo_lock, flags);
	if (idx >= 0 && idx < limbo_n)
		age = ++limbo_age[idx];
	raw_spin_unlock_irqrestore(&limbo_lock, flags);
	return age;
}

#define LIMBO_MAX_AGE	3	/* process passes before a stray is given up on */

/*
 * §3 limbo outcomes, run at existing process-context trigger points (acquire
 * end, resize end) - no new worker. Per entry: whole block now assembled from
 * avail+limbo members and a deficit exists -> flip it whole; the pool has an
 * open seat -> promote; grown old without its siblings -> genuinely free (a
 * stray we cannot serve and cannot complete is dead weight). cma_mutex held.
 */
static void cma_limbo_process(void)
{
	int i = 0;

	if (!pb_enabled())
		return;
	while (i < READ_ONCE(limbo_n)) {
		unsigned long flags, pfn;
		struct page *p;
		u8 av, sv, lb;

		raw_spin_lock_irqsave(&limbo_lock, flags);
		if (i >= limbo_n) {
			raw_spin_unlock_irqrestore(&limbo_lock, flags);
			break;
		}
		p = limbo_pages[i];
		raw_spin_unlock_irqrestore(&limbo_lock, flags);
		pfn = page_to_pfn(p);

		/* Complete block sitting in avail+limbo, and a deficit: flip it
		 * whole - the "組齊 -> 整塊翻 CMA" outcome. */
		pb_peek(pfn, &av, &sv, &lb);
		if (cma_reservoir_deficit() && cma_floor_ok(1) && !sv && lb &&
		    (u8)(av | lb) == pb_full_mask()) {
			unsigned long key = pfn >> cma_pb_order;
			struct page *subs[8];
			int got, j, ret;

			got = pool_extract_block(key, subs, CMA_SUBBLKS);
			/* pull the block's limbo members (including @p) */
			for (j = 0; j < READ_ONCE(limbo_n) && got < CMA_SUBBLKS;) {
				struct page *lp;

				raw_spin_lock_irqsave(&limbo_lock, flags);
				lp = (j < limbo_n) ? limbo_pages[j] : NULL;
				raw_spin_unlock_irqrestore(&limbo_lock, flags);
				if (!lp)
					break;
				if ((page_to_pfn(lp) >> cma_pb_order) == key) {
					lp = limbo_del_idx(j);
					if (lp) {
						pb_track(page_to_pfn(lp), 0, PB_LIMBO);
						subs[got++] = lp;
					}
					continue;	/* j now holds swapped entry */
				}
				j++;
			}
			ret = (got == CMA_SUBBLKS) ?
				cma_commit_compound_block(key << cma_pb_order, subs) :
				-EAGAIN;
			if (ret) {
				/* give members back: pool if seated, else free */
				for (j = 0; j < got; j++)
					if (!pool_push(subs[j]))
						__free_pages(subs[j], PAGE_ORDER);
				if (ret == -EIO) {
					cma_capable = false;
					return;
				}
			}
			continue;	/* entry i changed either way: re-examine */
		}

		/* Open pool seat: promote the stray into it. */
		if (atomic_read(&pool_count) + READ_ONCE(served_count) <
		    READ_ONCE(pool_want)) {
			p = limbo_del_idx(i);
			if (!p)
				continue;
			pb_track(page_to_pfn(p), 0, PB_LIMBO);
			if (!pool_push_grow(p))
				__free_pages(p, PAGE_ORDER);
			continue;	/* slot i holds the swapped-in entry */
		}

		/* Aged out: its siblings never showed - stop hoarding. */
		if (limbo_age_bump(i) > LIMBO_MAX_AGE) {
			p = limbo_del_idx(i);
			if (!p)
				continue;
			pb_track(page_to_pfn(p), 0, PB_LIMBO);
			__free_pages(p, PAGE_ORDER);
			continue;
		}
		i++;
	}
}

/*
 * Grab the FREE missing sibling windows of @any_pfn's pageblock with targeted
 * movable-mode contig ranges and push them into the pool. §6 pairing: called
 * right after grab_free lands a block (its buddy neighbors are often still
 * free - grab them before anyone else fragments the block) and from the
 * pairing pass over half-full pb entries. Returns windows gained.
 */
static int cma_grab_missing_siblings(unsigned long any_pfn, int max_grabs)
{
	unsigned long base, w;
	u8 av, sv, lb, missing, full;
	int b, got = 0;

	if (!pb_enabled() || !kapi.k_alloc_contig_range ||
	    !kapi.k_prep_compound_page)
		return 0;
	full = pb_full_mask();
	base = any_pfn & ~(CMA_PB_NR - 1);
	pb_peek(any_pfn, &av, &sv, &lb);
	missing = full & ~(av | sv | lb);
	if (!missing)
		return 0;
	for (b = 0; b < CMA_SUBBLKS && got < max_grabs; b++) {
		struct page *head;

		if (!(missing & (1U << b)))
			continue;
		if (atomic_read(&pool_count) + READ_ONCE(served_count) >=
		    READ_ONCE(pool_want) || !cma_external_ok())
			break;
		w = base + ((unsigned long)b << PAGE_ORDER);
		if (!pfn_valid(w) || cma_pb_mt(w) == migrate_cma_val)
			break;		/* never contig-grab through a CMA label */
		if (kapi.k_alloc_contig_range(w, w + (1UL << PAGE_ORDER),
					      GFP_KERNEL | __GFP_NOWARN |
					      __GFP_NORETRY))
			continue;
		head = pfn_to_page(w);
		rebuild_order9_compound(head);
		if (!pool_push_grow(head)) {
			__free_pages(head, PAGE_ORDER);
			break;
		}
		atomic_inc(&total_refilled);
		got++;
	}
	return got;
}

/*
 * §6 pairing pass (Phase 1.5, SUBBLKS > 1): walk the pb-hash for blocks the
 * pool already partly owns and fetch their free siblings with targeted grabs
 * - far cheaper than sweeping for brand-new blocks, and every success turns a
 * stray into (eventual) exchange material. Bounded per acquire run.
 */
static void cma_pair_fill(void)
{
	unsigned int i;
	int budget = 256;

	if (!pb_enabled())
		return;
	for (i = 0; i < PB_HASH_MAX && budget > 0; i++) {
		unsigned long flags, pb;
		u8 av, sv, lb, full = pb_full_mask();

		if (!atomic_read(&acquire_running) || !cma_external_ok() ||
		    atomic_read(&pool_count) + READ_ONCE(served_count) >=
			READ_ONCE(pool_want))
			break;
		if ((i & 0xff) == 0)
			cond_resched();
		raw_spin_lock_irqsave(&pb_lock, flags);
		av = pb_nodes[i].avail_mask;
		sv = pb_nodes[i].served_mask;
		lb = pb_nodes[i].limbo_mask;
		pb = pb_nodes[i].pb;
		raw_spin_unlock_irqrestore(&pb_lock, flags);
		if (!(av | sv | lb) || (u8)(av | sv | lb) == full)
			continue;	/* free node or already complete */
		budget -= CMA_SUBBLKS;
		cma_grab_missing_siblings(pb << cma_pb_order, CMA_SUBBLKS);
	}
}

/*
 * Complete a just-swept window's pageblock by grabbing every OTHER window of
 * the block (§6 Phase R "湊得齊" attempt). Only legal when no sibling is in
 * guardianship (those cases go to limbo and complete via exchange). On
 * success the WHOLE block is singles-held. On failure the partial grabs are
 * freed and the original window is untouched. cma_mutex held.
 */
static int cma_try_complete_block(unsigned long win)
{
	unsigned long base = win & ~(CMA_PB_NR - 1), w;
	unsigned long got_wins[8];
	u8 av, sv, lb;
	int b, got = 0, i;

	pb_peek(win, &av, &sv, &lb);
	if (av | sv | lb)
		return -EBUSY;		/* siblings guarded: limbo/exchange path */
	for (b = 0; b < CMA_SUBBLKS; b++) {
		w = base + ((unsigned long)b << PAGE_ORDER);
		if (w == win)
			continue;
		if (!pfn_valid(w) ||
		    kapi.k_alloc_contig_range(w, w + (1UL << PAGE_ORDER),
					      GFP_KERNEL | __GFP_NOWARN)) {
			for (i = 0; i < got; i++)
				cma_span_free_to_buddy(got_wins[i],
						       1UL << PAGE_ORDER);
			return -EAGAIN;
		}
		got_wins[got++] = w;
	}
	return 0;
}

/*
 * §6 Phase R consumer: feed one swept-and-assembled 2MB window (singles-held)
 * to the reservoir. Whole block assemblable -> flip it in directly, never
 * transiting the pool. Not assemblable (guarded or straggler siblings) ->
 * park in limbo up to the exchange capacity. Anything else -> free back.
 * Returns 0 consumed, -ENOSPC floor (caller stops Phase R), -EIO systemic,
 * -EAGAIN window freed back.
 */
static int cma_sweep_window_to_reservoir(unsigned long win)
{
	unsigned long base = win & ~(CMA_PB_NR - 1);
	int ret;

	mutex_lock(&cma_mutex);
	if (!cma_floor_ok(1)) {
		mutex_unlock(&cma_mutex);
		cma_span_free_to_buddy(win, 1UL << PAGE_ORDER);
		return -ENOSPC;
	}
	if (CMA_SUBBLKS == 1) {
		ret = cma_block_commit(base);	/* window IS the block */
		if (ret == -EIO)
			cma_capable = false;
		mutex_unlock(&cma_mutex);
		if (ret)
			cma_span_free_to_buddy(win, 1UL << PAGE_ORDER);
		return ret ? (ret == -EIO ? -EIO : -EAGAIN) : 0;
	}

	ret = cma_try_complete_block(win);
	if (ret == 0) {
		ret = cma_block_commit(base);	/* all windows singles-held */
		if (ret == -EIO)
			cma_capable = false;
		mutex_unlock(&cma_mutex);
		if (ret)
			cma_span_free_to_buddy(base, CMA_PB_NR);
		return ret ? (ret == -EIO ? -EIO : -EAGAIN) : 0;
	}
	/* stray: park for the exchange if it is worth holding */
	if (READ_ONCE(limbo_n) < cma_limbo_intake_cap()) {
		struct page *head = pfn_to_page(win);

		rebuild_order9_compound(head);
		if (limbo_add(head)) {
			pb_track(win, PB_LIMBO, 0);
			mutex_unlock(&cma_mutex);
			return 0;
		}
		/* raced full: back to singles and out */
		cma_span_make_singles(win, 1UL << PAGE_ORDER);
	}
	mutex_unlock(&cma_mutex);
	cma_span_free_to_buddy(win, 1UL << PAGE_ORDER);
	return -EAGAIN;
}

/* Victim search for the Phase Q seat swap: the least-complete pageblock
 * (fewest guarded sub-blocks) other than @exclude that still has an avail
 * member. Returns one avail member's pfn, or 0. Complete blocks are never
 * victims - they are the cma_able capital Phase Q exists to grow. */
static unsigned long pb_find_worst_avail(unsigned long exclude)
{
	unsigned long flags, best_pfn = 0;
	unsigned int i, best_pop = 0xff;

	for (i = 0; i < PB_HASH_MAX; i++) {
		unsigned long pb;
		unsigned int pop;
		u8 av, guard;

		if ((i & 0x3ff) == 0)
			cond_resched();
		raw_spin_lock_irqsave(&pb_lock, flags);
		av = pb_nodes[i].avail_mask;
		guard = av | pb_nodes[i].served_mask | pb_nodes[i].limbo_mask;
		pb = pb_nodes[i].pb;
		raw_spin_unlock_irqrestore(&pb_lock, flags);
		if (!av || pb == exclude || guard == pb_full_mask())
			continue;
		pop = pb_popcount8(guard);
		if (pop < best_pop) {
			best_pop = pop;
			best_pfn = (pb << cma_pb_order) +
				   ((unsigned long)__ffs(av) << PAGE_ORDER);
			if (pop == 1)
				break;	/* cannot get worse */
		}
	}
	return best_pfn;
}

/*
 * §6 Phase Q (quality): counts are met but some avail sub-blocks sit in
 * pageblocks the module will never own whole (foreign stragglers). Upgrade:
 * targeted-grab the missing windows of partly-owned blocks; each success
 * REALLY frees one least-complete stray (process context may genuinely free)
 * and seats the new sibling instead - pool size unchanged, cma_able rising.
 * Also sweeps the limbo pool. Returns true if anything improved; the caller
 * stops on false ("quality converged"). SUBBLKS > 1 only.
 */
static bool cma_phase_q(void)
{
	unsigned int i;
	int budget = 64;
	bool progress = false;

	if (!pb_enabled())
		return false;
	for (i = 0; i < PB_HASH_MAX && budget > 0; i++) {
		unsigned long flags, pb, base, w;
		u8 av, sv, lb, missing, full = pb_full_mask();
		int b;

		if (!atomic_read(&acquire_running))
			break;
		if ((i & 0xff) == 0)
			cond_resched();
		raw_spin_lock_irqsave(&pb_lock, flags);
		av = pb_nodes[i].avail_mask;
		sv = pb_nodes[i].served_mask;
		lb = pb_nodes[i].limbo_mask;
		pb = pb_nodes[i].pb;
		raw_spin_unlock_irqrestore(&pb_lock, flags);
		if (!av || (u8)(av | sv | lb) == full)
			continue;
		missing = full & ~(av | sv | lb);
		base = pb << cma_pb_order;
		for (b = 0; b < CMA_SUBBLKS && budget > 0; b++) {
			struct page *head, *victim;
			unsigned long vpfn;

			if (!(missing & (1U << b)))
				continue;
			w = base + ((unsigned long)b << PAGE_ORDER);
			if (!pfn_valid(w) || cma_pb_mt(w) == migrate_cma_val)
				break;
			if (kapi.k_alloc_contig_range(w, w + (1UL << PAGE_ORDER),
						      GFP_KERNEL | __GFP_NOWARN)) {
				budget--;
				continue;
			}
			head = pfn_to_page(w);
			rebuild_order9_compound(head);
			/* seat swap: really free one least-complete stray */
			vpfn = pb_find_worst_avail(pb);
			victim = vpfn ? pool_extract_pfn(vpfn) : NULL;
			if (victim) {
				__free_pages(victim, PAGE_ORDER);
				if (!pool_push(head))
					__free_pages(head, PAGE_ORDER);
				progress = true;
			} else if (!pool_push_grow(head)) {
				__free_pages(head, PAGE_ORDER);
				budget = 0;	/* no seats, no victims: done */
			} else {
				progress = true;
			}
			budget--;
		}
	}
	mutex_lock(&cma_mutex);
	cma_limbo_process();	/* 順手: promote/complete/evict strays */
	mutex_unlock(&cma_mutex);
	return progress;
}

/* Accept the first-block +512 accounting check when at least 3/4 of it shows:
 * the per-cpu vmstat fold our order-8 frees force can carry up to ~125 pages
 * of unrelated residue either way (stat threshold cap), while the failure
 * being tested for - migrate_cma_val not actually meaning MIGRATE_CMA - shows
 * a delta of ~0. 384 splits those cleanly. */
#define CMA_VERIFY_DELTA_MIN	(3L << (PAGE_ORDER - 2))
#define CMA_VERIFY_CAND_MAX	8

/*
 * First-block verification (§4): the ONE place the module proves, on live
 * memory, that its three preflight-derived beliefs hold before it will label
 * anything CMA: (a) pageblock_order_val is the kernel's real pageblock order,
 * (b) migrate_cma_val is really MIGRATE_CMA (accounting moves with it),
 * (c) CONTIG_RANGE_CMA can grab a block back. Runs UNCONDITIONALLY at every
 * init (cma_boot_build) and nowhere else - its outcome folds into cma_capable
 * as a boot constant, so there is no first-run runtime state to manage.
 *
 * The span is TWO pageblocks at DOUBLE alignment, fully held. That shape is
 * what makes an off-by-one-level pageblock_order_val belief SAFE to probe: if
 * the belief is one too small, the flip covers the whole held span and the
 * base+PB_NR readback (expected MOVABLE, reads CMA) catches it; if one too
 * big, the flip covers only the first real block and the base+PB_NR-1
 * readback (expected CMA, reads MOVABLE) catches it. Either way nothing
 * outside memory we hold was ever relabeled, and the flip-back restores it
 * whole. One allocation of order pb+1 provides the span where the kernel
 * allows it; otherwise (order-10 pageblocks on 6.1, whose max alloc order is
 * 10) one pageblock is allocated and its 2x-alignment sibling is grabbed with
 * a movable-mode contig range.
 *
 * Candidates may carry ANY whitelisted label ({0,1,2}, like every other
 * flip): the boundary checks compare against RECORDED pre-values, not against
 * a hardcoded MOVABLE, so steal residue does not defeat them. This matters in
 * the field: after the module itself has churned GFP_KERNEL blocks through
 * buddy for a while, movable high-order allocations come back with UNMOVABLE
 * residue labels almost every time (measured: a runtime enable on the OnePlus
 * exhausted eight strictly-MOVABLE candidates and silently fell back to v9).
 * A candidate with a non-whitelisted label is HELD until the hunt ends:
 * freeing it at once would just hand the same block back on the next try.
 *
 * On success the span becomes reservoir blocks #1 and #2 (zero waste). On a
 * pre-write mismatch: nothing was written, swap candidates (bounded, then
 * pr_warn + feature stays unverified). On any post-write failure: systemic -
 * restore the recorded label if still clean, otherwise ABANDON the first
 * block CMA-labeled (its label and freelist stay mutually consistent; the
 * memory remains app-usable movable; it merely refuses unmovable allocations
 * until reboot), pr_err, feature off.
 *
 * Returns 0 verified; -EAGAIN candidates exhausted; -EIO systemic failure.
 * cma_mutex held.
 */
static int cma_verify_first_span(void)
{
	const gfp_t mov_gfp = GFP_HIGHUSER_MOVABLE | __GFP_NOWARN | __GFP_RETRY_MAYFAIL;
	const unsigned long pb_nr = CMA_PB_NR, span_nr = 2 * CMA_PB_NR;
	unsigned long rej_pfn[CMA_VERIFY_CAND_MAX];
	unsigned long rej_nr[CMA_VERIFY_CAND_MAX];
	unsigned long base = 0;
	int nrej = 0, tries, ret = -EAGAIN;
	int pre0, pre2;
	struct zone *z;
	long before, delta;

	for (tries = 0; tries < CMA_VERIFY_CAND_MAX && nrej < CMA_VERIFY_CAND_MAX;
	     tries++) {
		struct page *p;
		unsigned long pfn, cand;

		if (cma_pb_order + 1 <= GH_MAX_ALLOC_ORDER) {
			/* one naturally 2x-aligned allocation = the whole span */
			p = alloc_pages(mov_gfp, cma_pb_order + 1);
			if (!p)
				break;
			cand = page_to_pfn(p);
			if (!cma_mt_flippable(cma_pb_mt(cand)) ||
			    !cma_mt_flippable(cma_pb_mt(cand + pb_nr))) {
				cma_span_make_singles(cand, span_nr);
				rej_pfn[nrej] = cand;
				rej_nr[nrej++] = span_nr;
				continue;
			}
			cma_span_make_singles(cand, span_nr);
			base = cand;
			break;
		}
		/*
		 * pb+1 exceeds the alloc limit (order-10 pageblocks, 6.1): hold
		 * single pageblocks one by one and PAIR any two that are each
		 * other's 2x-alignment buddy - the stash doubles as the pairing
		 * pool, both halves are then already held and NO foreign memory
		 * is ever contig-grabbed. Adjacent blocks freed together (a
		 * prior rmmod, a balloon exit) sit near each other on the
		 * freelists, so consecutive allocations pair quickly. Fall back
		 * to contig-grabbing the not-yet-held sibling only when it
		 * still sits free/movable outside (measured: on churned memory
		 * that grab alone fails eight times out of eight - pairing is
		 * what makes a runtime-insmod verification viable at pb=10).
		 */
		p = alloc_pages(mov_gfp, cma_pb_order);
		if (!p)
			break;
		pfn = page_to_pfn(p);
		cand = pfn & ~(span_nr - 1);	/* 2x-aligned span base */
		{
			unsigned long sib = (cand == pfn) ? pfn + pb_nr
							  : cand;
			int j, pair = -1;

			for (j = 0; j < nrej; j++)
				if (rej_nr[j] == pb_nr && rej_pfn[j] == sib) {
					pair = j;
					break;
				}
			if (pair >= 0 && cma_mt_flippable(cma_pb_mt(pfn)) &&
			    cma_mt_flippable(cma_pb_mt(sib))) {
				/* both halves held: consume the stash entry */
				rej_pfn[pair] = rej_pfn[nrej - 1];
				rej_nr[pair] = rej_nr[nrej - 1];
				nrej--;
				cma_span_make_singles(pfn, pb_nr);
				base = cand;	/* stashed half is singles already */
				break;
			}
			if (pfn_valid(sib) && pfn_valid(sib + pb_nr - 1) &&
			    page_zone(pfn_to_page(sib)) == page_zone(p) &&
			    cma_mt_flippable(cma_pb_mt(pfn)) &&
			    cma_mt_flippable(cma_pb_mt(sib)) &&
			    !kapi.k_alloc_contig_range(sib, sib + pb_nr,
					GFP_KERNEL | __GFP_NOWARN)) {
				cma_span_make_singles(cand, span_nr);
				base = cand;
				break;
			}
			cma_span_make_singles(pfn, pb_nr);
			rej_pfn[nrej] = pfn;
			rej_nr[nrej++] = pb_nr;
		}
	}
	if (!base)
		goto out;	/* -EAGAIN: no flippable span obtainable */

	/* Pre-write reads (non-destructive): RECORD the boundary labels the
	 * post-write checks compare against. pre0 is base's REAL block's label
	 * (whatever the real pageblock order is) - the restore value. */
	pre0 = cma_pb_mt(base);
	pre2 = cma_pb_mt(base + pb_nr);
	if (!cma_mt_flippable(pre0) ||
	    !cma_mt_flippable(cma_pb_mt(base + pb_nr - 1)) ||
	    !cma_mt_flippable(pre2)) {
		cma_span_free_to_buddy(base, span_nr);
		goto out;	/* raced relabel: treat as exhausted */
	}

	kapi.k_set_pageblock_migratetype(pfn_to_page(base), migrate_cma_val);

	/* Post-write boundary reads: base+PB_NR-1 catches a too-BIG order
	 * belief (the real flip fell short of our believed block), base+PB_NR
	 * catches a too-SMALL one (the real flip overflowed into the second
	 * half - its label must still read exactly what was recorded). */
	if (cma_pb_mt(base + pb_nr - 1) != migrate_cma_val ||
	    cma_pb_mt(base + pb_nr) != pre2) {
		pr_err("cma: pageblock_order_val=%d verification FAILED (post-write mt %d/%d, expected %d/%d) - feature off\n",
		       pageblock_order_val,
		       cma_pb_mt(base + pb_nr - 1), cma_pb_mt(base + pb_nr),
		       migrate_cma_val, pre2);
		kapi.k_set_pageblock_migratetype(pfn_to_page(base), pre0);
		cma_span_free_to_buddy(base, span_nr);
		ret = -EIO;
		goto out;
	}

	/* Semantic check: one 2MB freed into the flipped block must move the
	 * zone's CMA-free accounting by (about) +512 - proving migrate_cma_val
	 * IS the accounting migratetype, not merely a writable label. */
	z = page_zone(pfn_to_page(base));
	before = (long)zone_page_state(z, NR_FREE_CMA_PAGES);
	cma_span_free_to_buddy(base, 1UL << PAGE_ORDER);
	delta = (long)zone_page_state(z, NR_FREE_CMA_PAGES) - before;
	if (delta < CMA_VERIFY_DELTA_MIN) {
		pr_err("cma: migrate_cma_val=%d verification FAILED (CMA-free delta %ld after +512 free) - feature off\n",
		       migrate_cma_val, delta);
		goto abandon_first;
	}

	/* End-to-end CMA-mode grab of exactly what was freed. */
	if (kapi.k_alloc_contig_range_cma(base, base + (1UL << PAGE_ORDER),
					  GFP_KERNEL | __GFP_NOWARN)) {
		pr_err("cma: CONTIG_RANGE_CMA grab-back verification FAILED - feature off\n");
		goto abandon_first;
	}

	/* Verified. First half (fully held again) is reservoir block #1... */
	cma_span_free_to_buddy(base, pb_nr);
	cma_blocks[cma_blocks_n++] = base;
	/* ...second half goes through the standard commit = block #2 (zero
	 * waste). Its label was just screened flippable, so -EPERM is all but
	 * impossible; -EIO here would mean the setter works at base but not at
	 * base+pb_nr - treat as systemic and undo block #1 too. */
	if (cma_block_commit(base + pb_nr) != 0) {
		cma_span_free_to_buddy(base + pb_nr, pb_nr);
		if (cma_block_demolish(base) == 0)
			cma_blocks_n--;
		ret = -EIO;
		goto out;
	}
	ret = 0;
	pr_info("cma: first-block verification passed (pb_order=%d migrate_cma=%d pfn=%lx)\n",
		cma_pb_order, migrate_cma_val, base);
	goto out;

abandon_first:
	/*
	 * 2MB of the first block already sits on its labeled freelist, so an
	 * unflip would break label/freelist consistency. Leave the label, free
	 * the rest of the first block into it (consistent), free the still-
	 * MOVABLE second half normally, and forget the block. Worst case one
	 * stray CMA-labeled pageblock until reboot - inert and app-usable.
	 */
	pr_err("cma: abandoning one CMA-labeled pageblock at pfn=%lx\n", base);
	cma_span_free_to_buddy(base + (1UL << PAGE_ORDER),
			       pb_nr - (1UL << PAGE_ORDER));
	cma_span_free_to_buddy(base + pb_nr, pb_nr);
	ret = -EIO;
out:
	while (nrej-- > 0)
		cma_span_free_to_buddy(rej_pfn[nrej], rej_nr[nrej]);
	/* Candidates exhausted is a QUIET failure mode by design (nothing was
	 * written), but the operator must be able to see why an enable flow
	 * silently stayed on the v9 path - the exact trap a runtime enable on
	 * a churned system fell into before this warning existed. */
	if (ret == -EAGAIN)
		pr_warn("cma: first-block verification found no usable candidate span (feature stays unverified)\n");
	return ret;
}

#define CMA_BUILD_REJECTS	16

/*
 * Build the reservoir toward @target_blocks pageblocks (the first-block
 * verification has already run at init - see cma_boot_build). Per block, the
 * cheap path: allocate one whole pageblock, whitelist, flip, single readback,
 * free in place.
 *
 * The per-block allocation is deliberately UNMOVABLE (GFP_KERNEL), not
 * movable: a movable request is ALLOC_CMA-eligible, and once the growing
 * reservoir dominates the zone's free memory the allocator starts handing the
 * builder its own CMA blocks back (label 3 -> whitelist reject), starving the
 * build - measured on the OnePlus 6.12: movable builds stalled at 1684/3840
 * blocks with the reject stash full of our own pages. GFP_KERNEL can never be
 * served from a CMA freelist, at the cost of an UNMOVABLE steal-residue label
 * on the block - which is exactly what the {0,1,2} whitelist exists to accept
 * (the flip is safe regardless of label because every page is held).
 *
 * Rejected candidates (foreign-CMA/highatomic/CHP labels) are held to the end
 * - freeing one immediately would hand the same block right back. Every early
 * stop is a pr_warn with the reason (§10). cma_mutex held, process context.
 */
static void cma_reservoir_build(int target_blocks, unsigned long reserve_pages)
{
	const gfp_t blk_gfp = GFP_KERNEL | __GFP_NOWARN | __GFP_RETRY_MAYFAIL;
	struct page *rej[CMA_BUILD_REJECTS];
	const char *stop = NULL;
	int nrej = 0, i, ret;

	while (cma_blocks_n < target_blocks) {
		struct page *p;
		unsigned long base;
		int mt;

		if (!cma_floor_ok(1)) {
			stop = "headroom floor (cma_reservoir_floor_mb)";
			break;
		}
		/* Leave @reserve_pages of FREE non-CMA memory for whoever runs
		 * after the build (the boot prefill): the build itself consumes
		 * exactly that pool, and si_mem_available can't see the
		 * difference (measured: an unreserved build left the prefill
		 * 0/256 with MemAvailable still reading gigabytes). */
		if (reserve_pages &&
		    cma_free_noncma_pages() < reserve_pages + 2 * CMA_PB_NR) {
			stop = "leaving free memory for the pool prefill";
			break;
		}
		p = alloc_pages(blk_gfp, cma_pb_order);
		if (!p) {
			stop = "pageblock allocation failed";
			break;
		}
		base = page_to_pfn(p);
		mt = cma_pb_mt(base);
		if (!cma_mt_flippable(mt)) {	/* §9 whitelist */
			rej[nrej++] = p;
			if (nrej >= CMA_BUILD_REJECTS) {
				stop = "too many unflippable candidates";
				break;
			}
			continue;
		}
		cma_span_make_singles(base, CMA_PB_NR);
		ret = cma_block_commit(base);
		if (ret) {
			/* label can't change while fully held, so this is the
			 * -EIO readback path: systemic. Free the held block
			 * (label restored/unchanged) and stop the feature. */
			cma_span_free_to_buddy(base, CMA_PB_NR);
			cma_capable = false;
			stop = "flip readback failed";
			break;
		}
		cond_resched();
	}

	for (i = 0; i < nrej; i++)
		__free_pages(rej[i], cma_pb_order);
	if (stop)
		pr_warn("cma: reservoir build stopped: %s (%d/%d blocks)\n",
			stop, cma_blocks_n, target_blocks);
}

/*
 * Boot-time capability latch, want alignment and reservoir build - called
 * from module_init AFTER kapi_init and BEFORE the pool prefill (§10):
 * reservoir blocks are whole pageblocks and want the cleanest memory this
 * boot will ever offer; the prefill's order-9 allocations then draw from what
 * remains. GFP_KERNEL prefill can never draw from the blocks just committed
 * (unmovable allocations cannot enter CMA freelists - the property the whole
 * reservoir stands on).
 *
 * The first-block verification runs HERE, EVERY init, regardless of whether a
 * reservoir was asked for - deliberately: capability becomes a boot constant
 * settled while memory is at its cleanest, and no runtime enable flow ever
 * needs a lazy first-run verification again (a runtime verify on churned
 * memory was measured failing candidate hunts that boot-time trivially
 * passes). The two blocks the protocol seeds cost ~one contig grab to trim
 * back out when no reservoir is wanted.
 */
static void cma_boot_build(void)
{
	int target_blocks, ret;

	cma_capable = kapi_can_cma();

	/* moveable_to_cma status (RO): does the vendor kernel already redirect ALL
	 * movable allocations to CMA (restrict_cma_redirect resolved AND currently
	 * false)? Just an observation of the key, independent of cma_capable. When 1,
	 * both levers are redundant and their writes become accepted no-ops. */
	mtc_vender_already_allowed =
		(kapi.k_restrict_cma_redirect &&
		 !static_key_enabled(kapi.k_restrict_cma_redirect)) ? 1 : 0;

	if (pool_want_with_cma < 0)
		pool_want_with_cma = 0;
	if (!cma_capable) {
		if (pool_want_with_cma)
			pr_warn("cma: pool_want_with_cma=%d requested but unavailable (syms=%d migrate_cma_val=%d pageblock_order_val=%d) - v9 path\n",
				pool_want_with_cma,
				!!(kapi.k_set_pageblock_migratetype &&
				   kapi.k_get_pfnblock_flags_mask &&
				   kapi.k_alloc_contig_range_cma),
				migrate_cma_val, pageblock_order_val);
		pool_want_with_cma = 0;
		return;
	}
	cma_pb_order = pageblock_order_val;
	if (pb_enabled()) {			/* arm the completeness index */
		unsigned long flags;

		raw_spin_lock_irqsave(&pb_lock, flags);
		pb_reset_locked();
		raw_spin_unlock_irqrestore(&pb_lock, flags);
	}

	/* Clamp + align both targets (§2/§5): want <= with_cma <= size_max,
	 * whole pageblocks each while enabled (0 stays the no-reservoir value). */
	pool_want = cma_align_2mb(pool_want);
	if (pool_want_with_cma > 0) {
		if (pool_want_with_cma < pool_want)
			pool_want_with_cma = pool_want;
		if (pool_want_with_cma > pool_size_max)
			pool_want_with_cma = pool_size_max;
		pool_want_with_cma = cma_align_2mb(pool_want_with_cma);
	}
	target_blocks = pool_want_with_cma > 0 ?
		(pool_want_with_cma - pool_want) / CMA_SUBBLKS : 0;

	mutex_lock(&cma_mutex);
	ret = cma_floor_ok(2) ? cma_verify_first_span() : -EAGAIN;
	if (ret) {
		cma_capable = false;		/* boot constant: no retry */
		cma_pb_order = -1;
		if (pool_want_with_cma)
			pr_warn("cma: verification failed (%d), pool_want_with_cma=%d falls back to v9\n",
				ret, pool_want_with_cma);
		pool_want_with_cma = 0;
		mutex_unlock(&cma_mutex);
		return;
	}
	if (cma_blocks_n < target_blocks) {
		/* Reserve the prefill's budget (pool_want order-9 pages) plus
		 * a 64MB compaction slack in FREE non-CMA memory, so building
		 * the reservoir first cannot starve the prefill that follows. */
		cma_reservoir_build(target_blocks,
				    ((unsigned long)pool_want << PAGE_ORDER) +
				    (64UL << (20 - PAGE_SHIFT)));
	}
	/* Verification seeded two blocks: trim whatever exceeds the target
	 * (all of it when no reservoir is wanted) - the fresh blocks are fully
	 * free, so the grab-backs are instant. */
	if (cma_blocks_n > target_blocks)
		cma_reservoir_demolish(target_blocks * CMA_SUBBLKS, false);
	mutex_unlock(&cma_mutex);

	if (cma_blocks_n)
		pr_info("cma: reservoir ready: %d pageblock(s) x %d MB = %d MB (target %d x 2MB)\n",
			cma_blocks_n, 2 * CMA_SUBBLKS,
			cma_pool_cma_2mb() * 2,
			pool_want_with_cma - pool_want);

	/* Apply insmod-time lever desires now that cma_capable and the vendor status
	 * are settled (reached only on the cma_capable success path; runtime sysfs
	 * writes apply immediately instead). Both are inert when the vendor already
	 * redirects movable->CMA, or when a lever's kernel support is missing. The
	 * gfp hook is registered later in module_init; setting the soft flag here
	 * just pre-arms it. */
	if (!mtc_vender_already_allowed) {
		if (mtc_restrict_want >= 0 && kapi_can_restrict_flip())
			mtc_restrict_set_state(mtc_restrict_want == 0);	/* knob 0 => restrict on */
		if (mtc_gfp_hook_want)
			WRITE_ONCE(cma_bypass_enabled, true);
	}
}

/*
 * pool_want_with_cma (§5): the total guardianship target.
 *   write big:   target only - construction is exclusively acquire's job.
 *   write small: clamped to pool_want below; excess reservoir is demolished
 *                NOW, emptiest blocks first.
 *   write 0:     disable - demolish the whole reservoir; capability and
 *                verification results survive, so re-enabling does not
 *                re-test.
 * Writing pool_want above with_cma pulls with_cma along (cma_want_follow);
 * the reverse direction clamps here. Boot-time writes only record: init
 * clamps, aligns and builds.
 */
static int pool_want_with_cma_set(const char *val, const struct kernel_param *kp)
{
	int v;

	if (kstrtoint(val, 10, &v))
		return -EINVAL;
	if (v < 0)
		v = 0;

	if (!READ_ONCE(pool_ready)) {
		WRITE_ONCE(pool_want_with_cma, v);
		return 0;
	}
	if (v == 0) {
		mutex_lock(&cma_mutex);
		WRITE_ONCE(pool_want_with_cma, 0);
		cma_reservoir_demolish(0, true);
		if (cma_blocks_n)
			pr_warn("cma: %d block(s) resisted demolition, still reservoir\n",
				cma_blocks_n);
		mutex_unlock(&cma_mutex);
		return 0;
	}
	if (!cma_capable)
		return -ENOSYS;

	v = max(v, READ_ONCE(pool_want));
	v = min(v, pool_size_max);
	v = cma_align_2mb(v);
	mutex_lock(&cma_mutex);
	WRITE_ONCE(pool_want_with_cma, v);
	cma_reservoir_demolish(v, false);	/* no-op unless shrinking */
	mutex_unlock(&cma_mutex);
	return 0;
}

static int pool_want_with_cma_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(pool_want_with_cma));
}

static const struct kernel_param_ops pool_want_with_cma_ops = {
	.set = pool_want_with_cma_set,
	.get = pool_want_with_cma_get,
};
module_param_cb(pool_want_with_cma, &pool_want_with_cma_ops, NULL, 0600);
MODULE_PARM_DESC(pool_want_with_cma,
	"Total guardianship target in 2MB pages (pool + CMA reservoir); 0 = reservoir off (v9 behavior)");

static int pool_cma_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", cma_pool_cma_2mb());
}

static const struct kernel_param_ops pool_cma_ops = {
	.get = pool_cma_get,
};
module_param_cb(pool_cma, &pool_cma_ops, NULL, 0400);
MODULE_PARM_DESC(pool_cma, "CMA reservoir size in 2MB-page equivalents (read-only)");

/*
 * moveable_to_cma: expose the two levers + status to userspace. Precedence for
 * the two writable levers:
 *   insmod-time (pool_ready=0): record the desire; cma_boot_build() applies it
 *                               once cma_capable + vender status are settled.
 *   !cma_capable:               -ENOSYS (whole feature off).
 *   vender_already_allowed:     accepted no-op (vendor already redirects, both
 *                               levers redundant) - reads still show real state.
 *   lever unsupported:          -ENOSYS (restrict key / gfp hook absent).
 */
static int mtc_vender_already_allowed_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", mtc_vender_already_allowed);
}
static const struct kernel_param_ops mtc_vender_ops = {
	.get = mtc_vender_already_allowed_get,
};
module_param_cb(moveable_to_cma_vender_already_allowed, &mtc_vender_ops, NULL, 0400);
MODULE_PARM_DESC(moveable_to_cma_vender_already_allowed,
	"1 = vendor kernel already redirects all movable allocations to CMA (restrict_cma_redirect resolved AND false); then both levers' writes are no-ops (read-only)");

/* The inverse of the kernel's restrict_cma_redirect static key, phrased as "is
 * the redirect enabled" so the value tracks the outcome, not the switch:
 *   1 = restrict DISABLED  = plain movable CAN migrate into CMA;
 *   0 = restrict active    = it cannot.
 * The vendor already allowing (key false) therefore reads back 1. Write 1 to
 * open movable->CMA globally. */
static int mtc_restrict_set(const char *val, const struct kernel_param *kp)
{
	int v;

	if (kstrtoint(val, 10, &v))
		return -EINVAL;
	v = !!v;				/* 1 = redirect enabled (restrict off) */

	if (!READ_ONCE(pool_ready)) {		/* insmod-time: record; init applies */
		mtc_restrict_want = v;
		return 0;
	}
	if (!cma_capable)
		return -ENOSYS;
	if (mtc_vender_already_allowed)		/* redundant: accepted no-op */
		return 0;
	if (!kapi_can_restrict_flip())
		return -ENOSYS;
	return mtc_restrict_set_state(v == 0);	/* restrict on iff redirect off; may sleep */
}

static int mtc_restrict_get(char *buf, const struct kernel_param *kp)
{
	int st = mtc_restrict_get_state();	/* -1 unavail, else key state (1=restrict on) */

	return sysfs_emit(buf, "%d\n", st < 0 ? -1 : !st);
}

static const struct kernel_param_ops mtc_restrict_ops = {
	.set = mtc_restrict_set,
	.get = mtc_restrict_get,
};
module_param_cb(moveable_to_cma_restrict_cma_redirect_disabled, &mtc_restrict_ops, NULL, 0600);
MODULE_PARM_DESC(moveable_to_cma_restrict_cma_redirect_disabled,
	"restrict_cma_redirect DISABLED (= is movable->CMA redirect on): write 1=disable restrict (open movable->CMA globally), 0=restrict on (block). Read: 1=can migrate in, 0=blocked, -1=unresolvable. Vendor-already-allowed reads 1. No-op if vender_already_allowed; -ENOSYS if CMA/key unavailable");

/* The __GFP_CMA bypass hook lever: arm/disarm the soft flag the registered
 * probe checks (see cma_bypass_wants). Lets page cache / mTHP anon consume the
 * reservoir without the global restrict flip. */
static int mtc_gfp_hook_set(const char *val, const struct kernel_param *kp)
{
	int v;

	if (kstrtoint(val, 10, &v))
		return -EINVAL;
	v = !!v;

	if (!READ_ONCE(pool_ready)) {		/* insmod-time: record; init applies */
		mtc_gfp_hook_want = v;
		return 0;
	}
	if (!cma_capable)
		return -ENOSYS;
	if (mtc_vender_already_allowed)		/* redundant: accepted no-op */
		return 0;
	if (!kapi_can_gfp_hook())
		return -ENOSYS;
	WRITE_ONCE(cma_bypass_enabled, v);
	return 0;
}

static int mtc_gfp_hook_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(cma_bypass_enabled) ? 1 : 0);
}

static const struct kernel_param_ops mtc_gfp_hook_ops = {
	.set = mtc_gfp_hook_set,
	.get = mtc_gfp_hook_get,
};
module_param_cb(moveable_to_cma_gfp_cma_hook, &mtc_gfp_hook_ops, NULL, 0600);
MODULE_PARM_DESC(moveable_to_cma_gfp_cma_hook,
	"__GFP_CMA bypass hook: write 1=arm 0=disarm (lets page cache / mTHP anon consume the reservoir). Read=armed state. No-op if vender_already_allowed; -ENOSYS if CMA/hook unavailable");

static int pool_avail_cma_able_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", cma_avail_cma_able_2mb());
}

static const struct kernel_param_ops pool_avail_cma_able_ops = {
	.get = pool_avail_cma_able_get,
};
module_param_cb(pool_avail_cma_able, &pool_avail_cma_able_ops, NULL, 0400);
MODULE_PARM_DESC(pool_avail_cma_able,
	"Avail pool pages flippable to CMA as whole pageblocks (read-only)");

/*
 * §11 cma_usage: GUI occupancy view of the reservoir. The module is the ONLY
 * party that can produce it - the reservoir is no registered struct cma (so
 * debugfs cannot see it) and global CmaFree mixes in the vendor carveouts.
 * Racy-by-design snapshot (pages move while we scan; GUI use tolerates it),
 * buddy-order / folio_nr_pages jumps + cond_resched keep it cheap, and a ~1s
 * cache keeps a polling GUI from rescanning gigabytes. used_anon_mb doubles
 * as the acquire cost estimate: roughly how much must migrate out before a VM
 * start.
 */
static unsigned long cma_usage_v[5];	/* pages: total/free/used/anon/file */
static int cma_usage_b[3];		/* blocks: free/partial/full */
static unsigned long cma_usage_jiffies;	/* 0 = no snapshot yet */

static int cma_usage_get(char *buf, const struct kernel_param *kp)
{
	unsigned long free_p = 0, anon_p = 0, file_p = 0, total_p;
	int bfree = 0, bpart = 0, bfull = 0, i, n;

	mutex_lock(&cma_mutex);
	if (cma_usage_jiffies &&
	    time_before(jiffies, cma_usage_jiffies + HZ))
		goto emit;		/* ~1s cache: numbers, not a string -
					 * snprintf would be a new undefined
					 * symbol; sysfs_emit is already ours */

	for (i = 0; i < cma_blocks_n; i++) {
		unsigned long pfn = cma_blocks[i];
		unsigned long end = pfn + CMA_PB_NR, bf = 0;

		while (pfn < end) {
			struct page *p = pfn_to_page(pfn);
			unsigned long step = 1;

			if (PageBuddy(p)) {
				unsigned int order =
					(unsigned int)READ_ONCE(page_private(p));

				if (order > GH_MAX_ALLOC_ORDER)
					order = 0;	/* racy read: distrust */
				step = min(1UL << order, end - pfn);
				bf += step;
			} else if (page_count(p) == 0) {
				bf++;		/* pcp-parked / in-flight free */
			} else {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0)
				struct folio *folio = page_folio(p);
				unsigned long fn = folio_nr_pages(folio);

				step = fn ? min(fn, end - pfn) : 1;
				if (folio_test_anon(folio))
					anon_p += step;
				else if (folio_test_lru(folio))
					file_p += step;
#else
				/* pre-folio kernels cannot run the reservoir
				 * (no Gunyah, feature compile-gated) - keep
				 * the scan compiling with page-flag spellings */
				struct page *head = compound_head(p);
				unsigned long fn = compound_nr(head);

				step = fn ? min(fn, end - pfn) : 1;
				if (PageAnon(head))
					anon_p += step;
				else if (PageLRU(head))
					file_p += step;
#endif
			}
			pfn += step;
		}
		free_p += bf;
		if (bf >= CMA_PB_NR)
			bfree++;
		else if (bf == 0)
			bfull++;
		else
			bpart++;
		cond_resched();
	}
	/* feature-off / empty: no shift by an unvalidated order */
	total_p = cma_blocks_n ? (unsigned long)cma_blocks_n << cma_pb_order : 0;
	cma_usage_v[0] = total_p;
	cma_usage_v[1] = free_p;
	cma_usage_v[2] = total_p - min(free_p, total_p);
	cma_usage_v[3] = anon_p;
	cma_usage_v[4] = file_p;
	cma_usage_b[0] = bfree;
	cma_usage_b[1] = bpart;
	cma_usage_b[2] = bfull;
	cma_usage_jiffies = jiffies ? jiffies : 1;
emit:
	n = sysfs_emit(buf,
		       "reservoir_mb=%lu\nfree_mb=%lu\nused_mb=%lu\nused_anon_mb=%lu\nused_file_mb=%lu\nblocks_free=%d\nblocks_partial=%d\nblocks_full=%d\n",
		       cma_usage_v[0] >> (20 - PAGE_SHIFT),
		       cma_usage_v[1] >> (20 - PAGE_SHIFT),
		       cma_usage_v[2] >> (20 - PAGE_SHIFT),
		       cma_usage_v[3] >> (20 - PAGE_SHIFT),
		       cma_usage_v[4] >> (20 - PAGE_SHIFT),
		       cma_usage_b[0], cma_usage_b[1], cma_usage_b[2]);
	mutex_unlock(&cma_mutex);
	return n;
}

static const struct kernel_param_ops cma_usage_ops = {
	.get = cma_usage_get,
};
module_param_cb(cma_usage, &cma_usage_ops, NULL, 0400);
MODULE_PARM_DESC(cma_usage,
	"Reservoir occupancy snapshot: free/used/anon/file MB + block states (read-only, ~1s cached)");

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
		/* Whole-token match without strlen: a matched strncmp implies
		 * known[i] has >= tlen chars, so [tlen] is a valid read (the
		 * NUL iff the lengths match too). strlen(known[i]) here quietly
		 * became an out-of-line libcall on the 5.10 toolchain once the
		 * symbol list outgrew clang's unroll threshold - a new
		 * undefined symbol the ABI check forbids. */
		for (i = 0; i < ARRAY_SIZE(known); i++)
			if (!strncmp(p, known[i], tlen) &&
			    known[i][tlen] == '\0') {
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

	/* CMA reservoir setter/reader: resolved only on [6.1, 6.16). Below 6.1
	 * there is no Gunyah to serve; from 6.16 the migratetype rework
	 * (MIGRATE_ISOLATE becomes a standalone bit) changes pageblock-flag
	 * semantics out from under both symbols and 6.18 makes the setter
	 * static - so the feature auto-disables to the v9 path there, exactly
	 * like a failed resolution would. The CMA-mode contig shim additionally
	 * needs the preflight-supplied migratetype value. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0) && \
    LINUX_VERSION_CODE < KERNEL_VERSION(6, 16, 0)
	kapi.k_set_pageblock_migratetype = resolve_gated("set_pageblock_migratetype");
	kapi.k_get_pfnblock_flags_mask	 = resolve_gated("get_pfnblock_flags_mask");

	/* moveable_to_cma restrict lever: the static-key togglers (kprobe-resolved
	 * funcs) and the restrict_cma_redirect DATA symbol. The data symbol needs
	 * kallsyms_lookup_name (kprobe cannot target data); CONFIG_KALLSYMS_ALL=y on
	 * these GKI bases keeps it in kallsyms. Any miss just leaves the lever
	 * unavailable (kapi_can_restrict_flip() false, writes -ENOSYS). */
	kapi.k_kallsyms_lookup_name	 = resolve_kfunc("kallsyms_lookup_name");
	kapi.k_static_key_enable	 = resolve_gated("static_key_enable");
	kapi.k_static_key_disable	 = resolve_gated("static_key_disable");
	/* restrict_cma_redirect is not in kapi_abi.tsv (data symbol, unverifiable),
	 * so it has no preflight/disable_kapi entry: resolve it purely by name. The
	 * restrict lever stays opt-in (default untouched) and only ever flips on an
	 * explicit sysfs write, bounding the blast radius if a kernel ever carried a
	 * different symbol under this name. */
	if (kapi.k_kallsyms_lookup_name)
		kapi.k_restrict_cma_redirect = (struct static_key *)
			kapi.k_kallsyms_lookup_name("restrict_cma_redirect");
#endif
	kapi.k_alloc_contig_range_cma	 = (kraw.k_alloc_contig_range &&
					    migrate_cma_val >= 0) ?
						kapi_alloc_contig_range_cma_shim : NULL;

	if (disable_kapi && *disable_kapi)
		pr_info("kapi: ABI guard active, disable_kapi=\"%s\"\n", disable_kapi);
	pr_info("kapi: id1=%d id2=%d id3=%d A(sysreclaim)=%d drain=%d drop_slab=%d cma=%d restrict_flip=%d\n",
		kapi_can_v1(), kapi_can_sweep(), kapi_can_evict_b(),
		kapi_has_sys_reclaim(), !!kapi.k_lru_add_drain_all, !!kapi.k_drop_slab,
		kapi_can_cma(), kapi_can_restrict_flip());
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
	/* §6 pairing: the block just entered the index - grab its still-free
	 * siblings NOW, while buddy adjacency makes them likely free too
	 * (no-op on SUBBLKS == 1). */
	cma_grab_missing_siblings(page_to_pfn(p), CMA_SUBBLKS);
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
	while (atomic_read(&acquire_running) && cma_external_ok() &&
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
	if (served_contains(pfn))
		return false;			/* lent to a VM: stage-2 maps these pages */

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
	int got = 0, got_r = 0, i;
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

	/*
	 * §6 outer condition: the sweep serves TWO consumers in fill order -
	 * the pool while its reserve is short of pool_want (external intake
	 * capped by cma_external_ok), then the RESERVOIR while guardianship is
	 * short of pool_want_with_cma (Phase R: assembled windows flip to CMA
	 * or park in limbo, never transiting the pool). Feature off reduces
	 * both terms to the v9 condition exactly.
	 */
	while (atomic_read(&acquire_running) &&
	       ((cma_external_ok() &&
		 atomic_read(&pool_count) + served_count < READ_ONCE(pool_want)) ||
		cma_reservoir_deficit())) {
		struct page *head;
		int ret;

		cond_resched();

		if (advanced >= span_pages) {
			reason = cma_reservoir_deficit() ?
				 "cma sources exhausted" :
				 "scanned all present memory";
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

		if (atomic_read(&pool_count) + served_count <
		    READ_ONCE(pool_want) && cma_external_ok()) {
			/* pool first (§6 fill order) */
			head = pfn_to_page(pfn);
			rebuild_order9_compound(head);
			if (!pool_push_grow(head)) {
				__free_pages(head, PAGE_ORDER);
				reason = "pool capacity full";
				break;
			}
			atomic_inc(&total_refilled);
			got++;
			if (pb_enabled())	/* pairing: buddies often still free */
				cma_grab_missing_siblings(pfn, CMA_SUBBLKS);
		} else if (cma_reservoir_deficit()) {
			/* Phase R: the window feeds the reservoir */
			ret = cma_sweep_window_to_reservoir(pfn);
			if (ret == -ENOSPC) {
				floor_hit = true;
				reason = "cma headroom floor";
				break;
			}
			if (ret == -EIO) {
				reason = "cma flip failed (systemic)";
				break;
			}
			if (ret == 0)
				got_r++;
		} else {
			/* both targets met between the check and the grab */
			cma_span_free_to_buddy(pfn, nr);
		}
	}

	/* No break took a specific reason -> the while-condition ended it: the user
	 * interrupted (acquire=0) or the target was reached. Surface it for the GUI. */
	if (!reason)
		reason = atomic_read(&acquire_running) ?
			 (READ_ONCE(pool_want_with_cma) > 0 ?
			  "reached target(with_cma)" : "reached target")
						       : "stopped by user";
	WRITE_ONCE(acquire_stop_reason, reason);

	pr_info("acquire(%s) done: +%d pool +%d reservoir-fed, avail=%d want=%d pool_cma=%d evicted=%luMB scanned=%lu/%lu blocks (present=%luMB span=%luMB) floor_hit=%d\n",
		assist == ASSIST_B ? "id3/B" : "id2/A", got, got_r,
		atomic_read(&pool_count),
		READ_ONCE(pool_want), cma_pool_cma_2mb(),
		evicted >> (20 - PAGE_SHIFT),
		examined, present_blocks,
		(present_blocks << PAGE_ORDER) >> (20 - PAGE_SHIFT),
		span_pages >> (20 - PAGE_SHIFT), floor_hit);
}

static void acquire_worker(struct work_struct *w)
{
	/*
	 * v10 Phase S (before everything): when total guardianship already
	 * meets pool_want_with_cma, the pool's shortfall comes OUT OF THE
	 * RESERVOIR first (§6 source order) - a targeted CMA-mode grab per
	 * block, seconds instead of a sweep, and no external memory is taken
	 * that would push guardianship past the total target. Only when
	 * guardianship is BELOW the total target do the external phases below
	 * lead. Stage-in conserves guardianship 1:1, so checking once is
	 * checking always.
	 */
	if (cma_capable && READ_ONCE(pool_want_with_cma) > 0) {
		mutex_lock(&cma_mutex);
		if (cma_guardianship_2mb() >= READ_ONCE(pool_want_with_cma))
			cma_stage_in();
		mutex_unlock(&cma_mutex);
	}

	/*
	 * v10 Phase 1.5 prep (SUBBLKS > 1): refresh the derived index (cheap
	 * insurance against overflow drift), then the pairing pass - targeted
	 * grabs of free siblings of blocks the pool already partly owns (§6).
	 * Both no-ops on order-9 devices.
	 */
	pb_rebuild();
	cma_pair_fill();

	/*
	 * Phase 0 (all modes): drop reclaimable slab (dentry/inode) once. That slab
	 * is not on the LRU so no acquire path can reclaim it, yet a single dentry
	 * page poisons a whole window - freeing it up front unpoisons windows for
	 * every mode (including v1's alloc_contig_pages and the Phase 1 fast-path).
	 * Skipped when Phase S already met the target: dropping the system's
	 * dentry/inode cache is a real cost, pointless with no deficit left.
	 */
	if (atomic_read(&pool_count) + served_count < READ_ONCE(pool_want) &&
	    READ_ONCE(acquire_drop_slab) && kapi.k_drop_slab)
		kapi.k_drop_slab();

	/*
	 * Phase 1 (all modes): harvest already-free order-9/10 blocks from buddy
	 * - instant, no migration. Drains what's cheaply available (now including
	 * blocks the slab drop just freed) before paying for migration in Phase 2.
	 * cma_external_ok caps EXTERNAL intake at the with_cma total (§9); a
	 * remaining pool shortfall past that point is Phase S2's job.
	 */
	while (atomic_read(&acquire_running) && cma_external_ok() &&
	       atomic_read(&pool_count) + served_count < READ_ONCE(pool_want)) {
		if (!acquire_grab_free(false))
			break;
		cond_resched();
	}

	/*
	 * v10 Phase S2: pool still short after the cheap external grabs -
	 * cannibalize the reservoir BEFORE the expensive sweep (§6 fill order:
	 * pool first, reservoir later). This is what makes acquire fast when
	 * assembled blocks sit in cma_blocks[] but guardianship is below the
	 * total target (e.g. a prefill that fell short at load): VM serving
	 * capacity must not wait minutes of zone sweep for memory the module
	 * already guards. The reservoir deficit this leaves behind is Phase
	 * R's to rebuild.
	 */
	if (cma_capable && READ_ONCE(pool_want_with_cma) > 0 &&
	    atomic_read(&pool_count) + served_count < READ_ONCE(pool_want)) {
		mutex_lock(&cma_mutex);
		cma_stage_in();
		mutex_unlock(&cma_mutex);
	}

	/* Phase 2: migration for the fragmented remainder, plus a reclaim assist:
	 * id=3 = per-block evict (B), id=2 = system-wide reclaim (A), id=1 = old.
	 * The sweep also runs Phase R (reservoir fill) per its §6 outer
	 * condition; mode 1 remains pool-only legacy. */
	if (READ_ONCE(acquire_mode) == 3)
		acquire_sweep(ASSIST_B);	/* per-block evict to zram */
	else if (READ_ONCE(acquire_mode) == 2)
		acquire_sweep(ASSIST_A);	/* strided system-wide reclaim */
	else
		acquire_worker_v1();

	/*
	 * v10 tail: exchange limbo strays against full-avail pool groups (the
	 * Phase R "待配滿/掃畢 -> 交換" step), settle limbo outcomes, then
	 * Phase Q - when every count is met but part of the pool sits in
	 * blocks we will never own whole, trade those strays for completable
	 * siblings until nothing improves ("quality converged").
	 */
	if (cma_capable && READ_ONCE(pool_want_with_cma) > 0) {
		mutex_lock(&cma_mutex);
		cma_limbo_exchange();
		cma_limbo_process();
		mutex_unlock(&cma_mutex);
		if (pb_enabled() && atomic_read(&acquire_running) &&
		    atomic_read(&pool_count) + served_count >=
			READ_ONCE(pool_want) &&
		    cma_avail_cma_able_2mb() < atomic_read(&pool_count)) {
			int rounds = 8;
			bool prog = true;

			while (prog && rounds-- > 0 &&
			       atomic_read(&acquire_running))
				prog = cma_phase_q();
			if (!prog && atomic_read(&acquire_running))
				WRITE_ONCE(acquire_stop_reason,
					   "quality converged");
		}
	}
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
	/* Nothing to do if the pool reserve meets want AND total guardianship
	 * meets pool_want_with_cma (§6 outer condition - a reservoir deficit
	 * alone is enough for acquire to have Phase R work to do). */
	if (atomic_read(&pool_count) + served_count >= READ_ONCE(pool_want) &&
	    !cma_reservoir_deficit()) {
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

