// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * NXP PNX49xx ALSA SoC platform driver
 *
 * Copyright (C) 2026 BHmsWare <bhmsgamexbox2010@gmail.com>
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

/* DSP volume boundaries */
#define PNX49XX_DSP_MIN_VOLUME		0
#define PNX49XX_DSP_MAX_VOLUME		10

/* sample rates */
#define PNX49XX_DSP_MIN_SAMPLE_RATE		8000
#define PNX49XX_DSP_MAX_SAMPLE_RATE		40000

/* hardware FIFO buffer size */
#define PNX49XX_DSP_HW_BUF_SIZE		0x40

/* hardware DSP registers */
#define PNX49XX_DSP_SETUP_REG		0x04
#define PNX49XX_DSP_CMD_REG			0x40
#define PNX49XX_DSP_DATA_REG		0x44

/* hardware sysconf registers */
#define PNX49XX_SYSCONF_XEN_REG					0x03
#define PNX49XX_SYSCONF_AUDIO_PHAS_CLK_CTL_REG	0x58

static const DECLARE_TLV_DB_SCALE(digital_tlv, -1500, 100, 1);

/* private structure declaration */
struct pnx49xx_audio {
	void __iomem *regs;
	void __iomem *sysconf_regs;
	struct hrtimer timer;
	struct snd_pcm_substream *substream;
	spinlock_t lock;
	u32 buffer_pos;
	u32 period_size_bytes;
	u32 buffer_size_bytes;
	ktime_t timer_interval;
};

/* pcm hardware description */
static const struct snd_pcm_hardware pnx49xx_pcm_hardware = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_INTERLEAVED,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_KNOT,
	.rate_min = PNX49XX_DSP_MIN_SAMPLE_RATE,
	.rate_max = PNX49XX_DSP_MAX_SAMPLE_RATE,
	.channels_min = 1,
	.channels_max = 1,
	.buffer_bytes_max = 32 * 1024,
	.period_bytes_min = 512,
	.period_bytes_max = 4 * 1024,
	.periods_min = 2,
	.periods_max = 8,
};

/* sample rate list */
static const unsigned int pnx49xx_rates[] = {
	PNX49XX_DSP_MIN_SAMPLE_RATE,
	PNX49XX_DSP_MAX_SAMPLE_RATE
};

/* non-standard sample rates here */
static const struct snd_pcm_hw_constraint_list pnx49xx_rate_constraints = {
	.count = ARRAY_SIZE(pnx49xx_rates),
	.list = pnx49xx_rates,
	.mask = 0,
};

/* helper function to interact with DSP */
static u32 pnx49xx_pcm_dsp_peek(void __iomem *regs, int cmd)
{
	writel(0x86, regs + PNX49XX_DSP_SETUP_REG);

	writel(cmd, regs + PNX49XX_DSP_CMD_REG);
	return readl(regs + PNX49XX_DSP_DATA_REG);
}

/* helper function to interact with DSP */
static void pnx49xx_pcm_dsp_poke(void __iomem *regs, int cmd, int data)
{
	writel(0x86, regs + PNX49XX_DSP_SETUP_REG);

	writel(cmd, regs + PNX49XX_DSP_CMD_REG);
	writel(data, regs + PNX49XX_DSP_DATA_REG);
}

static void pnx49xx_pcm_dsp_init(void __iomem *regs, void __iomem *sysconf_regs)
{
	u32 val;

	/* enable dsp crystal */
	writel(0x31, sysconf_regs + PNX49XX_SYSCONF_AUDIO_PHAS_CLK_CTL_REG);
	val = readb(sysconf_regs + PNX49XX_SYSCONF_XEN_REG);
	writeb(val | 0x12, sysconf_regs + PNX49XX_SYSCONF_XEN_REG);

	/* init command sequence */
	pnx49xx_pcm_dsp_poke(regs, 0x7021, 0xffff);

	pnx49xx_pcm_dsp_poke(regs, 0x71a0, 0);
	pnx49xx_pcm_dsp_poke(regs, 0x71a2, 0x1cde);

	pnx49xx_pcm_dsp_poke(regs, 0x71b2, 0x1ea8);
	pnx49xx_pcm_dsp_poke(regs, 0x71b4, 2);

	pnx49xx_pcm_dsp_poke(regs, 0x71e1, 0x0022);
	pnx49xx_pcm_dsp_poke(regs, 0x71e3, 0x0011);
}

/* get volume */
static int pnx49xx_pcm_get_volume(void __iomem *regs)
{
	u32 val;

	/* check if output channel is masked */
	val = pnx49xx_pcm_dsp_peek(regs, 0x71b0);
	if ((val & 0x2048) != 0) {
		return 0;
	} else {
		return (pnx49xx_pcm_dsp_peek(regs, 0x71b3) & 0xf) + 1;
	}
}

/* set volume */
static void pnx49xx_pcm_set_volume(void __iomem *regs, int level)
{
	if (!level) {
		/* if level == 0, disable the amplifier and set the volume down to 0 */
		pnx49xx_pcm_dsp_poke(regs, 0x71b3, 0);
		pnx49xx_pcm_dsp_poke(regs, 0x71b0, 0x22fc);
	} else {
		/* if level > 0, enable the amplifier and set the volume */
		pnx49xx_pcm_dsp_poke(regs, 0x71b3, (level - 1) * 0x11);
		pnx49xx_pcm_dsp_poke(regs, 0x71b0, 0x2b4);
	}
}

/* do nothing on close */
static void pnx49xx_pcm_dsp_close(void __iomem *regs)
{
}

static enum hrtimer_restart pnx49xx_timer_callback(struct hrtimer *timer)
{
	struct pnx49xx_audio *priv = container_of(timer, struct pnx49xx_audio, timer);
	struct snd_pcm_substream *substream = priv->substream;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;
	bool period_elapsed = false;
	unsigned int fifo_full;
	s16 sample;
	int i;

	if (!snd_pcm_running(substream))
		return HRTIMER_NORESTART;

	runtime = substream->runtime;

	spin_lock_irqsave(&priv->lock, flags);

	/* we'll interact with the registers directly
	 * without helpers to reach the best performance. */
	writel(0x86, priv->regs + PNX49XX_DSP_SETUP_REG);

	/* get number of bytes still in FIFO buffer */
	writel(0x7192, priv->regs + PNX49XX_DSP_CMD_REG);
	fifo_full = readl(priv->regs + PNX49XX_DSP_DATA_REG);
	if (fifo_full >= PNX49XX_DSP_HW_BUF_SIZE)
		return HRTIMER_NORESTART;

	/* push data to remained free cells in FIFO buffer;
	 * free = (PNX49XX_DSP_HW_BUF_SIZE - fifo_full) */
	writel(0x7191, priv->regs + PNX49XX_DSP_CMD_REG);
	for (i = PNX49XX_DSP_HW_BUF_SIZE; i > fifo_full; i--) {
		sample = *(u16 *)(runtime->dma_area + priv->buffer_pos);
		/* DSP is likely 13-bit, shift them. */
		writel(sample >> 3, priv->regs + PNX49XX_DSP_DATA_REG);

		priv->buffer_pos += 2; 

		if ((priv->buffer_pos % priv->period_size_bytes) == 0)
			period_elapsed = true;

		/* not enough data in the ALSA software buffer */
		if (priv->buffer_pos >= priv->buffer_size_bytes) {
			priv->buffer_pos = 0;
			break;
		}
	}

	spin_unlock_irqrestore(&priv->lock, flags);

	if (period_elapsed)
		snd_pcm_period_elapsed(substream);

	hrtimer_forward_now(timer, priv->timer_interval);
	return HRTIMER_RESTART;
}


/* get volume */
static int pnx49xx_pcm_vol_get(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	u32 val;

	struct snd_soc_component *comp = snd_kcontrol_chip(kcontrol);
	struct pnx49xx_audio *priv = snd_soc_card_get_drvdata(comp->card);

	val = pnx49xx_pcm_get_volume(priv->regs);
	ucontrol->value.integer.value[0] = val & 0xf;

	return 0;
}

/* set volume */
static int pnx49xx_pcm_vol_put(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	u32 val;

	struct snd_soc_component *comp = snd_kcontrol_chip(kcontrol);
	struct pnx49xx_audio *priv = snd_soc_card_get_drvdata(comp->card);

	val = ucontrol->value.integer.value[0];
	pnx49xx_pcm_set_volume(priv->regs, val);

	return 0;
}

/* min and max volume supported my this DSP */
static const struct soc_mixer_control pnx49xx_pcm_vol_setup = {
	.reg = 0,
	.min = PNX49XX_DSP_MIN_VOLUME,
	.max = PNX49XX_DSP_MAX_VOLUME,
};

/* controls list */
static const struct snd_kcontrol_new pnx49xx_pcm_controls[] = {
	{
		.iface				= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name				= "Master Volume",
		.info				= snd_soc_info_volsw,
		.get				= pnx49xx_pcm_vol_get,
		.put				= pnx49xx_pcm_vol_put,
		.private_value		= (unsigned long)&pnx49xx_pcm_vol_setup,
		.tlv = {
			.p = digital_tlv,
		},
	},
};

/* this function allocates all buffers needed for playback */
static int pnx49xx_pcm_new(struct snd_soc_component *component,
                               struct snd_soc_pcm_runtime *rtd)
{
    struct snd_pcm *pcm = rtd->pcm;

    return snd_pcm_set_managed_buffer_all(pcm,
                                          SNDRV_DMA_TYPE_DEV,
                                          component->dev,
                                          4 * 1024,
                                          32 * 1024);
}

/* open the playback */
static int pnx49xx_pcm_open(struct snd_soc_component *component, struct snd_pcm_substream *substream)
{
	struct pnx49xx_audio *priv = snd_soc_card_get_drvdata(component->card);

	snd_soc_set_runtime_hwparams(substream, &pnx49xx_pcm_hardware);
	priv->substream = substream;
	
	/* the DSP uses non-standard rate 40000 Hz for audio playback
	 * so we need to use constraint list array. */
	return snd_pcm_hw_constraint_list(substream->runtime, 0,
					  SNDRV_PCM_HW_PARAM_RATE,
					  &pnx49xx_rate_constraints);
}

/* close the playback */
static int pnx49xx_pcm_close(struct snd_soc_component *component, struct snd_pcm_substream *substream)
{
	struct pnx49xx_audio *priv = snd_soc_card_get_drvdata(component->card);

	pnx49xx_pcm_dsp_close(priv->regs);

	return 0;
}

/* set playback parameters */
static int pnx49xx_pcm_hw_params(struct snd_soc_component *component, 
				 struct snd_pcm_substream *substream, 
				 struct snd_pcm_hw_params *params)
{
	struct pnx49xx_audio *priv = snd_soc_card_get_drvdata(component->card);
	int rate;

	priv->period_size_bytes = params_period_bytes(params);
	priv->buffer_size_bytes = params_buffer_bytes(params);

	rate = params_rate(params);
	
	/* only 8000 Hz and 40000 Hz */
	pnx49xx_pcm_dsp_poke(priv->regs, 0x71a1,
			4 | (rate == PNX49XX_DSP_MAX_SAMPLE_RATE));
	
	return 0;
}

/* we didn't allocate anything... */
static int pnx49xx_pcm_hw_free(struct snd_soc_component *component, struct snd_pcm_substream *substream)
{
	return 0;
}

/* playback trigger */
static int pnx49xx_pcm_trigger(struct snd_soc_component *component, struct snd_pcm_substream *substream, int cmd)
{
	struct pnx49xx_audio *priv = snd_soc_card_get_drvdata(component->card);
	u32 us_interval;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		priv->buffer_pos = 0;
		
		/* wake up the driver every time half of the hardware FIFO buffer empties */
		us_interval = (PNX49XX_DSP_HW_BUF_SIZE * 1000000) / substream->runtime->rate;
		priv->timer_interval = us_to_ktime(us_interval / 2);

		/* enable high resolution timer, because DSP on this turd chipset
		 * doesn't have a dedicated INTC line for its interrupts (ig). */
		hrtimer_start(&priv->timer, priv->timer_interval, HRTIMER_MODE_REL);
		break;

	case SNDRV_PCM_TRIGGER_STOP:
		/* disable high resolution timer */
		hrtimer_try_to_cancel(&priv->timer);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

/* get the current hardware position of an audio stream */
static snd_pcm_uframes_t pnx49xx_pcm_pointer(struct snd_soc_component *component, struct snd_pcm_substream *substream)
{
	struct pnx49xx_audio *priv = snd_soc_card_get_drvdata(component->card);
	return bytes_to_frames(substream->runtime, priv->buffer_pos);
}

/* callback structure */
static const struct snd_soc_component_driver pnx49xx_soc_component = {
	.name				= "pnx49xx-pcm",

	.controls			= pnx49xx_pcm_controls,
	.num_controls		= ARRAY_SIZE(pnx49xx_pcm_controls),

	.pcm_new			= pnx49xx_pcm_new,
	.open				= pnx49xx_pcm_open,
	.close				= pnx49xx_pcm_close,
	.hw_params			= pnx49xx_pcm_hw_params,
	.hw_free			= pnx49xx_pcm_hw_free,
	.trigger			= pnx49xx_pcm_trigger,
	.pointer			= pnx49xx_pcm_pointer,
};

/* Digital Audio Interface structure */
static struct snd_soc_dai_driver pnx49xx_dai = {
	.name = "pnx49xx-pcm-dai",
	.playback = {
		.stream_name = "Master",
		.channels_min = 1,
		.channels_max = 1,
		.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_KNOT,
		.rate_min = PNX49XX_DSP_MIN_SAMPLE_RATE,
		.rate_max = PNX49XX_DSP_MAX_SAMPLE_RATE,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
};

static int pnx49xx_pcm_probe(struct platform_device *pdev)
{
	struct pnx49xx_audio *priv;
	struct snd_soc_card *card;
	struct snd_soc_dai_link *dai_link;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	spin_lock_init(&priv->lock);

	/* get DSP register virtual base address */
	priv->regs = devm_platform_ioremap_resource_byname(pdev, "dsp");
	if (IS_ERR(priv->regs))
		return PTR_ERR(priv->regs);

	/* get sysconf register virtual base address.
	 *
	 * Generally we enable crystals and control
	 * audio phase clocks here. */
	priv->sysconf_regs = devm_platform_ioremap_resource_byname(pdev, "sysconf");
	if (IS_ERR(priv->sysconf_regs))
		return PTR_ERR(priv->sysconf_regs);

	/* setup the high resolution timer */
	hrtimer_setup(&priv->timer, pnx49xx_timer_callback, CLOCK_MONOTONIC, HRTIMER_MODE_REL);

	/* registering ASoC component */
	ret = snd_soc_register_component(&pdev->dev, &pnx49xx_soc_component,
					       &pnx49xx_dai, 1);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register ASoC component\n");
		return ret;
	}

	/* setting up the dai_link and fields inside it */
	dai_link = devm_kzalloc(&pdev->dev, sizeof(*dai_link), GFP_KERNEL);
	if (!dai_link)
		return -ENOMEM;
	
	dai_link->name = "PNX49xx PCM";
	dai_link->stream_name = "PNX49xx PCM";

	dai_link->cpus = devm_kzalloc(&pdev->dev, sizeof(*dai_link->cpus), GFP_KERNEL);
	dai_link->codecs = devm_kzalloc(&pdev->dev, sizeof(*dai_link->codecs), GFP_KERNEL);
	dai_link->platforms = devm_kzalloc(&pdev->dev, sizeof(*dai_link->platforms), GFP_KERNEL);
	
	if (!dai_link->cpus || !dai_link->codecs || !dai_link->platforms)
		return -ENOMEM;
	
	dai_link->num_cpus = 1;
	dai_link->num_codecs = 1;
	dai_link->num_platforms = 1;

	dai_link->cpus->dai_name = "pnx49xx-pcm-dai";
	dai_link->codecs->dai_name = "snd-soc-dummy-dai";
	dai_link->codecs->name = "snd-soc-dummy";
	dai_link->platforms->name = dev_name(&pdev->dev);
	
	/* setting up the sound card structure */
	card = devm_kzalloc(&pdev->dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	snd_soc_card_set_drvdata(card, priv);
	
	card->name = "pnx49xx-card";
	card->dev = &pdev->dev;
	card->dai_link = dai_link;
	card->num_links = 1;
	
	/* registering ASoC sound card */
	ret = snd_soc_register_card(card);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register sound card: %d\n", ret);
		return ret;
	}

	pnx49xx_pcm_dsp_init(priv->regs, priv->sysconf_regs);

	dev_info(&pdev->dev, "ASoC driver loaded\n");
	return 0;
}

static const struct of_device_id pnx49xx_pcm_of_match[] = {
	{ .compatible = "nxp,pnx49xx-audio", },
	{ }
};
MODULE_DEVICE_TABLE(of, pnx49xx_pcm_of_match);

static struct platform_driver pnx49xx_pcm_driver = {
	.driver = {
		.name = "pnx49xx-audio",
		.of_match_table = pnx49xx_pcm_of_match,
	},
	.probe = pnx49xx_pcm_probe,
};
module_platform_driver(pnx49xx_pcm_driver);

MODULE_AUTHOR("BHmsWare <bhmsgamexbox2010@gmail.com>");
MODULE_DESCRIPTION("NXP PNX49xx ALSA SoC platform driver");
MODULE_LICENSE("GPL");
