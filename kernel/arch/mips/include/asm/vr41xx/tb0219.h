
#ifndef __TANBAC_TB0219_H
#define __TANBAC_TB0219_H

#include <asm/vr41xx/irq.h>

#define TB0219_PCI_SLOT1_PIN		2
#define TB0219_PCI_SLOT2_PIN		3
#define TB0219_PCI_SLOT3_PIN		4

#define TB0219_PCI_SLOT1_IRQ		GIU_IRQ(TB0219_PCI_SLOT1_PIN)
#define TB0219_PCI_SLOT2_IRQ		GIU_IRQ(TB0219_PCI_SLOT2_PIN)
#define TB0219_PCI_SLOT3_IRQ		GIU_IRQ(TB0219_PCI_SLOT3_PIN)

#endif /* __TANBAC_TB0219_H */
