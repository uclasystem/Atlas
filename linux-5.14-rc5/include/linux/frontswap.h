/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FRONTSWAP_H
#define _LINUX_FRONTSWAP_H

#include <linux/swap.h>
#include <linux/mm.h>
#include <linux/bitops.h>
#include <linux/jump_label.h>

/*
 * Return code to denote that requested number of
 * frontswap pages are unused(moved to page cache).
 * Used in shmem_unuse and try_to_unuse.
 */
#define FRONTSWAP_PAGES_UNUSED	2

struct frontswap_ops {
	void (*init)(unsigned); /* this swap type was just swapon'ed */
	int (*store)(unsigned, pgoff_t, struct page *); /* store a page */
	int (*load)(unsigned, pgoff_t, struct page *); /* load a page */
	void (*invalidate_page)(unsigned, pgoff_t); /* page no longer needed */
	void (*invalidate_area)(unsigned); /* swap type just swapoff'ed */
#ifdef RSWAP_KERNEL_SUPPORT
	int (*load_async)(unsigned, pgoff_t, struct page *); /* async load a page */
	int (*load_async_early_map)(pgoff_t, struct page *, u64, struct vm_area_struct*, pte_t*, pte_t); /* async load a page */
	int (*pref_async_early_map)(pgoff_t, struct page *, u64, struct vm_area_struct*, pte_t*, pte_t, int); /* async load a page */
	int (*poll_load)(int); /* poll cpu for one load */
#if RSWAP_KERNEL_SUPPORT >= 2
	int (*store_on_core)(unsigned, pgoff_t, struct page *, int core); /* store a page on a certain core */
	int (*poll_store)(int); /* poll the store queue of a certain core */
#endif // RSWAP_KERNEL_SUPPORT >= 2
#if RSWAP_KERNEL_SUPPORT >= 3
	int (*peek_load)(int); /* peek the load queue to check if the demand read is done */
	int (*peek_store)(int); /* peek the store queue to cehck if writes are done */
	int (*peek_pref)(int); /* peek the pref queue to check if the prefetch read is done */
	int (*poll_pref)(int); /* poll the prefetch queue */
	int (*pref_async)(unsigned, pgoff_t, struct page *, int); /* async prefetch a page, need to specify the cpu */
#endif // RSWAP_KERNEL_SUPPORT >= 3
#endif // RSWAP_KERNEL_SUPPORT
	struct frontswap_ops *next; /* private pointer to next ops */
};

extern void frontswap_register_ops(struct frontswap_ops *ops);
extern void frontswap_shrink(unsigned long);
extern unsigned long frontswap_curr_pages(void);
extern void frontswap_writethrough(bool);
#define FRONTSWAP_HAS_EXCLUSIVE_GETS
extern void frontswap_tmem_exclusive_gets(bool);

extern bool __frontswap_test(struct swap_info_struct *, pgoff_t);
extern void __frontswap_init(unsigned type, unsigned long *map);
extern int __frontswap_store(struct page *page, int core);
extern int __frontswap_poll_store(int core);
extern int __frontswap_load(struct page *page, bool sync);
extern int __frontswap_load_early_map(struct page *page, u64 vaddr,struct vm_area_struct *vma, pte_t* ptep, pte_t orig_pte);
extern int __frontswap_pref_async_early_map(struct page *page, u64 vaddr,struct vm_area_struct *vma, pte_t* ptep, pte_t orig_pte, int cpu);
extern int __frontswap_poll_load(int core);
extern int __frontswap_peek_load(int core);
extern int __frontswap_peek_store(int core);
extern int __frontswap_poll_pref(int core);
extern int __frontswap_peek_pref(int core);
extern int __frontswap_pref_async(struct page *page, int cpu);
extern void __frontswap_invalidate_page(unsigned, pgoff_t);
extern void __frontswap_invalidate_area(unsigned);

#ifdef CONFIG_FRONTSWAP
extern struct static_key_false frontswap_enabled_key;
extern struct frontswap_ops *frontswap_ops;

static inline bool frontswap_enabled(void)
{
	return static_branch_unlikely(&frontswap_enabled_key);
}

static inline bool frontswap_test(struct swap_info_struct *sis, pgoff_t offset)
{
	return __frontswap_test(sis, offset);
}

static inline void frontswap_map_set(struct swap_info_struct *p,
				     unsigned long *map)
{
	p->frontswap_map = map;
}

static inline unsigned long *frontswap_map_get(struct swap_info_struct *p)
{
	return p->frontswap_map;
}
#else
/* all inline routines become no-ops and all externs are ignored */

static inline bool frontswap_enabled(void)
{
	return false;
}

static inline bool frontswap_test(struct swap_info_struct *sis, pgoff_t offset)
{
	return false;
}

static inline void frontswap_map_set(struct swap_info_struct *p,
				     unsigned long *map)
{
}

static inline unsigned long *frontswap_map_get(struct swap_info_struct *p)
{
	return NULL;
}
#endif

static inline int frontswap_store(struct page *page)
{
	if (frontswap_enabled())
		return __frontswap_store(page, -1); // -1 for current core

	return -1;
}

static inline int frontswap_load(struct page *page)
{
	if (frontswap_enabled())
		return __frontswap_load(page, true);

	return -1;
}

#ifdef RSWAP_KERNEL_SUPPORT
static inline int frontswap_load_async(struct page *page)
{
	if (frontswap_enabled())
		return __frontswap_load(page, false);

	return -1;
}

static inline int frontswap_poll_load(int cpu)
{
	if (frontswap_enabled())
		return __frontswap_poll_load(cpu);

	return -1;
}

#if RSWAP_KERNEL_SUPPORT >= 2
static inline int frontswap_store_on_core(struct page *page, int core)
{
	if (frontswap_enabled())
		return __frontswap_store(page, core);

	return -1;
}

static inline int frontswap_poll_store(int core)
{
	if (frontswap_enabled())
		return __frontswap_poll_store(core);

	return -1;
}
#else // RSWAP_KERNEL_SUPPORT < 2
static inline int frontswap_store_on_core(struct page *page, int core)
{
	return frontswap_store(page);
}

static inline int frontswap_poll_store(int core)
{
	return -1;
}
#endif // RSWAP_KERNEL_SUPPORT >= 2
#if RSWAP_KERNEL_SUPPORT >= 3
static inline int frontswap_peek_load(int core)
{
	if (frontswap_enabled())
		return __frontswap_peek_load(core);
	return -1;
}

static inline int frontswap_peek_store(int core)
{
	if (frontswap_enabled())
		return __frontswap_peek_store(core);
	return -1;
}
#else // RSWAP_KERNEL_SUPPORT < 3
static inline int frontswap_peek_load(int core)
{
	return -1;
}

static inline int frontswap_peek_store(int core)
{
	return -1;
}
#endif // RSWAP_KERNEL_SUPPORT >= 3
#endif // RSWAP_KERNEL_SUPPORT

static inline void frontswap_invalidate_page(unsigned type, pgoff_t offset)
{
	if (frontswap_enabled())
		__frontswap_invalidate_page(type, offset);
}

static inline void frontswap_invalidate_area(unsigned type)
{
	if (frontswap_enabled())
		__frontswap_invalidate_area(type);
}

static inline void frontswap_init(unsigned type, unsigned long *map)
{
#ifdef CONFIG_FRONTSWAP
	__frontswap_init(type, map);
#endif
}

#endif /* _LINUX_FRONTSWAP_H */
