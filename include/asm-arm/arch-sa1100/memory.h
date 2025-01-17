/*
 * linux/include/asm-arm/arch-sa1100/memory.h
 *
 * Copyright (c) 1999 Nicolas Pitre <nico@visuaide.com>
 */

#ifndef __ASM_ARCH_MEMORY_H
#define __ASM_ARCH_MEMORY_H


/*
 * Task size: 3GB
 */
#define TASK_SIZE	(0xc0000000UL)

/*
 * Page offset: 3GB
 */
#define PAGE_OFFSET	(0xc0000000UL)
#define PHYS_OFFSET	(0x00000000UL)

#define __virt_to_phys__is_a_macro
#define __phys_to_virt__is_a_macro

/*
 * The following gives a maximum memory size of 128MB (32MB in each bank).
 *
 * Does this still need to be optimised for one bank machines?
 */
#define __virt_to_phys(x)	(((x) & 0xe0ffffff) | ((x) & 0x06000000) << 2)
#define __phys_to_virt(x)	(((x) & 0xe7ffffff) | ((x) & 0x30000000) >> 2)

/*
 * Virtual view <-> DMA view memory address translations
 * virt_to_bus: Used to translate the virtual address to an
 *		 address suitable to be passed to set_dma_addr
 * bus_to_virt: Used to convert an address for DMA operations
 *		 to an address that the kernel can use.
 *
 * On the SA1100, bus addresses are equivalent to physical addresses.
 */
#define __virt_to_bus__is_a_macro
#define __bus_to_virt__is_a_macro
#define __virt_to_bus(x)	 __virt_to_phys(x)
#define __bus_to_virt(x)	 __phys_to_virt(x)

#endif
