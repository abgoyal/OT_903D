

#ifndef __ASM_ARCH_DMA_H
#define __ASM_ARCH_DMA_H __FILE__

#include <plat/dma.h>
#include <linux/sysdev.h>

#define MAX_DMA_TRANSFER_SIZE   0x100000 /* Data Unit is half word  */


enum dma_ch {
	DMACH_XD0,
	DMACH_XD1,
	DMACH_SDI,
	DMACH_SPI0,
	DMACH_SPI1,
	DMACH_UART0,
	DMACH_UART1,
	DMACH_UART2,
	DMACH_TIMER,
	DMACH_I2S_IN,
	DMACH_I2S_OUT,
	DMACH_PCM_IN,
	DMACH_PCM_OUT,
	DMACH_MIC_IN,
	DMACH_USB_EP1,
	DMACH_USB_EP2,
	DMACH_USB_EP3,
	DMACH_USB_EP4,
	DMACH_UART0_SRC2,	/* s3c2412 second uart sources */
	DMACH_UART1_SRC2,
	DMACH_UART2_SRC2,
	DMACH_UART3,		/* s3c2443 has extra uart */
	DMACH_UART3_SRC2,
	DMACH_MAX,		/* the end entry */
};

#define DMACH_LOW_LEVEL	(1<<28)	/* use this to specifiy hardware ch no */

/* we have 4 dma channels */
#if !defined(CONFIG_CPU_S3C2443) && !defined(CONFIG_CPU_S3C2416)
#define S3C_DMA_CHANNELS		(4)
#else
#define S3C_DMA_CHANNELS		(6)
#endif

/* types */

enum s3c2410_dma_state {
	S3C2410_DMA_IDLE,
	S3C2410_DMA_RUNNING,
	S3C2410_DMA_PAUSED
};


enum s3c2410_dma_loadst {
	S3C2410_DMALOAD_NONE,
	S3C2410_DMALOAD_1LOADED,
	S3C2410_DMALOAD_1RUNNING,
	S3C2410_DMALOAD_1LOADED_1RUNNING,
};


/* flags */

#define S3C2410_DMAF_SLOW         (1<<0)   /* slow, so don't worry about
					    * waiting for reloads */
#define S3C2410_DMAF_AUTOSTART    (1<<1)   /* auto-start if buffer queued */

#define S3C2410_DMAF_CIRCULAR	(1 << 2)	/* no circular dma support */

/* dma buffer */

struct s3c2410_dma_buf;


struct s3c2410_dma_buf {
	struct s3c2410_dma_buf	*next;
	int			 magic;		/* magic */
	int			 size;		/* buffer size in bytes */
	dma_addr_t		 data;		/* start of DMA data */
	dma_addr_t		 ptr;		/* where the DMA got to [1] */
	void			*id;		/* client's id */
};

/* [1] is this updated for both recv/send modes? */

struct s3c2410_dma_stats {
	unsigned long		loads;
	unsigned long		timeout_longest;
	unsigned long		timeout_shortest;
	unsigned long		timeout_avg;
	unsigned long		timeout_failed;
};

struct s3c2410_dma_map;


struct s3c2410_dma_chan {
	/* channel state flags and information */
	unsigned char		 number;      /* number of this dma channel */
	unsigned char		 in_use;      /* channel allocated */
	unsigned char		 irq_claimed; /* irq claimed for channel */
	unsigned char		 irq_enabled; /* irq enabled for channel */
	unsigned char		 xfer_unit;   /* size of an transfer */

	/* channel state */

	enum s3c2410_dma_state	 state;
	enum s3c2410_dma_loadst	 load_state;
	struct s3c2410_dma_client *client;

	/* channel configuration */
	enum s3c2410_dmasrc	 source;
	enum dma_ch		 req_ch;
	unsigned long		 dev_addr;
	unsigned long		 load_timeout;
	unsigned int		 flags;		/* channel flags */

	struct s3c24xx_dma_map	*map;		/* channel hw maps */

	/* channel's hardware position and configuration */
	void __iomem		*regs;		/* channels registers */
	void __iomem		*addr_reg;	/* data address register */
	unsigned int		 irq;		/* channel irq */
	unsigned long		 dcon;		/* default value of DCON */

	/* driver handles */
	s3c2410_dma_cbfn_t	 callback_fn;	/* buffer done callback */
	s3c2410_dma_opfn_t	 op_fn;		/* channel op callback */

	/* stats gathering */
	struct s3c2410_dma_stats *stats;
	struct s3c2410_dma_stats  stats_store;

	/* buffer list and information */
	struct s3c2410_dma_buf	*curr;		/* current dma buffer */
	struct s3c2410_dma_buf	*next;		/* next buffer to load */
	struct s3c2410_dma_buf	*end;		/* end of queue */

	/* system device */
	struct sys_device	dev;
};

typedef unsigned long dma_device_t;

static inline bool s3c_dma_has_circular(void)
{
	return false;
}

#endif /* __ASM_ARCH_DMA_H */