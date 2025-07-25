/**
 * core.c - DesignWare USB3 DRD Controller Core file
 *
 * Copyright (C) 2010-2011 Texas Instruments Incorporated - http://www.ti.com
 *
 * Authors: Felipe Balbi <balbi@ti.com>,
 *	    Sebastian Andrzej Siewior <bigeasy@linutronix.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2  of
 * the License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/of.h>
#include <linux/acpi.h>
#include <linux/pinctrl/consumer.h>

#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/usb/of.h>
#include <linux/usb/otg.h>
#include <linux/irq.h>

#include "core.h"
#include "gadget.h"
#include "io.h"

#include "debug.h"

#define DWC3_DEFAULT_AUTOSUSPEND_DELAY	500 /* ms */

static int count;
static struct dwc3 *dwc3_instance[DWC_CTRL_COUNT];

static void dwc3_check_params(struct dwc3 *dwc);

void dwc3_usb3_phy_suspend(struct dwc3 *dwc, int suspend)
{
	u32			reg;

	if (dwc->dis_u3_susphy_quirk)
		return;

	reg = dwc3_readl(dwc->regs, DWC3_GUSB3PIPECTL(0));

	if (suspend)
		reg |= DWC3_GUSB3PIPECTL_SUSPHY;
	else
		reg &= ~DWC3_GUSB3PIPECTL_SUSPHY;

	dwc3_writel(dwc->regs, DWC3_GUSB3PIPECTL(0), reg);

	if (dwc->dual_port) {
		reg = dwc3_readl(dwc->regs, DWC3_GUSB3PIPECTL(1));

		if (suspend)
			reg |= DWC3_GUSB3PIPECTL_SUSPHY;
		else
			reg &= ~DWC3_GUSB3PIPECTL_SUSPHY;

		dwc3_writel(dwc->regs, DWC3_GUSB3PIPECTL(1), reg);
	}
}

/**
 * dwc3_get_dr_mode - Validates and sets dr_mode
 * @dwc: pointer to our context structure
 */
static int dwc3_get_dr_mode(struct dwc3 *dwc)
{
	enum usb_dr_mode mode;
	struct device *dev = dwc->dev;
	unsigned int hw_mode;

	if (dwc->dr_mode == USB_DR_MODE_UNKNOWN)
		dwc->dr_mode = USB_DR_MODE_OTG;

	mode = dwc->dr_mode;
	hw_mode = DWC3_GHWPARAMS0_MODE(dwc->hwparams.hwparams0);

	switch (hw_mode) {
	case DWC3_GHWPARAMS0_MODE_GADGET:
		if (IS_ENABLED(CONFIG_USB_DWC3_HOST)) {
			dev_err(dev,
				"Controller does not support host mode.\n");
			return -EINVAL;
		}
		mode = USB_DR_MODE_PERIPHERAL;
		break;
	case DWC3_GHWPARAMS0_MODE_HOST:
		if (IS_ENABLED(CONFIG_USB_DWC3_GADGET)) {
			dev_err(dev,
				"Controller does not support device mode.\n");
			return -EINVAL;
		}
		mode = USB_DR_MODE_HOST;
		break;
	default:
		if (IS_ENABLED(CONFIG_USB_DWC3_HOST))
			mode = USB_DR_MODE_HOST;
		else if (IS_ENABLED(CONFIG_USB_DWC3_GADGET))
			mode = USB_DR_MODE_PERIPHERAL;
	}

	if (mode != dwc->dr_mode) {
		dev_warn(dev,
			 "Configuration mismatch. dr_mode forced to %s\n",
			 mode == USB_DR_MODE_HOST ? "host" : "gadget");

		dwc->dr_mode = mode;
	}

	return 0;
}

static void dwc3_event_buffers_cleanup(struct dwc3 *dwc);

void dwc3_set_prtcap(struct dwc3 *dwc, u32 mode)
{
	u32 reg;

	reg = dwc3_readl(dwc->regs, DWC3_GCTL);
	reg &= ~(DWC3_GCTL_PRTCAPDIR(DWC3_GCTL_PRTCAP_OTG));
	reg |= DWC3_GCTL_PRTCAPDIR(mode);
	dwc3_writel(dwc->regs, DWC3_GCTL, reg);
	dwc->current_dr_role = mode;
}

void dwc3_en_sleep_mode(struct dwc3 *dwc)
{
	u32 reg;

	if (dwc->dis_enblslpm_quirk)
		return;

	reg = dwc3_readl(dwc->regs, DWC3_GUSB2PHYCFG(0));
	reg |= DWC3_GUSB2PHYCFG_ENBLSLPM;
	dwc3_writel(dwc->regs, DWC3_GUSB2PHYCFG(0), reg);

	if (dwc->dual_port) {
		reg = dwc3_readl(dwc->regs, DWC3_GUSB2PHYCFG(1));
		reg |= DWC3_GUSB2PHYCFG_ENBLSLPM;
		dwc3_writel(dwc->regs, DWC3_GUSB2PHYCFG(1), reg);
	}
}

void dwc3_dis_sleep_mode(struct dwc3 *dwc)
{
	u32 reg;

	reg = dwc3_readl(dwc->regs, DWC3_GUSB2PHYCFG(0));
	reg &= ~DWC3_GUSB2PHYCFG_ENBLSLPM;
	dwc3_writel(dwc->regs, DWC3_GUSB2PHYCFG(0), reg);
}

void dwc3_set_mode(struct dwc3 *dwc, u32 mode)
{
	unsigned long flags;

	spin_lock_irqsave(&dwc->lock, flags);
	dwc->desired_dr_role = mode;
	spin_unlock_irqrestore(&dwc->lock, flags);

	queue_work(system_freezable_wq, &dwc->drd_work);
}

u32 dwc3_core_fifo_space(struct dwc3_ep *dep, u8 type)
{
	struct dwc3		*dwc = dep->dwc;
	u32			reg;

	dwc3_writel(dwc->regs, DWC3_GDBGFIFOSPACE,
			DWC3_GDBGFIFOSPACE_NUM(dep->number) |
			DWC3_GDBGFIFOSPACE_TYPE(type));

	reg = dwc3_readl(dwc->regs, DWC3_GDBGFIFOSPACE);

	return DWC3_GDBGFIFOSPACE_SPACE_AVAILABLE(reg);
}

/**
 * dwc3_core_soft_reset - Issues core soft reset and PHY reset
 * @dwc: pointer to our context structure
 */
static int dwc3_core_soft_reset(struct dwc3 *dwc)
{
	u32		reg;
	int		retries = 1000;
	int		ret;

	pr_err("%s: dwc->maximum_speed %d\n",
				__func__, dwc->maximum_speed);

	/* Reset and initialize PHYs */
	usb_phy_reset(dwc->usb2_phy);
	ret = usb_phy_init(dwc->usb2_phy);
	if (ret) {
		pr_err("%s: usb_phy_init(dwc->usb2_phy) returned %d\n",
				__func__, ret);
		return ret;
	}

	usb_phy_reset(dwc->usb2_phy1);
	ret = usb_phy_init(dwc->usb2_phy1);
	if (ret) {
		pr_err("%s: usb_phy_init(dwc->usb2_phy1) returned %d\n",
				__func__, ret);
		return ret;
	}

	if (dwc->maximum_speed <= USB_SPEED_HIGH)
		goto generic_phy_init;

	usb_phy_reset(dwc->usb3_phy);
	ret = usb_phy_init(dwc->usb3_phy);
	if (ret == -EBUSY) {
		/*
		 * Setting Max speed as high when USB3 PHY initialiation
		 * is failing and USB superspeed can't be supported.
		 */
		dwc->maximum_speed = USB_SPEED_HIGH;
	} else if (ret) {
		pr_err("%s: usb_phy_init(dwc->usb3_phy) returned %d\n",
				__func__, ret);
		return ret;
	}

	usb_phy_reset(dwc->usb3_phy1);
	ret = usb_phy_init(dwc->usb3_phy1);
	if (ret) {
		pr_err("%s: usb_phy_init(dwc->usb3_phy1) returned %d\n",
				__func__, ret);
		return ret;
	}

generic_phy_init:
	ret = phy_init(dwc->usb2_generic_phy);
	if (ret < 0)
		return ret;

	ret = phy_init(dwc->usb3_generic_phy);
	if (ret < 0) {
		phy_exit(dwc->usb2_generic_phy);
		return ret;
	}

	/*
	 * We're resetting only the device side because, if we're in host mode,
	 * XHCI driver will reset the host block. If dwc3 was configured for
	 * host-only mode, then we can return early.
	 */
	if (dwc->dr_mode == USB_DR_MODE_HOST)
		return 0;

	reg = dwc3_readl(dwc->regs, DWC3_DCTL);
	reg |= DWC3_DCTL_CSFTRST;
	dwc3_writel(dwc->regs, DWC3_DCTL, reg);

	do {
		reg = dwc3_readl(dwc->regs, DWC3_DCTL);
		if (!(reg & DWC3_DCTL_CSFTRST))
			goto done;

		udelay(1);
	} while (--retries);

	phy_exit(dwc->usb3_generic_phy);
	phy_exit(dwc->usb2_generic_phy);

	return -ETIMEDOUT;

done:
	/*
	 * For DWC_usb31 controller, once DWC3_DCTL_CSFTRST bit is cleared,
	 * we must wait at least 50ms before accessing the PHY domain
	 * (synchronization delay). DWC_usb31 programming guide section 1.3.2.
	 */
	if (dwc3_is_usb31(dwc))
		msleep(50);

	return 0;
}

/*
 * dwc3_frame_length_adjustment - Adjusts frame length if required
 * @dwc3: Pointer to our controller context structure
 */
static void dwc3_frame_length_adjustment(struct dwc3 *dwc)
{
	u32 reg;
	u32 dft;

	if (dwc->revision < DWC3_REVISION_250A)
		return;

	if (dwc->fladj == 0)
		return;

	reg = dwc3_readl(dwc->regs, DWC3_GFLADJ);
	dft = reg & DWC3_GFLADJ_30MHZ_MASK;
	if (dft != dwc->fladj) {
		reg &= ~DWC3_GFLADJ_30MHZ_MASK;
		reg |= DWC3_GFLADJ_30MHZ_SDBND_SEL | dwc->fladj;
		dwc3_writel(dwc->regs, DWC3_GFLADJ, reg);
	}
}

/**
 * dwc3_free_one_event_buffer - Frees one event buffer
 * @dwc: Pointer to our controller context structure
 * @evt: Pointer to event buffer to be freed
 */
static void dwc3_free_one_event_buffer(struct dwc3 *dwc,
		struct dwc3_event_buffer *evt)
{
	dma_free_coherent(dwc->sysdev, evt->length, evt->buf, evt->dma);
}

/**
 * dwc3_alloc_one_event_buffer - Allocates one event buffer structure
 * @dwc: Pointer to our controller context structure
 * @length: size of the event buffer
 *
 * Returns a pointer to the allocated event buffer structure on success
 * otherwise ERR_PTR(errno).
 */
static struct dwc3_event_buffer *dwc3_alloc_one_event_buffer(struct dwc3 *dwc,
		unsigned length)
{
	struct dwc3_event_buffer	*evt;

	evt = devm_kzalloc(dwc->dev, sizeof(*evt), GFP_KERNEL);
	if (!evt)
		return ERR_PTR(-ENOMEM);

	evt->dwc	= dwc;
	evt->length	= length;
	evt->cache	= devm_kzalloc(dwc->dev, length, GFP_KERNEL);
	if (!evt->cache)
		return ERR_PTR(-ENOMEM);

	evt->buf	= dma_alloc_coherent(dwc->sysdev, length,
			&evt->dma, GFP_KERNEL);
	if (!evt->buf)
		return ERR_PTR(-ENOMEM);

	return evt;
}

/**
 * dwc3_free_event_buffers - frees all allocated event buffers
 * @dwc: Pointer to our controller context structure
 */
static void dwc3_free_event_buffers(struct dwc3 *dwc)
{
	struct dwc3_event_buffer	*evt;

	evt = dwc->ev_buf;
	if (evt) {
		dwc3_free_one_event_buffer(dwc, evt);
		dwc->ev_buf = NULL;
	}

	/* free GSI related event buffers */
	dwc3_notify_event(dwc, DWC3_GSI_EVT_BUF_FREE, 0);
}

/**
 * dwc3_alloc_event_buffers - Allocates @num event buffers of size @length
 * @dwc: pointer to our controller context structure
 * @length: size of event buffer
 *
 * Returns 0 on success otherwise negative errno. In the error case, dwc
 * may contain some buffers allocated but not all which were requested.
 */
static int dwc3_alloc_event_buffers(struct dwc3 *dwc, unsigned length)
{
	struct dwc3_event_buffer *evt;
	unsigned int hw_mode;

	hw_mode = DWC3_GHWPARAMS0_MODE(dwc->hwparams.hwparams0);
	if (hw_mode == DWC3_GHWPARAMS0_MODE_HOST) {
		dwc->ev_buf = NULL;
		return 0;
	}

	evt = dwc3_alloc_one_event_buffer(dwc, length);
	if (IS_ERR(evt)) {
		dev_err(dwc->dev, "can't allocate event buffer\n");
		return PTR_ERR(evt);
	}
	dwc->ev_buf = evt;

	/* alloc GSI related event buffers */
	dwc3_notify_event(dwc, DWC3_GSI_EVT_BUF_ALLOC, 0);
	return 0;
}

/**
 * dwc3_event_buffers_setup - setup our allocated event buffers
 * @dwc: pointer to our controller context structure
 *
 * Returns 0 on success otherwise negative errno.
 */
int dwc3_event_buffers_setup(struct dwc3 *dwc)
{
	struct dwc3_event_buffer	*evt;

	if (!dwc->ev_buf)
		return 0;

	evt = dwc->ev_buf;
	evt->lpos = 0;
	dwc3_writel(dwc->regs, DWC3_GEVNTADRLO(0),
			lower_32_bits(evt->dma));
	dwc3_writel(dwc->regs, DWC3_GEVNTADRHI(0),
			upper_32_bits(evt->dma));
	dwc3_writel(dwc->regs, DWC3_GEVNTSIZ(0),
			DWC3_GEVNTSIZ_SIZE(evt->length));
	dwc3_writel(dwc->regs, DWC3_GEVNTCOUNT(0), 0);

	/* setup GSI related event buffers */
	dwc3_notify_event(dwc, DWC3_GSI_EVT_BUF_SETUP, 0);
	return 0;
}

static void dwc3_event_buffers_cleanup(struct dwc3 *dwc)
{
	struct dwc3_event_buffer	*evt;
	u32				reg;

	if (!dwc->ev_buf)
		return;
	/*
	 * Exynos platforms may not be able to access event buffer if the
	 * controller failed to halt on dwc3_core_exit().
	 */
	reg = dwc3_readl(dwc->regs, DWC3_DSTS);
	if (!(reg & DWC3_DSTS_DEVCTRLHLT))
		return;

	evt = dwc->ev_buf;

	evt->lpos = 0;

	dwc3_writel(dwc->regs, DWC3_GEVNTADRLO(0), 0);
	dwc3_writel(dwc->regs, DWC3_GEVNTADRHI(0), 0);
	dwc3_writel(dwc->regs, DWC3_GEVNTSIZ(0), DWC3_GEVNTSIZ_INTMASK
			| DWC3_GEVNTSIZ_SIZE(0));
	dwc3_writel(dwc->regs, DWC3_GEVNTCOUNT(0), 0);

	/* cleanup GSI related event buffers */
	dwc3_notify_event(dwc, DWC3_GSI_EVT_BUF_CLEANUP, 0);
}

static int dwc3_alloc_scratch_buffers(struct dwc3 *dwc)
{
	if (!dwc->has_hibernation)
		return 0;

	if (!dwc->nr_scratch)
		return 0;

	dwc->scratchbuf = kmalloc_array(dwc->nr_scratch,
			DWC3_SCRATCHBUF_SIZE, GFP_KERNEL);
	if (!dwc->scratchbuf)
		return -ENOMEM;

	return 0;
}

static int dwc3_setup_scratch_buffers(struct dwc3 *dwc)
{
	dma_addr_t scratch_addr;
	u32 param;
	int ret;

	if (!dwc->has_hibernation)
		return 0;

	if (!dwc->nr_scratch)
		return 0;

	 /* should never fall here */
	if (!WARN_ON(dwc->scratchbuf))
		return 0;

	scratch_addr = dma_map_single(dwc->sysdev, dwc->scratchbuf,
			dwc->nr_scratch * DWC3_SCRATCHBUF_SIZE,
			DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dwc->sysdev, scratch_addr)) {
		dev_err(dwc->sysdev, "failed to map scratch buffer\n");
		ret = -EFAULT;
		goto err0;
	}

	dwc->scratch_addr = scratch_addr;

	param = lower_32_bits(scratch_addr);

	ret = dwc3_send_gadget_generic_command(dwc,
			DWC3_DGCMD_SET_SCRATCHPAD_ADDR_LO, param);
	if (ret < 0)
		goto err1;

	param = upper_32_bits(scratch_addr);

	ret = dwc3_send_gadget_generic_command(dwc,
			DWC3_DGCMD_SET_SCRATCHPAD_ADDR_HI, param);
	if (ret < 0)
		goto err1;

	return 0;

err1:
	dma_unmap_single(dwc->sysdev, dwc->scratch_addr, dwc->nr_scratch *
			DWC3_SCRATCHBUF_SIZE, DMA_BIDIRECTIONAL);

err0:
	return ret;
}

static void dwc3_free_scratch_buffers(struct dwc3 *dwc)
{
	if (!dwc->has_hibernation)
		return;

	if (!dwc->nr_scratch)
		return;

	 /* should never fall here */
	if (!WARN_ON(dwc->scratchbuf))
		return;

	dma_unmap_single(dwc->sysdev, dwc->scratch_addr, dwc->nr_scratch *
			DWC3_SCRATCHBUF_SIZE, DMA_BIDIRECTIONAL);
	kfree(dwc->scratchbuf);
}

static void dwc3_core_num_eps(struct dwc3 *dwc)
{
	struct dwc3_hwparams	*parms = &dwc->hwparams;

	dwc->num_eps = DWC3_NUM_EPS(parms);
}

static void dwc3_cache_hwparams(struct dwc3 *dwc)
{
	struct dwc3_hwparams	*parms = &dwc->hwparams;

	parms->hwparams0 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS0);
	parms->hwparams1 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS1);
	parms->hwparams2 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS2);
	parms->hwparams3 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS3);
	parms->hwparams4 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS4);
	parms->hwparams5 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS5);
	parms->hwparams6 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS6);
	parms->hwparams7 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS7);
	parms->hwparams8 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS8);
}

static int dwc3_core_ulpi_init(struct dwc3 *dwc)
{
	int intf;
	int ret = 0;

	intf = DWC3_GHWPARAMS3_HSPHY_IFC(dwc->hwparams.hwparams3);

	if (intf == DWC3_GHWPARAMS3_HSPHY_IFC_ULPI ||
	    (intf == DWC3_GHWPARAMS3_HSPHY_IFC_UTMI_ULPI &&
	     dwc->hsphy_interface &&
	     !strncmp(dwc->hsphy_interface, "ulpi", 4)))
		ret = dwc3_ulpi_init(dwc);

	return ret;
}

/**
 * dwc3_phy_setup - Configure USB PHY Interface of DWC3 Core
 * @dwc: Pointer to our controller context structure
 *
 * Returns 0 on success. The USB PHY interfaces are configured but not
 * initialized. The PHY interfaces and the PHYs get initialized together with
 * the core in dwc3_core_init.
 */
static int dwc3_phy_setup(struct dwc3 *dwc)
{
	u32 reg;

	reg = dwc3_readl(dwc->regs, DWC3_GUSB3PIPECTL(0));
	if (dwc->dual_port) {
		if (reg != dwc3_readl(dwc->regs, DWC3_GUSB3PIPECTL(1)))
			dev_warn(dwc->dev, "Reset values of pipectl registers are different!\n");
	}

	/*
	 * Make sure UX_EXIT_PX is cleared as that causes issues with some
	 * PHYs. Also, this bit is not supposed to be used in normal operation.
	 */
	reg &= ~DWC3_GUSB3PIPECTL_UX_EXIT_PX;

	/*
	 * Above 1.94a, it is recommended to set DWC3_GUSB3PIPECTL_SUSPHY
	 * to '0' during coreConsultant configuration. So default value
	 * will be '0' when the core is reset. Application needs to set it
	 * to '1' after the core initialization is completed.
	 */
	if (dwc->revision > DWC3_REVISION_194A)
		reg |= DWC3_GUSB3PIPECTL_SUSPHY;

	if (dwc->u2ss_inp3_quirk)
		reg |= DWC3_GUSB3PIPECTL_U2SSINP3OK;

	if (dwc->dis_rxdet_inp3_quirk)
		reg |= DWC3_GUSB3PIPECTL_DISRXDETINP3;

	if (dwc->req_p1p2p3_quirk)
		reg |= DWC3_GUSB3PIPECTL_REQP1P2P3;

	if (dwc->del_p1p2p3_quirk)
		reg |= DWC3_GUSB3PIPECTL_DEP1P2P3_EN;

	if (dwc->del_phy_power_chg_quirk)
		reg |= DWC3_GUSB3PIPECTL_DEPOCHANGE;

	if (dwc->lfps_filter_quirk)
		reg |= DWC3_GUSB3PIPECTL_LFPSFILT;

	if (dwc->rx_detect_poll_quirk)
		reg |= DWC3_GUSB3PIPECTL_RX_DETOPOLL;

	if (dwc->tx_de_emphasis_quirk)
		reg |= DWC3_GUSB3PIPECTL_TX_DEEPH(dwc->tx_de_emphasis);

	if (dwc->dis_u3_susphy_quirk)
		reg &= ~DWC3_GUSB3PIPECTL_SUSPHY;

	if (dwc->dis_del_phy_power_chg_quirk)
		reg &= ~DWC3_GUSB3PIPECTL_DEPOCHANGE;

	if (dwc->ssp_u3_u0_quirk)
		reg |= (DWC3_GUSB3PIPECTL_UX_EXIT_PX |
				DWC3_GUSB3PIPECTL_P3EXSIGP2);

	dwc3_writel(dwc->regs, DWC3_GUSB3PIPECTL(0), reg);
	if (dwc->dual_port)
		dwc3_writel(dwc->regs, DWC3_GUSB3PIPECTL(1), reg);

	reg = dwc3_readl(dwc->regs, DWC3_GUSB2PHYCFG(0));
	if (dwc->dual_port) {
		if (reg != dwc3_readl(dwc->regs, DWC3_GUSB2PHYCFG(1)))
			dev_warn(dwc->dev, "Reset values of usb2phycfg registers are different!\n");
	}

	/* Select the HS PHY interface */
	switch (DWC3_GHWPARAMS3_HSPHY_IFC(dwc->hwparams.hwparams3)) {
	case DWC3_GHWPARAMS3_HSPHY_IFC_UTMI_ULPI:
		if (dwc->hsphy_interface &&
				!strncmp(dwc->hsphy_interface, "utmi", 4)) {
			reg &= ~DWC3_GUSB2PHYCFG_ULPI_UTMI;
			break;
		} else if (dwc->hsphy_interface &&
				!strncmp(dwc->hsphy_interface, "ulpi", 4)) {
			reg |= DWC3_GUSB2PHYCFG_ULPI_UTMI;
			dwc3_writel(dwc->regs, DWC3_GUSB2PHYCFG(0), reg);
		} else {
			/* Relying on default value. */
			if (!(reg & DWC3_GUSB2PHYCFG_ULPI_UTMI))
				break;
		}
		/* FALLTHROUGH */
	case DWC3_GHWPARAMS3_HSPHY_IFC_ULPI:
		/* FALLTHROUGH */
	default:
		break;
	}

	switch (dwc->hsphy_mode) {
	case USBPHY_INTERFACE_MODE_UTMI:
		reg &= ~(DWC3_GUSB2PHYCFG_PHYIF_MASK |
		       DWC3_GUSB2PHYCFG_USBTRDTIM_MASK);
		reg |= DWC3_GUSB2PHYCFG_PHYIF(UTMI_PHYIF_8_BIT) |
		       DWC3_GUSB2PHYCFG_USBTRDTIM(USBTRDTIM_UTMI_8_BIT);
		break;
	case USBPHY_INTERFACE_MODE_UTMIW:
		reg &= ~(DWC3_GUSB2PHYCFG_PHYIF_MASK |
		       DWC3_GUSB2PHYCFG_USBTRDTIM_MASK);
		reg |= DWC3_GUSB2PHYCFG_PHYIF(UTMI_PHYIF_16_BIT) |
		       DWC3_GUSB2PHYCFG_USBTRDTIM(USBTRDTIM_UTMI_16_BIT);
		break;
	default:
		break;
	}

	/*
	 * Above 1.94a, it is recommended to set DWC3_GUSB2PHYCFG_SUSPHY to
	 * '0' during coreConsultant configuration. So default value will
	 * be '0' when the core is reset. Application needs to set it to
	 * '1' after the core initialization is completed.
	 */
	if (dwc->revision > DWC3_REVISION_194A)
		reg |= DWC3_GUSB2PHYCFG_SUSPHY;

	if (dwc->dis_u2_susphy_quirk)
		reg &= ~DWC3_GUSB2PHYCFG_SUSPHY;

	if (dwc->dis_enblslpm_quirk)
		reg &= ~DWC3_GUSB2PHYCFG_ENBLSLPM;

	if (dwc->dis_u2_freeclk_exists_quirk)
		reg &= ~DWC3_GUSB2PHYCFG_U2_FREECLK_EXISTS;

	dwc3_writel(dwc->regs, DWC3_GUSB2PHYCFG(0), reg);
	if (dwc->dual_port)
		dwc3_writel(dwc->regs, DWC3_GUSB2PHYCFG(1), reg);

	return 0;
}

static void dwc3_core_exit(struct dwc3 *dwc)
{
	dwc3_event_buffers_cleanup(dwc);

	usb_phy_set_suspend(dwc->usb2_phy1, 1);
	usb_phy_set_suspend(dwc->usb2_phy, 1);
	usb_phy_set_suspend(dwc->usb3_phy1, 1);
	usb_phy_set_suspend(dwc->usb3_phy, 1);
	phy_power_off(dwc->usb2_generic_phy);
	phy_power_off(dwc->usb3_generic_phy);

	usb_phy_shutdown(dwc->usb2_phy1);
	usb_phy_shutdown(dwc->usb2_phy);
	usb_phy_shutdown(dwc->usb3_phy1);
	usb_phy_shutdown(dwc->usb3_phy);
	phy_exit(dwc->usb2_generic_phy);
	phy_exit(dwc->usb3_generic_phy);
}

static bool dwc3_core_is_valid(struct dwc3 *dwc)
{
	u32 reg;

	reg = dwc3_readl(dwc->regs, DWC3_GSNPSID);

	/* This should read as U3 followed by revision number */
	if ((reg & DWC3_GSNPSID_MASK) == 0x55330000) {
		/* Detected DWC_usb3 IP */
		dwc->revision = reg;
	} else if ((reg & DWC3_GSNPSID_MASK) == 0x33310000) {
		/* Detected DWC_usb31 IP */
		dwc->revision = dwc3_readl(dwc->regs, DWC3_VER_NUMBER);
		dwc->revision |= DWC3_REVISION_IS_DWC31;
		dwc->versiontype = dwc3_readl(dwc->regs, DWC3_VER_TYPE);
	} else {
		return false;
	}

	return true;
}

static void dwc3_core_setup_global_control(struct dwc3 *dwc)
{
	u32 hwparams4 = dwc->hwparams.hwparams4;
	u32 reg;

	reg = dwc3_readl(dwc->regs, DWC3_GCTL);
	reg &= ~DWC3_GCTL_SCALEDOWN_MASK;

	switch (DWC3_GHWPARAMS1_EN_PWROPT(dwc->hwparams.hwparams1)) {
	case DWC3_GHWPARAMS1_EN_PWROPT_CLK:
		/**
		 * WORKAROUND: DWC3 revisions between 2.10a and 2.50a have an
		 * issue which would cause xHCI compliance tests to fail.
		 *
		 * Because of that we cannot enable clock gating on such
		 * configurations.
		 *
		 * Refers to:
		 *
		 * STAR#9000588375: Clock Gating, SOF Issues when ref_clk-Based
		 * SOF/ITP Mode Used
		 */
		if ((dwc->dr_mode == USB_DR_MODE_HOST ||
				dwc->dr_mode == USB_DR_MODE_OTG) &&
				(dwc->revision >= DWC3_REVISION_210A &&
				dwc->revision <= DWC3_REVISION_250A))
			reg |= DWC3_GCTL_DSBLCLKGTNG | DWC3_GCTL_SOFITPSYNC;
		else
			reg &= ~DWC3_GCTL_DSBLCLKGTNG;
		break;
	case DWC3_GHWPARAMS1_EN_PWROPT_HIB:
		/* enable hibernation here */
		dwc->nr_scratch = DWC3_GHWPARAMS4_HIBER_SCRATCHBUFS(hwparams4);

		/*
		 * REVISIT Enabling this bit so that host-mode hibernation
		 * will work. Device-mode hibernation is not yet implemented.
		 */
		reg |= DWC3_GCTL_GBLHIBERNATIONEN;
		break;
	default:
		/* nothing */
		break;
	}

	/* check if current dwc3 is on simulation board */
	if (dwc->hwparams.hwparams6 & DWC3_GHWPARAMS6_EN_FPGA) {
		dev_info(dwc->dev, "Running with FPGA optmizations\n");
		dwc->is_fpga = true;
	}

	WARN_ONCE(dwc->disable_scramble_quirk && !dwc->is_fpga,
			"disable_scramble cannot be used on non-FPGA builds\n");

	if (dwc->disable_scramble_quirk && dwc->is_fpga)
		reg |= DWC3_GCTL_DISSCRAMBLE;
	else
		reg &= ~DWC3_GCTL_DISSCRAMBLE;

	if (dwc->u2exit_lfps_quirk)
		reg |= DWC3_GCTL_U2EXIT_LFPS;

	/*
	 * WORKAROUND: DWC3 revisions <1.90a have a bug
	 * where the device can fail to connect at SuperSpeed
	 * and falls back to high-speed mode which causes
	 * the device to enter a Connect/Disconnect loop
	 */
	if (dwc->revision < DWC3_REVISION_190A)
		reg |= DWC3_GCTL_U2RSTECN;

	if (dwc->disable_clk_gating)
		reg |= DWC3_GCTL_DSBLCLKGTNG;

	dwc3_writel(dwc->regs, DWC3_GCTL, reg);
}

static int dwc3_core_get_phy(struct dwc3 *dwc);
static int dwc3_core_ulpi_init(struct dwc3 *dwc);

/**
 * dwc3_core_init - Low-level initialization of DWC3 Core
 * @dwc: Pointer to our controller context structure
 *
 * Returns 0 on success otherwise negative errno.
 */
int dwc3_core_init(struct dwc3 *dwc)
{
	u32			reg;
	int			ret;

	if (!dwc3_core_is_valid(dwc)) {
		dev_err(dwc->dev, "this is not a DesignWare USB3 DRD Core\n");
		ret = -ENODEV;
		goto err0;
	}

	dwc3_cache_hwparams(dwc);
	dwc3_check_params(dwc);
	ret = dwc3_get_dr_mode(dwc);
	if (ret) {
		ret = -EINVAL;
		goto err0;
	}

	/*
	 * Write Linux Version Code to our GUID register so it's easy to figure
	 * out which kernel version a bug was found.
	 */
	dwc3_writel(dwc->regs, DWC3_GUID, LINUX_VERSION_CODE);

	/* Handle USB2.0-only core configuration */
	if (DWC3_GHWPARAMS3_SSPHY_IFC(dwc->hwparams.hwparams3) ==
			DWC3_GHWPARAMS3_SSPHY_IFC_DIS) {
		if (dwc->maximum_speed >= USB_SPEED_SUPER)
			dwc->maximum_speed = USB_SPEED_HIGH;
	}

	ret = dwc3_phy_setup(dwc);
	if (ret)
		goto err0;

	if (!dwc->ulpi_ready) {
		ret = dwc3_core_ulpi_init(dwc);
		if (ret) {
			if (ret == -ETIMEDOUT) {
				dwc3_core_soft_reset(dwc);
				ret = -EPROBE_DEFER;
			}
			goto err0;
		}
		dwc->ulpi_ready = true;
	}

	if (!dwc->phys_ready) {
		ret = dwc3_core_get_phy(dwc);
		if (ret)
			goto err0a;
		dwc->phys_ready = true;
	}

	ret = dwc3_core_soft_reset(dwc);
	if (ret)
		goto err0a;

	dwc3_core_setup_global_control(dwc);
	dwc3_core_num_eps(dwc);

	ret = dwc3_setup_scratch_buffers(dwc);
	if (ret)
		goto err1;

	/* Adjust Frame Length */
	dwc3_frame_length_adjustment(dwc);

	usb_phy_set_suspend(dwc->usb2_phy, 0);
	usb_phy_set_suspend(dwc->usb2_phy1, 0);
	if (dwc->maximum_speed >= USB_SPEED_SUPER) {
		usb_phy_set_suspend(dwc->usb3_phy, 0);
		usb_phy_set_suspend(dwc->usb3_phy1, 0);
	}

	ret = phy_power_on(dwc->usb2_generic_phy);
	if (ret < 0)
		goto err2;

	ret = phy_power_on(dwc->usb3_generic_phy);
	if (ret < 0)
		goto err3;

	/*
	 * ENDXFER polling is available on version 3.10a and later of
	 * the DWC_usb3 controller. It is NOT available in the
	 * DWC_usb31 controller.
	 */
	if (!dwc3_is_usb31(dwc) && dwc->revision >= DWC3_REVISION_310A) {
		reg = dwc3_readl(dwc->regs, DWC3_GUCTL2);
		reg |= DWC3_GUCTL2_RST_ACTBITLATER;
		dwc3_writel(dwc->regs, DWC3_GUCTL2, reg);
	}

	if (dwc->revision >= DWC3_REVISION_250A) {
		reg = dwc3_readl(dwc->regs, DWC3_GUCTL1);

		/*
		 * Enable hardware control of sending remote wakeup
		 * in HS when the device is in the L1 state.
		 */
		if (dwc->revision >= DWC3_REVISION_290A)
			reg |= DWC3_GUCTL1_DEV_L1_EXIT_BY_HW;

		if (dwc->dis_tx_ipgap_linecheck_quirk)
			reg |= DWC3_GUCTL1_TX_IPGAP_LINECHECK_DIS;

		if (dwc->parkmode_disable_ss_quirk)
			reg |= DWC3_GUCTL1_PARKMODE_DISABLE_SS;

		/*
		 * STAR: 9001415732: Host failure when Park mode is enabled:
		 * Disable parkmode for Gen1 controllers to fix the stall
		 * seen during host mode transfers on multiple endpoints.
		 */
		if (!dwc3_is_usb31(dwc)) {
			reg |= DWC3_GUCTL1_PARKMODE_DISABLE_SS;
			reg |= DWC3_GUCTL1_PARKMODE_DISABLE_HS;
			reg |= DWC3_GUCTL1_PARKMODE_DISABLE_FSLS;
		}

		dwc3_writel(dwc->regs, DWC3_GUCTL1, reg);
	}

	/*
	 * STAR: 9001346572:Host stops transfers to other EPs when a single
	 * USB2.0 EP NAKs continuously requires to disable internal retry
	 * feature
	 */
	if ((dwc->revision == DWC3_USB31_REVISION_170A) &&
		(dwc->versiontype == DWC3_USB31_VER_TYPE_GA)) {
		reg = dwc3_readl(dwc->regs, DWC3_GUCTL3);
		reg |= DWC3_GUCTL3_USB20_RETRY_DISABLE;
		dwc3_writel(dwc->regs, DWC3_GUCTL3, reg);
	}

	dwc3_notify_event(dwc, DWC3_CONTROLLER_POST_RESET_EVENT, 0);

	/*
	 * Workaround for STAR 9001198391 which affects dwc3 core
	 * version 3.20a only. Default HP timer value is incorrectly
	 * set to 3us. Reprogram HP timer value to support USB 3.1
	 * HP timer ECN.
	 */
	if (!dwc3_is_usb31(dwc) &&  dwc->revision == DWC3_REVISION_320A) {
		reg = dwc3_readl(dwc->regs, DWC3_GUCTL2);
		reg &= ~DWC3_GUCTL2_HP_TIMER_MASK;
		reg |= DWC3_GUCTL2_HP_TIMER(11);
		dwc3_writel(dwc->regs, DWC3_GUCTL2, reg);
	}

	/*
	 * Workaround for STAR 9001285599 which affects dwc3 core version 3.20a
	 * only. If the PM TIMER ECN is enabled thru GUCTL2[19], then link
	 * compliance test (TD7.21) may fail. If the ECN is not enabled
	 * GUCTL2[19] = 0), the controller will use the old timer value (5us),
	 * which is still fine for Link Compliance test. Hence Do not enable
	 * PM TIMER ECN in V3.20a by setting GUCTL2[19] by default,
	 * instead use GUCTL2[19] = 0.
	 */
	if (dwc->revision == DWC3_REVISION_320A) {
		reg = dwc3_readl(dwc->regs, DWC3_GUCTL2);
		reg &= ~DWC3_GUCTL2_LC_TIMER;
		dwc3_writel(dwc->regs, DWC3_GUCTL2, reg);
	}

	/* set inter-packet gap 199.794ns to improve EL_23 margin */
	if (dwc->revision >= DWC3_USB31_REVISION_170A) {
		reg = dwc3_readl(dwc->regs, DWC3_GUCTL1);
		reg |= DWC3_GUCTL1_IP_GAP_ADD_ON(1);
		dwc3_writel(dwc->regs, DWC3_GUCTL1, reg);
	}

	return 0;

err3:
	phy_power_off(dwc->usb2_generic_phy);

err2:
	usb_phy_set_suspend(dwc->usb2_phy1, 1);
	usb_phy_set_suspend(dwc->usb3_phy1, 1);
	usb_phy_set_suspend(dwc->usb2_phy, 1);
	usb_phy_set_suspend(dwc->usb3_phy, 1);
	dwc3_free_scratch_buffers(dwc);

err1:
	usb_phy_shutdown(dwc->usb2_phy1);
	usb_phy_shutdown(dwc->usb3_phy1);
	usb_phy_shutdown(dwc->usb2_phy);
	usb_phy_shutdown(dwc->usb3_phy);
	phy_exit(dwc->usb2_generic_phy);
	phy_exit(dwc->usb3_generic_phy);

err0a:
	dwc3_ulpi_exit(dwc);

err0:
	return ret;
}

static int dwc3_core_get_phy(struct dwc3 *dwc)
{
	struct device		*dev = dwc->dev;
	struct device_node	*node = dev->of_node;
	int ret;

	if (node) {
		dwc->usb2_phy = devm_usb_get_phy_by_phandle(dev, "usb-phy", 0);
		dwc->usb3_phy = devm_usb_get_phy_by_phandle(dev, "usb-phy", 1);
		if (dwc->dual_port) {
			dwc->usb2_phy1 = devm_usb_get_phy_by_phandle(dev,
								"usb-phy", 2);
			dwc->usb3_phy1 = devm_usb_get_phy_by_phandle(dev,
								"usb-phy", 3);
		}
	} else {
		dwc->usb2_phy = devm_usb_get_phy(dev, USB_PHY_TYPE_USB2);
		dwc->usb3_phy = devm_usb_get_phy(dev, USB_PHY_TYPE_USB3);
	}

	if (IS_ERR(dwc->usb2_phy)) {
		ret = PTR_ERR(dwc->usb2_phy);
		if (ret == -ENXIO || ret == -ENODEV) {
			dwc->usb2_phy = NULL;
		} else if (ret == -EPROBE_DEFER) {
			return ret;
		} else {
			dev_err(dev, "no usb2 phy configured\n");
			return ret;
		}
	}

	if (IS_ERR(dwc->usb3_phy)) {
		ret = PTR_ERR(dwc->usb3_phy);
		if (ret == -ENXIO || ret == -ENODEV) {
			dwc->usb3_phy = NULL;
		} else if (ret == -EPROBE_DEFER) {
			return ret;
		} else {
			dev_err(dev, "no usb3 phy configured\n");
			return ret;
		}
	}

	if (dwc->dual_port) {
		if (IS_ERR(dwc->usb2_phy1)) {
			ret = PTR_ERR(dwc->usb2_phy1);
			if (ret == -ENXIO || ret == -ENODEV) {
				dwc->usb2_phy1 = NULL;
			} else {
				if (ret != -EPROBE_DEFER)
					dev_err(dev, "no usb2 phy1 configured\n");
				return ret;
			}
		}

		if (IS_ERR(dwc->usb3_phy1)) {
			ret = PTR_ERR(dwc->usb3_phy1);
			if (ret == -ENXIO || ret == -ENODEV) {
				dwc->usb3_phy1 = NULL;
			} else {
				if (ret != -EPROBE_DEFER)
					dev_err(dev, "no usb3 phy1 configured\n");
				return ret;
			}
		}
	}

	dwc->usb2_generic_phy = devm_phy_get(dev, "usb2-phy");
	if (IS_ERR(dwc->usb2_generic_phy)) {
		ret = PTR_ERR(dwc->usb2_generic_phy);
		if (ret == -ENOSYS || ret == -ENODEV) {
			dwc->usb2_generic_phy = NULL;
		} else if (ret == -EPROBE_DEFER) {
			return ret;
		} else {
			dev_err(dev, "no usb2 phy configured\n");
			return ret;
		}
	}

	dwc->usb3_generic_phy = devm_phy_get(dev, "usb3-phy");
	if (IS_ERR(dwc->usb3_generic_phy)) {
		ret = PTR_ERR(dwc->usb3_generic_phy);
		if (ret == -ENOSYS || ret == -ENODEV) {
			dwc->usb3_generic_phy = NULL;
		} else if (ret == -EPROBE_DEFER) {
			return ret;
		} else {
			dev_err(dev, "no usb3 phy configured\n");
			return ret;
		}
	}

	return 0;
}

static void __maybe_unused dwc3_core_exit_mode(struct dwc3 *dwc)
{
	switch (dwc->dr_mode) {
	case USB_DR_MODE_PERIPHERAL:
		dwc3_gadget_exit(dwc);
		break;
	case USB_DR_MODE_HOST:
		dwc3_host_exit(dwc);
		break;
	case USB_DR_MODE_OTG:
		dwc3_drd_exit(dwc);
		break;
	default:
		/* do nothing */
		break;
	}

	/* de-assert DRVVBUS for HOST and OTG mode */
	dwc3_set_prtcap(dwc, DWC3_GCTL_PRTCAP_DEVICE);
}

static void (*notify_event)(struct dwc3 *, unsigned int, unsigned int);
void dwc3_set_notifier(void (*notify)(struct dwc3 *, unsigned int,
							unsigned int))
{
	notify_event = notify;
}
EXPORT_SYMBOL(dwc3_set_notifier);

int dwc3_notify_event(struct dwc3 *dwc, unsigned int event, unsigned int value)
{
	int ret = 0;

	if (notify_event)
		notify_event(dwc, event, value);
	else
		ret = -ENODEV;

	return ret;
}
EXPORT_SYMBOL(dwc3_notify_event);

static void dwc3_get_properties(struct dwc3 *dwc)
{
	struct device		*dev = dwc->dev;
	u8			lpm_nyet_threshold;
	u8			tx_de_emphasis;
	u8			hird_threshold;

	/* default to highest possible threshold */
	lpm_nyet_threshold = 0xf;

	/* default to -3.5dB de-emphasis */
	tx_de_emphasis = 1;

	/*
	 * default to assert utmi_sleep_n and use maximum allowed HIRD
	 * threshold value of 0b1100
	 */
	hird_threshold = 12;

	dwc->maximum_speed = usb_get_maximum_speed(dev);
	dwc->max_hw_supp_speed = dwc->maximum_speed;
	dwc->dr_mode = usb_get_dr_mode(dev);
	dwc->hsphy_mode = of_usb_get_phy_mode(dev->of_node);

	dwc->sysdev_is_parent = device_property_read_bool(dev,
				"linux,sysdev_is_parent");
	if (dwc->sysdev_is_parent)
		dwc->sysdev = dwc->dev->parent;
	else
		dwc->sysdev = dwc->dev;

	dwc->has_lpm_erratum = device_property_read_bool(dev,
				"snps,has-lpm-erratum");
	device_property_read_u8(dev, "snps,lpm-nyet-threshold",
				&lpm_nyet_threshold);
	dwc->is_utmi_l1_suspend = device_property_read_bool(dev,
				"snps,is-utmi-l1-suspend");
	device_property_read_u8(dev, "snps,hird-threshold",
				&hird_threshold);

	device_property_read_u32(dev, "snps,xhci-imod-value",
			&dwc->xhci_imod_value);

	dwc->core_id = -1;
	device_property_read_u32(dev, "usb-core-id", &dwc->core_id);

	dwc->usb3_lpm_capable = device_property_read_bool(dev,
				"snps,usb3_lpm_capable");

	dwc->needs_fifo_resize = device_property_read_bool(dev,
				"tx-fifo-resize");

	dwc->disable_scramble_quirk = device_property_read_bool(dev,
				"snps,disable_scramble_quirk");
	dwc->u2exit_lfps_quirk = device_property_read_bool(dev,
				"snps,u2exit_lfps_quirk");
	dwc->u2ss_inp3_quirk = device_property_read_bool(dev,
				"snps,u2ss_inp3_quirk");
	dwc->req_p1p2p3_quirk = device_property_read_bool(dev,
				"snps,req_p1p2p3_quirk");
	dwc->del_p1p2p3_quirk = device_property_read_bool(dev,
				"snps,del_p1p2p3_quirk");
	dwc->del_phy_power_chg_quirk = device_property_read_bool(dev,
				"snps,del_phy_power_chg_quirk");
	dwc->lfps_filter_quirk = device_property_read_bool(dev,
				"snps,lfps_filter_quirk");
	dwc->rx_detect_poll_quirk = device_property_read_bool(dev,
				"snps,rx_detect_poll_quirk");
	dwc->dis_u3_susphy_quirk = device_property_read_bool(dev,
				"snps,dis_u3_susphy_quirk");
	dwc->dis_u2_susphy_quirk = device_property_read_bool(dev,
				"snps,dis_u2_susphy_quirk");
	dwc->dis_enblslpm_quirk = device_property_read_bool(dev,
				"snps,dis_enblslpm_quirk");
	dwc->dis_rxdet_inp3_quirk = device_property_read_bool(dev,
				"snps,dis_rxdet_inp3_quirk");
	dwc->dis_u2_freeclk_exists_quirk = device_property_read_bool(dev,
				"snps,dis-u2-freeclk-exists-quirk");
	dwc->dis_del_phy_power_chg_quirk = device_property_read_bool(dev,
				"snps,dis-del-phy-power-chg-quirk");
	dwc->dis_tx_ipgap_linecheck_quirk = device_property_read_bool(dev,
				"snps,dis-tx-ipgap-linecheck-quirk");
	dwc->parkmode_disable_ss_quirk = device_property_read_bool(dev,
				"snps,parkmode-disable-ss-quirk");

	dwc->tx_de_emphasis_quirk = device_property_read_bool(dev,
				"snps,tx_de_emphasis_quirk");
	dwc->ssp_u3_u0_quirk = device_property_read_bool(dev,
				"snps,ssp-u3-u0-quirk");
	device_property_read_u8(dev, "snps,tx_de_emphasis",
				&tx_de_emphasis);
	device_property_read_string(dev, "snps,hsphy_interface",
				    &dwc->hsphy_interface);
	device_property_read_u32(dev, "snps,quirk-frame-length-adjustment",
				 &dwc->fladj);
	dwc->enable_bus_suspend = device_property_read_bool(dev,
					"snps,bus-suspend-enable");
	dwc->usb3_u1u2_disable = device_property_read_bool(dev,
					"snps,usb3-u1u2-disable");
	dwc->disable_clk_gating = device_property_read_bool(dev,
					"snps,disable-clk-gating");
	device_property_read_u32(dev, "num-gsi-eps", &dwc->num_gsi_eps);

	dwc->dis_metastability_quirk = device_property_read_bool(dev,
				"snps,dis_metastability_quirk");

	dwc->lpm_nyet_threshold = lpm_nyet_threshold;
	dwc->tx_de_emphasis = tx_de_emphasis;

	dwc->hird_threshold = hird_threshold
		| (dwc->is_utmi_l1_suspend << 4);

	dwc->imod_interval = 0;
}

/* check whether the core supports IMOD */
bool dwc3_has_imod(struct dwc3 *dwc)
{
	return ((dwc3_is_usb3(dwc) &&
		 dwc->revision >= DWC3_REVISION_300A) ||
		(dwc3_is_usb31(dwc) &&
		 dwc->revision >= DWC3_USB31_REVISION_120A));
}

static void dwc3_check_params(struct dwc3 *dwc)
{
	struct device *dev = dwc->dev;

	/* Check for proper value of imod_interval */
	if (dwc->imod_interval && !dwc3_has_imod(dwc)) {
		dev_warn(dwc->dev, "Interrupt moderation not supported\n");
		dwc->imod_interval = 0;
	}

	/*
	 * Workaround for STAR 9000961433 which affects only version
	 * 3.00a of the DWC_usb3 core. This prevents the controller
	 * interrupt from being masked while handling events. IMOD
	 * allows us to work around this issue. Enable it for the
	 * affected version.
	 */
	if (!dwc->imod_interval &&
	    (dwc->revision == DWC3_REVISION_300A))
		dwc->imod_interval = 1;

	/* Check the maximum_speed parameter */
	switch (dwc->maximum_speed) {
	case USB_SPEED_LOW:
	case USB_SPEED_FULL:
	case USB_SPEED_HIGH:
	case USB_SPEED_SUPER:
	case USB_SPEED_SUPER_PLUS:
		break;
	default:
		dev_err(dev, "invalid maximum_speed parameter %d\n",
			dwc->maximum_speed);
		/* fall through */
	case USB_SPEED_UNKNOWN:
		/* default to superspeed */
		dwc->maximum_speed = USB_SPEED_SUPER;

		/*
		 * default to superspeed plus if we are capable.
		 */
		if (dwc3_is_usb31(dwc) &&
		    (DWC3_GHWPARAMS3_SSPHY_IFC(dwc->hwparams.hwparams3) ==
		     DWC3_GHWPARAMS3_SSPHY_IFC_GEN2))
			dwc->maximum_speed = USB_SPEED_SUPER_PLUS;

		break;
	}
}

static int dwc3_probe(struct platform_device *pdev)
{
	struct device		*dev = &pdev->dev;
	struct resource		*res;
	struct dwc3		*dwc;

	int			ret;

	void __iomem		*regs;
	int			irq;
	char			dma_ipc_log_ctx_name[40];

	if (count >= DWC_CTRL_COUNT) {
		dev_err(dev, "Err dwc instance %d >= %d available\n",
					count, DWC_CTRL_COUNT);
		ret = -EINVAL;
		return ret;
	}

	dwc = devm_kzalloc(dev, sizeof(*dwc), GFP_KERNEL);
	if (!dwc)
		return -ENOMEM;

	dwc->dev = dev;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "missing memory resource\n");
		return -ENODEV;
	}

	dwc->reg_phys = res->start;
	dwc->xhci_resources[0].start = res->start;
	dwc->xhci_resources[0].end = dwc->xhci_resources[0].start +
					DWC3_XHCI_REGS_END;
	dwc->xhci_resources[0].flags = res->flags;
	dwc->xhci_resources[0].name = res->name;

	res->start += DWC3_GLOBALS_REGS_START;

	irq = platform_get_irq(to_platform_device(dwc->dev), 0);

	ret = devm_request_irq(dev, irq, dwc3_interrupt, IRQF_SHARED, "dwc3",
			dwc);
	if (ret) {
		dev_err(dwc->dev, "failed to request irq #%d --> %d\n",
				irq, ret);
		return -ENODEV;
	}

	/* will be enabled in dwc3_msm_resume() */
	disable_irq(irq);
	dwc->irq = irq;
	/*
	 * Request memory region but exclude xHCI regs,
	 * since it will be requested by the xhci-plat driver.
	 */
	regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(regs)) {
		ret = PTR_ERR(regs);
		goto err0;
	}

	dwc->dwc_wq = alloc_ordered_workqueue("dwc_wq", WQ_HIGHPRI);
	if (!dwc->dwc_wq) {
		dev_err(dev,
			"%s: Unable to create workqueue dwc_wq\n", __func__);
		goto err0;
	}

	INIT_WORK(&dwc->bh_work, dwc3_bh_work);
	dwc->regs	= regs;
	dwc->regs_size	= resource_size(res);

	dwc3_get_properties(dwc);
	platform_set_drvdata(pdev, dwc);

	init_waitqueue_head(&dwc->wait_linkstate);
	spin_lock_init(&dwc->lock);

	pm_runtime_no_callbacks(dev);
	pm_runtime_set_active(dev);
	if (dwc->enable_bus_suspend) {
		pm_runtime_set_autosuspend_delay(dev,
			DWC3_DEFAULT_AUTOSUSPEND_DELAY);
		pm_runtime_use_autosuspend(dev);
	}
	pm_runtime_enable(dev);
	pm_runtime_forbid(dev);

	ret = dwc3_alloc_event_buffers(dwc, DWC3_EVENT_BUFFERS_SIZE);
	if (ret) {
		dev_err(dwc->dev, "failed to allocate event buffers\n");
		ret = -ENOMEM;
		goto err1;
	}

	ret = dwc3_alloc_scratch_buffers(dwc);
	if (ret)
		goto err2;

	if (dwc->dr_mode == USB_DR_MODE_OTG ||
		dwc->dr_mode == USB_DR_MODE_PERIPHERAL) {
		ret = dwc3_gadget_init(dwc);
		if (ret) {
			dev_err(dwc->dev, "gadget init failed %d\n", ret);
			goto err3;
		}
	}

	dwc->dwc_ipc_log_ctxt = ipc_log_context_create(NUM_LOG_PAGES,
					dev_name(dwc->dev), 0);
	if (!dwc->dwc_ipc_log_ctxt)
		dev_err(dwc->dev, "Error getting ipc_log_ctxt\n");

	snprintf(dma_ipc_log_ctx_name, sizeof(dma_ipc_log_ctx_name),
					"%s.ep_events", dev_name(dwc->dev));
	dwc->dwc_dma_ipc_log_ctxt = ipc_log_context_create(NUM_LOG_PAGES,
						dma_ipc_log_ctx_name, 0);
	if (!dwc->dwc_dma_ipc_log_ctxt)
		dev_err(dwc->dev, "Error getting ipc_log_ctxt for ep_events\n");

	dwc3_instance[count] = dwc;
	dwc->index = count;
	count++;

	pm_runtime_allow(dev);
	dwc3_debugfs_init(dwc);
	pm_runtime_put(dev);

	dma_set_max_seg_size(dev, UINT_MAX);

	return 0;

err3:
	dwc3_free_scratch_buffers(dwc);
err2:
	dwc3_free_event_buffers(dwc);

err1:
	destroy_workqueue(dwc->dwc_wq);

err0:
	/*
	 * restore res->start back to its original value so that, in case the
	 * probe is deferred, we don't end up getting error in request the
	 * memory region the next time probe is called.
	 */
	res->start -= DWC3_GLOBALS_REGS_START;

	return ret;
}

static int dwc3_remove(struct platform_device *pdev)
{
	struct dwc3	*dwc = platform_get_drvdata(pdev);
	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	/*
	 * restore res->start back to its original value so that, in case the
	 * probe is deferred, we don't end up getting error in request the
	 * memory region the next time probe is called.
	 */
	res->start -= DWC3_GLOBALS_REGS_START;

	dwc3_debugfs_exit(dwc);
	dwc3_gadget_exit(dwc);

	dwc3_core_exit(dwc);
	dwc3_ulpi_exit(dwc);

	pm_runtime_allow(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);
	pm_runtime_set_suspended(&pdev->dev);

	dwc3_free_event_buffers(dwc);
	dwc3_free_scratch_buffers(dwc);

	ipc_log_context_destroy(dwc->dwc_ipc_log_ctxt);
	dwc->dwc_ipc_log_ctxt = NULL;
	count--;
	dwc3_instance[dwc->index] = NULL;

	return 0;
}

#ifdef CONFIG_PM
static int dwc3_suspend_common(struct dwc3 *dwc)
{
	unsigned long	flags;

	switch (dwc->dr_mode) {
	case USB_DR_MODE_PERIPHERAL:
	case USB_DR_MODE_OTG:
		spin_lock_irqsave(&dwc->lock, flags);
		dwc3_gadget_suspend(dwc);
		spin_unlock_irqrestore(&dwc->lock, flags);
		break;
	case USB_DR_MODE_HOST:
	default:
		/* do nothing */
		break;
	}

	dwc3_core_exit(dwc);

	return 0;
}

static int dwc3_resume_common(struct dwc3 *dwc)
{
	unsigned long	flags;
	int		ret;

	ret = dwc3_core_init(dwc);
	if (ret)
		return ret;

	switch (dwc->dr_mode) {
	case USB_DR_MODE_PERIPHERAL:
	case USB_DR_MODE_OTG:
		spin_lock_irqsave(&dwc->lock, flags);
		dwc3_gadget_resume(dwc);
		spin_unlock_irqrestore(&dwc->lock, flags);
		/* FALLTHROUGH */
	case USB_DR_MODE_HOST:
	default:
		/* do nothing */
		break;
	}

	return 0;
}

static int dwc3_runtime_checks(struct dwc3 *dwc)
{
	switch (dwc->dr_mode) {
	case USB_DR_MODE_PERIPHERAL:
	case USB_DR_MODE_OTG:
		if (dwc->connected)
			return -EBUSY;
		break;
	case USB_DR_MODE_HOST:
	default:
		/* do nothing */
		break;
	}

	return 0;
}

static int dwc3_runtime_suspend(struct device *dev)
{
	struct dwc3     *dwc = dev_get_drvdata(dev);
	int		ret;

	/* Check if platform glue driver handling PM, if not then handle here */
	if (!dwc3_notify_event(dwc, DWC3_CORE_PM_SUSPEND_EVENT, 0))
		return 0;

	if (dwc3_runtime_checks(dwc))
		return -EBUSY;

	ret = dwc3_suspend_common(dwc);
	if (ret)
		return ret;

	device_init_wakeup(dev, true);

	return 0;
}

static int dwc3_runtime_resume(struct device *dev)
{
	struct dwc3     *dwc = dev_get_drvdata(dev);
	int		ret;

	/* Check if platform glue driver handling PM, if not then handle here */
	if (!dwc3_notify_event(dwc, DWC3_CORE_PM_RESUME_EVENT, 0))
		return 0;

	device_init_wakeup(dev, false);

	ret = dwc3_resume_common(dwc);
	if (ret)
		return ret;

	switch (dwc->dr_mode) {
	case USB_DR_MODE_PERIPHERAL:
	case USB_DR_MODE_OTG:
		dwc3_gadget_process_pending_events(dwc);
		break;
	case USB_DR_MODE_HOST:
	default:
		/* do nothing */
		break;
	}

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put(dev);

	return 0;
}

static int dwc3_runtime_idle(struct device *dev)
{
	struct dwc3     *dwc = dev_get_drvdata(dev);

	switch (dwc->dr_mode) {
	case USB_DR_MODE_PERIPHERAL:
	case USB_DR_MODE_OTG:
		if (dwc3_runtime_checks(dwc))
			return -EBUSY;
		break;
	case USB_DR_MODE_HOST:
	default:
		/* do nothing */
		break;
	}

	pm_runtime_mark_last_busy(dev);
	pm_runtime_autosuspend(dev);

	return 0;
}
#endif /* CONFIG_PM */

#ifdef CONFIG_PM_SLEEP
static int dwc3_suspend(struct device *dev)
{
	struct dwc3	*dwc = dev_get_drvdata(dev);
	int		ret;

	/* Check if platform glue driver handling PM, if not then handle here */
	if (!dwc3_notify_event(dwc, DWC3_CORE_PM_SUSPEND_EVENT, 0))
		return 0;

	ret = dwc3_suspend_common(dwc);
	if (ret)
		return ret;

	pinctrl_pm_select_sleep_state(dev);

	return 0;
}

static int dwc3_resume(struct device *dev)
{
	struct dwc3	*dwc = dev_get_drvdata(dev);
	int		ret;

	/* Check if platform glue driver handling PM, if not then handle here */
	if (!dwc3_notify_event(dwc, DWC3_CORE_PM_RESUME_EVENT, 0)) {
		/*
		 * If the core was in host mode during suspend, then set the
		 * runtime PM state as active to reflect actual state of device
		 * which is now out of LPM. This allows runtime_suspend later.
		 */
		if (dwc->current_dr_role == DWC3_GCTL_PRTCAP_HOST &&
		    dwc->ignore_wakeup_src_in_hostmode)
			goto runtime_set_active;

		return 0;
	}

	pinctrl_pm_select_default_state(dev);

	ret = dwc3_resume_common(dwc);
	if (ret)
		return ret;

runtime_set_active:
	pm_runtime_disable(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	return 0;
}
#endif /* CONFIG_PM_SLEEP */

static const struct dev_pm_ops dwc3_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(dwc3_suspend, dwc3_resume)
	SET_RUNTIME_PM_OPS(dwc3_runtime_suspend, dwc3_runtime_resume,
			dwc3_runtime_idle)
};

#ifdef CONFIG_OF
static const struct of_device_id of_dwc3_match[] = {
	{
		.compatible = "snps,dwc3"
	},
	{
		.compatible = "synopsys,dwc3"
	},
	{ },
};
MODULE_DEVICE_TABLE(of, of_dwc3_match);
#endif

#ifdef CONFIG_ACPI

#define ACPI_ID_INTEL_BSW	"808622B7"

static const struct acpi_device_id dwc3_acpi_match[] = {
	{ ACPI_ID_INTEL_BSW, 0 },
	{ },
};
MODULE_DEVICE_TABLE(acpi, dwc3_acpi_match);
#endif

static struct platform_driver dwc3_driver = {
	.probe		= dwc3_probe,
	.remove		= dwc3_remove,
	.driver		= {
		.name	= "dwc3",
		.of_match_table	= of_match_ptr(of_dwc3_match),
		.acpi_match_table = ACPI_PTR(dwc3_acpi_match),
		.pm	= &dwc3_dev_pm_ops,
	},
};

module_platform_driver(dwc3_driver);

MODULE_ALIAS("platform:dwc3");
MODULE_AUTHOR("Felipe Balbi <balbi@ti.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("DesignWare USB3 DRD Controller Driver");
