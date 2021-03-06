/*
 * Copyright 2010 Tilera Corporation. All Rights Reserved.
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation, version 2.
 *
 *   This program is distributed in the hope that it will be useful, but
 *   WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, GOOD TITLE or
 *   NON INFRINGEMENT.  See the GNU General Public License for
 *   more details.
 */

#ifndef _ASM_TILE_CACHEFLUSH_H
#define _ASM_TILE_CACHEFLUSH_H

#include <arch/chip.h>

/* Keep includes the same across arches.  */
#include <linux/mm.h>
#include <linux/cache.h>
#include <asm/system.h>
#include <arch/icache.h>

/* Caches are physically-indexed and so don't need special treatment */
#define flush_cache_all()			do { } while (0)
#define flush_cache_mm(mm)			do { } while (0)
#define flush_cache_dup_mm(mm)			do { } while (0)
#define flush_cache_range(vma, start, end)	do { } while (0)
#define flush_cache_page(vma, vmaddr, pfn)	do { } while (0)
#define ARCH_IMPLEMENTS_FLUSH_DCACHE_PAGE 0
#define flush_dcache_page(page)			do { } while (0)
#define flush_dcache_mmap_lock(mapping)		do { } while (0)
#define flush_dcache_mmap_unlock(mapping)	do { } while (0)
#define flush_cache_vmap(start, end)		do { } while (0)
#define flush_cache_vunmap(start, end)		do { } while (0)
#define flush_icache_page(vma, pg)		do { } while (0)
#define flush_icache_user_range(vma, pg, adr, len)	do { } while (0)

/* Flush the icache just on this cpu */
extern void __flush_icache_range(unsigned long start, unsigned long end);

/* Flush the entire icache on this cpu. */
#define __flush_icache() __flush_icache_range(0, CHIP_L1I_CACHE_SIZE())

#ifdef CONFIG_SMP
/*
 * When the kernel writes to its own text we need to do an SMP
 * broadcast to make the L1I coherent everywhere.  This includes
 * module load and single step.
 */
extern void flush_icache_range(unsigned long start, unsigned long end);
#else
#define flush_icache_range __flush_icache_range
#endif

/*
 * An update to an executable user page requires icache flushing.
 * We could carefully update only tiles that are running this process,
 * and rely on the fact that we flush the icache on every context
 * switch to avoid doing extra work here.  But for now, I'll be
 * conservative and just do a global icache flush.
 */
static inline void copy_to_user_page(struct vm_area_struct *vma,
				     struct page *page, unsigned long vaddr,
				     void *dst, void *src, int len)
{
	memcpy(dst, src, len);
	if (vma->vm_flags & VM_EXEC) {
		flush_icache_range((unsigned long) dst,
				   (unsigned long) dst + len);
	}
}

#define copy_from_user_page(vma, page, vaddr, dst, src, len) \
	memcpy((dst), (src), (len))

/*
 * Invalidate a VA range; pads to L2 cacheline boundaries.
 *
 * Note that on TILE64, __inv_buffer() actually flushes modified
 * cache lines in addition to invalidating them, i.e., it's the
 * same as __finv_buffer().
 */
static inline void __inv_buffer(void *buffer, size_t size)
{
	char *next = (char *)((long)buffer & -L2_CACHE_BYTES);
	char *finish = (char *)L2_CACHE_ALIGN((long)buffer + size);
	while (next < finish) {
		__insn_inv(next);
		next += CHIP_INV_STRIDE();
	}
}

/* Flush a VA range; pads to L2 cacheline boundaries. */
static inline void __flush_buffer(void *buffer, size_t size)
{
	char *next = (char *)((long)buffer & -L2_CACHE_BYTES);
	char *finish = (char *)L2_CACHE_ALIGN((long)buffer + size);
	while (next < finish) {
		__insn_flush(next);
		next += CHIP_FLUSH_STRIDE();
	}
}

/* Flush & invalidate a VA range; pads to L2 cacheline boundaries. */
static inline void __finv_buffer(void *buffer, size_t size)
{
	char *next = (char *)((long)buffer & -L2_CACHE_BYTES);
	char *finish = (char *)L2_CACHE_ALIGN((long)buffer + size);
	while (next < finish) {
		__insn_finv(next);
		next += CHIP_FINV_STRIDE();
	}
}


/* Invalidate a VA range, then memory fence. */
static inline void inv_buffer(void *buffer, size_t size)
{
	__inv_buffer(buffer, size);
	mb_incoherent();
}

/* Flush a VA range, then memory fence. */
static inline void flush_buffer(void *buffer, size_t size)
{
	__flush_buffer(buffer, size);
	mb_incoherent();
}

/* Flush & invalidate a VA range, then memory fence. */
static inline void finv_buffer(void *buffer, size_t size)
{
	__finv_buffer(buffer, size);
	mb_incoherent();
}

/*
 * Flush & invalidate a VA range that is homed remotely on a single core,
 * waiting until the memory controller holds the flushed values.
 */
static inline void finv_buffer_remote(void *buffer, size_t size)
{
	char *p;
	int i;

	/*
	 * Flush and invalidate the buffer out of the local L1/L2
	 * and request the home cache to flush and invalidate as well.
	 */
	__finv_buffer(buffer, size);

	/*
	 * Wait for the home cache to acknowledge that it has processed
	 * all the flush-and-invalidate requests.  This does not mean
	 * that the flushed data has reached the memory controller yet,
	 * but it does mean the home cache is processing the flushes.
	 */
	__insn_mf();

	/*
	 * Issue a load to the last cache line, which can't complete
	 * until all the previously-issued flushes to the same memory
	 * controller have also completed.  If we weren't striping
	 * memory, that one load would be sufficient, but since we may
	 * be, we also need to back up to the last load issued to
	 * another memory controller, which would be the point where
	 * we crossed an 8KB boundary (the granularity of striping
	 * across memory controllers).  Keep backing up and doing this
	 * until we are before the beginning of the buffer, or have
	 * hit all the controllers.
	 */
	for (i = 0, p = (char *)buffer + size - 1;
	     i < (1 << CHIP_LOG_NUM_MSHIMS()) && p >= (char *)buffer;
	     ++i) {
		const unsigned long STRIPE_WIDTH = 8192;

		/* Force a load instruction to issue. */
		*(volatile char *)p;

		/* Jump to end of previous stripe. */
		p -= STRIPE_WIDTH;
		p = (char *)((unsigned long)p | (STRIPE_WIDTH - 1));
	}

	/* Wait for the loads (and thus flushes) to have completed. */
	__insn_mf();
}

#endif /* _ASM_TILE_CACHEFLUSH_H */
