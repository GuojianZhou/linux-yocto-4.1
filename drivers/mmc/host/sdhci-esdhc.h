/*
 * Freescale eSDHC controller driver generics for OF and pltfm.
 *
 * Copyright (c) 2007 Freescale Semiconductor, Inc.
 * Copyright (c) 2009 MontaVista Software, Inc.
 * Copyright (c) 2010 Pengutronix e.K.
 *   Author: Wolfram Sang <w.sang@pengutronix.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License.
 */

#ifndef _DRIVERS_MMC_SDHCI_ESDHC_H
#define _DRIVERS_MMC_SDHCI_ESDHC_H

/*
 * Ops and quirks for the Freescale eSDHC controller.
 */

#define ESDHC_DEFAULT_QUIRKS	(SDHCI_QUIRK_FORCE_BLK_SZ_2048 | \
				SDHCI_QUIRK_NO_BUSY_IRQ | \
				SDHCI_QUIRK_NONSTANDARD_CLOCK | \
				SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK | \
				SDHCI_QUIRK_PIO_NEEDS_DELAY | \
				SDHCI_QUIRK_RESTORE_IRQS_AFTER_RESET | \
				SDHCI_QUIRK_NO_HISPD_BIT)

#define ESDHC_SYSTEM_CONTROL	0x2c
#define ESDHC_CLOCK_MASK	0x0000fff0
#define ESDHC_PREDIV_SHIFT	8
#define ESDHC_DIVIDER_SHIFT	4
#define ESDHC_CLOCK_CRDEN	0x00000008
#define ESDHC_CLOCK_PEREN	0x00000004
#define ESDHC_CLOCK_HCKEN	0x00000002
#define ESDHC_CLOCK_IPGEN	0x00000001


#define ESDHCI_PRESENT_STATE	0x24
#define ESDHC_CLK_STABLE	0x00000008

#define ESDHC_CAPABILITIES_1	0x114
#define ESDHC_MODE_MASK		0x00000007
#define ESDHC_MODE_DDR50_SEL	0xfffffffc
#define ESDHC_MODE_DDR50	0x00000004

#define ESDHC_CLOCK_CONTROL	0x144
#define ESDHC_CLKLPBK_EXTPIN	0x80000000
#define ESDHC_CMDCLK_SHIFTED	0x00008000

/* SDHC Adapter Card Type */
#define ESDHC_ADAPTER_TYPE_1      0x1	/* eMMC Card Rev4.5 */
#define ESDHC_ADAPTER_TYPE_2      0x2	/* SD/MMC Legacy Card */
#define ESDHC_ADAPTER_TYPE_3      0x3	/* eMMC Card Rev4.4 */
#define ESDHC_ADAPTER_TYPE_4      0x4	/* Reserved */
#define ESDHC_ADAPTER_TYPE_5      0x5	/* MMC Card */
#define ESDHC_ADAPTER_TYPE_6      0x6	/* SD Card Rev2.0 Rev3.0 */
#define ESDHC_NO_ADAPTER          0x7	/* No Card is Present*/

/* pltfm-specific */
#define ESDHC_HOST_CONTROL_LE	0x20

/*
 * P2020 interpretation of the SDHCI_HOST_CONTROL register
 */
#define ESDHC_CTRL_4BITBUS          (0x1 << 1)
#define ESDHC_CTRL_8BITBUS          (0x2 << 1)
#define ESDHC_CTRL_BUSWIDTH_MASK    (0x3 << 1)

/* OF-specific */
#define ESDHC_DMA_SYSCTL	0x40c
#define ESDHC_DMA_SNOOP		0x00000040
#define ESDHC_FLUSH_ASYNC_FIFO	0x00040000
#define ESDHC_USE_PERICLK	0x00080000
#define ESDHC_INT_DMA_ERROR	0x10000000

#define ESDHC_HOST_CONTROL_RES	0x01
#define ESDHC_VOL_SEL		0x04

#endif /* _DRIVERS_MMC_SDHCI_ESDHC_H */
