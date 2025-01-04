/*
 *	linux/kernel/resource.c
 *
 * Copyright (C) 1999	Linus Torvalds
 * Copyright (C) 1999	Martin Mares <mj@ucw.cz>
 *
 * Arbitrary resource management.
 */

#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/malloc.h>
#include <linux/spinlock.h>

struct resource ioport_resource = { "PCI IO", 0x0000, 0xFFFF, IORESOURCE_IO };
struct resource iomem_resource = { "PCI mem", 0x00000000, 0xFFFFFFFF, IORESOURCE_MEM };

static rwlock_t resource_lock = RW_LOCK_UNLOCKED;

/*
 * This generates reports for /proc/ioports and /proc/iomem
 */
static char * do_resource_list(struct resource *entry, const char *fmt, int offset, char *buf, char *end)
{
	if (offset < 0)
		offset = 0;

	while (entry) {
		const char *name = entry->name;
		unsigned long from, to;

		// Check if there is enough space in the buffer
		if ((int) (end - buf) < 80)
			return buf;

		from = entry->start;
		to = entry->end;
		if (!name)
			name = "<BAD>";

		buf += snprintf(buf, end - buf, fmt + offset, from, to, name);

		// Recursively handle child resources
		if (entry->child)
			buf = do_resource_list(entry->child, fmt, offset - 2, buf, end);

		entry = entry->sibling;
	}

	return buf;
}

int get_resource_list(struct resource *root, char *buf, int size)
{
	char *fmt = "        %08lx-%08lx : %s\n";
	int retval;

	if (root == &ioport_resource)
		fmt = "        %04lx-%04lx : %s\n";

	// Use read lock for accessing resources
	read_lock(&resource_lock);
	retval = do_resource_list(root->child, fmt, 8, buf, buf + size) - buf;
	read_unlock(&resource_lock);

	return retval;
}

/* Return the conflict entry if you can't request it */
static struct resource * __request_resource(struct resource *root, struct resource *new)
{
	unsigned long start = new->start;
	unsigned long end = new->end;
	struct resource *tmp, **p;

	if (end < start)
		return root;
	if (start < root->start)
		return root;
	if (end > root->end)
		return root;

	p = &root->child;
	while (1) {
		tmp = *p;
		if (!tmp || tmp->start > end) {
			new->sibling = tmp;
			*p = new;
			new->parent = root;
			return NULL;
		}
		p = &tmp->sibling;
		if (tmp->end < start)
			continue;
		return tmp;
	}
}

int request_resource(struct resource *root, struct resource *new)
{
	struct resource *conflict;

	write_lock(&resource_lock);
	conflict = __request_resource(root, new);
	write_unlock(&resource_lock);

	return conflict ? -EBUSY : 0;
}

int release_resource(struct resource *old)
{
	struct resource **p;

	p = &old->parent->child;

	while (*p) {
		if (*p == old) {
			*p = old->sibling;
			old->parent = NULL;
			return 0;
		}
		p = &(*p)->sibling;
	}
	return -EINVAL;
}

/*
 * Find empty slot in the resource tree given range and alignment.
 */
static int find_resource(struct resource *root, struct resource *new,
			 unsigned long size,
			 unsigned long min, unsigned long max,
			 unsigned long align, struct pci_dev *dev)
{
	struct resource *this = root->child;
	unsigned long start = root->start;
	unsigned long end;

	while (this) {
		end = this->start;
		if (start < min)
			start = min;
		if (end > max)
			end = max;
		start = (start + align - 1) & ~(align - 1);

		// Fixup the resource based on the device
		start = resource_fixup(dev, new, start, size);

		if (start < end && end - start >= size) {
			new->start = start;
			new->end = start + size - 1;
			return 0;
		}

		start = this->end + 1;
		this = this->sibling;
	}
	return -EBUSY;
}

/*
 * Allocate empty slot in the resource tree given range and alignment.
 */
int allocate_resource(struct resource *root, struct resource *new,
		      unsigned long size,
		      unsigned long min, unsigned long max,
		      unsigned long align, struct pci_dev *dev)
{
	int err;

	write_lock(&resource_lock);
	err = find_resource(root, new, size, min, max, align, dev);
	if (err >= 0 && __request_resource(root, new))
		err = -EBUSY;
	write_unlock(&resource_lock);

	return err;
}

/*
 * Compatibility functions for IO resources.
 */
struct resource * __request_region(struct resource *parent, unsigned long start, unsigned long n, const char *name)
{
	struct resource *res = kmalloc(sizeof(*res), GFP_KERNEL);

	if (!res)
		return NULL;

	memset(res, 0, sizeof(*res));
	res->name = name;
	res->start = start;
	res->end = start + n - 1;
	res->flags = IORESOURCE_BUSY;

	write_lock(&resource_lock);
	while (1) {
		struct resource *conflict = __request_resource(parent, res);
		if (!conflict)
			break;

		if (conflict != parent) {
			parent = conflict;
			if (!(conflict->flags & IORESOURCE_BUSY))
				continue;
		}

		// Resource allocation failed, free memory
		kfree(res);
		return NULL;
	}
	write_unlock(&resource_lock);

	return res;
}

int __check_region(struct resource *parent, unsigned long start, unsigned long n)
{
	struct resource *res;

	res = __request_region(parent, start, n, "check-region");
	if (!res)
		return -EBUSY;

	release_resource(res);
	kfree(res);
	return 0;
}

void __release_region(struct resource *parent, unsigned long start, unsigned long n)
{
	struct resource **p;
	unsigned long end = start + n - 1;

	p = &parent->child;
	while (*p) {
		struct resource *res = *p;

		if (res->start == start && res->end == end && !(res->flags & IORESOURCE_BUSY)) {
			*p = res->sibling;
			kfree(res);
			return;
		}
		p = &res->sibling;
	}
	printk("Trying to free nonexistent resource <%04lx-%04lx>\n", start, end);
}

/*
 * Reserve IO ports or memory regions during boot.
 */
#define MAXRESERVE 4
static int __init reserve_setup(char *str)
{
	int io_start, io_num, reserved = 0;
	static struct resource reserve[MAXRESERVE];

	while (get_option(&str, &io_start) == 2) {
		if (get_option(&str, &io_num) == 0)
			break;

		if (reserved < MAXRESERVE) {
			struct resource *res = &reserve[reserved];
			res->name = "reserved";
			res->start = io_start;
			res->end = io_start + io_num - 1;
			res->child = NULL;
			if (request_resource(io_start >= 0x10000 ? &iomem_resource : &ioport_resource, res) == 0)
				reserved++;
		}
	}

	return 1;
}

__setup("reserve=", reserve_setup);
