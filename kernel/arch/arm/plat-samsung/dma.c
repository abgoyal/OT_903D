

struct s3c2410_dma_buf;

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/errno.h>

#include <mach/dma.h>
#include <mach/irqs.h>

/* dma channel state information */
struct s3c2410_dma_chan s3c2410_chans[S3C_DMA_CHANNELS];
struct s3c2410_dma_chan *s3c_dma_chan_map[DMACH_MAX];


struct s3c2410_dma_chan *s3c_dma_lookup_channel(unsigned int channel)
{
	if (channel & DMACH_LOW_LEVEL)
		return &s3c2410_chans[channel & ~DMACH_LOW_LEVEL];
	else
		return s3c_dma_chan_map[channel];
}


int s3c2410_dma_set_opfn(unsigned int channel, s3c2410_dma_opfn_t rtn)
{
	struct s3c2410_dma_chan *chan = s3c_dma_lookup_channel(channel);

	if (chan == NULL)
		return -EINVAL;

	pr_debug("%s: chan=%p, op rtn=%p\n", __func__, chan, rtn);

	chan->op_fn = rtn;

	return 0;
}
EXPORT_SYMBOL(s3c2410_dma_set_opfn);

int s3c2410_dma_set_buffdone_fn(unsigned int channel, s3c2410_dma_cbfn_t rtn)
{
	struct s3c2410_dma_chan *chan = s3c_dma_lookup_channel(channel);

	if (chan == NULL)
		return -EINVAL;

	pr_debug("%s: chan=%p, callback rtn=%p\n", __func__, chan, rtn);

	chan->callback_fn = rtn;

	return 0;
}
EXPORT_SYMBOL(s3c2410_dma_set_buffdone_fn);

int s3c2410_dma_setflags(unsigned int channel, unsigned int flags)
{
	struct s3c2410_dma_chan *chan = s3c_dma_lookup_channel(channel);

	if (chan == NULL)
		return -EINVAL;

	chan->flags = flags;
	return 0;
}
EXPORT_SYMBOL(s3c2410_dma_setflags);
