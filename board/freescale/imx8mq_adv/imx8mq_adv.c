// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2018 NXP
 */

#include <common.h>
#include <efi_loader.h>
#include <env.h>
#include <init.h>
#include <malloc.h>
#include <errno.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <miiphy.h>
#include <netdev.h>
#include <asm/mach-imx/iomux-v3.h>
#include <asm-generic/gpio.h>
#include <fsl_esdhc_imx.h>
#include <mmc.h>
#include <asm/arch/imx8mq_pins.h>
#include <asm/arch/sys_proto.h>
#include <asm/mach-imx/gpio.h>
#include <asm/mach-imx/mxc_i2c.h>
#include <asm/arch/clock.h>
#include <spl.h>
#include <linux/bitops.h>
#include <power/pmic.h>
#include <power/pfuze100_pmic.h>
#include "../common/tcpc.h"
#include "../common/pfuze.h"
#include <usb.h>
#include <dwc3-uboot.h>

#include <asm/setup.h>
#include <u-boot/sha256.h>
static void setup_mac_addr(void)
{
	int ret;
	char *uid;
	u8 mac_addr[6];
	u8 hash[32];
	int size = sizeof(hash);
	struct tag_serialnr serialnr;
	struct ocotp_regs *ocotp = (struct ocotp_regs *)OCOTP_BASE_ADDR;
	struct fuse_bank *bank = &ocotp->bank[0];
	struct fuse_bank0_regs *fuse = (struct fuse_bank0_regs *)bank->fuse_regs;
	serialnr.low = fuse->uid_low;
	serialnr.high = fuse->uid_high;

	if (env_get("ethaddr"))
		return;

	uid = (char *)&serialnr;
	ret = hash_block("sha256", (void *)uid, strlen((const char*)uid), hash, &size);
	if (ret) {
		printf("%s: failed to calculate SHA256\n", __func__);
		return;
	}

	memcpy(mac_addr, hash, 6);

	mac_addr[0] &= 0xfe;
	mac_addr[0] |= 0x02;
	eth_env_set_enetaddr("ethaddr", mac_addr);
}

DECLARE_GLOBAL_DATA_PTR;

#define UART_PAD_CTRL	(PAD_CTL_DSE6 | PAD_CTL_FSEL1)

#define WDOG_PAD_CTRL	(PAD_CTL_DSE6 | PAD_CTL_HYS | PAD_CTL_PUE)

static iomux_v3_cfg_t const wdog_pads[] = {
	IMX8MQ_PAD_GPIO1_IO02__WDOG1_WDOG_B | MUX_PAD_CTRL(WDOG_PAD_CTRL),
};

static iomux_v3_cfg_t const uart_pads[] = {
#if defined(CONFIG_TARGET_IMX8MQ_ECU150FL)
	IMX8MQ_PAD_UART4_RXD__UART4_RX | MUX_PAD_CTRL(UART_PAD_CTRL),
	IMX8MQ_PAD_UART4_TXD__UART4_TX | MUX_PAD_CTRL(UART_PAD_CTRL),
#else
	IMX8MQ_PAD_UART1_RXD__UART1_RX | MUX_PAD_CTRL(UART_PAD_CTRL),
	IMX8MQ_PAD_UART1_TXD__UART1_TX | MUX_PAD_CTRL(UART_PAD_CTRL),
#endif
};

#if CONFIG_IS_ENABLED(EFI_HAVE_CAPSULE_SUPPORT)
struct efi_fw_image fw_images[] = {
	{
		.image_type_id = IMX_BOOT_IMAGE_GUID,
		.fw_name = u"IMX8MQ-EVK-RAW",
		.image_index = 1,
	},
};

struct efi_capsule_update_info update_info = {
	.dfu_string = "mmc 0=flash-bin raw 0x42 0x2000 mmcpart 1",
	.images = fw_images,
};

u8 num_image_type_guids = ARRAY_SIZE(fw_images);
#endif /* EFI_HAVE_CAPSULE_SUPPORT */

int board_early_init_f(void)
{
	struct wdog_regs *wdog = (struct wdog_regs *)WDOG1_BASE_ADDR;

	imx_iomux_v3_setup_multiple_pads(wdog_pads, ARRAY_SIZE(wdog_pads));
	set_wdog_reset(wdog);

	imx_iomux_v3_setup_multiple_pads(uart_pads, ARRAY_SIZE(uart_pads));

	return 0;
}

#ifdef CONFIG_FSL_QSPI
int board_qspi_init(void)
{
	set_clk_qspi();

	return 0;
}
#endif

/* layout of baseboard id */
#define IMX8MQ_GPIO3_IO25 IMX_GPIO_NR(3, 25)  //board_id[2]:1
#define IMX8MQ_GPIO4_IO31 IMX_GPIO_NR(4, 31)  //board_id[2]:0

/* GPIO port description */
static unsigned long imx8m_gpio_ports[] = {
	[0] = GPIO1_BASE_ADDR,
	[1] = GPIO2_BASE_ADDR,
	[2] = GPIO3_BASE_ADDR,
	[3] = GPIO4_BASE_ADDR,
	[4] = GPIO5_BASE_ADDR,
};

/* use legacy gpio operations before device model is ready. */
static int gpio_direction_input_legacy(unsigned int gpio)
{
	unsigned int port;
	struct gpio_regs *regs;
	u32 l;

	port = gpio/32;

	gpio &= 0x1f;

	regs = (struct gpio_regs *)imx8m_gpio_ports[port];

	l = readl(&regs->gpio_dir);
	/* set direction as input. */
	l &= ~(1 << gpio);
	writel(l, &regs->gpio_dir);

	return 0;
}

static int gpio_get_value_legacy(unsigned gpio)
{
	unsigned int port;
	struct gpio_regs *regs;
	u32 val;

	port = gpio/32;

	gpio &= 0x1f;

	regs = (struct gpio_regs *)imx8m_gpio_ports[port];

	val = (readl(&regs->gpio_dr) >> gpio) & 0x01;

	return val;
}

int get_imx8m_baseboard_id(void)
{
	int  i = 0, value = 0;
	int baseboard_id;
	int pin[2];

	/* initialize the pin array */
	pin[0] = IMX8MQ_GPIO4_IO31;
	pin[1] = IMX8MQ_GPIO3_IO25;

	/* Set gpio direction as input and get the input value */
	baseboard_id = 0;
	for (i = 0; i < 2; i++) {
		gpio_direction_input_legacy(pin[i]);
		if ((value = gpio_get_value_legacy(pin[i])) < 0) {
			printf("Error! Read gpio port: %d failed!\n", pin[i]);
			return -1;
		} else
			baseboard_id |= ((value & 0x01) << i);
	}

	return baseboard_id;
}

#ifdef CONFIG_IMX_TRUSTY_OS
int get_tee_load(ulong *load)
{
	int board_id;

	board_id = get_imx8m_baseboard_id();
	//board_id = ADV_IMX8_DDR_2G; //+=
	/* load TEE to the last 32M of DDR */
	if (board_id == ADV_IMX8_DDR_1G) {
		/* for 1G DDR board */
		*load = (ulong)TEE_LOAD_ADDR_1G;
	} else if ((board_id == ADV_IMX8_DDR_2G) {
		/* for 2G DDR board */
		*load = (ulong)TEE_LOAD_ADDR_2G;
	} else if ((board_id == ADV_IMX8_DDR_4G) {
		/* for 4G DDR board */
		*load = (ulong)TEE_LOAD_ADDR_4G;
	} else {
		/* for 2G DDR board  */
		*load = (ulong)TEE_LOAD_ADDR_2G;
	}

	return 0;
}
#endif

int board_phys_sdram_size(phys_size_t *ddr_size)
{
	if (!ddr_size)
		return -EINVAL;

	int baseboard_id;

	baseboard_id = get_imx8m_baseboard_id();
	//baseboard_id = ADV_IMX8_DDR_2G; //+=
	if (baseboard_id == ADV_IMX8_DDR_1G) {
		/* 1G DDR size */
		*ddr_size = 0x40000000;
	} else if (baseboard_id == ADV_IMX8_DDR_2G) {
		/* 2G DDR size */
		*ddr_size = 0x80000000;
	} else if (baseboard_id == ADV_IMX8_DDR_4G) {
		/* 4G DDR size */
		*ddr_size = 0x100000000;
	} else {
		/* 2G DDR size */
		*ddr_size = 0x80000000;
	}

	return 0;
}

#ifdef CONFIG_FEC_MXC
static int setup_fec(void)
{
	struct iomuxc_gpr_base_regs *gpr =
		(struct iomuxc_gpr_base_regs *)IOMUXC_GPR_BASE_ADDR;

	/* Use 125M anatop REF_CLK1 for ENET1, not from external */
	clrsetbits_le32(&gpr->gpr[1],
		IOMUXC_GPR_GPR1_GPR_ENET1_TX_CLK_SEL_MASK, 0);
	return set_clk_enet(ENET_125MHZ);
}

int board_phy_config(struct phy_device *phydev)
{
	if (phydev->drv->config)
		phydev->drv->config(phydev);

#ifndef CONFIG_DM_ETH
	/* enable rgmii rxc skew and phy mode select to RGMII copper */
	phy_write(phydev, MDIO_DEVAD_NONE, 0x1d, 0x1f);
	phy_write(phydev, MDIO_DEVAD_NONE, 0x1e, 0x8);

	phy_write(phydev, MDIO_DEVAD_NONE, 0x1d, 0x05);
	phy_write(phydev, MDIO_DEVAD_NONE, 0x1e, 0x100);
#endif

	return 0;
}
#endif

#ifdef CONFIG_USB_DWC3

#define USB_PHY_CTRL0			0xF0040
#define USB_PHY_CTRL0_REF_SSP_EN	BIT(2)

#define USB_PHY_CTRL1			0xF0044
#define USB_PHY_CTRL1_RESET		BIT(0)
#define USB_PHY_CTRL1_COMMONONN		BIT(1)
#define USB_PHY_CTRL1_ATERESET		BIT(3)
#define USB_PHY_CTRL1_VDATSRCENB0	BIT(19)
#define USB_PHY_CTRL1_VDATDETENB0	BIT(20)

#define USB_PHY_CTRL2			0xF0048
#define USB_PHY_CTRL2_TXENABLEN0	BIT(8)

static struct dwc3_device dwc3_device_data = {
#ifdef CONFIG_SPL_BUILD
	.maximum_speed = USB_SPEED_HIGH,
#else
	.maximum_speed = USB_SPEED_SUPER,
#endif
	.base = USB1_BASE_ADDR,
	.dr_mode = USB_DR_MODE_PERIPHERAL,
	.index = 0,
	.power_down_scale = 2,
};

int usb_gadget_handle_interrupts(int index)
{
	dwc3_uboot_handle_interrupt(index);
	return 0;
}

static void dwc3_nxp_usb_phy_init(struct dwc3_device *dwc3)
{
	u32 RegData;

	RegData = readl(dwc3->base + USB_PHY_CTRL1);
	RegData &= ~(USB_PHY_CTRL1_VDATSRCENB0 | USB_PHY_CTRL1_VDATDETENB0 |
			USB_PHY_CTRL1_COMMONONN);
	RegData |= USB_PHY_CTRL1_RESET | USB_PHY_CTRL1_ATERESET;
	writel(RegData, dwc3->base + USB_PHY_CTRL1);

	RegData = readl(dwc3->base + USB_PHY_CTRL0);
	RegData |= USB_PHY_CTRL0_REF_SSP_EN;
	writel(RegData, dwc3->base + USB_PHY_CTRL0);

	RegData = readl(dwc3->base + USB_PHY_CTRL2);
	RegData |= USB_PHY_CTRL2_TXENABLEN0;
	writel(RegData, dwc3->base + USB_PHY_CTRL2);

	RegData = readl(dwc3->base + USB_PHY_CTRL1);
	RegData &= ~(USB_PHY_CTRL1_RESET | USB_PHY_CTRL1_ATERESET);
	writel(RegData, dwc3->base + USB_PHY_CTRL1);
}
#endif

#ifdef CONFIG_USB_TCPC
struct tcpc_port port;
struct tcpc_port_config port_config = {
	.i2c_bus = 0,
	.addr = 0x50,
	.port_type = TYPEC_PORT_UFP,
	.max_snk_mv = 20000,
	.max_snk_ma = 3000,
	.max_snk_mw = 15000,
	.op_snk_mv = 9000,
};

struct gpio_desc type_sel_desc;
static iomux_v3_cfg_t ss_mux_gpio[] = {
	IMX8MQ_PAD_NAND_RE_B__GPIO3_IO15 | MUX_PAD_CTRL(NO_PAD_CTRL),
};

void ss_mux_select(enum typec_cc_polarity pol)
{
	if (pol == TYPEC_POLARITY_CC1)
		dm_gpio_set_value(&type_sel_desc, 1);
	else
		dm_gpio_set_value(&type_sel_desc, 0);
}

static int setup_typec(void)
{
	int ret;

	imx_iomux_v3_setup_multiple_pads(ss_mux_gpio, ARRAY_SIZE(ss_mux_gpio));

	ret = dm_gpio_lookup_name("GPIO3_15", &type_sel_desc);
	if (ret) {
		printf("%s lookup GPIO3_15 failed ret = %d\n", __func__, ret);
		return -ENODEV;
	}

	ret = dm_gpio_request(&type_sel_desc, "typec_sel");
	if (ret) {
		printf("%s request typec_sel failed ret = %d\n", __func__, ret);
		return -ENODEV;
	}

	dm_gpio_set_dir_flags(&type_sel_desc, GPIOD_IS_OUT);

	ret = tcpc_init(&port, port_config, &ss_mux_select);
	if (ret) {
		printf("%s: tcpc init failed, err=%d\n",
		       __func__, ret);
	}

	return ret;
}
#endif

#if defined(CONFIG_USB_DWC3) || defined(CONFIG_USB_XHCI_IMX8M)
int board_usb_init(int index, enum usb_init_type init)
{
	int ret = 0;

	if (index == 0 && init == USB_INIT_DEVICE) {
		imx8m_usb_power(index, true);
#ifdef CONFIG_USB_TCPC
		ret = tcpc_setup_ufp_mode(&port);
#endif
		dwc3_nxp_usb_phy_init(&dwc3_device_data);
		return dwc3_uboot_init(&dwc3_device_data);
	} else if (index == 0 && init == USB_INIT_HOST) {
#ifdef CONFIG_USB_TCPC
		ret = tcpc_setup_dfp_mode(&port);
#endif
		return ret;
	}

	return 0;
}

int board_usb_cleanup(int index, enum usb_init_type init)
{
	int ret = 0;
	if (index == 0 && init == USB_INIT_DEVICE) {
		dwc3_uboot_exit(index);
		imx8m_usb_power(index, false);
	} else if (index == 0 && init == USB_INIT_HOST) {
#ifdef CONFIG_USB_TCPC
		ret = tcpc_disable_src_vbus(&port);
#endif
	}

	return ret;
}
#endif

int board_init(void)
{
#ifdef CONFIG_FSL_QSPI
	board_qspi_init();
#endif

#ifdef CONFIG_FEC_MXC
	setup_fec();
#endif

#if defined(CONFIG_USB_DWC3) || defined(CONFIG_USB_XHCI_IMX8M)
	init_usb_clk();
#endif

#ifdef CONFIG_USB_TCPC
	setup_typec();
#endif
	return 0;
}

int board_late_init(void)
{
#ifdef CONFIG_ENV_VARS_UBOOT_RUNTIME_CONFIG
	env_set("board_name", "EVK");
	env_set("board_rev", "iMX8MQ");
#endif

#ifdef CONFIG_ENV_IS_IN_MMC
	board_late_mmc_env_init();
#endif

	int baseboard_id;
	baseboard_id = get_imx8m_baseboard_id();
	//baseboard_id = ADV_IMX8_DDR_2G; //+=
	if (baseboard_id == ADV_IMX8_DDR_1G) {
		/* 1G DDR size */
		env_set("bootargs_ram_capacity", "cma=296M galcore.contiguousSize=33554432");
	} else if (baseboard_id == ADV_IMX8_DDR_2G) {
		/* 2G DDR size */
		env_set("bootargs_ram_capacity", "cma=296M galcore.contiguousSize=33554432");	
	} else if (baseboard_id == ADV_IMX8_DDR_4G) {
		/* 4G DDR size */
		env_set("bootargs_ram_capacity", "cma=384M");
	} else {
		/* 2G DDR size */
		env_set("bootargs_ram_capacity", "cma=296M galcore.contiguousSize=33554432");	
	}

#if defined(CONFIG_TARGET_IMX8MQ_ECU150)
	setup_mac_addr();
	puts("DeviceName is ECU150.\n");
	env_set("devicename", "ecu150");
#elif defined(CONFIG_TARGET_IMX8MQ_ECU150FL)
	puts("DeviceName is ECU150FL.\n");
	env_set("devicename", "ecu150fl");
#elif defined(CONFIG_TARGET_IMX8MQ_ECU150A1)
	puts("DeviceName is ECU150A1.\n");
	env_set("devicename", "ecu150a1");
#endif

	return 0;
}

#ifdef CONFIG_ANDROID_SUPPORT
bool is_power_key_pressed(void) {
	return (bool)(!!(readl(SNVS_HPSR) & (0x1 << 6)));
}
#endif

#ifdef CONFIG_FSL_FASTBOOT
#ifdef CONFIG_ANDROID_RECOVERY
int is_recovery_key_pressing(void)
{
	return 0; /* TODO */
}
#endif /* CONFIG_ANDROID_RECOVERY */
#endif /* CONFIG_FSL_FASTBOOT */