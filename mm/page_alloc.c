/*
 *  linux/mm/page_alloc.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *  Swap reorganised 29.12.95, Stephen Tweedie
 *  Support of BIGMEM added by Gerhard Wichert, Siemens AG, July 1999
 *  Reshaped it to be a zoned allocator, Ingo Molnar, Red Hat, 1999
 */

#include <linux/config.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/swapctl.h>
#include <linux/interrupt.h>
#include <linux/pagemap.h>
#include <linux/bootmem.h>

int nr_swap_pages = 0;
int nr_lru_pages;
LIST_HEAD(lru_cache);

static zone_t zones [MAX_NR_ZONES] =
	{
		{ name: "DMA" },
		{ name: "Normal" },
		{ name: "HighMem" }
	};

zonelist_t zonelists [NR_GFPINDEX];

/*
 * Free_page() adds the page to the free lists. This is optimized for
 * fast normal cases (no error jumps taken normally).
 *
 * The way to optimize jumps for gcc-2.2.2 is to:
 *  - select the "normal" case and put it inside the if () { XXX }
 *  - no else-statements if you can avoid them
 *
 * With the above two rules, you get a straight-line execution path
 * for the normal case, giving better asm-code.
 */

#define memlist_init(x) INIT_LIST_HEAD(x)
#define memlist_add_head list_add
#define memlist_add_tail list_add_tail
#define memlist_del list_del
#define memlist_entry list_entry
#define memlist_next(x) ((x)->next)
#define memlist_prev(x) ((x)->prev)

/*
 * Temporary debugging check.
 */
#define BAD_RANGE(zone,x) (((zone) != (x)->zone) || (((x)-mem_map) < (zone)->offset) || (((x)-mem_map) >= (zone)->offset+(zone)->size))

/*
 * Buddy system. Hairy. You really aren't expected to understand this
 *
 * Hint: -mask = 1+~mask
 */

void __free_pages_ok (struct page *page, unsigned long order)
{
	unsigned long index, page_idx, mask, flags;
	free_area_t *area;
	struct page *base;
	zone_t *zone;

	/*
	 * Subtle. We do not want to test this in the inlined part of
	 * __free_page() - it's a rare condition and just increases
	 * cache footprint unnecesserily. So we do an 'incorrect'
	 * decrement on page->count for reserved pages, but this part
	 * makes it safe.
	 */
	if (PageReserved(page))
		return;

	if (page-mem_map >= max_mapnr)
		BUG();
	if (PageSwapCache(page))
		BUG();
	if (PageLocked(page))
		BUG();

	zone = page->zone;

	mask = (~0UL) << order;
	base = mem_map + zone->offset;
	page_idx = page - base;
	if (page_idx & ~mask)
		BUG();
	index = page_idx >> (1 + order);

	area = zone->free_area + order;

	spin_lock_irqsave(&zone->lock, flags);

	zone->free_pages -= mask;

	while (mask + (1 << (MAX_ORDER-1))) {
		struct page *buddy1, *buddy2;

		if (area >= zone->free_area + MAX_ORDER)
			BUG();
		if (!test_and_change_bit(index, area->map))
			/*
			 * the buddy page is still allocated.
			 */
			break;
		/*
		 * Move the buddy up one level.
		 */
		buddy1 = base + (page_idx ^ -mask);
		buddy2 = base + page_idx;
		if (BAD_RANGE(zone,buddy1))
			BUG();
		if (BAD_RANGE(zone,buddy2))
			BUG();

		memlist_del(&buddy1->list);
		mask <<= 1;
		area++;
		index >>= 1;
		page_idx &= mask;
	}
	memlist_add_head(&(base + page_idx)->list, &area->free_list);

	spin_unlock_irqrestore(&zone->lock, flags);
}

#define MARK_USED(index, order, area) \
	change_bit((index) >> (1+(order)), (area)->map)

static inline struct page * expand (zone_t *zone, struct page *page,
	 unsigned long index, int low, int high, free_area_t * area)
{
	unsigned long size = 1 << high;

	while (high > low) {
		if (BAD_RANGE(zone,page))
			BUG();
		area--;
		high--;
		size >>= 1;
		memlist_add_head(&(page)->list, &(area)->free_list);
		MARK_USED(index, high, area);
		index += size;
		page += size;
	}
	if (BAD_RANGE(zone,page))
		BUG();
	return page;
}

static inline struct page * rmqueue (zone_t *zone, unsigned long order)
{
	free_area_t * area = zone->free_area + order;
	unsigned long curr_order = order;
	struct list_head *head, *curr;
	unsigned long flags;
	struct page *page;

	spin_lock_irqsave(&zone->lock, flags);
	do {
		head = &area->free_list;
		curr = memlist_next(head);

		if (curr != head) {
			unsigned int index;

			page = memlist_entry(curr, struct page, list);
			if (BAD_RANGE(zone,page))
				BUG();
			memlist_del(curr);
			index = (page - mem_map) - zone->offset;
			MARK_USED(index, curr_order, area);
			zone->free_pages -= 1 << order;

			page = expand(zone, page, index, order, curr_order, area);
			spin_unlock_irqrestore(&zone->lock, flags);

			set_page_count(page, 1);
			if (BAD_RANGE(zone,page))
				BUG();
			return page;	
		}
		curr_order++;
		area++;
	} while (curr_order < MAX_ORDER);
	spin_unlock_irqrestore(&zone->lock, flags);

	return NULL;
}

#define ZONE_BALANCED(zone) \
	(((zone)->free_pages > (zone)->pages_low) && (!(zone)->low_on_memory))

static inline int zone_balance_memory (zone_t *zone, int gfp_mask)
{
	int freed;

	if (zone->free_pages >= zone->pages_low) {
		if (!zone->low_on_memory)
			return 1;
		/*
		 * Simple hysteresis: exit 'low memory mode' if
		 * the upper limit has been reached:
		 */
		if (zone->free_pages >= zone->pages_high) {
			zone->low_on_memory = 0;
			return 1;
		}
	} else
		zone->low_on_memory = 1;

	/*
	 * In the atomic allocation case we only 'kick' the
	 * state machine, but do not try to free pages
	 * ourselves.
	 */
	if (!(gfp_mask & __GFP_WAIT))
		return 1;

	current->flags |= PF_MEMALLOC;
	freed = try_to_free_pages(gfp_mask);
	current->flags &= ~PF_MEMALLOC;

	if (!freed && !(gfp_mask & (__GFP_MED | __GFP_HIGH)))
		return 0;
	return 1;
}

/*
 * We are still balancing memory in a global way:
 */
static inline int balance_memory (int gfp_mask)
{
	unsigned long free = nr_free_pages();
	static int low_on_memory = 0;
	int freed;

	if (free >= freepages.low) {
		if (!low_on_memory)
			return 1;
		/*
		 * Simple hysteresis: exit 'low memory mode' if
		 * the upper limit has been reached:
		 */
		if (free >= freepages.high) {
			low_on_memory = 0;
			return 1;
		}
	} else
		low_on_memory = 1;

	/*
	 * In the atomic allocation case we only 'kick' the
	 * state machine, but do not try to free pages
	 * ourselves.
	 */
	if (!(gfp_mask & __GFP_WAIT))
		return 1;

	current->flags |= PF_MEMALLOC;
	freed = try_to_free_pages(gfp_mask);
	current->flags &= ~PF_MEMALLOC;

	if (!freed && !(gfp_mask & (__GFP_MED | __GFP_HIGH)))
		return 0;
	return 1;
}

/*
 * This is the 'heart' of the zoned buddy allocator:
 */
struct page * __alloc_pages (zonelist_t *zonelist, unsigned long order)
{
	zone_t **zone, *z;
	struct page *page;
	int gfp_mask;

	/*
	 * (If anyone calls gfp from interrupts nonatomically then it
	 * will sooner or later tripped up by a schedule().)
	 *
	 * We are falling back to lower-level zones if allocation
	 * in a higher zone fails.
	 */
	zone = zonelist->zones;
	gfp_mask = zonelist->gfp_mask;
	for (;;) {
		z = *(zone++);
		if (!z)
			break;
		if (!z->size)
			BUG();
		/*
		 * If this is a recursive call, we'd better
		 * do our best to just allocate things without
		 * further thought.
		 */
		if (!(current->flags & PF_MEMALLOC))
			/*
			 * fastpath
			 */
			if (!ZONE_BALANCED(z))
				goto balance;
		/*
		 * This is an optimization for the 'higher order zone
		 * is empty' case - it can happen even in well-behaved
		 * systems, think the page-cache filling up all RAM.
		 * We skip over empty zones. (this is not exact because
		 * we do not take the spinlock and it's not exact for
		 * the higher order case, but will do it for most things.)
		 */
ready:
		if (z->free_pages) {
			page = rmqueue(z, order);
			if (page)
				return page;
		}
	}

	/*
	 * If we can schedule, do so, and make sure to yield.
	 * We may be a real-time process, and if kswapd is
	 * waiting for us we need to allow it to run a bit.
	 */
	if (gfp_mask & __GFP_WAIT) {
		current->policy |= SCHED_YIELD;
		schedule();
	}

nopage:
	return NULL;

/*
 * The main chunk of the balancing code is in this offline branch:
 */
balance:
	if (!balance_memory(gfp_mask))
		goto nopage;
	goto ready;
}

/*
 * Total amount of free (allocatable) RAM:
 */
unsigned int nr_free_pages (void)
{
	unsigned int sum;
	zone_t *zone;

	sum = 0;
	for (zone = zones; zone < zones + MAX_NR_ZONES; zone++)
		sum += zone->free_pages;
	return sum;
}

/*
 * Amount of free RAM allocatable as buffer memory:
 */
unsigned int nr_free_buffer_pages (void)
{
	unsigned int sum;
	zone_t *zone;

	sum = nr_lru_pages;
	for (zone = zones; zone <= zones+ZONE_NORMAL; zone++)
		sum += zone->free_pages;
	return sum;
}

#if CONFIG_HIGHMEM
unsigned int nr_free_highpages (void)
{
	return zones[ZONE_HIGHMEM].free_pages;
}
#endif

/*
 * Show free area list (used inside shift_scroll-lock stuff)
 * We also calculate the percentage fragmentation. We do this by counting the
 * memory on each free list with the exception of the first item on the list.
 */
void show_free_areas(void)
{
 	unsigned long order;
	unsigned type;

	printk("Free pages:      %6dkB (%6dkB HighMem)\n",
		nr_free_pages() << (PAGE_SHIFT-10),
		nr_free_highpages() << (PAGE_SHIFT-10));

	printk("( Free: %d, lru_cache: %d (%d %d %d) )\n",
		nr_free_pages(),
		nr_lru_pages,
		freepages.min,
		freepages.low,
		freepages.high);

	for (type = 0; type < MAX_NR_ZONES; type++) {
		struct list_head *head, *curr;
		zone_t *zone = zones + type;
 		unsigned long nr, total, flags;

		printk("  %s: ", zone->name);

		total = 0;
		if (zone->size) {
			spin_lock_irqsave(&zone->lock, flags);
		 	for (order = 0; order < MAX_ORDER; order++) {
				head = &(zone->free_area + order)->free_list;
				curr = head;
				nr = 0;
				for (;;) {
					curr = memlist_next(curr);
					if (curr == head)
						break;
					nr++;
				}
				total += nr * (1 << order);
				printk("%lu*%lukB ", nr,
						(PAGE_SIZE>>10) << order);
			}
			spin_unlock_irqrestore(&zone->lock, flags);
		}
		printk("= %lukB)\n", total * (PAGE_SIZE>>10));
	}

#ifdef SWAP_CACHE_INFO
	show_swap_cache_info();
#endif	
}

/*
 * Builds allocation fallback zone lists. We are basically ready
 * to do NUMA-allocations, only this function has to be modified
 * and the zonelists array be made per-CPU.
 */
static inline void build_zonelists (void)
{
	int i, j, k;

	for (i = 0; i < NR_GFPINDEX; i++) {
		zonelist_t *zonelist;
		zone_t *zone;

		zonelist = zonelists + i;
		memset(zonelist, 0, sizeof(*zonelist));

		zonelist->gfp_mask = i;
		j = 0;
		k = ZONE_NORMAL;
		if (i & __GFP_HIGHMEM)
			k = ZONE_HIGHMEM;
		if (i & __GFP_DMA)
			k = ZONE_DMA;

		switch (k) {
			default:
				BUG();
			/*
			 * fallthrough:
			 */
			case ZONE_HIGHMEM:
				zone = zones + ZONE_HIGHMEM;
				if (zone->size) {
#ifndef CONFIG_HIGHMEM
					BUG();
#endif
					zonelist->zones[j++] = zone;
				}
			case ZONE_NORMAL:
				zone = zones + ZONE_NORMAL;
				if (zone->size)
					zonelist->zones[j++] = zone;
			case ZONE_DMA:
				zonelist->zones[j++] = zones + ZONE_DMA;
		}
		zonelist->zones[j++] = NULL;
	} 
}

#define LONG_ALIGN(x) (((x)+(sizeof(long))-1)&~((sizeof(long))-1))

/*
 * Set up the zone data structures:
 *   - mark all pages reserved
 *   - mark all memory queues empty
 *   - clear the memory bitmaps
 */
void __init free_area_init(unsigned int *zones_size)
{
	struct page * p;
	unsigned long i, j;
	unsigned long map_size;
	unsigned int totalpages, offset;

	totalpages = 0;
	for (i = 0; i < MAX_NR_ZONES; i++) {
		unsigned long size = zones_size[i];
		totalpages += size;
	}
	printk("totalpages: %08x\n", totalpages);

	/*
	 * Select nr of pages we try to keep free for important stuff
	 * with a minimum of 10 pages and a maximum of 256 pages, so
	 * that we don't waste too much memory on large systems.
	 * This is fairly arbitrary, but based on some behaviour
	 * analysis.
	 */
	i = totalpages >> 7;
	if (i < 10)
		i = 10;
	if (i > 256)
		i = 256;
	freepages.min = i;
	freepages.low = i * 2;
	freepages.high = i * 3;

	/*
	 * Some architectures (with lots of mem and discontinous memory
	 * maps) have to search for a good mem_map area:
	 */
	map_size = totalpages*sizeof(struct page);
	mem_map = (struct page *) alloc_bootmem(map_size);

	/*
	 * Initially all pages are reserved - free ones are freed
	 * up by free_all_bootmem() once the early boot process is
	 * done.
	 */
	for (p = mem_map; p < mem_map + totalpages; p++) {
		set_page_count(p, 0);
		p->flags = (1 << PG_DMA);
		SetPageReserved(p);
		init_waitqueue_head(&p->wait);
		memlist_init(&p->list);
	}

	offset = 0;	
	for (j = 0; j < MAX_NR_ZONES; j++) {
		zone_t *zone = zones + j;
		unsigned long mask = -1;
		unsigned long size;

		size = zones_size[j];

		printk("zone(%ld): %ld pages.\n", j, size);
		zone->size = size;
		if (!size)
			continue;

		zone->offset = offset;
		/*
		 * It's unnecessery to balance the high memory zone
		 */
		if (j != ZONE_HIGHMEM) {
			zone->pages_low = freepages.low;
			zone->pages_high = freepages.high;
		}
		zone->low_on_memory = 0;

		for (i = 0; i < size; i++) {
			struct page *page = mem_map + offset + i;
			page->zone = zone;
			if (j != ZONE_HIGHMEM)
				page->virtual = __page_address(page);
		}

		offset += size;
		for (i = 0; i < MAX_ORDER; i++) {
			unsigned long bitmap_size;

			memlist_init(&zone->free_area[i].free_list);
			mask += mask;
			size = (size + ~mask) & mask;
			bitmap_size = size >> i;
			bitmap_size = (bitmap_size + 7) >> 3;
			bitmap_size = LONG_ALIGN(bitmap_size);
			zone->free_area[i].map = 
				(unsigned int *) alloc_bootmem(bitmap_size);
		}
	}
	build_zonelists();
}
