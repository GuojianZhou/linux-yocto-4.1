#ifndef __PPC_FSL_SOC_H
#define __PPC_FSL_SOC_H
#ifdef __KERNEL__

#include <asm/mmu.h>

struct spi_device;

extern phys_addr_t get_dcsrbase(void);
extern phys_addr_t get_immrbase(void);
extern void __iomem *get_cpu_l2_base(int cpu);
#if defined(CONFIG_CPM2) || defined(CONFIG_QUICC_ENGINE) || defined(CONFIG_8xx)
extern u32 get_brgfreq(void);
extern u32 get_baudrate(void);
#else
static inline u32 get_brgfreq(void) { return -1; }
static inline u32 get_baudrate(void) { return -1; }
#endif
extern u32 fsl_get_sys_freq(void);

struct spi_board_info;
struct device_node;

extern void fsl_rstcr_restart(char *cmd);

#ifdef CONFIG_FSL_PMC
int mpc85xx_pmc_set_wake(struct device *dev, bool enable);
void mpc85xx_pmc_set_lossless_ethernet(int enable);
#else
static inline int mpc85xx_pmc_set_wake(struct device *dev, bool enable)
{
	return -ENODEV;
}
#define mpc85xx_pmc_set_lossless_ethernet(enable)	do { } while (0)
#endif

/* The different ports that the DIU can be connected to */
enum fsl_diu_monitor_port {
	FSL_DIU_PORT_DVI,	/* DVI */
	FSL_DIU_PORT_LVDS,	/* Single-link LVDS */
	FSL_DIU_PORT_DLVDS	/* Dual-link LVDS */
};

struct platform_diu_data_ops {
	u32 (*get_pixel_format)(enum fsl_diu_monitor_port port,
		unsigned int bpp);
	void (*set_gamma_table)(enum fsl_diu_monitor_port port,
		char *gamma_table_base);
	void (*set_monitor_port)(enum fsl_diu_monitor_port port);
	void (*set_pixel_clock)(unsigned int pixclock);
	enum fsl_diu_monitor_port (*valid_monitor_port)
		(enum fsl_diu_monitor_port port);
	void (*release_bootmem)(void);
};

extern struct platform_diu_data_ops diu_ops;

void fsl_hv_restart(char *cmd);
void fsl_hv_halt(void);

/*
 * Cast the ccsrbar to 64-bit parameter so that the assembly
 * code can be compatible with both 32-bit & 36-bit.
 */
extern void mpc85xx_enter_deep_sleep(u64 ccsrbar, u32 powmgtreq);
#endif
#endif
