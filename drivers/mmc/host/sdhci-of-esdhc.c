/*
 * Freescale eSDHC controller driver.
 *
 * Copyright (c) 2007, 2010, 2012 Freescale Semiconductor, Inc.
 * Copyright (c) 2009 MontaVista Software, Inc.
 *
 * Authors: Xiaobo Xie <X.Xie@freescale.com>
 *	    Anton Vorontsov <avorontsov@ru.mvista.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include <linux/err.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mmc/host.h>
#include "sdhci-pltfm.h"
#include "sdhci-esdhc.h"
#include <asm/mpc85xx.h>

#define VENDOR_V_22	0x12
#define VENDOR_V_23	0x13

static u32 svr;

static u32 esdhc_readl(struct sdhci_host *host, int reg)
{
	u32 ret;

	ret = in_be32(host->ioaddr + reg);
	/*
	 * The bit of ADMA flag in eSDHC is not compatible with standard
	 * SDHC register, so set fake flag SDHCI_CAN_DO_ADMA2 when ADMA is
	 * supported by eSDHC.
	 * And for many FSL eSDHC controller, the reset value of field
	 * SDHCI_CAN_DO_ADMA1 is one, but some of them can't support ADMA,
	 * only these vendor version is greater than 2.2/0x12 support ADMA.
	 * For FSL eSDHC, must aligned 4-byte, so use 0xFC to read the
	 * the verdor version number, oxFE is SDHCI_HOST_VERSION.
	 */
	if ((reg == SDHCI_CAPABILITIES) && (ret & SDHCI_CAN_DO_ADMA1)) {
		u32 tmp = in_be32(host->ioaddr + SDHCI_SLOT_INT_STATUS);
		tmp = (tmp & SDHCI_VENDOR_VER_MASK) >> SDHCI_VENDOR_VER_SHIFT;
		if (tmp > VENDOR_V_22)
			ret |= SDHCI_CAN_DO_ADMA2;
	}

	/*
	 * Clock of eSDHC host don't support to be disabled and enabled.
	 * So clock stable bit doesn't behave as it mean, So fix it to
	 * '1' to avoid misreading.
	 */
	if (reg == SDHCI_PRESENT_STATE)
		ret |= ESDHC_CLK_STABLE;

	return ret;
}

static u16 esdhc_readw(struct sdhci_host *host, int reg)
{
	u16 ret;
	int base = reg & ~0x3;
	int shift = (reg & 0x2) * 8;

	if (unlikely(reg == SDHCI_HOST_VERSION))
		ret = in_be32(host->ioaddr + base) & 0xffff;
	else
		ret = (in_be32(host->ioaddr + base) >> shift) & 0xffff;

	/* T4240-R1.0-R2.0 had a incorrect vendor version and spec version */
	if ((reg == SDHCI_HOST_VERSION) &&
			((SVR_SOC_VER(svr) == SVR_T4240) &&
			 (SVR_REV(svr) <= 0x20)))
		ret = (VENDOR_V_23 << SDHCI_VENDOR_VER_SHIFT) | SDHCI_SPEC_200;

	return ret;
}

static u8 esdhc_readb(struct sdhci_host *host, int reg)
{
	int base = reg & ~0x3;
	int shift = (reg & 0x3) * 8;
	u8 ret = (in_be32(host->ioaddr + base) >> shift) & 0xff;

	/*
	 * "DMA select" locates at offset 0x28 in SD specification, but on
	 * P5020 or P3041, it locates at 0x29.
	 */
	if (reg == SDHCI_HOST_CONTROL) {
		u32 dma_bits;

		dma_bits = in_be32(host->ioaddr + reg);
		/* DMA select is 22,23 bits in Protocol Control Register */
		dma_bits = (dma_bits >> 5) & SDHCI_CTRL_DMA_MASK;

		/* fixup the result */
		ret &= ~SDHCI_CTRL_DMA_MASK;
		ret |= dma_bits;
	}

	return ret;
}

static void esdhc_writel(struct sdhci_host *host, u32 val, int reg)
{
	/*
	 * Enable IRQSTATEN[BGESEN] is just to set IRQSTAT[BGE]
	 * when SYSCTL[RSTD]) is set for some special operations.
	 * No any impact other operation.
	 */
	if (reg == SDHCI_INT_ENABLE)
		val |= SDHCI_INT_BLK_GAP;
	sdhci_be32bs_writel(host, val, reg);
}

static void esdhc_writew(struct sdhci_host *host, u16 val, int reg)
{
	if (reg == SDHCI_BLOCK_SIZE) {
		/*
		 * Two last DMA bits are reserved, and first one is used for
		 * non-standard blksz of 4096 bytes that we don't support
		 * yet. So clear the DMA boundary bits.
		 */
		val &= ~SDHCI_MAKE_BLKSZ(0x7, 0);
	}
	sdhci_be32bs_writew(host, val, reg);
}

static void esdhc_writeb(struct sdhci_host *host, u8 val, int reg)
{
	/*
	 * "DMA select" location is offset 0x28 in SD specification, but on
	 * P5020 or P3041, it's located at 0x29.
	 */
	if (reg == SDHCI_HOST_CONTROL) {
		u32 dma_bits;

		/*
		 * If host control register is not standard, exit
		 * this function
		 */
		if (host->quirks2 & SDHCI_QUIRK2_BROKEN_HOST_CONTROL)
			return;

		/* DMA select is 22,23 bits in Protocol Control Register */
		dma_bits = (val & SDHCI_CTRL_DMA_MASK) << 5;
		clrsetbits_be32(host->ioaddr + reg , SDHCI_CTRL_DMA_MASK << 5,
			dma_bits);
		val &= ~SDHCI_CTRL_DMA_MASK;
		val |= in_be32(host->ioaddr + reg) & SDHCI_CTRL_DMA_MASK;
	}

	/* Prevent SDHCI core from writing reserved bits (e.g. HISPD). */
	if (reg == SDHCI_HOST_CONTROL)
		val &= ~ESDHC_HOST_CONTROL_RES;

	/*
	 * If we have this quirk:
	 * 1. Disabled the clock.
	 * 2. Perform reset all command.
	 * 3. Enable the clock.
	 */
	if ((reg == SDHCI_SOFTWARE_RESET) &&
			(host->quirks2 & SDHCI_QUIRK2_BROKEN_RESET_ALL) &&
			(val & SDHCI_RESET_ALL)) {
		u32 temp;

		temp = esdhc_readl(host, ESDHC_SYSTEM_CONTROL);
		temp &= ~ESDHC_CLOCK_CRDEN;
		esdhc_writel(host, temp, ESDHC_SYSTEM_CONTROL);

		sdhci_be32bs_writeb(host, val, reg);

		temp |= ESDHC_CLOCK_CRDEN;
		esdhc_writel(host, temp, ESDHC_SYSTEM_CONTROL);

		return;
	}

	if (reg == SDHCI_POWER_CONTROL) {
		/* eSDHC don't support gate off power */
		if (!host->pwr || !val)
			return;

		if (SVR_SOC_VER(svr) == SVR_T4240) {
			u8 vol;

			vol = sdhci_be32bs_readb(host, reg);
			if (host->pwr == SDHCI_POWER_180)
				vol &= ~ESDHC_VOL_SEL;
			else
				vol |= ESDHC_VOL_SEL;
		} else
			return;
	}

	sdhci_be32bs_writeb(host, val, reg);
}

/*
 * For Abort or Suspend after Stop at Block Gap, ignore the ADMA
 * error(IRQSTAT[ADMAE]) if both Transfer Complete(IRQSTAT[TC])
 * and Block Gap Event(IRQSTAT[BGE]) are also set.
 * For Continue, apply soft reset for data(SYSCTL[RSTD]);
 * and re-issue the entire read transaction from beginning.
 */
static void esdhci_of_adma_workaround(struct sdhci_host *host, u32 intmask)
{
	u32 tmp;
	bool applicable;
	dma_addr_t dmastart;
	dma_addr_t dmanow;

	tmp = in_be32(host->ioaddr + SDHCI_SLOT_INT_STATUS);
	tmp = (tmp & SDHCI_VENDOR_VER_MASK) >> SDHCI_VENDOR_VER_SHIFT;

	applicable = (intmask & SDHCI_INT_DATA_END) &&
		(intmask & SDHCI_INT_BLK_GAP) &&
		(tmp == VENDOR_V_23);
	if (applicable) {

		sdhci_reset(host, SDHCI_RESET_DATA);
		host->data->error = 0;
		dmastart = sg_dma_address(host->data->sg);
		dmanow = dmastart + host->data->bytes_xfered;
		/*
		 * Force update to the next DMA block boundary.
		 */
		dmanow = (dmanow & ~(SDHCI_DEFAULT_BOUNDARY_SIZE - 1)) +
			SDHCI_DEFAULT_BOUNDARY_SIZE;
		host->data->bytes_xfered = dmanow - dmastart;
		sdhci_writel(host, dmanow, SDHCI_DMA_ADDRESS);

		return;
	}

	/*
	 * Check for A-004388: eSDHC DMA might not stop if error
	 * occurs on system transaction
	 * Impact list:
	 * T4240-4160-R1.0 B4860-4420-R1.0-R2.0 P1010-1014-R1.0
	 * P3041-R1.0-R2.0-R1.1 P2041-2040-R1.0-R1.1-R2.0
	 * P5020-5010-R2.0-R1.0 P5040-5021-R2.0-R2.1
	 */
	if (!(((SVR_SOC_VER(svr) == SVR_T4240) && (SVR_REV(svr) == 0x10)) ||
		((SVR_SOC_VER(svr) == SVR_T4160) && (SVR_REV(svr) == 0x10)) ||
		((SVR_SOC_VER(svr) == SVR_B4420) && (SVR_REV(svr) == 0x10)) ||
		((SVR_SOC_VER(svr) == SVR_B4420) && (SVR_REV(svr) == 0x20)) ||
		((SVR_SOC_VER(svr) == SVR_B4860) && (SVR_REV(svr) == 0x10)) ||
		((SVR_SOC_VER(svr) == SVR_B4860) && (SVR_REV(svr) == 0x20)) ||
		((SVR_SOC_VER(svr) == SVR_P1010) && (SVR_REV(svr) == 0x10)) ||
		((SVR_SOC_VER(svr) == SVR_P1014) && (SVR_REV(svr) == 0x10)) ||
		((SVR_SOC_VER(svr) == SVR_P3041) && (SVR_REV(svr) <= 0x20)) ||
		((SVR_SOC_VER(svr) == SVR_P2041) && (SVR_REV(svr) <= 0x20)) ||
		((SVR_SOC_VER(svr) == SVR_P2040) && (SVR_REV(svr) <= 0x20)) ||
		((SVR_SOC_VER(svr) == SVR_P5020) && (SVR_REV(svr) <= 0x20)) ||
		((SVR_SOC_VER(svr) == SVR_P5010) && (SVR_REV(svr) <= 0x20)) ||
		((SVR_SOC_VER(svr) == SVR_P5021) && (SVR_REV(svr) <= 0x21)) ||
		((SVR_SOC_VER(svr) == SVR_P5040) && (SVR_REV(svr) <= 0x21))))
		return;

	sdhci_reset(host, SDHCI_RESET_DATA);

	if (host->flags & SDHCI_USE_ADMA) {
		u32 mod, i, offset;
		u8 *desc;
		dma_addr_t addr;
		struct scatterlist *sg;
		__le32 *dataddr;
		__le32 *cmdlen;

		/*
		 * If block count was enabled, in case read transfer there
		 * is no data was corrupted
		 */
		mod = esdhc_readl(host, SDHCI_TRANSFER_MODE);
		if ((mod & SDHCI_TRNS_BLK_CNT_EN) &&
				(host->data->flags & MMC_DATA_READ))
			host->data->error = 0;

		BUG_ON(!host->data);
		desc = host->adma_table;
		for_each_sg(host->data->sg, sg, host->sg_count, i) {
			addr = sg_dma_address(sg);
			offset = (4 - (addr & 0x3)) & 0x3;
			if (offset)
				desc += 8;
			desc += 8;
		}

		/*
		 * Add an extra zero descriptor next to the
		 * terminating descriptor.
		 */
		desc += 8;
		WARN_ON((desc - host->adma_table) > (128 * 2 + 1) * 4);

		dataddr = (__le32 __force *)(desc + 4);
		cmdlen = (__le32 __force *)desc;

		cmdlen[0] = cpu_to_le32(0);
		dataddr[0] = cpu_to_le32(0);
	}

	if ((host->flags & SDHCI_USE_SDMA) &&
			(host->data->flags & MMC_DATA_READ))
		host->data->error = 0;

	return;
}

static int esdhc_of_enable_dma(struct sdhci_host *host)
{
	setbits32(host->ioaddr + ESDHC_DMA_SYSCTL, ESDHC_DMA_SNOOP);
	return 0;
}

static unsigned int esdhc_of_get_max_clock(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);

	return pltfm_host->clock;
}

static unsigned int esdhc_of_get_min_clock(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);

	return pltfm_host->clock / 256 / 16;
}

static void esdhc_of_set_clock(struct sdhci_host *host, unsigned int clock)
{
	int pre_div = 2;
	int div = 1;
	u32 temp;
	u32 actual_clk;
	u32 timeout;

	host->mmc->actual_clock = 0;

	if (clock == 0)
		return;

	/* Workaround to reduce the clock frequency for p1010 esdhc */
	if (of_find_compatible_node(NULL, NULL, "fsl,p1010-esdhc")) {
		if (clock > 20000000)
			clock -= 5000000;
		if (clock > 40000000)
			clock -= 5000000;
	}

	temp = sdhci_readl(host, ESDHC_SYSTEM_CONTROL);
	temp &= ~(ESDHC_CLOCK_IPGEN | ESDHC_CLOCK_HCKEN | ESDHC_CLOCK_PEREN
		| ESDHC_CLOCK_MASK | ESDHC_CLOCK_CRDEN);
	sdhci_writel(host, temp, ESDHC_SYSTEM_CONTROL);

	while (host->max_clk / pre_div / 16 > clock && pre_div < 256)
		pre_div *= 2;

	while (host->max_clk / pre_div / div > clock && div < 16)
		div++;

	dev_dbg(mmc_dev(host->mmc), "desired SD clock: %d, actual: %d\n",
		clock, host->max_clk / pre_div / div);

	actual_clk = host->max_clk / pre_div / div;
	pre_div >>= 1;
	div--;

	temp = sdhci_readl(host, ESDHC_SYSTEM_CONTROL);
	temp |= (ESDHC_CLOCK_IPGEN | ESDHC_CLOCK_HCKEN | ESDHC_CLOCK_PEREN
		| (div << ESDHC_DIVIDER_SHIFT)
		| (pre_div << ESDHC_PREDIV_SHIFT));
	sdhci_writel(host, temp, ESDHC_SYSTEM_CONTROL);

	/* Wait max 20 ms */
	timeout = 20;
	while (!(sdhci_readl(host, ESDHCI_PRESENT_STATE) & ESDHC_CLK_STABLE)) {
		if (timeout == 0) {
			pr_err("%s: Internal clock never "
				"stabilised.\n", mmc_hostname(host->mmc));
			return;
		}
		timeout--;
		mdelay(1);
	}

	temp |= ESDHC_CLOCK_CRDEN;
	sdhci_writel(host, temp, ESDHC_SYSTEM_CONTROL);

	if (host->quirks & SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK) {
		host->timeout_clk = actual_clk / 1000;
		host->mmc->max_busy_timeout = (1 << 27) / host->timeout_clk;
	}
}

static void esdhc_of_platform_reset_enter(struct sdhci_host *host, u8 mask)
{
	if ((host->quirks2 & SDHCI_QUIRK2_DISABLE_CLOCK_BEFORE_RESET) &&
	    (mask & SDHCI_RESET_ALL)) {
		u16 clk;

		clk = esdhc_readw(host, SDHCI_CLOCK_CONTROL);
		clk &= ~ESDHC_CLOCK_CRDEN;
		esdhc_writew(host, clk, SDHCI_CLOCK_CONTROL);
	}
}

static void esdhc_of_platform_reset_exit(struct sdhci_host *host, u8 mask)
{
	if ((host->quirks2 & SDHCI_QUIRK2_DISABLE_CLOCK_BEFORE_RESET) &&
	    (mask & SDHCI_RESET_ALL)) {
		u16 clk;

		clk = esdhc_readw(host, SDHCI_CLOCK_CONTROL);
		clk |= ESDHC_CLOCK_CRDEN;
		esdhc_writew(host, clk, SDHCI_CLOCK_CONTROL);
	}
}

static void esdhc_of_platform_init(struct sdhci_host *host)
{
	u32 vvn;

	svr =  mfspr(SPRN_SVR);
	vvn = in_be32(host->ioaddr + SDHCI_SLOT_INT_STATUS);
	vvn = (vvn & SDHCI_VENDOR_VER_MASK) >> SDHCI_VENDOR_VER_SHIFT;
	if (vvn == VENDOR_V_22)
		host->quirks2 |= SDHCI_QUIRK2_HOST_NO_CMD23;

	if (vvn > VENDOR_V_22)
		host->quirks &= ~SDHCI_QUIRK_NO_BUSY_IRQ;

	/*
	 * Check for A-005055: A glitch is generated on the card clock
	 * due to software reset or a clock change
	 * Impact list:
	 * T4240-4160-R1.0 B4860-4420-R1.0-R2.0 P3041-R1.0-R1.1-R2.0
	 * P2041-2040-R1.0-R1.1-R2.0 P1010-1014-R1.0 P5020-5010-R1.0-R2.0
	 * P5040-5021-R1.0-R2.0-R2.1
	 */
	if (((SVR_SOC_VER(svr) == SVR_T4240) && (SVR_REV(svr) == 0x10)) ||
	    ((SVR_SOC_VER(svr) == SVR_T4240) && (SVR_REV(svr) == 0x20)) ||
	    ((SVR_SOC_VER(svr) == SVR_T4160) && (SVR_REV(svr) == 0x10)) ||
	    ((SVR_SOC_VER(svr) == SVR_T4160) && (SVR_REV(svr) == 0x20)) ||
	    ((SVR_SOC_VER(svr) == SVR_B4860) && (SVR_REV(svr) == 0x10)) ||
	    ((SVR_SOC_VER(svr) == SVR_B4860) && (SVR_REV(svr) == 0x20)) ||
	    ((SVR_SOC_VER(svr) == SVR_B4420) && (SVR_REV(svr) == 0x10)) ||
	    ((SVR_SOC_VER(svr) == SVR_B4420) && (SVR_REV(svr) == 0x20)) ||
	    ((SVR_SOC_VER(svr) == SVR_P5040) && (SVR_REV(svr) <= 0x21)) ||
	    ((SVR_SOC_VER(svr) == SVR_P5020) && (SVR_REV(svr) <= 0x20)) ||
	    ((SVR_SOC_VER(svr) == SVR_P5021) && (SVR_REV(svr) <= 0x21)) ||
	    ((SVR_SOC_VER(svr) == SVR_P5010) && (SVR_REV(svr) <= 0x20)) ||
	    ((SVR_SOC_VER(svr) == SVR_P3041) && (SVR_REV(svr) <= 0x20)) ||
	    ((SVR_SOC_VER(svr) == SVR_P2041) && (SVR_REV(svr) <= 0x20)) ||
	    ((SVR_SOC_VER(svr) == SVR_P2040) && (SVR_REV(svr) <= 0x20)) ||
	    ((SVR_SOC_VER(svr) == SVR_P1014) && (SVR_REV(svr) == 0x10)) ||
	    ((SVR_SOC_VER(svr) == SVR_P1010) && (SVR_REV(svr) == 0x10)))
		host->quirks2 |= SDHCI_QUIRK2_DISABLE_CLOCK_BEFORE_RESET;
}

static void esdhc_get_pltfm_irq(struct sdhci_host *host, u32 *irq)
{
	*irq |= ESDHC_INT_DMA_ERROR;
}

static void esdhc_pltfm_irq_handler(struct sdhci_host *host, u32 intmask)
{
	if (intmask & (ESDHC_INT_DMA_ERROR | SDHCI_INT_ADMA_ERROR)) {
		host->data->error = -EIO;
		pr_err("%s: ADMA error\n", mmc_hostname(host->mmc));
		sdhci_adma_show_error(host);
		esdhci_of_adma_workaround(host, intmask);
	}
}

static void esdhc_pltfm_set_bus_width(struct sdhci_host *host, int width)
{
	u32 ctrl;

	switch (width) {
	case MMC_BUS_WIDTH_8:
		ctrl = ESDHC_CTRL_8BITBUS;
		break;

	case MMC_BUS_WIDTH_4:
		ctrl = ESDHC_CTRL_4BITBUS;
		break;

	default:
		ctrl = 0;
		break;
	}

	clrsetbits_be32(host->ioaddr + SDHCI_HOST_CONTROL,
			ESDHC_CTRL_BUSWIDTH_MASK, ctrl);
}

static void esdhc_reset(struct sdhci_host *host, u8 mask)
{
	esdhc_of_platform_reset_enter(host, mask);
	sdhci_reset(host, mask);
	esdhc_of_platform_reset_exit(host, mask);

	sdhci_writel(host, host->ier, SDHCI_INT_ENABLE);
	sdhci_writel(host, host->ier, SDHCI_SIGNAL_ENABLE);
}

/* Return: 1 - the card is present; 0 - card is absent */
static int esdhc_of_get_cd(struct sdhci_host *host)
{
	u32 present;
	u32 sysctl;

	if (host->flags & SDHCI_DEVICE_DEAD)
		return 0;
	if (host->quirks2 & SDHCI_QUIRK2_FORCE_CMD13_DETECT_CARD)
		return -ENOSYS;

	sysctl = sdhci_be32bs_readl(host, SDHCI_CLOCK_CONTROL);

	/* Enable the controller clock to update the present state */
	sdhci_be32bs_writel(host, sysctl | SDHCI_CLOCK_INT_EN,
			SDHCI_CLOCK_CONTROL);

	/* Detect the card present or absent */
	present = sdhci_be32bs_readl(host, SDHCI_PRESENT_STATE);
	present &= (SDHCI_CARD_PRESENT | SDHCI_CARD_CDPL);

	/* Resave the previous to System control register */
	sdhci_be32bs_writel(host, sysctl, SDHCI_CLOCK_CONTROL);

	return !!present;
}

static const struct sdhci_ops sdhci_esdhc_ops = {
	.read_l = esdhc_readl,
	.read_w = esdhc_readw,
	.read_b = esdhc_readb,
	.write_l = esdhc_writel,
	.write_w = esdhc_writew,
	.write_b = esdhc_writeb,
	.set_clock = esdhc_of_set_clock,
	.enable_dma = esdhc_of_enable_dma,
	.get_max_clock = esdhc_of_get_max_clock,
	.get_min_clock = esdhc_of_get_min_clock,
	.get_platform_irq = esdhc_get_pltfm_irq,
	.handle_platform_irq = esdhc_pltfm_irq_handler,
	.platform_init = esdhc_of_platform_init,
	.get_cd = esdhc_of_get_cd,
	.adma_workaround = esdhci_of_adma_workaround,
	.set_bus_width = esdhc_pltfm_set_bus_width,
	.reset = esdhc_reset,
	.set_uhs_signaling = sdhci_set_uhs_signaling,
};

#ifdef CONFIG_PM

static u32 esdhc_proctl;
static int esdhc_of_suspend(struct device *dev)
{
	struct sdhci_host *host = dev_get_drvdata(dev);

	esdhc_proctl = sdhci_be32bs_readl(host, SDHCI_HOST_CONTROL);

	return sdhci_suspend_host(host);
}

static int esdhc_of_resume(struct device *dev)
{
	struct sdhci_host *host = dev_get_drvdata(dev);
	int ret = sdhci_resume_host(host);

	if (ret == 0) {
		/* Isn't this already done by sdhci_resume_host() ? --rmk */
		esdhc_of_enable_dma(host);
		sdhci_be32bs_writel(host, esdhc_proctl, SDHCI_HOST_CONTROL);
	}

	return ret;
}

static const struct dev_pm_ops esdhc_pmops = {
	.suspend	= esdhc_of_suspend,
	.resume		= esdhc_of_resume,
};
#define ESDHC_PMOPS (&esdhc_pmops)
#else
#define ESDHC_PMOPS NULL
#endif

static const struct sdhci_pltfm_data sdhci_esdhc_pdata = {
	/*
	 * card detection could be handled via GPIO
	 * eSDHC cannot support End Attribute in NOP ADMA descriptor
	 */
	.quirks = ESDHC_DEFAULT_QUIRKS | SDHCI_QUIRK_BROKEN_CARD_DETECTION
		| SDHCI_QUIRK_NO_CARD_NO_RESET
		| SDHCI_QUIRK_NO_ENDATTR_IN_NOPDESC,
	.ops = &sdhci_esdhc_ops,
};

static int sdhci_esdhc_probe(struct platform_device *pdev)
{
	struct sdhci_host *host;
	struct device_node *np;
	int ret;

	host = sdhci_pltfm_init(pdev, &sdhci_esdhc_pdata, 0);
	if (IS_ERR(host))
		return PTR_ERR(host);

	sdhci_get_of_property(pdev);

	np = pdev->dev.of_node;
	if (of_device_is_compatible(np, "fsl,p2020-esdhc")) {
		/*
		 * Freescale messed up with P2020 as it has a non-standard
		 * host control register
		 */
		host->quirks2 |= SDHCI_QUIRK2_BROKEN_HOST_CONTROL;
	}

	/* call to generic mmc_of_parse to support additional capabilities */
	ret = mmc_of_parse(host->mmc);
	if (ret)
		goto err;

	mmc_of_parse_voltage(np, &host->ocr_mask);

	ret = sdhci_add_host(host);
	if (ret)
		goto err;

	return 0;
 err:
	sdhci_pltfm_free(pdev);
	return ret;
}

static const struct of_device_id sdhci_esdhc_of_match[] = {
	{ .compatible = "fsl,mpc8379-esdhc" },
	{ .compatible = "fsl,mpc8536-esdhc" },
	{ .compatible = "fsl,esdhc" },
	{ }
};
MODULE_DEVICE_TABLE(of, sdhci_esdhc_of_match);

static struct platform_driver sdhci_esdhc_driver = {
	.driver = {
		.name = "sdhci-esdhc",
		.of_match_table = sdhci_esdhc_of_match,
		.pm = ESDHC_PMOPS,
	},
	.probe = sdhci_esdhc_probe,
	.remove = sdhci_pltfm_unregister,
};

module_platform_driver(sdhci_esdhc_driver);

MODULE_DESCRIPTION("SDHCI OF driver for Freescale MPC eSDHC");
MODULE_AUTHOR("Xiaobo Xie <X.Xie@freescale.com>, "
	      "Anton Vorontsov <avorontsov@ru.mvista.com>");
MODULE_LICENSE("GPL v2");
