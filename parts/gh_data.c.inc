/* SPDX: part of gh_hugepage_reserve.ko - textually #included by
 * gh_hugepage_reserve.c (unity build, single translation unit). NOT compiled
 * separately and needs no include guard; all statics stay file-local.
 * Content: pb-hash index, served-page table, pool push/pop, CMA label/tripwire */

/* ================================================================== */
/*  pb-hash: pageblock-completeness index (v10 §3, SUBBLKS > 1 only)  */
/*                                                                    */
/*  A DERIVED index over every 2MB sub-block the module guards,       */
/*  grouped by pageblock: which siblings sit in the avail pool, which */
/*  are lent out, which are parked in limbo. Everything it feeds      */
/*  (pairing, exchange, shrink classes, cma_able stats) is an         */
/*  OPTIMIZATION - the index can be rebuilt at any time from the pool */
/*  array + served table + limbo pool, and losing an entry (overflow) */
/*  degrades pairing quality, never correctness. Not populated at all */
/*  on SUBBLKS == 1 devices, where every sub-block IS its pageblock.  */
/* ================================================================== */

#define PB_HASH_MAX	16384U		/* power of two; u16 chain indexes */
#define PB_NULL		0xFFFFU

#define PB_AVAIL	0x1		/* sub-block sits in the avail pool */
#define PB_SERVED	0x2		/* sub-block lent to a VM */
#define PB_LIMBO	0x4		/* sub-block parked in the limbo pool */

struct pb_node {
	unsigned long	pb;		/* key: pfn >> cma_pb_order */
	u8		avail_mask;
	u8		served_mask;
	u8		limbo_mask;
	u16		next;		/* freelist or bucket chain index */
};

static struct pb_node pb_nodes[PB_HASH_MAX];
static u16 pb_bucket[PB_HASH_MAX];
static u16 pb_free_head = PB_NULL;
static int pb_count, pb_overflow;
static int pb_full_avail;	/* entries whose avail_mask covers the whole
				 * pageblock = groups flippable to CMA now
				 * (pool_avail_cma_able, exchange capacity) */
/* LEAF raw lock: mask updates happen from under pool_lock, served_lock and
 * limbo_lock holders (both lock domains touch the same masks - §3), so this
 * lock must never wrap any of them. It never does: every pb_* helper takes
 * only pb_lock. */
static DEFINE_RAW_SPINLOCK(pb_lock);

static inline bool pb_enabled(void)
{
	return cma_capable && cma_pb_order > PAGE_ORDER;
}

static inline u8 pb_full_mask(void)
{
	return (u8)((1U << CMA_SUBBLKS) - 1);
}

static inline unsigned int pb_hash(unsigned long pb)
{
	return (unsigned int)pb & (PB_HASH_MAX - 1);
}

static inline u8 pb_bit_of(unsigned long pfn)
{
	return (u8)(1U << ((pfn >> PAGE_ORDER) & (CMA_SUBBLKS - 1)));
}

/* Inline byte popcount: hweight8 emits an out-of-line __sw_hweight8 libcall
 * on these toolchains - a new undefined symbol the ABI check forbids. */
static inline unsigned int pb_popcount8(u8 v)
{
	v = v - ((v >> 1) & 0x55);
	v = (v & 0x33) + ((v >> 2) & 0x33);
	return (v + (v >> 4)) & 0x0f;
}

/* pb_lock held. Returns node index or PB_NULL; *prevp (may be NULL) gets the
 * chain predecessor for unlinking (collisions resolved by exact key compare,
 * same scheme as the served table). */
static u16 pb_find_locked(unsigned long pb, u16 *prevp)
{
	unsigned int b = pb_hash(pb);
	u16 n, prev = PB_NULL;

	for (n = pb_bucket[b]; n != PB_NULL; prev = n, n = pb_nodes[n].next)
		if (pb_nodes[n].pb == pb)
			break;
	if (prevp)
		*prevp = prev;
	return n;
}

static void pb_reset_locked(void)
{
	unsigned int i;

	for (i = 0; i < PB_HASH_MAX; i++) {
		pb_nodes[i].pb = 0;
		pb_nodes[i].avail_mask = 0;
		pb_nodes[i].served_mask = 0;
		pb_nodes[i].limbo_mask = 0;
		pb_nodes[i].next = (i + 1 < PB_HASH_MAX) ? (u16)(i + 1) : PB_NULL;
		pb_bucket[i] = PB_NULL;
	}
	pb_free_head = 0;
	pb_count = 0;
	pb_full_avail = 0;
}

/*
 * THE mask mutation entry point: move @pfn's sub-block bit between tracking
 * sets (@set / @clear are PB_AVAIL/PB_SERVED/PB_LIMBO combos). All-bits-empty
 * frees the node. Atomic-legal: raw leaf lock, static freelist, no
 * allocation. Maintenance points (§3): serve (avail->served), hook reclaim
 * (served->avail), hook escape (served->gone), reacquire (served->avail),
 * reconcile purge (served->gone - the easy one to miss), pool entry (->avail),
 * pool drain (avail->gone), limbo in/out.
 */
static void pb_track(unsigned long pfn, int set, int clear)
{
	unsigned long flags, pb;
	u16 n, prev;
	u8 bit, old_avail, full;

	if (!pb_enabled())
		return;
	pb = pfn >> cma_pb_order;
	bit = pb_bit_of(pfn);

	raw_spin_lock_irqsave(&pb_lock, flags);
	n = pb_find_locked(pb, &prev);
	if (n == PB_NULL) {
		unsigned int b;

		if (!set)
			goto out;	/* clearing an untracked block: no-op */
		if (pb_free_head == PB_NULL) {
			pb_overflow++;	/* index full: this sub-block's tracking
					 * is lost until the next pb_rebuild */
			goto out;
		}
		n = pb_free_head;
		pb_free_head = pb_nodes[n].next;
		b = pb_hash(pb);
		pb_nodes[n].pb = pb;
		pb_nodes[n].avail_mask = 0;
		pb_nodes[n].served_mask = 0;
		pb_nodes[n].limbo_mask = 0;
		pb_nodes[n].next = pb_bucket[b];
		pb_bucket[b] = n;
		pb_count++;
		prev = PB_NULL;		/* inserted at bucket head */
	}
	old_avail = pb_nodes[n].avail_mask;
	if (set & PB_AVAIL)
		pb_nodes[n].avail_mask |= bit;
	if (set & PB_SERVED)
		pb_nodes[n].served_mask |= bit;
	if (set & PB_LIMBO)
		pb_nodes[n].limbo_mask |= bit;
	if (clear & PB_AVAIL)
		pb_nodes[n].avail_mask &= ~bit;
	if (clear & PB_SERVED)
		pb_nodes[n].served_mask &= ~bit;
	if (clear & PB_LIMBO)
		pb_nodes[n].limbo_mask &= ~bit;

	full = pb_full_mask();
	if (old_avail == full && pb_nodes[n].avail_mask != full)
		pb_full_avail--;
	else if (old_avail != full && pb_nodes[n].avail_mask == full)
		pb_full_avail++;

	if (!(pb_nodes[n].avail_mask | pb_nodes[n].served_mask |
	      pb_nodes[n].limbo_mask)) {
		if (prev == PB_NULL)
			pb_bucket[pb_hash(pb)] = pb_nodes[n].next;
		else
			pb_nodes[prev].next = pb_nodes[n].next;
		pb_nodes[n].next = pb_free_head;
		pb_free_head = n;
		pb_count--;
	}
out:
	raw_spin_unlock_irqrestore(&pb_lock, flags);
}

/* Snapshot @pfn's pageblock masks (zeros when untracked). Any context. */
static void pb_peek(unsigned long pfn, u8 *avail, u8 *served, u8 *limbo)
{
	unsigned long flags;
	u16 n;

	*avail = *served = *limbo = 0;
	if (!pb_enabled())
		return;
	raw_spin_lock_irqsave(&pb_lock, flags);
	n = pb_find_locked(pfn >> cma_pb_order, NULL);
	if (n != PB_NULL) {
		*avail = pb_nodes[n].avail_mask;
		*served = pb_nodes[n].served_mask;
		*limbo = pb_nodes[n].limbo_mask;
	}
	raw_spin_unlock_irqrestore(&pb_lock, flags);
}

/* ---- Limbo pool (§3 待配池): held order-9 compounds that cannot be
 * served (not counted avail) and are waiting for their siblings, capped
 * small. Outcomes run at existing process-context trigger points (acquire,
 * resize) - no new worker: block completed -> whole flip to CMA (deficit) or
 * seats refilled; grown old -> genuinely freed. ---- */

#define LIMBO_MAX	64
static struct page *limbo_pages[LIMBO_MAX];
static u8  limbo_age[LIMBO_MAX];	/* process passes survived */
static int limbo_n;
static DEFINE_RAW_SPINLOCK(limbo_lock);	/* leaf; may nest inside pool_lock */

/* Any context. Caller does the pb_track(PB_LIMBO) bookkeeping. */
static bool limbo_add(struct page *page)
{
	unsigned long flags;
	bool ok = false;

	raw_spin_lock_irqsave(&limbo_lock, flags);
	if (limbo_n < LIMBO_MAX) {
		limbo_pages[limbo_n] = page;
		limbo_age[limbo_n] = 0;
		limbo_n++;
		ok = true;
	}
	raw_spin_unlock_irqrestore(&limbo_lock, flags);
	return ok;
}

/* Remove and return the limbo entry at @idx (swap-with-last), NULL if gone.
 * Caller does the pb_track(0, PB_LIMBO) bookkeeping. */
static struct page *limbo_del_idx(int idx)
{
	unsigned long flags;
	struct page *p = NULL;

	raw_spin_lock_irqsave(&limbo_lock, flags);
	if (idx >= 0 && idx < limbo_n) {
		p = limbo_pages[idx];
		limbo_pages[idx] = limbo_pages[limbo_n - 1];
		limbo_age[idx] = limbo_age[limbo_n - 1];
		limbo_n--;
	}
	raw_spin_unlock_irqrestore(&limbo_lock, flags);
	return p;
}

/* Avail pool pages that could flip to CMA as WHOLE pageblocks right now.
 * SUBBLKS == 1: every avail page is a whole, aligned pageblock, so this is
 * simply pool_avail. SUBBLKS > 1: full-avail pb-hash entries x SUBBLKS. */
static int cma_avail_cma_able_2mb(void)
{
	if (!cma_capable)
		return 0;
	if (CMA_SUBBLKS == 1)
		return atomic_read(&pool_count);
	return READ_ONCE(pb_full_avail) * CMA_SUBBLKS;
}

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
	/* Every served_del caller means "this sub-block stops being lent out"
	 * (hook match, reacquire); whether it becomes avail again is the
	 * caller's pool-entry step (which sets PB_AVAIL there). */
	if (found)
		pb_track(pfn, 0, PB_SERVED);
	return found;
}

/* Read-only lookup: is this pfn currently lent out to a VM? Used by the
 * acquire sweep to skip windows whose pages a guest's stage-2 maps -
 * migrating those (they are LRU + mlocked, and pin coverage on the lend
 * path is unverified) silently breaks the guest mapping: the guest then
 * SIGBUSes on pages the host considers happily migrated. */
static bool served_contains(unsigned long pfn)
{
	unsigned long flags;
	u16 n;
	bool found = false;

	raw_spin_lock_irqsave(&served_lock, flags);
	for (n = served_bucket[served_hash(pfn)]; n != SERVED_NULL;
	     n = served_nodes[n].next) {
		if (served_nodes[n].pfn == pfn) {
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
			/* purge = this sub-block stops being "served" without
			 * ever re-entering the pool: the pb-hash must drop the
			 * bit too or the block reads more complete than it is
			 * (§3 - the maintenance point that is easy to miss). */
			pb_track(served_nodes[n].pfn, 0, PB_SERVED);
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
	/* --- CMA reservoir (v10): required as a set, see kapi_can_cma() --- */
	void (*k_set_pageblock_migratetype)(struct page *page, int migratetype);
	/* Read a pageblock's migratetype label (mask 0x7). MUST be the kernel's
	 * copy, never the module's inline get/set: those compute the bit index
	 * from the BUILD kernel's pageblock_order, which lands on the wrong bits
	 * whenever build != device order (the exact case pageblock_order_val
	 * exists for). Resolved only on [6.1, 6.16). */
	unsigned long (*k_get_pfnblock_flags_mask)(const struct page *page,
			unsigned long pfn, unsigned long mask);
	/* CMA-mode contig grab (isolates/undoes with the CMA migratetype, like
	 * cma_alloc does). ONLY legal inside blocks whose label is already CMA -
	 * ours via cma_blocks[] - because the isolation undo rewrites the label
	 * with the mode's migratetype. */
	int (*k_alloc_contig_range_cma)(unsigned long start, unsigned long end,
			gfp_t gfp_mask);
	/* --- optional enhancers: NULL just degrades the feature, never disables --- */
	void (*k_lru_add_drain_all)(void);	/* accurate PageLRU for the gate */
	void (*k_drop_slab)(void);		/* unpoison slab-held windows */
	void (*k_drain_all_pages)(struct zone *);	/* flush pcp so parked order-9
						 * frees reach the reclaim hook */
	struct mem_cgroup *(*k_mem_cgroup_from_task)(struct task_struct *p);
	unsigned long (*k_try_to_free_mem_cgroup_pages)(struct mem_cgroup *memcg,
			unsigned long nr);	/* GFP_KERNEL, swap allowed */
	/* --- moveable_to_cma restrict lever (resolved on [6.1,6.16), else NULL) --- */
	/* restrict_cma_redirect is a DATA symbol (struct static_key_true; its embedded
	 * struct static_key is the first member, so its address IS a usable
	 * struct static_key *). kprobe cannot target data, so it is located via
	 * k_kallsyms_lookup_name and is deliberately absent from kapi_abi.tsv: a data
	 * symbol has no call signature to kCFI-check and no btf_trace typedef, so the
	 * BTF preflight cannot vet it. NULL => the restrict lever is unavailable. */
	struct static_key *k_restrict_cma_redirect;
	unsigned long (*k_kallsyms_lookup_name)(const char *name);
	void (*k_static_key_enable)(struct static_key *key);
	void (*k_static_key_disable)(struct static_key *key);
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

/* CMA-mode grab: arg 3 is the runtime MIGRATE_CMA value on <= 6.12 (our
 * build-time MIGRATE_CMA may not match the device kernel's - that is why the
 * preflight supplies it), ACR_FLAGS_CMA on the >= 6.16 acr_flags_t contract.
 * The >= 6.16 branch is dormant today (the whole feature compile-gates off
 * there because the 6.16 migratetype rework changed pageblock-flag semantics
 * and 6.18 made the setter static), but the shim stays correct if that gate
 * is ever relaxed. */
static int kapi_alloc_contig_range_cma_shim(unsigned long start, unsigned long end,
					    gfp_t gfp_mask)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
	return kraw.k_alloc_contig_range(start, end, 1 /* ACR_FLAGS_CMA */, gfp_mask);
#else
	return kraw.k_alloc_contig_range(start, end,
					 (unsigned int)READ_ONCE(migrate_cma_val),
					 gfp_mask);
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
static inline bool kapi_can_cma(void)		/* v10 CMA reservoir */
{
	return kapi.k_set_pageblock_migratetype && kapi.k_get_pfnblock_flags_mask &&
	       kapi.k_alloc_contig_range_cma && kapi.k_alloc_contig_pages &&
	       kapi.k_prep_compound_page &&
	       migrate_cma_val >= 0 &&
	       pageblock_order_val >= PAGE_ORDER && pageblock_order_val <= 11;
}

/* ================================================================== */
/*  CMA reservoir: label read + pool-entry tripwire                   */
/* ================================================================== */

/* Migratetype label of @pfn's pageblock through the KERNEL's reader (see the
 * kapi field comment: the module's inline would mis-index when build != device
 * pageblock_order). Pure bitmap read - safe from any context, no lock. */
static inline int cma_pb_mt(unsigned long pfn)
{
	return (int)kapi.k_get_pfnblock_flags_mask(pfn_to_page(pfn), pfn, 0x7);
}

/* §9 flip whitelist: labels a fully-held block may carry into a flip.
 * UNMOVABLE(0)/MOVABLE(1)/RECLAIMABLE(2) are the stable first three
 * enumerators everywhere and are FUNGIBLE on a block whose every page we
 * hold (steal residue means nothing without free pages to steal). Everything
 * else - CMA (someone's carveout), HIGHATOMIC (reserve ledger), ISOLATE,
 * >= 6 (vendor CHP, panic-guarded) - is rejected without needing its value. */
static inline bool cma_mt_flippable(int mt)
{
	return mt == 0 || mt == 1 || mt == 2;
}

/*
 * C4 tripwire, called at EVERY pool entry point (take_frozen / push /
 * push_grow): a page whose pageblock is labeled CMA - ours or anyone's - must
 * never be held by the pool. Ours, because a reservoir page entering the held
 * pool without stage-in corrupts the guardianship accounting; anyone else's,
 * because pooling a vendor-CMA page steals from a carveout that cma_alloc will
 * later demand back. Returns true = refuse (caller lets the page continue to
 * buddy, where it lands on the CMA freelist - the correct home either way).
 * Normally never fires (v9 alloc paths are !movable so never draw from CMA
 * freelists; the B-class shrink protection keeps reservoir blocks whole); the
 * counter going nonzero is a logic-bug alarm, not an expected event - hence
 * counter + rate-limited pr_err, no VM_WARN (production kernels run DEBUG_VM=n)
 * and certainly no panic. Inert until the preflight supplied migrate_cma_val
 * and the reader resolved, so v9-path devices pay one branch. */
static bool cma_pool_entry_reject(struct page *page)
{
	unsigned long pfn;

	if (READ_ONCE(migrate_cma_val) < 0 || !kapi.k_get_pfnblock_flags_mask)
		return false;
	pfn = page_to_pfn(page);
	if (cma_pb_mt(pfn) != migrate_cma_val)
		return false;
	atomic_long_inc(&dbg_cma_leak);
	pr_err_ratelimited("cma: CMA-labeled page pfn=%lx refused pool entry (cma_leak tripwire)\n",
			   pfn);
	return true;
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
	if (page)
		pb_track(page_to_pfn(page), PB_SERVED, PB_AVAIL);
	return page;
}

static bool pool_push(struct page *page)
{
	unsigned long flags;
	int idx;
	bool ok;

	if (cma_pool_entry_reject(page))	/* C4 tripwire */
		return false;
	raw_spin_lock_irqsave(&pool_lock, flags);
	idx = atomic_read(&pool_count);
	ok = (idx < pool_total);
	if (ok) {
		page_pool[idx] = page;
		atomic_set(&pool_count, idx + 1);
	}
	raw_spin_unlock_irqrestore(&pool_lock, flags);
	if (ok)
		pb_track(page_to_pfn(page), PB_AVAIL, 0);
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

	if (cma_pool_entry_reject(page))	/* C4 tripwire */
		return false;
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
	if (ok)
		pb_track(page_to_pfn(page), PB_AVAIL, 0);
	return ok;
}

