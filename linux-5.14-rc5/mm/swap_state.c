// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/mm/swap_state.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *  Swap reorganised 29.12.95, Stephen Tweedie
 *
 *  Rewritten to use page cache, (C) 1998 Stephen Tweedie
 */
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/init.h>
#include <linux/pagemap.h>
#include <linux/backing-dev.h>
#include <linux/blkdev.h>
#include <linux/pagevec.h>
#include <linux/migrate.h>
#include <linux/vmalloc.h>
#include <linux/swap_slots.h>
#include <linux/huge_mm.h>
#include <linux/shmem_fs.h>
#include "internal.h"

// [RMGrid]
#include <linux/frontswap.h>
#include <linux/swap_stats.h>
#include <linux/page_idle.h>
// [Hermit]
#include <linux/hermit.h>

/*
 * swapper_space is a fiction, retained to simplify the path through
 * vmscan's shrink_page_list.
 */
static const struct address_space_operations swap_aops = {
	.writepage = swap_writepage,
	.set_page_dirty = swap_set_page_dirty,
#ifdef CONFIG_MIGRATION
	.migratepage = migrate_page,
#endif
	.writepage_on_core = swap_writepage_on_core,
	.poll_write = frontswap_poll_store,
};

struct address_space *swapper_spaces[MAX_SWAPFILES] __read_mostly;
static unsigned int nr_swapper_spaces[MAX_SWAPFILES] __read_mostly;
static bool enable_vma_readahead __read_mostly = true;

/* [Hermit] control prefetching:
 * 	== 0: kernel default strategy.
 *	>  0: fixed swapin window. Prefetch (n - 1) pages at most.
 */
static int readahead_win __read_mostly = 0;

#define SWAP_RA_WIN_SHIFT	(PAGE_SHIFT / 2)
#define SWAP_RA_HITS_MASK	((1UL << SWAP_RA_WIN_SHIFT) - 1)
#define SWAP_RA_HITS_MAX	SWAP_RA_HITS_MASK
#define SWAP_RA_WIN_MASK	(~PAGE_MASK & ~SWAP_RA_HITS_MASK)

#define SWAP_RA_HITS(v)		((v) & SWAP_RA_HITS_MASK)
#define SWAP_RA_WIN(v)		(((v) & SWAP_RA_WIN_MASK) >> SWAP_RA_WIN_SHIFT)
#define SWAP_RA_ADDR(v)		((v) & PAGE_MASK)

#define SWAP_RA_VAL(addr, win, hits)				\
	(((addr) & PAGE_MASK) |					\
	 (((win) << SWAP_RA_WIN_SHIFT) & SWAP_RA_WIN_MASK) |	\
	 ((hits) & SWAP_RA_HITS_MASK))

/* Initial readahead hits is 4 to start up with a small window */
#define GET_SWAP_RA_VAL(vma)					\
	(atomic_long_read(&(vma)->swap_readahead_info) ? : 4)

#define INC_CACHE_INFO(x)	data_race(swap_cache_info.x++)
#define ADD_CACHE_INFO(x, nr)	data_race(swap_cache_info.x += (nr))

static struct {
	unsigned long add_total;
	unsigned long del_total;
	unsigned long find_success;
	unsigned long find_total;
} swap_cache_info;

static atomic_t swapin_readahead_hits = ATOMIC_INIT(4);

void show_swap_cache_info(void)
{
	printk("%lu pages in swap cache\n", total_swapcache_pages());
	printk("Swap cache stats: add %lu, delete %lu, find %lu/%lu\n",
		swap_cache_info.add_total, swap_cache_info.del_total,
		swap_cache_info.find_success, swap_cache_info.find_total);
	printk("Free swap  = %ldkB\n",
		get_nr_swap_pages() << (PAGE_SHIFT - 10));
	printk("Total swap = %lukB\n", total_swap_pages << (PAGE_SHIFT - 10));
}

void *get_shadow_from_swap_cache(swp_entry_t entry)
{
	struct address_space *address_space = swap_address_space(entry);
	pgoff_t idx = swp_offset(entry);
	struct page *page;

	page = xa_load(&address_space->i_pages, idx);
	if (xa_is_value(page))
		return page;
	return NULL;
}

/*
 * add_to_swap_cache resembles add_to_page_cache_locked on swapper_space,
 * but sets SwapCache flag and private instead of mapping and index.
 */
int add_to_swap_cache(struct page *page, swp_entry_t entry,
			gfp_t gfp, void **shadowp)
{
	struct address_space *address_space = swap_address_space(entry);
	pgoff_t idx = swp_offset(entry);
	unsigned long nr = thp_nr_pages(page);
	struct page *old;

	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(PageSwapCache(page), page);
	VM_BUG_ON_PAGE(!PageSwapBacked(page), page);
	VM_BUG_ON(nr != 1);

	page_ref_add(page, nr);
	SetPageSwapCache(page);
	old = hermit_swapcache[idx];
	if(old == NULL){
		hmt_sc_store(idx, page);
		set_page_private(page, entry.val);
		address_space->nrpages += nr;
		__mod_node_page_state(page_pgdat(page), NR_FILE_PAGES, nr);
		__mod_lruvec_page_state(page, NR_SWAPCACHE, nr);
		ADD_CACHE_INFO(add_total, nr);
		return 0;
	}

	BUG();

	ClearPageSwapCache(page);
	page_ref_sub(page, nr);
	return -1;
}

/*
 * This must be called only on pages that have
 * been verified to be in the swap cache.
 */
void __delete_from_swap_cache(struct page *page,
			swp_entry_t entry, void *shadow)
{
	struct address_space *address_space = swap_address_space(entry);
	int i, nr = thp_nr_pages(page);
	pgoff_t idx = swp_offset(entry);

	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(!PageSwapCache(page), page);
	VM_BUG_ON_PAGE(PageWriteback(page), page);

	for (i = 0; i < nr; i++) {
		struct page* entry = hmt_sc_load(idx + i);
		VM_BUG_ON_PAGE(entry != page, entry);
		set_page_private(page + i, 0);
		hmt_sc_store(idx + i, NULL);
	}
	ClearPageSwapCache(page);
	address_space->nrpages -= nr;
	__mod_node_page_state(page_pgdat(page), NR_FILE_PAGES, -nr);
	__mod_lruvec_page_state(page, NR_SWAPCACHE, -nr);
	ADD_CACHE_INFO(del_total, nr);
}

/**
 * add_to_swap - allocate swap space for a page
 * @page: page we want to move to swap
 *
 * Allocate swap space for the page and add the page to the
 * swap cache.  Caller needs to hold the page lock.
 */
int add_to_swap_profiling(struct page *page, bool relaxed,
			  uint64_t pf_breakdown[])
{
	swp_entry_t entry;
	int err;

	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(!PageUptodate(page), page);

	// adc_pf_breakdown_stt(pf_breakdown, ADC_ALLOC_SWAP_SLOT,
	// 		     pf_cycles_start());
	entry = hermit_get_swap_page(page, relaxed);
	// adc_pf_breakdown_end(pf_breakdown, ADC_ALLOC_SWAP_SLOT,
	// 		     pf_cycles_end());
	if (!entry.val)
		return 0;

	/*
	 * XArray node allocations from PF_MEMALLOC contexts could
	 * completely exhaust the page allocator. __GFP_NOMEMALLOC
	 * stops emergency reserves from being allocated.
	 *
	 * TODO: this could cause a theoretical memory reclaim
	 * deadlock in the swap out path.
	 */
	/*
	 * Add it to the swap cache.
	 */
	// adc_pf_breakdown_stt(pf_breakdown, ADC_ADD_TO_SWAPCACHE,
	// 		     pf_cycles_start());
	err = add_to_swap_cache(page, entry,
			__GFP_HIGH|__GFP_NOMEMALLOC|__GFP_NOWARN, NULL);
	// adc_pf_breakdown_end(pf_breakdown, ADC_ADD_TO_SWAPCACHE,
	// 		     pf_cycles_end());
	if (err)
		/*
		 * add_to_swap_cache() doesn't return -EEXIST, so we can safely
		 * clear SWAP_HAS_CACHE flag.
		 */
		goto fail;
	/*
	 * Normally the page will be dirtied in unmap because its pte should be
	 * dirty. A special case is MADV_FREE page. The page's pte could have
	 * dirty bit cleared but the page's SwapBacked bit is still set because
	 * clearing the dirty bit and SwapBacked bit has no lock protected. For
	 * such page, unmap will not set dirty bit for it, so page reclaim will
	 * not write the page out. This can cause data corruption when the page
	 * is swap in later. Always setting the dirty bit for the page solves
	 * the problem.
	 */
	set_page_dirty(page);

	return 1;

fail:
	put_swap_page(page, entry);
	return 0;
}

inline int add_to_swap(struct page *page)
{
	return add_to_swap_profiling(page, false, NULL);
}

/*
 * This must be called only on pages that have
 * been verified to be in the swap cache and locked.
 * It will never put the page into the free list,
 * the caller has a reference on the page.
 */
void delete_from_swap_cache(struct page *page)
{
	swp_entry_t entry = { .val = page_private(page) };
	struct address_space *address_space = swap_address_space(entry);

	xa_lock_irq(&address_space->i_pages);
	__delete_from_swap_cache(page, entry, NULL);
	xa_unlock_irq(&address_space->i_pages);

	put_swap_page(page, entry);
	page_ref_sub(page, thp_nr_pages(page));
}

void clear_shadow_from_swap_cache(int type, unsigned long begin,
				unsigned long end)
{
	return;
}

/*
 * If we are the only user, then try to free up the swap cache.
 *
 * Its ok to check for PageSwapCache without the page lock
 * here because we are going to recheck again inside
 * try_to_free_swap() _with_ the lock.
 * 					- Marcelo
 */
void free_swap_cache(struct page *page)
{
	if (PageSwapCache(page) && !page_mapped(page) && trylock_page(page)) {
		try_to_free_swap(page);
		unlock_page(page);
	}
}

/*
 * Perform a free_page(), also freeing any swap cache associated with
 * this page if it is the last user of the page.
 */
void free_page_and_swap_cache(struct page *page)
{
	free_swap_cache(page);
	if (!is_huge_zero_page(page))
		put_page(page);
}

/*
 * Passed an array of pages, drop them all from swapcache and then release
 * them.  They are removed from the LRU and freed if this is their last use.
 */
void free_pages_and_swap_cache(struct page **pages, int nr)
{
	struct page **pagep = pages;
	int i;

	lru_add_drain();
	for (i = 0; i < nr; i++)
		free_swap_cache(pagep[i]);
	release_pages(pagep, nr);
}

static inline bool swap_use_vma_readahead(void)
{
	// return READ_ONCE(enable_vma_readahead) && !atomic_read(&nr_rotate_swap);
	// [Hermit] force vma based swapin
	return READ_ONCE(enable_vma_readahead);
}

/*
 * Lookup a swap entry in the swap cache. A found page will be returned
 * unlocked and with its refcount incremented - we rely on the kernel
 * lock getting page table operations atomic even if we drop the page
 * lock before returning.
 */
struct page *hermit_lookup_swap_cache(swp_entry_t entry,
				      struct vm_area_struct *vma,
				      unsigned long addr,
				      struct ds_area_struct *dsa)
{
	struct page *page;
	struct swap_info_struct *si;

	si = get_swap_device(entry);
	if (!si)
		return NULL;
	// page = find_get_page(swap_address_space(entry), swp_offset(entry));
	page = hmt_sc_load_get(swp_offset(entry));
	put_swap_device(si);

	INC_CACHE_INFO(find_total);
	if (page) {
		bool vma_ra = swap_use_vma_readahead();
		bool readahead;

		INC_CACHE_INFO(find_success);
		/*
		 * At the moment, we don't support PG_readahead for anon THP
		 * so let's bail out rather than confusing the readahead stat.
		 */
		if (unlikely(PageTransCompound(page)))
			return page;

		readahead = TestClearPageReadahead(page);
		// vma_readahead stats
		if (vma && vma_ra) {
			unsigned long ra_val;
			int win, hits;

			ra_val = GET_SWAP_RA_VAL(vma);
			win = SWAP_RA_WIN(ra_val);
			hits = SWAP_RA_HITS(ra_val);
			if (readahead)
				hits = min_t(int, hits + 1, SWAP_RA_HITS_MAX);
			atomic_long_set(&vma->swap_readahead_info,
					SWAP_RA_VAL(addr, win, hits));
		}

		// cluster_readahead stats
		if (readahead) {
			count_vm_event(SWAP_RA_HIT);
			if (!vma || !vma_ra)
				atomic_inc(&swapin_readahead_hits);
		}

		// [Hermit] dsa prefetching stats, the same as logic for vmas
		if (dsa) {
			unsigned long ra_val;
			int win, hits;

			ra_val = GET_SWAP_RA_VAL(dsa);
			win = SWAP_RA_WIN(ra_val);
			hits = SWAP_RA_HITS(ra_val);
			if (readahead)
				hits = min_t(int, hits + 1, SWAP_RA_HITS_MAX);
			atomic_long_set(&dsa->swap_readahead_info,
					SWAP_RA_VAL(addr, win, hits));
		}
	}

	return page;
}

/**
 * find_get_incore_page - Find and get a page from the page or swap caches.
 * @mapping: The address_space to search.
 * @index: The page cache index.
 *
 * This differs from find_get_page() in that it will also look for the
 * page in the swap cache.
 *
 * Return: The found page or %NULL.
 */
struct page *find_get_incore_page(struct address_space *mapping, pgoff_t index)
{
	swp_entry_t swp;
	struct swap_info_struct *si;
	struct page *page = pagecache_get_page(mapping, index,
						FGP_ENTRY | FGP_HEAD, 0);

	if (!page)
		return page;
	if (!xa_is_value(page))
		return find_subpage(page, index);
	if (!shmem_mapping(mapping))
		return NULL;

	swp = radix_to_swp_entry(page);
	/* Prevent swapoff from happening to us */
	si = get_swap_device(swp);
	if (!si)
		return NULL;
	// page = find_get_page(swap_address_space(swp), swp_offset(swp));
	page = hmt_sc_load_get(swp_offset(swp));
	put_swap_device(si);
	return page;
}

/**
 * [Hermit] issue RDMA read request earlier for a demand fault page.
 * @cpu: pointer to a cpu id. *cpu is inited with -1.
 *       *cpu stores the cpu that frontswap should poll on
 */
static inline struct page *
__read_swap_cache_speculative(swp_entry_t entry, gfp_t gfp_mask,
			      struct vm_area_struct *vma, unsigned long addr,
			      bool *new_page_allocated, int *cpu,
			      int *adc_pf_bits, uint64_t pf_breakdown[])
{
	// struct swap_info_struct *si;
	struct page *page = NULL, *new_page = NULL;
	void *shadow = NULL;
	// [Hermit] for DSA record
	unsigned long refault_dist = 0;

	*new_page_allocated = false;

	if (!cpu) {
		pr_err("YIFAN: %s:%d\n", __func__, __LINE__);
		return NULL;
	}

	new_page = alloc_page_vma(gfp_mask, vma, addr);
	if (!new_page) {
		// new_page = find_get_page(swap_address_space(entry),
		// 		     swp_offset(entry));
		new_page = hmt_sc_load_get(swp_offset(entry));
		return new_page;
	}

	*cpu = hermit_issue_read(new_page, entry);
	// [RMGrid] profiling
	set_adc_pf_bits(adc_pf_bits, ADC_PF_MAJOR_BIT);
	adc_profile_counter_inc(ADC_ONDEMAND_SWAPIN);

	for (;;) {
		int err;
		/*
		 * First check the swap cache.  Since this is normally
		 * called after lookup_swap_cache() failed, re-calling
		 * that would confuse statistics.
		 */
		// si = get_swap_device(entry);
		// if (!si)
		// 	return NULL;
		// page = find_get_page(swap_address_space(entry),
		// 		     swp_offset(entry));
		page = hmt_sc_load_get(swp_offset(entry));
		// put_swap_device(si);
		if (page)
			goto fail_free;

		/*
		 * Just skip read ahead for unused swap slot.
		 * During swap_off when swap_slot_cache is disabled,
		 * we have to handle the race between putting
		 * swap entry in swap cache and marking swap slot
		 * as SWAP_HAS_CACHE.  That's done in later part of code or
		 * else swap_off will be aborted if we return NULL.
		 */
		if (!__swp_swapcount(entry) && swap_slot_cache_enabled)
			goto fail_free;

		/*
		 * Swap entry may have been freed since our caller observed it.
		 */
		err = swapcache_prepare(entry);
		if (!err)
			break;

		if (err != -EEXIST)
			goto fail_free;

		/*
		 * We might race against __delete_from_swap_cache(), and
		 * stumble across a swap_map entry whose SWAP_HAS_CACHE
		 * has not yet been cleared.  Or race against another
		 * __read_swap_cache_async(), which has set SWAP_HAS_CACHE
		 * in swap_map, but not yet added its page to swap cache.
		 */
		// cond_resched();
	}

	/*
	 * The swap entry is ours to swap in. Prepare the new page.
	 */
	page = new_page;

	if (hermit_mem_cgroup_swapin_charge_page(page, vma->vm_mm, gfp_mask,
						 adc_pf_bits, pf_breakdown)) {
		pr_err("YIFAN: %s:%d\n", __func__, __LINE__);
		goto fail_unlock;
	}

	/* May fail (-ENOMEM) if XArray node allocation failed. */
	if (add_to_swap_cache(page, entry, gfp_mask & GFP_RECLAIM_MASK,
			      &shadow)) {
		pr_err("YIFAN: %s:%d\n", __func__, __LINE__);
		goto fail_unlock;
	}
	// mem_cgroup_swapin_uncharge_swap(entry);

	if (shadow)
		refault_dist = hermit_workingset_refault(page, shadow);
	// if (refault_dist) { // [Hermit] update DSA
	// 	hermit_record_refault_dist(addr, refault_dist);
	// }

	/* Caller will initiate read into locked page */
	lru_cache_add(page);
	*new_page_allocated = true;

	return page;

fail_unlock:
	put_swap_page(page, entry);
	unlock_page(page);
	if (cpu) {
		adc_profile_counter_inc(ADC_OPTIM_FAILED);
		hermit_poll_read(*cpu, page, true, pf_breakdown);
	}
	put_page(page);
	return NULL;
fail_free:
	if (cpu) {
		adc_profile_counter_inc(ADC_OPTIM_FAILED);
		hermit_poll_read(*cpu, new_page, true, pf_breakdown);
	}
	put_page(new_page);
	return page;
}

struct page *
__read_swap_cache_async_without_charge_profiling(swp_entry_t entry, gfp_t gfp_mask,
				  struct vm_area_struct *vma,
				  unsigned long addr, bool *new_page_allocated,
				  int *adc_pf_bits, uint64_t pf_breakdown[])
{
	struct swap_info_struct *si;
	struct page *page;
	void *shadow = NULL;
	// [Hermit] for DSA record
	unsigned long refault_dist = 0;
	uint64_t pf_ts = -pf_cycles_start();

	*new_page_allocated = false;

	for (;;) {
		int err;
		/*
		 * First check the swap cache.  Since this is normally
		 * called after lookup_swap_cache() failed, re-calling
		 * that would confuse statistics.
		 */
		si = get_swap_device(entry);
		if (!si)
			return NULL;
		// page = find_get_page(swap_address_space(entry),
		// 		     swp_offset(entry));
		page = hmt_sc_load_get(swp_offset(entry));
		put_swap_device(si);
		if (page)
			return page;

		/*
		 * Just skip read ahead for unused swap slot.
		 * During swap_off when swap_slot_cache is disabled,
		 * we have to handle the race between putting
		 * swap entry in swap cache and marking swap slot
		 * as SWAP_HAS_CACHE.  That's done in later part of code or
		 * else swap_off will be aborted if we return NULL.
		 */
		if (!__swp_swapcount(entry) && swap_slot_cache_enabled)
			return NULL;

		/*
		 * Get a new page to read into from swap.  Allocate it now,
		 * before marking swap_map SWAP_HAS_CACHE, when -EEXIST will
		 * cause any racers to loop around until we add it to cache.
		 */
		page = alloc_page_vma(gfp_mask, vma, addr);
		if (!page)
			return NULL;

		/*
		 * Swap entry may have been freed since our caller observed it.
		 */
		err = swapcache_prepare(entry);
		if (!err)
			break;

		put_page(page);
		if (err != -EEXIST)
			return NULL;

		/*
		 * We might race against __delete_from_swap_cache(), and
		 * stumble across a swap_map entry whose SWAP_HAS_CACHE
		 * has not yet been cleared.  Or race against another
		 * __read_swap_cache_async(), which has set SWAP_HAS_CACHE
		 * in swap_map, but not yet added its page to swap cache.
		 */
		// cond_resched();
	}
	pf_ts += pf_cycles_end();
	adc_pf_breakdown_end(pf_breakdown, ADC_ALLOC_PAGE, pf_ts);

	/*
	 * The swap entry is ours to swap in. Prepare the new page.
	 */
	__SetPageLocked(page);
	__SetPageSwapBacked(page);

	// if (hermit_mem_cgroup_swapin_charge_page(page, vma->vm_mm, gfp_mask,
	// 					 adc_pf_bits, pf_breakdown)) {
	// 	pr_err("YIFAN: %s:%d\n", __func__, __LINE__);
	// 	goto unlock;
	// }

	/* May fail (-ENOMEM) if XArray node allocation failed. */
	if (add_to_swap_cache(page, entry, gfp_mask & GFP_RECLAIM_MASK,
			      &shadow)) {
		pr_err("YIFAN: %s:%d\n", __func__, __LINE__);
		goto unlock;
	}
	// mem_cgroup_swapin_uncharge_swap(entry);

	if (shadow)
		refault_dist = hermit_workingset_refault(page, shadow);
	// if (refault_dist) { // [Hermit] update DSA
	// 	hermit_record_refault_dist(addr, refault_dist);
	// }

	/* Caller will initiate read into locked page */
	lru_cache_add(page);
	*new_page_allocated = true;

	return page;

// [RMGrid] profiling
unlock:
	put_swap_page(page, entry);
	unlock_page(page);
	put_page(page);
	return NULL;
}

struct page *
__read_swap_cache_async_profiling_trylock(swp_entry_t entry, gfp_t gfp_mask,
				  struct vm_area_struct *vma,
				  unsigned long addr, bool *new_page_allocated,
				  int *adc_pf_bits, uint64_t pf_breakdown[])
{
	struct swap_info_struct *si;
	struct page *page;
	void *shadow = NULL;
	// [Hermit] for DSA record
	unsigned long refault_dist = 0;
	uint64_t pf_ts = -pf_cycles_start();

	*new_page_allocated = false;

	for (;;) {
		int err;
		/*
		 * First check the swap cache.  Since this is normally
		 * called after lookup_swap_cache() failed, re-calling
		 * that would confuse statistics.
		 */
		si = get_swap_device(entry);
		if (!si)
			return NULL;
		// page = find_get_page(swap_address_space(entry),
		// 		     swp_offset(entry));
		page = hmt_sc_load_get(swp_offset(entry));
		put_swap_device(si);
		if (page)
			return page;

		/*
		 * Just skip read ahead for unused swap slot.
		 * During swap_off when swap_slot_cache is disabled,
		 * we have to handle the race between putting
		 * swap entry in swap cache and marking swap slot
		 * as SWAP_HAS_CACHE.  That's done in later part of code or
		 * else swap_off will be aborted if we return NULL.
		 */
		if (!__swp_swapcount(entry) && swap_slot_cache_enabled)
			return NULL;

		/*
		 * Get a new page to read into from swap.  Allocate it now,
		 * before marking swap_map SWAP_HAS_CACHE, when -EEXIST will
		 * cause any racers to loop around until we add it to cache.
		 */
		page = alloc_page_vma(gfp_mask, vma, addr);
		if (!page)
			return NULL;

		/*
		 * Swap entry may have been freed since our caller observed it.
		 */
		err = swapcache_prepare_trylock(entry);
		if (!err)
			break;

		put_page(page);
		return NULL;
	}
	pf_ts += pf_cycles_end();
	adc_pf_breakdown_end(pf_breakdown, ADC_ALLOC_PAGE, pf_ts);

	/*
	 * The swap entry is ours to swap in. Prepare the new page.
	 */
	__SetPageLocked(page);
	__SetPageSwapBacked(page);

	if (hermit_mem_cgroup_swapin_charge_page(page, vma->vm_mm, gfp_mask,
						 adc_pf_bits, pf_breakdown)) {
		pr_err("YIFAN: %s:%d\n", __func__, __LINE__);
		goto unlock;
	}

	/* May fail (-ENOMEM) if XArray node allocation failed. */
	if (add_to_swap_cache(page, entry, gfp_mask & GFP_RECLAIM_MASK,
			      &shadow)) {
		pr_err("YIFAN: %s:%d\n", __func__, __LINE__);
		goto unlock;
	}
	// mem_cgroup_swapin_uncharge_swap(entry);

	if (shadow)
		refault_dist = hermit_workingset_refault(page, shadow);
	// if (refault_dist) { // [Hermit] update DSA
	// 	hermit_record_refault_dist(addr, refault_dist);
	// }

	/* Caller will initiate read into locked page */
	lru_cache_add(page);
	*new_page_allocated = true;

	return page;

// [RMGrid] profiling
unlock:
	put_swap_page(page, entry);
	unlock_page(page);
	put_page(page);
	return NULL;
}

struct page *
__read_swap_cache_async_profiling(swp_entry_t entry, gfp_t gfp_mask,
				  struct vm_area_struct *vma,
				  unsigned long addr, bool *new_page_allocated,
				  int *adc_pf_bits, uint64_t pf_breakdown[])
{
	struct swap_info_struct *si;
	struct page *page;
	void *shadow = NULL;
	// [Hermit] for DSA record
	unsigned long refault_dist = 0;
	uint64_t pf_ts = -pf_cycles_start();

	*new_page_allocated = false;

	for (;;) {
		int err;
		/*
		 * First check the swap cache.  Since this is normally
		 * called after lookup_swap_cache() failed, re-calling
		 * that would confuse statistics.
		 */
		si = get_swap_device(entry);
		if (!si)
			return NULL;
		// page = find_get_page(swap_address_space(entry),
		// 		     swp_offset(entry));
		page = hmt_sc_load_get(swp_offset(entry));
		put_swap_device(si);
		if (page)
			return page;

		/*
		 * Just skip read ahead for unused swap slot.
		 * During swap_off when swap_slot_cache is disabled,
		 * we have to handle the race between putting
		 * swap entry in swap cache and marking swap slot
		 * as SWAP_HAS_CACHE.  That's done in later part of code or
		 * else swap_off will be aborted if we return NULL.
		 */
		if (!__swp_swapcount(entry) && swap_slot_cache_enabled)
			return NULL;

		/*
		 * Get a new page to read into from swap.  Allocate it now,
		 * before marking swap_map SWAP_HAS_CACHE, when -EEXIST will
		 * cause any racers to loop around until we add it to cache.
		 */
		page = alloc_page_vma(gfp_mask, vma, addr);
		if (!page)
			return NULL;

		/*
		 * Swap entry may have been freed since our caller observed it.
		 */
		err = swapcache_prepare(entry);
		if (!err)
			break;

		put_page(page);
		if (err != -EEXIST)
			return NULL;

		/*
		 * We might race against __delete_from_swap_cache(), and
		 * stumble across a swap_map entry whose SWAP_HAS_CACHE
		 * has not yet been cleared.  Or race against another
		 * __read_swap_cache_async(), which has set SWAP_HAS_CACHE
		 * in swap_map, but not yet added its page to swap cache.
		 */
		// cond_resched();
	}
	pf_ts += pf_cycles_end();
	adc_pf_breakdown_end(pf_breakdown, ADC_ALLOC_PAGE, pf_ts);

	/*
	 * The swap entry is ours to swap in. Prepare the new page.
	 */
	__SetPageLocked(page);
	__SetPageSwapBacked(page);

	if (hermit_mem_cgroup_swapin_charge_page(page, vma->vm_mm, gfp_mask,
						 adc_pf_bits, pf_breakdown)) {
		pr_err("YIFAN: %s:%d\n", __func__, __LINE__);
		goto unlock;
	}

	/* May fail (-ENOMEM) if XArray node allocation failed. */
	if (add_to_swap_cache(page, entry, gfp_mask & GFP_RECLAIM_MASK,
			      &shadow)) {
		pr_err("YIFAN: %s:%d\n", __func__, __LINE__);
		goto unlock;
	}
	// mem_cgroup_swapin_uncharge_swap(entry);

	if (shadow)
		refault_dist = hermit_workingset_refault(page, shadow);
	// if (refault_dist) { // [Hermit] update DSA
	// 	hermit_record_refault_dist(addr, refault_dist);
	// }

	/* Caller will initiate read into locked page */
	lru_cache_add(page);
	*new_page_allocated = true;

	return page;

// [RMGrid] profiling
unlock:
	put_swap_page(page, entry);
	unlock_page(page);
	put_page(page);
	return NULL;
}

inline struct page *
__read_swap_cache_async(swp_entry_t entry, gfp_t gfp_mask,
				  struct vm_area_struct *vma,
				  unsigned long addr, bool *new_page_allocated)
{
	return __read_swap_cache_async_profiling(
		entry, gfp_mask, vma, addr, new_page_allocated, NULL, NULL);
}

/*
 * Locate a page of swap in physical memory, reserving swap cache space
 * and reading the disk if it is not already cached.
 * A failure return means that either the page allocation failed or that
 * the swap entry is no longer in use.
 */
struct page *read_swap_cache_async(swp_entry_t entry, gfp_t gfp_mask,
		struct vm_area_struct *vma, unsigned long addr, bool do_poll)
{
	bool page_was_allocated;
	struct page *retpage = __read_swap_cache_async(entry, gfp_mask,
			vma, addr, &page_was_allocated);

	if (page_was_allocated) {
		int cpu = get_cpu();
		swap_readpage(retpage, do_poll);
		put_cpu();
		hermit_poll_read(cpu, retpage, true, NULL);
		// [RMGrid] profiling
		adc_profile_counter_inc(ADC_ONDEMAND_SWAPIN);
		if (vma->vm_mm)
			count_memcg_event_mm(vma->vm_mm, ONDEMAND_SWAPIN);
	}

	return retpage;
}

static unsigned int __swapin_nr_pages(unsigned long prev_offset,
				      unsigned long offset,
				      int hits,
				      int max_pages,
				      int prev_win)
{
	unsigned int pages, last_ra;

	// [Hermit]
	if (readahead_win > 0)
		return readahead_win;

	/*
	 * This heuristic has been found to work well on both sequential and
	 * random loads, swapping to hard disk or to SSD: please don't ask
	 * what the "+ 2" means, it just happens to work well, that's all.
	 */
	pages = hits + 2;
	if (pages == 2) {
		/*
		 * We can have no readahead hits to judge by: but must not get
		 * stuck here forever, so check for an adjacent offset instead
		 * (and don't even bother to check whether swap type is same).
		 */
		if (offset != prev_offset + 1 && offset != prev_offset - 1)
			pages = 1;
	} else {
		unsigned int roundup = 4;
		while (roundup < pages)
			roundup <<= 1;
		pages = roundup;
	}

	if (pages > max_pages)
		pages = max_pages;

	/* Don't shrink readahead too fast */
	last_ra = prev_win / 2;
	if (pages < last_ra)
		pages = last_ra;

	return pages;
}

static unsigned long swapin_nr_pages(unsigned long offset)
{
	static unsigned long prev_offset;
	unsigned int hits, pages, max_pages;
	static atomic_t last_readahead_pages;

	max_pages = 1 << READ_ONCE(page_cluster);
	if (max_pages <= 1)
		return 1;

	hits = atomic_xchg(&swapin_readahead_hits, 0);
	pages = __swapin_nr_pages(READ_ONCE(prev_offset), offset, hits,
				  max_pages,
				  atomic_read(&last_readahead_pages));
	if (!hits)
		WRITE_ONCE(prev_offset, offset);
	atomic_set(&last_readahead_pages, pages);

	return pages;
}

/**
 * swap_cluster_readahead - swap in pages in hope we need them soon
 * @entry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @vmf: fault information
 *
 * Returns the struct page for entry and addr, after queueing swapin.
 *
 * Primitive swap readahead code. We simply read an aligned block of
 * (1 << page_cluster) entries in the swap area. This method is chosen
 * because it doesn't cost us any seek time.  We also make sure to queue
 * the 'original' request together with the readahead ones...
 *
 * This has been extended to use the NUMA policies from the mm triggering
 * the readahead.
 *
 * Caller must hold read mmap_lock if vmf->vma is not NULL.
 */
struct page *hermit_swap_cluster_readahead(swp_entry_t entry, gfp_t gfp_mask,
					   struct vm_fault *vmf,
					   struct ds_area_struct *dsa,
					   int *adc_pf_bits,
					   uint64_t pf_breakdown[])
{
	struct page *page;
	unsigned long entry_offset = swp_offset(entry);
	unsigned long offset = entry_offset;
	unsigned long start_offset, end_offset;
	unsigned long mask;
	struct swap_info_struct *si = swp_swap_info(entry);
	// struct blk_plug plug;
	bool do_poll = true, page_allocated;
	struct vm_area_struct *vma = vmf->vma;
	unsigned long addr = vmf->address;

	// [RMGrid]
	int cpu = -1;
	struct page *fault_page;
	bool demand_page_allocated;
	// [Hermit]
	bool hmt_spec_io = hmt_ctl_flag(HMT_SPEC_IO);

	mask = swapin_nr_pages(offset) - 1;

	// [RMGrid] issue demand page read first
	fault_page = hmt_spec_io ?
		     __read_swap_cache_speculative(
				     entry, gfp_mask, vma, addr,
				     &demand_page_allocated, &cpu, adc_pf_bits,
				     pf_breakdown) :
		     __read_swap_cache_async_profiling(
				     entry, gfp_mask, vma, addr,
				     &demand_page_allocated, adc_pf_bits,
				     pf_breakdown);
	if (cpu == -1 && demand_page_allocated) {
		cpu = get_cpu();
		swap_readpage(fault_page, do_poll);
		put_cpu();
		// [RMGrid] profiling
		set_adc_pf_bits(adc_pf_bits, ADC_PF_MAJOR_BIT);
		adc_profile_counter_inc(ADC_ONDEMAND_SWAPIN);
		if (vma->vm_mm)
			count_memcg_event_mm(vma->vm_mm, ONDEMAND_SWAPIN);
	}

	if (!mask)
		goto skip;

	/* Test swap type to make sure the dereference is safe */
	if (likely(si->flags & (SWP_BLKDEV | SWP_FS_OPS))) {
		struct inode *inode = si->swap_file->f_mapping->host;
		if (inode_read_congested(inode))
			goto skip;
	}

	do_poll = false;
	/* Read a page_cluster sized and aligned cluster around offset. */
	start_offset = offset & ~mask;
	end_offset = offset | mask;
	if (!start_offset)	/* First page is swap header. */
		start_offset++;
	if (end_offset >= si->max)
		end_offset = si->max - 1;

	// blk_start_plug(&plug);
	for (offset = start_offset; offset <= end_offset ; offset++) {
		if (offset == entry_offset)
			continue;
		/* Ok, do the async read-ahead now */
		page = __read_swap_cache_async_profiling(
			swp_entry(swp_type(entry), offset), gfp_mask, vma, addr,
			&page_allocated, adc_pf_bits, pf_breakdown);
		if (!page)
			continue;
		if (page_allocated) {
			swap_readpage_async(page);
			SetPageReadahead(page);
			count_vm_event(SWAP_RA);
			set_page_prefetch(page);
			// [RMGrid] profiling
			adc_profile_counter_inc(ADC_PREFETCH_SWAPIN);
			if (vma->vm_mm)
				count_memcg_event_mm(vma->vm_mm,
						     PREFETCH_SWAPIN);
		}
		put_page(page);
	}
	// blk_finish_plug(&plug);
skip:
	lru_add_drain();	/* Push any new pages onto the LRU now */
	if (demand_page_allocated)
		// [Hermit] we disabled preemption inside poll_load when polling
		hermit_poll_read(cpu, fault_page, true, pf_breakdown);
	return fault_page;
}

int init_swap_address_space(unsigned int type, unsigned long nr_pages)
{
	struct address_space *spaces, *space;
	unsigned int i, nr;

	nr = DIV_ROUND_UP(nr_pages, SWAP_ADDRESS_SPACE_PAGES);
	spaces = kvcalloc(nr, sizeof(struct address_space), GFP_KERNEL);
	if (!spaces)
		return -ENOMEM;
	for (i = 0; i < nr; i++) {
		space = spaces + i;
		xa_init_flags(&space->i_pages, XA_FLAGS_LOCK_IRQ);
		atomic_set(&space->i_mmap_writable, 0);
		space->a_ops = &swap_aops;
		/* swap cache doesn't use writeback related tags */
		mapping_set_no_writeback_tags(space);
	}
	nr_swapper_spaces[type] = nr;
	swapper_spaces[type] = spaces;

	return 0;
}

void exit_swap_address_space(unsigned int type)
{
	int i;
	struct address_space *spaces = swapper_spaces[type];

	for (i = 0; i < nr_swapper_spaces[type]; i++)
		VM_WARN_ON_ONCE(!mapping_empty(&spaces[i]));
	kvfree(spaces);
	nr_swapper_spaces[type] = 0;
	swapper_spaces[type] = NULL;
}

static inline void swap_ra_clamp_pfn(struct vm_area_struct *vma,
				     unsigned long faddr,
				     unsigned long lpfn,
				     unsigned long rpfn,
				     unsigned long *start,
				     unsigned long *end)
{
	*start = max3(lpfn, PFN_DOWN(vma->vm_start),
		      PFN_DOWN(faddr & PMD_MASK));
	*end = min3(rpfn, PFN_DOWN(vma->vm_end),
		    PFN_DOWN((faddr & PMD_MASK) + PMD_SIZE));
}

// [Hermit] add dsa field
void hermit_swap_ra_info(struct vm_fault *vmf, struct ds_area_struct *dsa,
			 struct vma_swap_readahead *ra_info)
{
	struct vm_area_struct *vma = vmf->vma;
	unsigned long ra_val;
	unsigned long faddr, pfn, fpfn;
	unsigned long start, end;
	pte_t *pte, *orig_pte;
	unsigned int max_win, hits, prev_win, win, left;
#ifndef CONFIG_64BIT
	pte_t *tpte;
#endif

	max_win = 1 << min_t(unsigned int, READ_ONCE(page_cluster),
			     SWAP_RA_ORDER_CEILING);
	if (max_win == 1) {
		ra_info->win = 1;
		return;
	}

	faddr = vmf->address;
	orig_pte = pte = pte_offset_map(vmf->pmd, faddr);

	fpfn = PFN_DOWN(faddr);
	// [Hermit] prioritize using dsa prefetch stats
	if (dsa) {
		ra_val = GET_SWAP_RA_VAL(dsa);
		pfn = PFN_DOWN(SWAP_RA_ADDR(ra_val));
		prev_win = SWAP_RA_WIN(ra_val);
		hits = SWAP_RA_HITS(ra_val);
		if (dsa->type == DSA_PARFOR || dsa->type == DSA_STREAMING) {
			// eagerly sequentially prefetch
			hits += 4;
			max_win = 32;
			ra_info->win = win = __swapin_nr_pages(
				pfn, fpfn, hits, max_win, prev_win);
		} else if (dsa->type == DSA_RANDOM) {
			// don't prefetch
			ra_info->win = win = 1;
		} else if (dsa->type == DSA_NON_DSA) {
			// normally prefetch for the per-thread dsa
			ra_info->win = win = __swapin_nr_pages(
				pfn, fpfn, hits, max_win, prev_win);
		} else { // un-recognized dsas
			ra_info->win = win = __swapin_nr_pages(
				pfn, fpfn, hits, max_win, prev_win);
		}

		// [Hermit] reset dsa prefetch stats as well
		atomic_long_set(&dsa->swap_readahead_info,
				SWAP_RA_VAL(faddr, win, 0));
	} else {
		ra_val = GET_SWAP_RA_VAL(vma);
		pfn = PFN_DOWN(SWAP_RA_ADDR(ra_val));
		prev_win = SWAP_RA_WIN(ra_val);
		hits = SWAP_RA_HITS(ra_val);
		ra_info->win = win =
			__swapin_nr_pages(pfn, fpfn, hits, max_win, prev_win);
	}

	atomic_long_set(&vma->swap_readahead_info,
			SWAP_RA_VAL(faddr, win, 0));

	if (win == 1) {
		pte_unmap(orig_pte);
		return;
	}

	/* Copy the PTEs because the page table may be unmapped */
	if (hmt_ctl_flag(HMT_PREF_ALWYS_ASCEND) || fpfn == pfn + 1)
		swap_ra_clamp_pfn(vma, faddr, fpfn, fpfn + win, &start, &end);
	else if (pfn == fpfn + 1)
		swap_ra_clamp_pfn(vma, faddr, fpfn - win + 1, fpfn + 1,
				  &start, &end);
	else {
		left = (win - 1) / 2;
		swap_ra_clamp_pfn(vma, faddr, fpfn - left, fpfn + win - left,
				  &start, &end);
	}
	ra_info->nr_pte = end - start;
	ra_info->offset = fpfn - start;
	pte -= ra_info->offset;
#ifdef CONFIG_64BIT
	ra_info->ptes = pte;
#else
	tpte = ra_info->ptes;
	for (pfn = start; pfn != end; pfn++)
		*tpte++ = *pte++;
#endif
	pte_unmap(orig_pte);
}

/**
 * swap_vma_readahead - swap in pages in hope we need them soon
 * @fentry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @vmf: fault information
 *
 * Returns the struct page for entry and addr, after queueing swapin.
 *
 * Primitive swap readahead code. We simply read in a few pages whose
 * virtual addresses are around the fault address in the same vma.
 *
 * Caller must hold read mmap_lock if vmf->vma is not NULL.
 *
 */
static struct page *
hermit_swap_vma_readahead(swp_entry_t fentry, gfp_t gfp_mask,
			  struct vm_fault *vmf, struct ds_area_struct *dsa,
			  int *adc_pf_bits, uint64_t pf_breakdown[])
{
	// struct blk_plug plug;
	struct vm_area_struct *vma = vmf->vma;
	struct pref_request pref_req = {
		.vma = vma,
		.ra_info = { .win = 1, },
		.gfp_mask = gfp_mask,
		.stt = 0,
		.faddr = vmf->address,
	};

	// [RMGrid]
	uint64_t pf_ts;
	int cpu = -1;
	struct page *fault_page;
	bool demand_page_allocated;
	// [Hermit]
	bool hmt_spec_io = hmt_ctl_flag(HMT_SPEC_IO);

	adc_pf_breakdown_stt(pf_breakdown, ADC_DEDUP_SWAPIN, pf_cycles_start());
	// [RMGrid] issue demand page read first
	fault_page = hmt_spec_io ?
		     __read_swap_cache_speculative(
				     fentry, gfp_mask, vma, vmf->address,
				     &demand_page_allocated, &cpu, adc_pf_bits,
				     pf_breakdown) :
		     __read_swap_cache_async_profiling(
				     fentry, gfp_mask, vma, vmf->address,
				     &demand_page_allocated, adc_pf_bits,
				     pf_breakdown);
	pf_ts = pf_cycles_end();
	adc_pf_breakdown_end(pf_breakdown, ADC_DEDUP_SWAPIN, pf_ts);
	adc_pf_breakdown_stt(pf_breakdown, ADC_PAGE_IO, pf_ts);

	hermit_swap_ra_info(vmf, dsa, &pref_req.ra_info);
	if (cpu == -1 && demand_page_allocated) {
		cpu = get_cpu();
		swap_readpage(fault_page, pref_req.ra_info.win == 1);
		put_cpu();
		// [RMGrid] profiling
		set_adc_pf_bits(adc_pf_bits, ADC_PF_MAJOR_BIT);
		adc_profile_counter_inc(ADC_ONDEMAND_SWAPIN);
		// count_memcg_event_mm(vma->vm_mm, ONDEMAND_SWAPIN);
	}

	if (pref_req.ra_info.win > 1) {
		hermit_vma_prefetch(&pref_req, cpu, adc_pf_bits, pf_breakdown);
		if (hmt_ctl_flag(HMT_PREF_THD))
			pref_request_enqueue(&pref_req);
	}

	if (demand_page_allocated)
		// [Hermit] we disabled preemption inside poll_load when polling
		hermit_poll_read(cpu, fault_page, true, pf_breakdown);
	adc_pf_breakdown_end(pf_breakdown, ADC_PAGE_IO, pf_cycles_end());
	return fault_page;
}

static inline struct page *swap_vma_readahead(swp_entry_t fentry, gfp_t gfp_mask,
				       struct vm_fault *vmf)
{
	return hermit_swap_vma_readahead(fentry, gfp_mask, vmf, NULL, NULL,
					 NULL);
}

/**
 * swapin_readahead - swap in pages in hope we need them soon
 * @entry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @vmf: fault information
 *
 * Returns the struct page for entry and addr, after queueing swapin.
 *
 * It's a main entry function for swap readahead. By the configuration,
 * it will read ahead blocks by cluster-based(ie, physical disk based)
 * or vma-based(ie, virtual address based on faulty address) readahead.
 */
struct page *hermit_swapin_readahead(swp_entry_t entry, gfp_t gfp_mask,
				     struct vm_fault *vmf,
				     struct ds_area_struct *dsa,
				     int *adc_pf_bits, uint64_t pf_breakdown[])
{
	// [Hermit] only support vma_readahead for simplicity
	return /* swap_use_vma_readahead() ? */
	       hermit_swap_vma_readahead(entry, gfp_mask, vmf, dsa,
					 adc_pf_bits, pf_breakdown) /* :
	       hermit_swap_cluster_readahead(entry, gfp_mask, vmf, dsa,
					     adc_pf_bits, pf_breakdown) */;
}

void hermit_vma_prefetch_direct_poll_work(struct work_struct *work)
{
	struct hermit_pref_work *pref_work =
		container_of(work, struct hermit_pref_work, work);

	struct page *page;
	pte_t *pte, pentry;
	swp_entry_t entry;
	unsigned int i = 0;
	bool page_allocated;
	int nr_prefed = 0;
	gfp_t gfp_mask = GFP_HIGHUSER_MOVABLE;
	int cpu;

	struct vm_area_struct *vma = pref_work->vma;
	u64 faddr = pref_work->faddr;
	u64 vaddr = faddr;
	const int peek_freq = 4;

	cpu = smp_processor_id();
	for (i = pref_work->stt, pte = &pref_work->ptes[i]; i < pref_work->nr_pte;
	     i++, pte++) {
		if (i == pref_work->offset)
			continue;
		pentry = *pte;
		if (pte_none(pentry))
			continue;
		if (pte_present(pentry))
			continue;
		entry = pte_to_swp_entry(pentry);
		if (unlikely(non_swap_entry(entry)))
			continue;
		vaddr = faddr + ((unsigned long)i - (unsigned long)pref_work->offset) * PAGE_SIZE;
		page = __read_swap_cache_async_profiling_trylock(
			entry, gfp_mask, vma, faddr, &page_allocated, NULL, NULL);
		if (!page)
    		continue;
		if (page_allocated) {
			/* XX: sepecify the cpu manually as we don't want to disable preemption */
			__frontswap_pref_async(page, cpu);
			SetPageReadahead(page);
			count_vm_event(SWAP_RA);
			set_page_prefetch(page);
			nr_prefed ++;
			if(nr_prefed % peek_freq == 0)
				__frontswap_peek_pref(cpu);
		}
		put_page(page);
	}

	if(nr_prefed){
		adc_counter_add(nr_prefed, ADC_ASYNC_PREF_PAGES);
		__frontswap_poll_pref(cpu);
	}
	kmem_cache_free(hermit_pref_work_cache, pref_work);
}

void hermit_vma_prefetch_direct_poll_direct_map_work(struct work_struct *work)
{
	struct hermit_pref_work *pref_work =
		container_of(work, struct hermit_pref_work, work);

	struct page *page;
	pte_t *pte, pentry;
	swp_entry_t entry;
	unsigned int i = 0;
	bool page_allocated;
	int nr_prefed = 0;
	gfp_t gfp_mask = GFP_HIGHUSER_MOVABLE;
	int cpu;

	struct vm_area_struct *vma = pref_work->vma;
	u64 faddr = pref_work->faddr;
	u64 vaddr = faddr;
	const int peek_freq = 4;

	cpu = smp_processor_id();
	for (i = pref_work->stt, pte = &pref_work->ptes[i]; i < pref_work->nr_pte;
	     i++, pte++) {
		if (i == pref_work->offset)
			continue;
		pentry = *pte;
		if (pte_none(pentry))
			continue;
		if (pte_present(pentry))
			continue;
		entry = pte_to_swp_entry(pentry);
		if (unlikely(non_swap_entry(entry)))
			continue;
		if(i < pref_work->offset)
			vaddr = faddr + ((unsigned long)pref_work->offset - (unsigned long)i) * PAGE_SIZE;
		else
			vaddr = faddr + ((unsigned long)i - (unsigned long)pref_work->offset) * PAGE_SIZE;
		page = __read_swap_cache_async_profiling_trylock(
			entry, gfp_mask, vma, faddr, &page_allocated, NULL, NULL);
		if (!page)
    		continue;
		if (page_allocated) {
			/* XX: sepecify the cpu manually as we don't want to disable preemption */
			__frontswap_pref_async_early_map(page, vaddr, vma, pte, pentry, cpu);
			SetPageReadahead(page);
			count_vm_event(SWAP_RA);
			set_page_prefetch(page);
			nr_prefed ++;
			if(nr_prefed % peek_freq == 0)
				__frontswap_peek_pref(cpu);
		}
		put_page(page);
	}

	if(nr_prefed){
		adc_counter_add(nr_prefed, ADC_ASYNC_PREF_PAGES);
		__frontswap_poll_pref(cpu);
	}
	kmem_cache_free(hermit_pref_work_cache, pref_work);
}

void hermit_vma_prefetch_work(struct work_struct *work)
{
	struct hermit_pref_work *pref_work =
		container_of(work, struct hermit_pref_work, work);

	struct page *page;
	pte_t *pte, pentry;
	swp_entry_t entry;
	unsigned int i = 0;
	bool page_allocated;
	int nr_prefed = 0;
	gfp_t gfp_mask = GFP_HIGHUSER_MOVABLE;

	struct vm_area_struct *vma = pref_work->vma;
	u64 faddr = pref_work->faddr;
	u64 vaddr = faddr;

	struct page** pages = NULL;
	bool batch_charge = hmt_ctl_flag(HMT_PREF_BATCH_CHARGE);
	if(batch_charge){
		kmalloc(sizeof(struct page*) * (pref_work->nr_pte - pref_work->stt), GFP_KERNEL);
		if(!pages){
			pr_err("%s: fail to alloc pages\n", __func__);
			goto err;		
		}
	}

	for (i = pref_work->stt, pte = &pref_work->ptes[i]; i < pref_work->nr_pte;
	     i++, pte++) {
		if (i == pref_work->offset)
			continue;
		pentry = *pte;
		if (pte_none(pentry))
			continue;
		if (pte_present(pentry))
			continue;
		entry = pte_to_swp_entry(pentry);
		if (unlikely(non_swap_entry(entry)))
			continue;
		vaddr = faddr + ((unsigned long)i - (unsigned long)pref_work->offset) * PAGE_SIZE;
		if(batch_charge)
			page = __read_swap_cache_async_without_charge_profiling(
				entry, gfp_mask, vma, faddr, &page_allocated, NULL, NULL);
		else 
			page = __read_swap_cache_async_profiling(
				entry, gfp_mask, vma, faddr, &page_allocated, NULL, NULL);
		if (!page)
    		continue;
		if (page_allocated) {
			// __frontswap_load_early_map(page, vaddr, vma, pte, pentry);
			swap_readpage_async(page);
			SetPageReadahead(page);
			count_vm_event(SWAP_RA);
			set_page_prefetch(page);
			if(batch_charge)	
				pages[nr_prefed++] = page;
			else
				nr_prefed ++;
		}
		put_page(page);
	}
err:
	if(nr_prefed){
		adc_counter_add(nr_prefed, ADC_ASYNC_PREF_PAGES);
		/* use GFP_ATOMIC flag to force charge the pages */
		if(batch_charge)
			hermit_mem_cgroup_swapin_charge_batch_profiling(pages, nr_prefed, vma->vm_mm, GFP_ATOMIC, NULL, NULL);
	}
	if(pages)
		kfree(pages);
	kmem_cache_free(hermit_pref_work_cache, pref_work);
}


int hermit_vma_prefetch_early_map(struct pref_request *pref_req, int cpu,
			int *adc_pf_bits, uint64_t pf_breakdown[])
{
	// struct blk_plug plug;
	struct page *page;
	pte_t *pte, pentry;
	swp_entry_t entry;
	unsigned int i = 0;
	bool page_allocated;
	int nr_prefed = 0;

	struct vm_area_struct *vma = pref_req->vma;
	struct vma_swap_readahead *ra_info = &pref_req->ra_info;
	u64 faddr = pref_req->faddr;
	u64 vaddr = faddr;
	// [RMGrid]
	uint64_t pf_ts = 0;

	adc_pf_breakdown_stt(pf_breakdown, ADC_PREFETCH, pf_cycles_start());

	if (ra_info->win == 1)
		goto done;	

	// blk_start_plug(&plug);
	for (i = pref_req->stt, pte = &ra_info->ptes[i]; i < ra_info->nr_pte;
	     i++, pte++) {
		if (cpu != -1 && frontswap_peek_load(cpu) == 0)
			break;
		if (i == ra_info->offset)
			continue;
		pentry = *pte;
		if (pte_none(pentry))
			continue;
		if (pte_present(pentry))
			continue;
		entry = pte_to_swp_entry(pentry);
		if (unlikely(non_swap_entry(entry)))
			continue;
		vaddr = faddr + ((unsigned long)i - (unsigned long)ra_info->offset) * PAGE_SIZE;
		pf_ts = get_cycles_start();
		adc_pf_breakdown_stt(pf_breakdown, ADC_RD_CACHE_ASYNC, pf_ts);
		page = __read_swap_cache_async_profiling(
			entry, pref_req->gfp_mask, vma, pref_req->faddr,
			&page_allocated, adc_pf_bits, pf_breakdown);
		pf_ts = get_cycles_end();
		adc_pf_breakdown_end(pf_breakdown, ADC_RD_CACHE_ASYNC, pf_ts);
		if (!page)
    		continue;
		if (page_allocated) {
			__frontswap_load_early_map(page, vaddr, vma, pte, pentry);
			SetPageReadahead(page);
			count_vm_event(SWAP_RA);
			set_page_prefetch(page);
			// [RMGrid] profiling
			adc_profile_counter_inc(ADC_PREFETCH_SWAPIN);
			nr_prefed++;
		}
		put_page(page);
	}
	// blk_finish_plug(&plug);
	// lru_add_drain();
	pref_req->stt = i;
done:
	adc_pf_breakdown_end(pf_breakdown, ADC_PREFETCH, pf_cycles_end());
	return nr_prefed; // return where prefetch stops
}

/** [Hermit]
 * vma prefetch.
 * @cpu: == -1 for normal prefetch, != -1 for speculative prefetch by peeking
 * RDMA queue status in each iteration
 */
int hermit_vma_prefetch(struct pref_request *pref_req, int cpu,
			int *adc_pf_bits, uint64_t pf_breakdown[])
{
	// struct blk_plug plug;
	struct page *page;
	pte_t *pte, pentry;
	swp_entry_t entry;
	unsigned int i = 0;
	bool page_allocated;
	int nr_prefed = 0;

	struct vm_area_struct *vma = pref_req->vma;
	struct vma_swap_readahead *ra_info = &pref_req->ra_info;

	// [RMGrid]
	uint64_t pf_ts = 0;

	adc_pf_breakdown_stt(pf_breakdown, ADC_PREFETCH, pf_cycles_start());

	if (ra_info->win == 1)
		goto done;

	// blk_start_plug(&plug);
	for (i = pref_req->stt, pte = &ra_info->ptes[i]; i < ra_info->nr_pte;
	     i++, pte++) {
		if (cpu != -1 && frontswap_peek_load(cpu) == 0)
			break;
		if (i == ra_info->offset)
			continue;
		pentry = *pte;
		if (pte_none(pentry))
			continue;
		if (pte_present(pentry))
			continue;
		entry = pte_to_swp_entry(pentry);
		if (unlikely(non_swap_entry(entry)))
			continue;
		pf_ts = get_cycles_start();
		adc_pf_breakdown_stt(pf_breakdown, ADC_RD_CACHE_ASYNC, pf_ts);
		page = __read_swap_cache_async_profiling(
			entry, pref_req->gfp_mask, vma, pref_req->faddr,
			&page_allocated, adc_pf_bits, pf_breakdown);
		pf_ts = get_cycles_end();
		adc_pf_breakdown_end(pf_breakdown, ADC_RD_CACHE_ASYNC, pf_ts);
		if (!page)
			continue;
		if (page_allocated) {
			swap_readpage_async(page);
			SetPageReadahead(page);
			count_vm_event(SWAP_RA);
			set_page_prefetch(page);
			// [RMGrid] profiling
			adc_profile_counter_inc(ADC_PREFETCH_SWAPIN);
			nr_prefed++;
		}
		put_page(page);
	}
	// blk_finish_plug(&plug);
	// lru_add_drain();
	pref_req->stt = i;
done:
	adc_pf_breakdown_end(pf_breakdown, ADC_PREFETCH, pf_cycles_end());
	return nr_prefed; // return where prefetch stops
}

#ifdef CONFIG_SYSFS
static ssize_t vma_ra_enabled_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n",
			  enable_vma_readahead ? "true" : "false");
}
static ssize_t vma_ra_enabled_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	if (!strncmp(buf, "true", 4) || !strncmp(buf, "1", 1))
		enable_vma_readahead = true;
	else if (!strncmp(buf, "false", 5) || !strncmp(buf, "0", 1))
		enable_vma_readahead = false;
	else
		return -EINVAL;

	return count;
}
static struct kobj_attribute vma_ra_enabled_attr =
	__ATTR(vma_ra_enabled, 0644, vma_ra_enabled_show,
	       vma_ra_enabled_store);

// [Hermit] control prefetching
static ssize_t readahead_win_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", readahead_win);
}
static ssize_t readahead_win_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	int err = kstrtoint(buf, 10, &readahead_win);
	if (err)
		return err;

	return count;
}
static struct kobj_attribute readahead_win_attr =
	__ATTR(readahead_win, 0644, readahead_win_show, readahead_win_store);


static struct attribute *swap_attrs[] = {
	&vma_ra_enabled_attr.attr,
	&readahead_win_attr.attr,
	NULL,
};

static const struct attribute_group swap_attr_group = {
	.attrs = swap_attrs,
};

static int __init swap_init_sysfs(void)
{
	int err;
	struct kobject *swap_kobj;

	swap_kobj = kobject_create_and_add("swap", mm_kobj);
	if (!swap_kobj) {
		pr_err("failed to create swap kobject\n");
		return -ENOMEM;
	}
	err = sysfs_create_group(swap_kobj, &swap_attr_group);
	if (err) {
		pr_err("failed to register swap group\n");
		goto delete_obj;
	}
	return 0;

delete_obj:
	kobject_put(swap_kobj);
	return err;
}
subsys_initcall(swap_init_sysfs);
#endif
