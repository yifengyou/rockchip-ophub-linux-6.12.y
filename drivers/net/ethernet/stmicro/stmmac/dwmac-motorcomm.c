// SPDX-License-Identifier: GPL-2.0-only
/*
 * DWMAC glue driver for Motorcomm PCI Ethernet controllers
 *
 * Ported to the 6.12 stmmac core API from the upstream driver
 * originally written by Yao Zi <me@ziyao.cc>.
 *
 * The 6.12 stmmac core has a different plat_stmmacenet_data layout
 * (no core_type / suspend / resume hooks, uses has_gmac4 instead) and
 * uses SIMPLE_DEV_PM_OPS + stmmac_suspend()/stmmac_resume() rather than
 * stmmac_simple_pm_ops / stmmac_plat_dat_alloc().  This driver adapts
 * the hardware bring-up (eFuse MAC read, MDIO PHY reset de-assert,
 * interrupt moderation, OOB-WoL disable) to that core.
 *
 * Copyright (c) 2025-2026 Yao Zi <me@ziyao.cc>
 * Copyright (c) 2026 AIoT-3588IED (6.12 port)
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/etherdevice.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/stmmac.h>

#include "stmmac.h"

#define DRIVER_NAME "dwmac-motorcomm"

#define PCI_VENDOR_ID_MOTORCOMM			0x1f0a

/* Register definition */
#define EPHY_CTRL				0x1004
/* Clearing this bit asserts resets for internal MDIO bus and PHY */
#define  EPHY_MDIO_PHY_RESET			BIT(0)
#define OOB_WOL_CTRL				0x1010
#define  OOB_WOL_CTRL_DIS			BIT(0)
#define MGMT_INT_CTRL0				0x1100
#define INT_MODERATION				0x1108
#define  INT_MODERATION_RX			GENMASK(11, 0)
#define  INT_MODERATION_TX			GENMASK(27, 16)
#define EFUSE_OP_CTRL_0				0x1500
#define  EFUSE_OP_MODE				GENMASK(1, 0)
#define   EFUSE_OP_ROW_READ			0x1
#define  EFUSE_OP_START				BIT(2)
#define  EFUSE_OP_ADDR				GENMASK(15, 8)
#define EFUSE_OP_CTRL_1				0x1504
#define  EFUSE_OP_DONE				BIT(1)
#define  EFUSE_OP_RD_DATA			GENMASK(31, 24)
#define SYS_RESET				0x152c
#define  SYS_RESET_RESET			BIT(31)
#define GMAC_OFFSET				0x2000

/* Constants */
#define EFUSE_READ_TIMEOUT_US			20000
#define EFUSE_PATCH_REGION_OFFSET		18
#define EFUSE_PATCH_MAX_NUM			39
#define EFUSE_ADDR_MACA0LR			0x1520
#define EFUSE_ADDR_MACA0HR			0x1524

/* Default programmable burst length (matches stmmac DEFAULT_DMA_PBL) */
#define MOTORCOMM_DEFAULT_PBL			256

struct motorcomm_efuse_patch {
	__le16 addr;
	__le32 data;
} __packed;

struct dwmac_motorcomm_priv {
	void __iomem *base;
};

static int motorcomm_efuse_read_byte(struct dwmac_motorcomm_priv *priv,
				     u8 offset, u8 *byte)
{
	u32 reg;
	int ret;

	writel(FIELD_PREP(EFUSE_OP_MODE, EFUSE_OP_ROW_READ)	|
	       FIELD_PREP(EFUSE_OP_ADDR, offset)		|
	       EFUSE_OP_START, priv->base + EFUSE_OP_CTRL_0);

	ret = readl_poll_timeout(priv->base + EFUSE_OP_CTRL_1,
				 reg, reg & EFUSE_OP_DONE, 2000,
				 EFUSE_READ_TIMEOUT_US);

	*byte = FIELD_GET(EFUSE_OP_RD_DATA, reg);

	return ret;
}

static int motorcomm_efuse_read_patch(struct dwmac_motorcomm_priv *priv,
				      u8 index,
				      struct motorcomm_efuse_patch *patch)
{
	u8 *p = (u8 *)patch, offset;
	int i, ret;

	for (i = 0; i < sizeof(*patch); i++) {
		offset = EFUSE_PATCH_REGION_OFFSET + sizeof(*patch) * index + i;

		ret = motorcomm_efuse_read_byte(priv, offset, &p[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int motorcomm_efuse_get_patch_value(struct dwmac_motorcomm_priv *priv,
					   u16 addr, u32 *value)
{
	struct motorcomm_efuse_patch patch;
	int i, ret;

	for (i = 0; i < EFUSE_PATCH_MAX_NUM; i++) {
		ret = motorcomm_efuse_read_patch(priv, i, &patch);
		if (ret)
			return ret;

		if (patch.addr == 0) {
			return -ENOENT;
		} else if (le16_to_cpu(patch.addr) == addr) {
			*value = le32_to_cpu(patch.data);
			return 0;
		}
	}

	return -ENOENT;
}

static int motorcomm_efuse_read_mac(struct device *dev,
				    struct dwmac_motorcomm_priv *priv, u8 *mac)
{
	u32 maca0lr, maca0hr;
	int ret;

	ret = motorcomm_efuse_get_patch_value(priv, EFUSE_ADDR_MACA0LR,
					      &maca0lr);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to read maca0lr from eFuse\n");

	ret = motorcomm_efuse_get_patch_value(priv, EFUSE_ADDR_MACA0HR,
					      &maca0hr);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to read maca0hr from eFuse\n");

	mac[0] = FIELD_GET(GENMASK(15, 8), maca0hr);
	mac[1] = FIELD_GET(GENMASK(7, 0), maca0hr);
	mac[2] = FIELD_GET(GENMASK(31, 24), maca0lr);
	mac[3] = FIELD_GET(GENMASK(23, 16), maca0lr);
	mac[4] = FIELD_GET(GENMASK(15, 8), maca0lr);
	mac[5] = FIELD_GET(GENMASK(7, 0), maca0lr);

	return 0;
}

static void motorcomm_deassert_mdio_phy_reset(struct dwmac_motorcomm_priv *priv)
{
	u32 reg = readl(priv->base + EPHY_CTRL);

	reg |= EPHY_MDIO_PHY_RESET;

	writel(reg, priv->base + EPHY_CTRL);
}

static void motorcomm_reset(struct dwmac_motorcomm_priv *priv)
{
	u32 reg = readl(priv->base + SYS_RESET);

	reg &= ~SYS_RESET_RESET;
	writel(reg, priv->base + SYS_RESET);

	reg |= SYS_RESET_RESET;
	writel(reg, priv->base + SYS_RESET);

	motorcomm_deassert_mdio_phy_reset(priv);
}

static void motorcomm_init(struct dwmac_motorcomm_priv *priv)
{
	writel(0x0, priv->base + MGMT_INT_CTRL0);

	writel(FIELD_PREP(INT_MODERATION_RX, 200) |
	       FIELD_PREP(INT_MODERATION_TX, 200),
	       priv->base + INT_MODERATION);

	/*
	 * OOB WOL must be disabled during normal operation, or DMA interrupts
	 * cannot be delivered to the host.
	 */
	writel(OOB_WOL_CTRL_DIS, priv->base + OOB_WOL_CTRL);
}

static struct plat_stmmacenet_data *
motorcomm_default_plat_data(struct pci_dev *pdev)
{
	struct plat_stmmacenet_data *plat;
	struct device *dev = &pdev->dev;

	plat = devm_kzalloc(dev, sizeof(*plat), GFP_KERNEL);
	if (!plat)
		return NULL;

	plat->mdio_bus_data = devm_kzalloc(dev, sizeof(*plat->mdio_bus_data),
					   GFP_KERNEL);
	if (!plat->mdio_bus_data)
		return NULL;

	plat->axi = devm_kzalloc(dev, sizeof(*plat->axi), GFP_KERNEL);
	if (!plat->axi)
		return NULL;

	plat->dma_cfg = devm_kzalloc(dev, sizeof(*plat->dma_cfg), GFP_KERNEL);
	if (!plat->dma_cfg)
		return NULL;

	plat->safety_feat_cfg = devm_kzalloc(dev,
					     sizeof(*plat->safety_feat_cfg),
					     GFP_KERNEL);
	if (!plat->safety_feat_cfg)
		return NULL;

	plat->dma_cfg->pbl		= MOTORCOMM_DEFAULT_PBL;
	plat->dma_cfg->pblx8		= true;
	plat->dma_cfg->txpbl		= 32;
	plat->dma_cfg->rxpbl		= 32;
	plat->dma_cfg->eame		= true;
	plat->dma_cfg->mixed_burst	= true;

	plat->axi->axi_wr_osr_lmt	= 1;
	plat->axi->axi_rd_osr_lmt	= 1;
	plat->axi->axi_blen[0]		= 4;
	plat->axi->axi_blen[1]		= 8;
	plat->axi->axi_blen[2]		= 16;
	plat->axi->axi_blen[3]		= 32;

	plat->bus_id		= pci_dev_id(pdev);
	plat->phy_interface	= PHY_INTERFACE_MODE_GMII;
	/*
	 * YT6801 requires a 25MHz clock input/oscillator to function, which
	 * is likely the source of CSR clock.
	 */
	plat->clk_csr		= STMMAC_CSR_20_35M;
	plat->has_gmac4		= 1;
	plat->tx_coe		= 1;
	plat->rx_coe		= 1;
	plat->clk_ref_rate	= 125000000;
	plat->flags		= STMMAC_FLAG_TSO_EN;

	return plat;
}

static void motorcomm_free_irq(void *data)
{
	struct pci_dev *pdev = data;

	pci_free_irq_vectors(pdev);
}

static int motorcomm_setup_irq(struct pci_dev *pdev,
			       struct stmmac_resources *res,
			       struct plat_stmmacenet_data *plat)
{
	int ret;

	ret = pci_alloc_irq_vectors(pdev, 6, 6, PCI_IRQ_MSIX);
	if (ret > 0) {
		res->rx_irq[0]	= pci_irq_vector(pdev, 0);
		res->tx_irq[0]	= pci_irq_vector(pdev, 4);
		res->irq	= pci_irq_vector(pdev, 5);

		plat->flags |= STMMAC_FLAG_MULTI_MSI_EN;
	} else {
		dev_info(&pdev->dev, "failed to allocate MSI-X vector: %d\n",
			 ret);
		dev_info(&pdev->dev, "try MSI instead\n");

		ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
		if (ret < 0)
			return dev_err_probe(&pdev->dev, ret,
					     "failed to allocate MSI\n");

		res->irq = pci_irq_vector(pdev, 0);
	}

	return devm_add_action_or_reset(&pdev->dev, motorcomm_free_irq, pdev);
}

static int motorcomm_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct plat_stmmacenet_data *plat;
	struct dwmac_motorcomm_priv *priv;
	struct stmmac_resources res = {};
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	plat = motorcomm_default_plat_data(pdev);
	if (!plat)
		return -ENOMEM;

	plat->bsp_priv = priv;

	ret = pcim_enable_device(pdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to enable device\n");

	priv->base = pcim_iomap_region(pdev, 0, DRIVER_NAME);
	if (IS_ERR(priv->base))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->base),
				     "failed to map IO region\n");

	pci_set_master(pdev);

	/*
	 * Some PCIe addon cards based on YT6801 don't deliver MSI(X) with ASPM
	 * enabled. Sadly there isn't a reliable way to read out the OEM of the
	 * card, so let's disable L1 state unconditionally for safety.
	 */
	ret = pci_disable_link_state(pdev, PCIE_LINK_STATE_L1);
	if (ret)
		dev_warn(&pdev->dev, "failed to disable L1 state: %d\n", ret);

	motorcomm_reset(priv);

	/*
	 * After system reset, the eFuse controller needs time to load its
	 * internal data. Without this delay, eFuse reads return all zeros,
	 * causing MAC address detection to fail.
	 */
	usleep_range(2000, 5000);

	ret = motorcomm_efuse_read_mac(&pdev->dev, priv, res.mac);
	if (ret == -ENOENT) {
		dev_warn(&pdev->dev, "eFuse contains no valid MAC address\n");
		dev_warn(&pdev->dev, "fallback to random MAC address\n");

		eth_random_addr(res.mac);
	} else if (ret) {
		return dev_err_probe(&pdev->dev, ret,
				     "failed to read MAC address from eFuse\n");
	}

	ret = motorcomm_setup_irq(pdev, &res, plat);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to setup IRQ\n");

	motorcomm_init(priv);

	res.addr = priv->base + GMAC_OFFSET;

	return stmmac_dvr_probe(&pdev->dev, plat, &res);
}

static void motorcomm_remove(struct pci_dev *pdev)
{
	stmmac_dvr_remove(&pdev->dev);
}

static int __maybe_unused motorcomm_pci_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	int ret;

	ret = stmmac_suspend(dev);
	if (ret)
		return ret;

	ret = pci_save_state(pdev);
	if (ret)
		return ret;

	pci_wake_from_d3(pdev, true);
	pci_set_power_state(pdev, PCI_D3hot);
	return 0;
}

static int __maybe_unused motorcomm_pci_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct net_device *ndev = dev_get_drvdata(dev);
	struct stmmac_priv *priv = ndev ? netdev_priv(ndev) : NULL;
	int ret;

	pci_restore_state(pdev);
	pci_set_power_state(pdev, PCI_D0);

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	pci_set_master(pdev);

	/*
	 * When recovering from D3hot, EPHY_MDIO_PHY_RESET is automatically
	 * asserted, and must be deasserted for normal operation.  Re-run
	 * the controller bring-up before handing control back to stmmac.
	 */
	if (priv && priv->plat && priv->plat->bsp_priv) {
		struct dwmac_motorcomm_priv *mc_priv = priv->plat->bsp_priv;

		motorcomm_deassert_mdio_phy_reset(mc_priv);
		motorcomm_init(mc_priv);
	}

	return stmmac_resume(dev);
}

static SIMPLE_DEV_PM_OPS(motorcomm_pm_ops, motorcomm_pci_suspend,
			 motorcomm_pci_resume);

static const struct pci_device_id dwmac_motorcomm_pci_id_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_MOTORCOMM, 0x6801) },
	{ },
};
MODULE_DEVICE_TABLE(pci, dwmac_motorcomm_pci_id_table);

static struct pci_driver dwmac_motorcomm_pci_driver = {
	.name = DRIVER_NAME,
	.id_table = dwmac_motorcomm_pci_id_table,
	.probe = motorcomm_probe,
	.remove = motorcomm_remove,
	.driver = {
		.pm = &motorcomm_pm_ops,
	},
};

module_pci_driver(dwmac_motorcomm_pci_driver);

MODULE_DESCRIPTION("DWMAC glue driver for Motorcomm PCI Ethernet controllers");
MODULE_AUTHOR("Yao Zi <me@ziyao.cc>");
MODULE_LICENSE("GPL");
