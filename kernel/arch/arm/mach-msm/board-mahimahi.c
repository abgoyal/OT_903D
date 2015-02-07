

#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>

#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/setup.h>

#include <mach/board.h>
#include <mach/hardware.h>
#include <mach/system.h>

#include "board-mahimahi.h"
#include "devices.h"
#include "proc_comm.h"

static uint debug_uart;

module_param_named(debug_uart, debug_uart, uint, 0);

static struct platform_device *devices[] __initdata = {
#if !defined(CONFIG_MSM_SERIAL_DEBUGGER)
	&msm_device_uart1,
#endif
	&msm_device_uart_dm1,
	&msm_device_nand,
};

static void __init mahimahi_init(void)
{
	platform_add_devices(devices, ARRAY_SIZE(devices));
}

static void __init mahimahi_fixup(struct machine_desc *desc, struct tag *tags,
				 char **cmdline, struct meminfo *mi)
{
	mi->nr_banks = 2;
	mi->bank[0].start = PHYS_OFFSET;
	mi->bank[0].node = PHYS_TO_NID(PHYS_OFFSET);
	mi->bank[0].size = (219*1024*1024);
	mi->bank[1].start = MSM_HIGHMEM_BASE;
	mi->bank[1].node = PHYS_TO_NID(MSM_HIGHMEM_BASE);
	mi->bank[1].size = MSM_HIGHMEM_SIZE;
}

static void __init mahimahi_map_io(void)
{
	msm_map_common_io();
	msm_clock_init();
}

extern struct sys_timer msm_timer;

MACHINE_START(MAHIMAHI, "mahimahi")
#ifdef CONFIG_MSM_DEBUG_UART
	.phys_io        = MSM_DEBUG_UART_PHYS,
	.io_pg_offst    = ((MSM_DEBUG_UART_BASE) >> 18) & 0xfffc,
#endif
	.boot_params	= 0x20000100,
	.fixup		= mahimahi_fixup,
	.map_io		= mahimahi_map_io,
	.init_irq	= msm_init_irq,
	.init_machine	= mahimahi_init,
	.timer		= &msm_timer,
MACHINE_END