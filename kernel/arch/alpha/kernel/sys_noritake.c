

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/bitops.h>

#include <asm/ptrace.h>
#include <asm/system.h>
#include <asm/dma.h>
#include <asm/irq.h>
#include <asm/mmu_context.h>
#include <asm/io.h>
#include <asm/pgtable.h>
#include <asm/core_apecs.h>
#include <asm/core_cia.h>
#include <asm/tlbflush.h>

#include "proto.h"
#include "irq_impl.h"
#include "pci_impl.h"
#include "machvec_impl.h"

/* Note mask bit is true for ENABLED irqs.  */
static int cached_irq_mask;

static inline void
noritake_update_irq_hw(int irq, int mask)
{
	int port = 0x54a;
	if (irq >= 32) {
	    mask >>= 16;
	    port = 0x54c;
	}
	outw(mask, port);
}

static void
noritake_enable_irq(unsigned int irq)
{
	noritake_update_irq_hw(irq, cached_irq_mask |= 1 << (irq - 16));
}

static void
noritake_disable_irq(unsigned int irq)
{
	noritake_update_irq_hw(irq, cached_irq_mask &= ~(1 << (irq - 16)));
}

static unsigned int
noritake_startup_irq(unsigned int irq)
{
	noritake_enable_irq(irq);
	return 0;
}

static void
noritake_end_irq(unsigned int irq)
{
        if (!(irq_desc[irq].status & (IRQ_DISABLED|IRQ_INPROGRESS)))
                noritake_enable_irq(irq);
}

static struct irq_chip noritake_irq_type = {
	.name		= "NORITAKE",
	.startup	= noritake_startup_irq,
	.shutdown	= noritake_disable_irq,
	.enable		= noritake_enable_irq,
	.disable	= noritake_disable_irq,
	.ack		= noritake_disable_irq,
	.end		= noritake_end_irq,
};

static void 
noritake_device_interrupt(unsigned long vector)
{
	unsigned long pld;
	unsigned int i;

	/* Read the interrupt summary registers of NORITAKE */
	pld = (((unsigned long) inw(0x54c) << 32)
	       | ((unsigned long) inw(0x54a) << 16)
	       | ((unsigned long) inb(0xa0) << 8)
	       | inb(0x20));

	/*
	 * Now for every possible bit set, work through them and call
	 * the appropriate interrupt handler.
	 */
	while (pld) {
		i = ffz(~pld);
		pld &= pld - 1; /* clear least bit set */
		if (i < 16) {
			isa_device_interrupt(vector);
		} else {
			handle_irq(i);
		}
	}
}

static void 
noritake_srm_device_interrupt(unsigned long vector)
{
	int irq;

	irq = (vector - 0x800) >> 4;

	/*
	 * I really hate to do this, too, but the NORITAKE SRM console also
	 * reports PCI vectors *lower* than I expected from the bit numbers
	 * in the documentation.
	 * But I really don't want to change the fixup code for allocation
	 * of IRQs, nor the alpha_irq_mask maintenance stuff, both of which
	 * look nice and clean now.
	 * So, here's this additional grotty hack... :-(
	 */
	if (irq >= 16)
		irq = irq + 1;

	handle_irq(irq);
}

static void __init
noritake_init_irq(void)
{
	long i;

	if (alpha_using_srm)
		alpha_mv.device_interrupt = noritake_srm_device_interrupt;

	outw(0, 0x54a);
	outw(0, 0x54c);

	for (i = 16; i < 48; ++i) {
		irq_desc[i].status = IRQ_DISABLED | IRQ_LEVEL;
		irq_desc[i].chip = &noritake_irq_type;
	}

	init_i8259a_irqs();
	common_init_isa_dma();
}



static int __init
noritake_map_irq(struct pci_dev *dev, u8 slot, u8 pin)
{
	static char irq_tab[15][5] __initdata = {
		/*INT    INTA   INTB   INTC   INTD */
		/* note: IDSELs 16, 17, and 25 are CORELLE only */
		{ 16+1,  16+1,  16+1,  16+1,  16+1},  /* IdSel 16,  QLOGIC */
		{   -1,    -1,    -1,    -1,    -1},  /* IdSel 17, S3 Trio64 */
		{   -1,    -1,    -1,    -1,    -1},  /* IdSel 18,  PCEB */
		{   -1,    -1,    -1,    -1,    -1},  /* IdSel 19,  PPB  */
		{   -1,    -1,    -1,    -1,    -1},  /* IdSel 20,  ???? */
		{   -1,    -1,    -1,    -1,    -1},  /* IdSel 21,  ???? */
		{ 16+2,  16+2,  16+3,  32+2,  32+3},  /* IdSel 22,  slot 0 */
		{ 16+4,  16+4,  16+5,  32+4,  32+5},  /* IdSel 23,  slot 1 */
		{ 16+6,  16+6,  16+7,  32+6,  32+7},  /* IdSel 24,  slot 2 */
		{ 16+8,  16+8,  16+9,  32+8,  32+9},  /* IdSel 25,  slot 3 */
		/* The following 5 are actually on PCI bus 1, which is 
		   across the built-in bridge of the NORITAKE only.  */
		{ 16+1,  16+1,  16+1,  16+1,  16+1},  /* IdSel 16,  QLOGIC */
		{ 16+8,  16+8,  16+9,  32+8,  32+9},  /* IdSel 17,  slot 3 */
		{16+10, 16+10, 16+11, 32+10, 32+11},  /* IdSel 18,  slot 4 */
		{16+12, 16+12, 16+13, 32+12, 32+13},  /* IdSel 19,  slot 5 */
		{16+14, 16+14, 16+15, 32+14, 32+15},  /* IdSel 20,  slot 6 */
	};
	const long min_idsel = 5, max_idsel = 19, irqs_per_slot = 5;
	return COMMON_TABLE_LOOKUP;
}

static u8 __init
noritake_swizzle(struct pci_dev *dev, u8 *pinp)
{
	int slot, pin = *pinp;

	if (dev->bus->number == 0) {
		slot = PCI_SLOT(dev->devfn);
	}
	/* Check for the built-in bridge */
	else if (PCI_SLOT(dev->bus->self->devfn) == 8) {
		slot = PCI_SLOT(dev->devfn) + 15; /* WAG! */
	}
	else
	{
		/* Must be a card-based bridge.  */
		do {
			if (PCI_SLOT(dev->bus->self->devfn) == 8) {
				slot = PCI_SLOT(dev->devfn) + 15;
				break;
			}
			pin = pci_swizzle_interrupt_pin(dev, pin);

			/* Move up the chain of bridges.  */
			dev = dev->bus->self;
			/* Slot of the next bridge.  */
			slot = PCI_SLOT(dev->devfn);
		} while (dev->bus->self);
	}
	*pinp = pin;
	return slot;
}

#if defined(CONFIG_ALPHA_GENERIC) || !defined(CONFIG_ALPHA_PRIMO)
static void
noritake_apecs_machine_check(unsigned long vector, unsigned long la_ptr)
{
#define MCHK_NO_DEVSEL 0x205U
#define MCHK_NO_TABT 0x204U

        struct el_common *mchk_header;
        unsigned int code;

        mchk_header = (struct el_common *)la_ptr;

        /* Clear the error before any reporting.  */
        mb();
        mb(); /* magic */
        draina();
        apecs_pci_clr_err();
        wrmces(0x7);
        mb();

        code = mchk_header->code;
        process_mcheck_info(vector, la_ptr, "NORITAKE APECS",
                            (mcheck_expected(0)
                             && (code == MCHK_NO_DEVSEL
                                 || code == MCHK_NO_TABT)));
}
#endif



#if defined(CONFIG_ALPHA_GENERIC) || !defined(CONFIG_ALPHA_PRIMO)
struct alpha_machine_vector noritake_mv __initmv = {
	.vector_name		= "Noritake",
	DO_EV4_MMU,
	DO_DEFAULT_RTC,
	DO_APECS_IO,
	.machine_check		= noritake_apecs_machine_check,
	.max_isa_dma_address	= ALPHA_MAX_ISA_DMA_ADDRESS,
	.min_io_address		= EISA_DEFAULT_IO_BASE,
	.min_mem_address	= APECS_AND_LCA_DEFAULT_MEM_BASE,

	.nr_irqs		= 48,
	.device_interrupt	= noritake_device_interrupt,

	.init_arch		= apecs_init_arch,
	.init_irq		= noritake_init_irq,
	.init_rtc		= common_init_rtc,
	.init_pci		= common_init_pci,
	.pci_map_irq		= noritake_map_irq,
	.pci_swizzle		= noritake_swizzle,
};
ALIAS_MV(noritake)
#endif

#if defined(CONFIG_ALPHA_GENERIC) || defined(CONFIG_ALPHA_PRIMO)
struct alpha_machine_vector noritake_primo_mv __initmv = {
	.vector_name		= "Noritake-Primo",
	DO_EV5_MMU,
	DO_DEFAULT_RTC,
	DO_CIA_IO,
	.machine_check		= cia_machine_check,
	.max_isa_dma_address	= ALPHA_MAX_ISA_DMA_ADDRESS,
	.min_io_address		= EISA_DEFAULT_IO_BASE,
	.min_mem_address	= CIA_DEFAULT_MEM_BASE,

	.nr_irqs		= 48,
	.device_interrupt	= noritake_device_interrupt,

	.init_arch		= cia_init_arch,
	.init_irq		= noritake_init_irq,
	.init_rtc		= common_init_rtc,
	.init_pci		= cia_init_pci,
	.kill_arch		= cia_kill_arch,
	.pci_map_irq		= noritake_map_irq,
	.pci_swizzle		= noritake_swizzle,
};
ALIAS_MV(noritake_primo)
#endif