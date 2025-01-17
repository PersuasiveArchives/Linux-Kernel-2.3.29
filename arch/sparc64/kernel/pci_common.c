/* $Id: pci_common.c,v 1.3 1999/09/04 22:26:32 ecd Exp $
 * pci_common.c: PCI controller common support.
 *
 * Copyright (C) 1999 David S. Miller (davem@redhat.com)
 */

#include <linux/string.h>
#include <linux/malloc.h>
#include <linux/init.h>

#include <asm/pbm.h>

/* Find the OBP PROM device tree node for a PCI device.
 * Return zero if not found.
 */
static int __init find_device_prom_node(struct pci_pbm_info *pbm,
					struct pci_dev *pdev,
					int bus_prom_node,
					struct linux_prom_pci_registers *pregs,
					int *nregs)
{
	int node;

	/*
	 * Return the PBM's PROM node in case we are it's PCI device,
	 * as the PBM's reg property is different to standard PCI reg
	 * properties. We would delete this device entry otherwise,
	 * which confuses XFree86's device probing...
	 */
	if ((pdev->bus->number == pbm->pci_bus->number) && (pdev->devfn == 0) &&
	    (pdev->vendor == PCI_VENDOR_ID_SUN) &&
	    (pdev->device == PCI_DEVICE_ID_SUN_PBM)) {
		*nregs = 0;
		return bus_prom_node;
	}

	node = prom_getchild(bus_prom_node);
	while (node != 0) {
		int err = prom_getproperty(node, "reg",
					   (char *)pregs,
					   sizeof(*pregs) * PROMREG_MAX);
		if (err == 0 || err == -1)
			goto do_next_sibling;
		if (((pregs[0].phys_hi >> 8) & 0xff) == pdev->devfn) {
			*nregs = err / sizeof(*pregs);
			return node;
		}

	do_next_sibling:
		node = prom_getsibling(node);
	}
	return 0;
}

/* Remove a PCI device from the device trees, then
 * free it up.  Note that this must run before
 * the device's resources are registered because we
 * do not handle unregistering them here.
 */
static void pci_device_delete(struct pci_dev *pdev)
{
	struct pci_dev **dpp;

	/* First, unlink from list of all devices. */
	dpp = &pci_devices;
	while (*dpp != NULL) {
		if (*dpp == pdev) {
			*dpp = pdev->next;
			pdev->next = NULL;
			break;
		}
		dpp = &(*dpp)->next;
	}

	/* Next, unlink from bus sibling chain. */
	dpp = &pdev->bus->devices;
	while (*dpp != NULL) {
		if (*dpp == pdev) {
			*dpp = pdev->sibling;
			pdev->sibling = NULL;
			break;
		}
		dpp = &(*dpp)->sibling;
	}

	/* Ok, all references are gone, free it up. */
	kfree(pdev);
}

/* Fill in the PCI device cookie sysdata for the given
 * PCI device.  This cookie is the means by which one
 * can get to OBP and PCI controller specific information
 * for a PCI device.
 */
static void __init pdev_cookie_fillin(struct pci_pbm_info *pbm,
				      struct pci_dev *pdev,
				      int bus_prom_node)
{
	struct linux_prom_pci_registers pregs[PROMREG_MAX];
	struct pcidev_cookie *pcp;
	int device_prom_node, nregs, err;

	device_prom_node = find_device_prom_node(pbm, pdev, bus_prom_node,
						 pregs, &nregs);
	if (device_prom_node == 0) {
		/* If it is not in the OBP device tree then
		 * there must be a damn good reason for it.
		 *
		 * So what we do is delete the device from the
		 * PCI device tree completely.  This scenerio
		 * is seen, for example, on CP1500 for the
		 * second EBUS/HappyMeal pair if the external
		 * connector for it is not present.
		 */
		pci_device_delete(pdev);
		return;
	}

	pcp = kmalloc(sizeof(*pcp), GFP_ATOMIC);
	if (pcp == NULL) {
		prom_printf("PCI_COOKIE: Fatal malloc error, aborting...\n");
		prom_halt();
	}
	pcp->pbm = pbm;
	pcp->prom_node = device_prom_node;
	memcpy(pcp->prom_regs, pregs, sizeof(pcp->prom_regs));
	pcp->num_prom_regs = nregs;
	err = prom_getproperty(device_prom_node, "name",
			       pcp->prom_name, sizeof(pcp->prom_name));
	if (err > 0)
		pcp->prom_name[err] = 0;
	else
		pcp->prom_name[0] = 0;
	if (strcmp(pcp->prom_name, "ebus") == 0) {
		struct linux_prom_ebus_ranges erng[PROM_PCIRNG_MAX];
		int iter;

		/* EBUS is special... */
		err = prom_getproperty(device_prom_node, "ranges",
				       (char *)&erng[0], sizeof(erng));
		if (err == 0 || err == -1) {
			prom_printf("EBUS: Fatal error, no range property\n");
			prom_halt();
		}
		err = (err / sizeof(erng[0]));
		for(iter = 0; iter < err; iter++) {
			struct linux_prom_ebus_ranges *ep = &erng[iter];
			struct linux_prom_pci_registers *ap;

			ap = &pcp->prom_assignments[iter];

			ap->phys_hi = ep->parent_phys_hi;
			ap->phys_mid = ep->parent_phys_mid;
			ap->phys_lo = ep->parent_phys_lo;
			ap->size_hi = 0;
			ap->size_lo = ep->size;
		}
		pcp->num_prom_assignments = err;
	} else {
		err = prom_getproperty(device_prom_node,
				       "assigned-addresses",
				       (char *)pcp->prom_assignments,
				       sizeof(pcp->prom_assignments));
		if (err == 0 || err == -1)
			pcp->num_prom_assignments = 0;
		else
			pcp->num_prom_assignments =
				(err / sizeof(pcp->prom_assignments[0]));
	}

	pdev->sysdata = pcp;
}

void __init pci_fill_in_pbm_cookies(struct pci_bus *pbus,
				    struct pci_pbm_info *pbm,
				    int prom_node)
{
	struct pci_dev *pdev;

	/* This loop is coded like this because the cookie
	 * fillin routine can delete devices from the tree.
	 */
	pdev = pbus->devices;
	while (pdev != NULL) {
		struct pci_dev *next = pdev->sibling;

		pdev_cookie_fillin(pbm, pdev, prom_node);

		pdev = next;
	}

	for (pbus = pbus->children; pbus; pbus = pbus->next) {
		struct pcidev_cookie *pcp = pbus->self->sysdata;
		pci_fill_in_pbm_cookies(pbus, pbm, pcp->prom_node);
	}
}

static void __init bad_assignment(struct linux_prom_pci_registers *ap,
				  struct resource *res,
				  int do_prom_halt)
{
	prom_printf("PCI: Bogus PROM assignment.\n");
	if (ap)
		prom_printf("PCI: phys[%08x:%08x:%08x] size[%08x:%08x]\n",
			    ap->phys_hi, ap->phys_mid, ap->phys_lo,
			    ap->size_hi, ap->size_lo);
	if (res)
		prom_printf("PCI: RES[%016lx-->%016lx:(%lx)]\n",
			    res->start, res->end, res->flags);
	prom_printf("Please email this information to davem@redhat.com\n");
	if (do_prom_halt)
		prom_halt();
}

static struct resource *
__init get_root_resource(struct linux_prom_pci_registers *ap,
			 struct pci_pbm_info *pbm)
{
	int space = (ap->phys_hi >> 24) & 3;

	switch (space) {
	case 0:
		/* Configuration space, silently ignore it. */
		return NULL;

	case 1:
		/* 16-bit IO space */
		return &pbm->io_space;

	case 2:
		/* 32-bit MEM space */
		return &pbm->mem_space;

	case 3:
	default:
		/* 64-bit MEM space, unsupported. */
		printk("PCI: 64-bit MEM assignment??? "
		       "Tell davem@redhat.com about it!\n");
		return NULL;
	};
}

static struct resource *
__init get_device_resource(struct linux_prom_pci_registers *ap,
			   struct pci_dev *pdev)
{
	int breg = (ap->phys_hi & 0xff);
	int space = (ap->phys_hi >> 24) & 3;

	switch (breg) {
	case  PCI_ROM_ADDRESS:
		/* It had better be MEM space. */
		if (space != 2)
			bad_assignment(ap, NULL, 0);

		return &pdev->resource[PCI_ROM_RESOURCE];

	case PCI_BASE_ADDRESS_0:
	case PCI_BASE_ADDRESS_1:
	case PCI_BASE_ADDRESS_2:
	case PCI_BASE_ADDRESS_3:
	case PCI_BASE_ADDRESS_4:
	case PCI_BASE_ADDRESS_5:
		return &pdev->resource[(breg - PCI_BASE_ADDRESS_0) / 4];

	default:
		bad_assignment(ap, NULL, 0);
		return NULL;
	};
}

static void __init pdev_record_assignments(struct pci_pbm_info *pbm,
					   struct pci_dev *pdev)
{
	struct pcidev_cookie *pcp = pdev->sysdata;
	int i;

	for (i = 0; i < pcp->num_prom_assignments; i++) {
		struct linux_prom_pci_registers *ap;
		struct resource *root, *res;

		/* The format of this property is specified in
		 * the PCI Bus Binding to IEEE1275-1994.
		 */
		ap = &pcp->prom_assignments[i];
		root = get_root_resource(ap, pbm);
		res = get_device_resource(ap, pdev);
		if (root == NULL || res == NULL)
			continue;

		/* Ok we know which resource this PROM assignment is
		 * for, sanity check it.
		 */
		if ((res->start & 0xffffffffUL) != ap->phys_lo)
			bad_assignment(ap, res, 1);

		/* Adjust the resource into the physical address space
		 * of this PBM.
		 */
		pbm->parent->resource_adjust(pdev, res, root);

		if (request_resource(root, res) < 0) {
			/* OK, there is some conflict.  But this is fine
			 * since we'll reassign it in the fixup pass.
			 * Nevertheless notify the user that OBP made
			 * an error.
			 */
			printk(KERN_ERR "PCI: Address space collision on region %ld "
			       "of device %s\n",
			       (res - &pdev->resource[0]), pdev->name);
		}
	}
}

void __init pci_record_assignments(struct pci_pbm_info *pbm,
				   struct pci_bus *pbus)
{
	struct pci_dev *pdev;

	for (pdev = pbus->devices; pdev; pdev = pdev->sibling)
		pdev_record_assignments(pbm, pdev);

	for (pbus = pbus->children; pbus; pbus = pbus->next)
		pci_record_assignments(pbm, pbus);
}

static void __init pdev_assign_unassigned(struct pci_pbm_info *pbm,
					  struct pci_dev *pdev)
{
	u32 reg;
	u16 cmd;
	int i, io_seen, mem_seen;

	io_seen = mem_seen = 0;
	for (i = 0; i < PCI_NUM_RESOURCES; i++) {
		struct resource *root, *res;
		unsigned long size, min, max, align;

		res = &pdev->resource[i];

		if (res->flags & IORESOURCE_IO)
			io_seen++;
		else if (res->flags & IORESOURCE_MEM)
			mem_seen++;

		/* If it is already assigned or the resource does
		 * not exist, there is nothing to do.
		 */
		if (res->parent != NULL || res->flags == 0UL)
			continue;

		/* Determine the root we allocate from. */
		if (res->flags & IORESOURCE_IO) {
			root = &pbm->io_space;
			min = root->start + 0x400UL;
			max = root->end;
		} else {
			root = &pbm->mem_space;
			min = root->start;
			max = min + 0x80000000UL;
		}

		size = res->end - res->start;
		align = size + 1;
		if (allocate_resource(root, res, size + 1, min, max, align) < 0) {
			/* uh oh */
			prom_printf("PCI: Failed to allocate resource %d for %s\n",
				    i, pdev->name);
			prom_halt();
		}

		/* Update PCI config space. */
		pbm->parent->base_address_update(pdev, i);
	}

	/* Special case, disable the ROM.  Several devices
	 * act funny (ie. do not respond to memory space writes)
	 * when it is left enabled.  A good example are Qlogic,ISP
	 * adapters.
	 */
	pci_read_config_dword(pdev, PCI_ROM_ADDRESS, &reg);
	reg &= ~PCI_ROM_ADDRESS_ENABLE;
	pci_write_config_dword(pdev, PCI_ROM_ADDRESS, reg);

	/* If we saw I/O or MEM resources, enable appropriate
	 * bits in PCI command register.
	 */
	if (io_seen || mem_seen) {
		pci_read_config_word(pdev, PCI_COMMAND, &cmd);
		if (io_seen)
			cmd |= PCI_COMMAND_IO;
		if (mem_seen)
			cmd |= PCI_COMMAND_MEMORY;
		pci_write_config_word(pdev, PCI_COMMAND, cmd);
	}

	/* If this is a PCI bridge or an IDE controller,
	 * enable bus mastering.  In the former case also
	 * set the cache line size correctly.
	 */
	if (((pdev->class >> 8) == PCI_CLASS_BRIDGE_PCI) ||
	    (((pdev->class >> 8) == PCI_CLASS_STORAGE_IDE) &&
	     ((pdev->class & 0x80) != 0))) {
		pci_read_config_word(pdev, PCI_COMMAND, &cmd);
		cmd |= PCI_COMMAND_MASTER;
		pci_write_config_word(pdev, PCI_COMMAND, cmd);

		if ((pdev->class >> 8) == PCI_CLASS_BRIDGE_PCI)
			pci_write_config_byte(pdev,
					      PCI_CACHE_LINE_SIZE,
					      (64 / sizeof(u32)));
	}
}

void __init pci_assign_unassigned(struct pci_pbm_info *pbm,
				  struct pci_bus *pbus)
{
	struct pci_dev *pdev;

	for (pdev = pbus->devices; pdev; pdev = pdev->sibling)
		pdev_assign_unassigned(pbm, pdev);

	for (pbus = pbus->children; pbus; pbus = pbus->next)
		pci_assign_unassigned(pbm, pbus);
}

static int __init pci_intmap_match(struct pci_dev *pdev, unsigned int *interrupt)
{
	struct pcidev_cookie *dev_pcp = pdev->sysdata;
	struct pci_pbm_info *pbm = dev_pcp->pbm;
	struct linux_prom_pci_registers *pregs = dev_pcp->prom_regs;
	unsigned int hi, mid, lo, irq;
	int i;

	if (pbm->num_pbm_intmap == 0)
		return 0;

	/* If we are underneath a PCI bridge, use PROM register
	 * property of parent bridge.
	 */
	if (pdev->bus->number != pbm->pci_first_busno) {
		struct pcidev_cookie *bus_pcp;
		int offset;

		bus_pcp = pdev->bus->self->sysdata;
		pregs = bus_pcp->prom_regs;
		offset = prom_getint(bus_pcp->prom_node,
				     "fcode-rom-offset");

		/* Did PROM know better and assign an interrupt other
		 * than #INTA to the device? - We test here for presence of
		 * FCODE on the card, in this case we assume PROM has set
		 * correct 'interrupts' property, unless it is quadhme.
		 */
		if (offset == -1 ||
		    !strcmp(bus_pcp->prom_name, "SUNW,qfe") ||
		    !strcmp(bus_pcp->prom_name, "qfe")) {
			/*
			 * No, use low slot number bits of child as IRQ line.
			 */
			*interrupt = ((*interrupt - 1 + PCI_SLOT(pdev->devfn)) & 3) + 1;
		}
	}

	hi   = pregs->phys_hi & pbm->pbm_intmask.phys_hi;
	mid  = pregs->phys_mid & pbm->pbm_intmask.phys_mid;
	lo   = pregs->phys_lo & pbm->pbm_intmask.phys_lo;
	irq  = *interrupt & pbm->pbm_intmask.interrupt;

	for (i = 0; i < pbm->num_pbm_intmap; i++) {
		if (pbm->pbm_intmap[i].phys_hi  == hi	&&
		    pbm->pbm_intmap[i].phys_mid == mid	&&
		    pbm->pbm_intmap[i].phys_lo  == lo	&&
		    pbm->pbm_intmap[i].interrupt == irq) {
			*interrupt = pbm->pbm_intmap[i].cinterrupt;
			return 1;
		}
	}

	prom_printf("pbm_intmap_match: bus %02x, devfn %02x: ",
		    pdev->bus->number, pdev->devfn);
	prom_printf("IRQ [%08x.%08x.%08x.%08x] not found in interrupt-map\n",
		    pregs->phys_hi, pregs->phys_mid, pregs->phys_lo, *interrupt);
	prom_printf("Please email this information to davem@redhat.com\n");
	prom_halt();
}

static void __init pdev_fixup_irq(struct pci_dev *pdev)
{
	struct pcidev_cookie *pcp = pdev->sysdata;
	struct pci_pbm_info *pbm = pcp->pbm;
	struct pci_controller_info *p = pbm->parent;
	unsigned int portid = p->portid;
	unsigned int prom_irq;
	int prom_node = pcp->prom_node;
	int err;

	err = prom_getproperty(prom_node, "interrupts",
			       (char *)&prom_irq, sizeof(prom_irq));
	if (err == 0 || err == -1) {
		pdev->irq = 0;
		return;
	}

	/* Fully specified already? */
	if (((prom_irq & PCI_IRQ_IGN) >> 6) == portid) {
		pdev->irq = p->irq_build(p, pdev, prom_irq);
		goto have_irq;
	}

	/* An onboard device? (bit 5 set) */
	if ((prom_irq & PCI_IRQ_INO) & 0x20) {
		pdev->irq = p->irq_build(p, pdev, (portid << 6 | prom_irq));
		goto have_irq;
	}

	/* Can we find a matching entry in the interrupt-map? */
	if (pci_intmap_match(pdev, &prom_irq)) {
		pdev->irq = p->irq_build(p, pdev, (portid << 6) | prom_irq);
		goto have_irq;
	}

	/* Ok, we have to do it the hard way. */
	{
		unsigned int bus, slot, line;

		bus = (pbm == &pbm->parent->pbm_B) ? (1 << 4) : 0;

		/* If we have a legal interrupt property, use it as
		 * the IRQ line.
		 */
		if (prom_irq > 0 && prom_irq < 5) {
			line = ((prom_irq - 1) & 3);
		} else {
			u8 pci_irq_line;

			/* Else just directly consult PCI config space. */
			pci_read_config_byte(pdev, PCI_INTERRUPT_PIN, &pci_irq_line);
			line = ((pci_irq_line - 1) & 3);
		}

		/* Now figure out the slot. */
		if (pdev->bus->number == pbm->pci_first_busno) {
			if (pbm == &pbm->parent->pbm_A)
				slot = (pdev->devfn >> 3) - 1;
			else
				slot = (pdev->devfn >> 3) - 2;
		} else {
			if (pbm == &pbm->parent->pbm_A)
				slot = (pdev->bus->self->devfn >> 3) - 1;
			else
				slot = (pdev->bus->self->devfn >> 3) - 2;
		}
		slot = slot << 2;

		pdev->irq = p->irq_build(p, pdev,
					 ((portid << 6) & PCI_IRQ_IGN) |
					 (bus | slot | line));
	}

have_irq:
	pci_write_config_byte(pdev, PCI_INTERRUPT_LINE,
			      pdev->irq & PCI_IRQ_INO);
}

void __init pci_fixup_irq(struct pci_pbm_info *pbm,
			  struct pci_bus *pbus)
{
	struct pci_dev *pdev;

	for (pdev = pbus->devices; pdev; pdev = pdev->sibling)
		pdev_fixup_irq(pdev);

	for (pbus = pbus->children; pbus; pbus = pbus->next)
		pci_fixup_irq(pbm, pbus);
}

/* Generic helper routines for PCI error reporting. */
void pci_scan_for_target_abort(struct pci_controller_info *p,
			       struct pci_pbm_info *pbm,
			       struct pci_bus *pbus)
{
	struct pci_dev *pdev;

	for (pdev = pbus->devices; pdev; pdev = pdev->sibling) {
		u16 status, error_bits;

		pci_read_config_word(pdev, PCI_STATUS, &status);
		error_bits =
			(status & (PCI_STATUS_SIG_TARGET_ABORT |
				   PCI_STATUS_REC_TARGET_ABORT));
		if (error_bits) {
			pci_write_config_word(pdev, PCI_STATUS, error_bits);
			printk("PCI%d(PBM%c): Device [%s] saw Target Abort [%016x]\n",
			       p->index, ((pbm == &p->pbm_A) ? 'A' : 'B'),
			       pdev->name, status);
		}
	}

	for (pbus = pbus->children; pbus; pbus = pbus->next)
		pci_scan_for_target_abort(p, pbm, pbus);
}

void pci_scan_for_master_abort(struct pci_controller_info *p,
			       struct pci_pbm_info *pbm,
			       struct pci_bus *pbus)
{
	struct pci_dev *pdev;

	for (pdev = pbus->devices; pdev; pdev = pdev->sibling) {
		u16 status, error_bits;

		pci_read_config_word(pdev, PCI_STATUS, &status);
		error_bits =
			(status & (PCI_STATUS_REC_MASTER_ABORT));
		if (error_bits) {
			pci_write_config_word(pdev, PCI_STATUS, error_bits);
			printk("PCI%d(PBM%c): Device [%s] received Master Abort [%016x]\n",
			       p->index, ((pbm == &p->pbm_A) ? 'A' : 'B'),
			       pdev->name, status);
		}
	}

	for (pbus = pbus->children; pbus; pbus = pbus->next)
		pci_scan_for_master_abort(p, pbm, pbus);
}

void pci_scan_for_parity_error(struct pci_controller_info *p,
			       struct pci_pbm_info *pbm,
			       struct pci_bus *pbus)
{
	struct pci_dev *pdev;

	for (pdev = pbus->devices; pdev; pdev = pdev->sibling) {
		u16 status, error_bits;

		pci_read_config_word(pdev, PCI_STATUS, &status);
		error_bits =
			(status & (PCI_STATUS_PARITY |
				   PCI_STATUS_DETECTED_PARITY));
		if (error_bits) {
			pci_write_config_word(pdev, PCI_STATUS, error_bits);
			printk("PCI%d(PBM%c): Device [%s] saw Parity Error [%016x]\n",
			       p->index, ((pbm == &p->pbm_A) ? 'A' : 'B'),
			       pdev->name, status);
		}
	}

	for (pbus = pbus->children; pbus; pbus = pbus->next)
		pci_scan_for_parity_error(p, pbm, pbus);
}
