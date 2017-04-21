/*
 * pmic_ccsm.c - Intel MID PMIC Charger Driver
 *
 * Copyright (C) 2011 Intel Corporation
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * Author: Jenny TC <jenny.tc@intel.com>
 * Author: Yegnesh Iyer <yegnesh.s.iyer@intel.com>
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/acpi.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/usb/otg.h>
#include <linux/power_supply.h>
#include <linux/thermal.h>
#include <linux/iio/consumer.h>
#include <linux/notifier.h>
#include <linux/mfd/intel_soc_pmic.h>
#include <linux/extcon.h>
#include "intel_pmic_ccsm.h"

/* Macros */
#define DRIVER_NAME "pmic_ccsm"
#define ADC_TO_TEMP 1
#define TEMP_TO_ADC 0
#define USB_WAKE_LOCK_TIMEOUT	(5 * HZ)

#define USBINPUTICC100VAL	100
#define CDP_INPUT_CURRENT_LIMIT 1500
#define HIGH_POWER_CHRG_CURRENT 2000
#define LOW_POWER_CHRG_CURRENT 500

#define INTERNAL_PHY_SUPPORTED(model) \
	((model == INTEL_PMIC_SCOVE) || (model == INTEL_PMIC_WCOVE))

#define NEED_ZONE_SPLIT(bprof)\
	 ((bprof->temp_mon_ranges < MIN_BATT_PROF))
#define NEXT_ZONE_OFFSET 2
#define BATTEMP_CHANNEL "BATTEMP0"
#define VBUS_CTRL_CDEV_NAME	"vbus_control"

#define RID_A_MIN 11150
#define RID_A_MAX 13640
#define RID_B_MAX 7480
#define RID_B_MIN 6120
#define RID_C_MAX 4015
#define RID_C_MIN 3285

#define IS_RID_A(rid) (rid > RID_A_MIN && rid < RID_A_MAX)
#define IS_RID_B(rid) (rid > RID_B_MIN && rid < RID_B_MAX)
#define IS_RID_C(rid) (rid > RID_C_MIN && rid < RID_C_MAX)

#define KELVIN_OFFSET	27315

/* Type definitions */
static int intel_pmic_handle_otgmode(bool enable);

/* Extern definitions */

/* Global declarations */
static DEFINE_MUTEX(pmic_lock);
static struct pmic_chrgr_drv_context chc;

u16 pmic_inlmt[][2] = {
	{ 100, CHGRCTRL1_FUSB_INLMT_100},
	{ 150, CHGRCTRL1_FUSB_INLMT_150},
	{ 500, CHGRCTRL1_FUSB_INLMT_500},
	{ 900, CHGRCTRL1_FUSB_INLMT_900},
	{ 1500, CHGRCTRL1_FUSB_INLMT_1500},
	{ 2000, CHGRCTRL1_FUSB_INLMT_1500},
	{ 2500, CHGRCTRL1_FUSB_INLMT_1500},
};

enum pmic_vbus_states {
	VBUS_ENABLE,
	VBUS_DISABLE,
	MAX_VBUSCTRL_STATES,
};

static int pmic_read_reg(u16 addr, u8 *val)
{
	int ret;

	ret = intel_soc_pmic_readb(addr);
	if (ret == -EIO) {
		dev_err(chc.dev, "%s:Error(%d): addr:data 0x%.4x\n",
				__func__, ret, addr);
		return ret;
	}
	*val = ret;
	return 0;
}

static int pmic_write_reg(u16 addr, u8 val)
{
	int ret;

	ret = intel_soc_pmic_writeb(addr, val);
	if (ret)
		dev_err(chc.dev, "%s:Error(%d): addr:data 0x%.4x:0x%.4x\n",
				__func__, ret, addr, val);
	return ret;
}

static int __pmic_write_tt(u8 addr, u8 data)
{
	int ret;

	/* If TT is locked return true */
	if (chc.tt_lock)
		return 0;

	ret = pmic_write_reg(chc.reg_map->pmic_chrttaddr, addr);
	if (!ret)
		ret = pmic_write_reg(chc.reg_map->pmic_chrttdata, data);
	return ret;
}

static inline int pmic_write_tt(u8 addr, u8 data)
{
	int ret;

	mutex_lock(&pmic_lock);
	ret = __pmic_write_tt(addr, data);
	mutex_unlock(&pmic_lock);
	return ret;
}

static int __pmic_read_tt(u8 addr, u8 *data)
{
	int ret;

	ret = pmic_write_reg(chc.reg_map->pmic_chrttaddr, addr);
	if (ret)
		return ret;

	/* Delay the TT read by 2ms to ensure that the data is populated
	 * in data register
	 */
	usleep_range(2000, 3000);

	return pmic_read_reg(chc.reg_map->pmic_chrttdata, data);
}

static inline int pmic_read_tt(u8 addr, u8 *data)
{
	int ret;

	mutex_lock(&pmic_lock);
	ret = __pmic_read_tt(addr, data);
	mutex_unlock(&pmic_lock);

	return ret;
}

void intel_pmic_ccsm_dump_regs(void)
{
	u8 data;
	int ret, i;
	u16 *reg;

	dev_dbg(chc.dev, "PMIC Register dump\n");
	dev_dbg(chc.dev, "====================\n");

	reg = (u16 *)chc.reg_map;

	for (i = 0; i < chc.reg_cnt; i++, reg++) {

		ret = pmic_read_reg(*reg, &data);
		if (!ret)
			dev_dbg(chc.dev, "%s=0x%x\n", pmic_regs_name[i], data);
	}
	dev_dbg(chc.dev, "====================\n");
}

static int pmic_ccsm_suspend(struct device *dev)
{
	int ret;

	/* Disable CHGDIS pin */
	ret = intel_soc_pmic_update(chc.reg_map->pmic_chgdisctrl,
			CHGDISFN_DIS_CCSM_VAL, CHGDISFN_CCSM_MASK);
	if (ret)
		dev_warn(chc.dev, "Error writing to register: %x\n",
			chc.reg_map->pmic_chgdisctrl);

	return ret;
}

static int pmic_ccsm_resume(struct device *dev)
{
	int ret;

	/* Enable CHGDIS pin */
	ret = intel_soc_pmic_update(chc.reg_map->pmic_chgdisctrl,
			CHGDISFN_EN_CCSM_VAL, CHGDISFN_CCSM_MASK);
	if (ret)
		dev_warn(chc.dev, "Error writing to register: %x\n",
			chc.reg_map->pmic_chgdisctrl);

	return ret;
}

const struct dev_pm_ops pmic_ccsm_pm = {
	.suspend = pmic_ccsm_suspend,
	.resume = pmic_ccsm_resume,
};

void acpi_pmic_enable_vbus(bool enable)
{
#if CONFIG_ACPI
	acpi_status status;
	acpi_handle handle = ACPI_HANDLE(intel_soc_pmic_dev());
	struct acpi_object_list args;
	union acpi_object object;

	if (!handle) {
		dev_err(intel_soc_pmic_dev(), "error null hanlder\n");
		return;
	}

	args.count = 1;
	args.pointer = &object;
	object.type = ACPI_TYPE_INTEGER;
	object.integer.value = enable ? 1 : 0;

	status = acpi_evaluate_object(handle, "VBUS", &args, NULL);
	if (ACPI_FAILURE(status))
		dev_err(intel_soc_pmic_dev(),
			"ACPI method call fail:%x\n", status);
#endif
}

int intel_pmic_enable_vbus(bool enable)
{
	int ret = 0;

	if (enable)
		ret = intel_soc_pmic_update(chc.reg_map->pmic_chgrctrl0,
				WDT_NOKICK_ENABLE, CHGRCTRL0_WDT_NOKICK_MASK);
	else
		ret = intel_soc_pmic_update(chc.reg_map->pmic_chgrctrl0,
				WDT_NOKICK_DISABLE, CHGRCTRL0_WDT_NOKICK_MASK);

	acpi_pmic_enable_vbus(enable);

	/* If access is blocked return success to avoid additional
	*  error handling at client side
	*/
	if (ret == -EACCES) {
		dev_warn(chc.dev, "IPC blocked due to unsigned kernel/invalid battery\n");
		ret = 0;
	}

	return ret;
}

static int intel_pmic_handle_otgmode(bool enable)
{
	int ret = 0;

	if (chc.pmic_model == INTEL_PMIC_BCOVE)
		return 0;

	if (enable)
		ret = intel_soc_pmic_update(chc.reg_map->pmic_chgrctrl1,
				CHGRCTRL1_OTGMODE_MASK,
				CHGRCTRL1_OTGMODE_MASK);
	else
		ret = intel_soc_pmic_update(chc.reg_map->pmic_chgrctrl1,
				0x0, CHGRCTRL1_OTGMODE_MASK);

	/* If access is blocked return success to avoid additional
	*  error handling at client side
	*/
	if (ret == -EACCES) {
		dev_warn(chc.dev, "IPC blocked due to unsigned kernel/invalid battery\n");
		ret = 0;
	}

	return ret;
}

static int pmic_get_usbid(void)
{
	int ret;
	struct iio_channel *indio_chan;
	int rid, id = RID_UNKNOWN;
	u8 val;

	ret = pmic_read_reg(chc.reg_map->pmic_schgrirq1, &val);
	if (ret)
		return RID_UNKNOWN;

	/* SCHGRIRQ1_REG SUSBIDDET bit definition:
	 * 00 = RID_A/B/C ; 01 = RID_GND ; 10 = RID_FLOAT
	 */
	if ((val & SCHRGRIRQ1_SUSBIDGNDDET_MASK) == SHRT_FLT_DET)
		return RID_FLOAT;
	else if ((val & SCHRGRIRQ1_SUSBIDGNDDET_MASK) == SHRT_GND_DET)
		return RID_GND;

	indio_chan = iio_channel_get(NULL, "USBID");
	if (IS_ERR_OR_NULL(indio_chan)) {
		dev_err(chc.dev, "Failed to get IIO channel USBID\n");
		return RID_UNKNOWN;
	}

	ret = iio_read_channel_raw(indio_chan, &rid);
	if (ret) {
		dev_err(chc.dev, "IIO channel read error for USBID\n");
		goto err_exit;
	}
	dev_dbg(chc.dev, "%s: rid=%d\n", __func__, rid);
	if (IS_RID_A(rid))
		id = RID_A;
	else if (IS_RID_B(rid))
		id = RID_B;
	else if (IS_RID_C(rid))
		id = RID_C;

err_exit:
	iio_channel_release(indio_chan);
	return id;
}

static int get_charger_type(void)
{
	int ret, i = 0;
	u8 val;
	int chgr_type, rid;

	do {
		ret = pmic_read_reg(chc.reg_map->pmic_usbsrcdetstat, &val);
		if (ret)
			return 0;
		i++;
		dev_dbg(chc.dev, "Read USBSRCDETSTATUS val: %x\n", val);

		if ((val & USBSRCDET_SUSBHWDET_DETSUCC) ==
				USBSRCDET_SUSBHWDET_DETSUCC)
			break;
		msleep(USBSRCDET_SLEEP_TIME);
	} while (i < USBSRCDET_RETRY_CNT);

	if ((val & USBSRCDET_SUSBHWDET_DETSUCC) !=
			USBSRCDET_SUSBHWDET_DETSUCC) {
		dev_err(chc.dev, "Charger detection unsuccessful after %dms\n",
			i * USBSRCDET_SLEEP_TIME);
		return 0;
	}

	chgr_type = (val & USBSRCDET_USBSRCRSLT_MASK) >> 2;
	dev_dbg(chc.dev, "Charger type after detection complete: %d\n",
			(val & USBSRCDET_USBSRCRSLT_MASK) >> 2);

	switch (chgr_type) {
	case PMIC_CHARGER_TYPE_SDP:
	case PMIC_CHARGER_TYPE_FLOAT_DP_DN:
		return POWER_SUPPLY_CHARGER_TYPE_USB_SDP;
	case PMIC_CHARGER_TYPE_DCP:
		return POWER_SUPPLY_CHARGER_TYPE_USB_DCP;
	case PMIC_CHARGER_TYPE_CDP:
		return POWER_SUPPLY_CHARGER_TYPE_USB_CDP;
	case PMIC_CHARGER_TYPE_ACA:
		rid = pmic_get_usbid();
		if (rid == RID_A)
			return POWER_SUPPLY_CHARGER_TYPE_ACA_DOCK;
		/* As PMIC detected the charger as ACA, if RID detection
		 * failed report type as ACA
		 */
		else
			return POWER_SUPPLY_CHARGER_TYPE_USB_ACA;
	case PMIC_CHARGER_TYPE_SE1:
		return POWER_SUPPLY_CHARGER_TYPE_SE1;
	case PMIC_CHARGER_TYPE_MHL:
		return POWER_SUPPLY_CHARGER_TYPE_MHL;
	default:
		return POWER_SUPPLY_CHARGER_TYPE_NONE;
	}
}

static void handle_internal_usbphy_notifications(int mask)
{
	struct power_supply_cable_props cap = {0};
	int evt = USB_EVENT_NONE;

	if (mask) {
		cap.chrg_evt = POWER_SUPPLY_CHARGER_EVENT_CONNECT;
		cap.chrg_type = get_charger_type();
		chc.charger_type = cap.chrg_type;

		if (cap.chrg_type == 0)
			return;
	} else {
		cap.chrg_evt = POWER_SUPPLY_CHARGER_EVENT_DISCONNECT;
		cap.chrg_type = chc.charger_type;
	}

	switch (cap.chrg_type) {
	case POWER_SUPPLY_CHARGER_TYPE_USB_SDP:
		if (cap.chrg_evt == POWER_SUPPLY_CHARGER_EVENT_CONNECT)
			evt =  USB_EVENT_VBUS;
		else
			evt =  USB_EVENT_NONE;
		if (chc.pdata->usb_compliance)
			cap.ma = USBINPUTICC100VAL;
		else
			cap.ma = LOW_POWER_CHRG_CURRENT;
		break;
	case POWER_SUPPLY_CHARGER_TYPE_USB_CDP:
		if (cap.chrg_evt == POWER_SUPPLY_CHARGER_EVENT_CONNECT)
			evt =  USB_EVENT_VBUS;
		else
			evt =  USB_EVENT_NONE;
		cap.ma = CDP_INPUT_CURRENT_LIMIT;
		break;
	case POWER_SUPPLY_CHARGER_TYPE_USB_DCP:
	case POWER_SUPPLY_CHARGER_TYPE_SE1:
	case POWER_SUPPLY_CHARGER_TYPE_USB_ACA:
		cap.ma = HIGH_POWER_CHRG_CURRENT;
		break;
	case POWER_SUPPLY_CHARGER_TYPE_ACA_DOCK:
	case POWER_SUPPLY_CHARGER_TYPE_ACA_A:
		cap.ma = HIGH_POWER_CHRG_CURRENT;
		if (cap.chrg_evt == POWER_SUPPLY_CHARGER_EVENT_CONNECT)
			evt = USB_EVENT_ID;
		else
			evt = USB_EVENT_NONE;
		break;
	case POWER_SUPPLY_CHARGER_TYPE_AC:
	case POWER_SUPPLY_CHARGER_TYPE_ACA_B:
	case POWER_SUPPLY_CHARGER_TYPE_ACA_C:
	case POWER_SUPPLY_CHARGER_TYPE_MHL:
	case POWER_SUPPLY_CHARGER_TYPE_B_DEVICE:
		cap.ma = HIGH_POWER_CHRG_CURRENT;
		break;
	case POWER_SUPPLY_CHARGER_TYPE_NONE:
	default:
		cap.ma = 0;
	}

	dev_dbg(chc.dev, "Notifying OTG ev:%d, evt:%d, chrg_type:%d, mA:%d\n",
			evt, cap.chrg_evt, cap.chrg_type,
			cap.ma);
	if (cap.chrg_evt == POWER_SUPPLY_CHARGER_EVENT_DISCONNECT)
		chc.charger_type = POWER_SUPPLY_CHARGER_TYPE_NONE;

	/*
	 * Open / Close D+/D- lines in USB detection switch
	 * due to WC PMIC bug only for SDP/CDP.
	 */
	pmic_write_reg(chc.reg_map->pmic_usbphyctrl,
			((evt == USB_EVENT_VBUS)
				|| (evt == USB_EVENT_ID)) ? 1 : 0);

	atomic_notifier_call_chain(&chc.otg->notifier,
				USB_EVENT_CHARGER, &cap);
	if (evt >= 0)
		atomic_notifier_call_chain(&chc.otg->notifier, evt, NULL);
}

static void handle_pwrsrc_interrupt(u16 int_reg, u16 stat_reg)
{
	int mask;
	u16 id_mask;
	struct power_supply_cable_props dcin_cable;

	id_mask = BIT_POS(PMIC_INT_USBIDFLTDET) |
				 BIT_POS(PMIC_INT_USBIDGNDDET);

	mutex_lock(&pmic_lock);
	if (int_reg & id_mask) {
		mask = (stat_reg & id_mask) == SHRT_GND_DET;
		/* Close/Open D+/D- lines in USB detection switch
		 * due to WC PMIC bug
		 */
		if (mask) {
			dev_info(chc.dev,
				"USB ID Detected. Notifying OTG driver\n");
			pmic_write_reg(chc.reg_map->pmic_usbphyctrl, 0x1);
			if (chc.vbus_state == VBUS_ENABLE) {
				if (chc.otg->set_vbus)
					chc.otg->set_vbus(chc.otg, true);
				else
					intel_pmic_enable_vbus(true);
				atomic_notifier_call_chain(&chc.otg->notifier,
						USB_EVENT_ID, &mask);
			}
		} else if ((int_reg & BIT_POS(PMIC_INT_USBIDFLTDET)) &&
				chc.otg_mode_enabled) {
			/* WA for OTG ID removal: PMIC interprets ID removal
			 * as ID_FLOAT. Check for ID float and otg_mode enabled
			 * to send ID disconnect.
			 * In order to avoid ctyp detection flow, disable otg
			 * mode during vbus turn off event
			 */
			dev_info(chc.dev,
				"USB ID Removed. Notifying OTG driver\n");
			if (chc.vbus_state == VBUS_ENABLE) {
				if (chc.otg->set_vbus)
					chc.otg->set_vbus(chc.otg, false);
				else
					intel_pmic_enable_vbus(false);
				atomic_notifier_call_chain(&chc.otg->notifier,
						USB_EVENT_NONE, NULL);
			}
			pmic_write_reg(chc.reg_map->pmic_usbphyctrl, 0x0);

		}
	}

	if ((int_reg & BIT_POS(PMIC_INT_USBIDDET)) &&
			(chc.vbus_state == VBUS_ENABLE)) {
		mask = !!(stat_reg & BIT_POS(PMIC_INT_USBIDDET));
		if (chc.otg->set_vbus)
			chc.otg->set_vbus(chc.otg, true);
		else
			intel_pmic_enable_vbus(true);
		atomic_notifier_call_chain(&chc.otg->notifier,
				USB_EVENT_ID, &mask);
	}
	mutex_unlock(&pmic_lock);

	if (int_reg & BIT_POS(PMIC_INT_VBUS)) {
		int ret;

		mask = !!(stat_reg & BIT_POS(PMIC_INT_VBUS));
		if (mask) {
			dev_info(chc.dev,
				"USB VBUS Detected. Notifying OTG driver\n");
			mutex_lock(&pmic_lock);
			chc.otg_mode_enabled =
				(stat_reg & id_mask) == SHRT_GND_DET;
			mutex_unlock(&pmic_lock);
		} else {
			dev_info(chc.dev,
				"USB VBUS Removed. Notifying OTG driver\n");
		}
		ret = intel_soc_pmic_readb(chc.reg_map->pmic_chgrctrl1);
		dev_dbg(chc.dev, "chgrctrl = %x", ret);
		if (ret & CHGRCTRL1_OTGMODE_MASK) {
			mutex_lock(&pmic_lock);
			chc.otg_mode_enabled = true;
			mutex_unlock(&pmic_lock);
		}

		/* Avoid charger-detection flow in case of host-mode */
		if (chc.is_internal_usb_phy && !chc.otg_mode_enabled)
			handle_internal_usbphy_notifications(mask);
		else if (!mask) {
			mutex_lock(&pmic_lock);
			chc.otg_mode_enabled =
					(stat_reg & id_mask) == SHRT_GND_DET;
			mutex_unlock(&pmic_lock);
		}
		mutex_lock(&pmic_lock);
		intel_pmic_handle_otgmode(chc.otg_mode_enabled);
		mutex_unlock(&pmic_lock);
	}

	if (int_reg & BIT_POS(PMIC_INT_DCIN)) {
		mask = !!(stat_reg & BIT_POS(PMIC_INT_DCIN));
		if (mask) {
			if (!chc.vdcin_det) {
				dev_info(chc.dev,
				"VDCIN Detected. Notifying charger framework\n");
				dcin_cable.chrg_evt =
					POWER_SUPPLY_CHARGER_EVENT_CONNECT;
				dcin_cable.chrg_type =
				POWER_SUPPLY_CHARGER_TYPE_WIRELESS;
				dcin_cable.ma = 900;
				atomic_notifier_call_chain(
					&power_supply_notifier,
					PSY_CABLE_EVENT, &dcin_cable);
				chc.vdcin_det = true;
			}
		} else {
			if (chc.vdcin_det) {
				dev_info(chc.dev,
				"VDCIN Removed.Notifying charger framework\n");
				dcin_cable.chrg_evt =
					POWER_SUPPLY_CHARGER_EVENT_DISCONNECT;
				dcin_cable.chrg_type =
				POWER_SUPPLY_CHARGER_TYPE_WIRELESS;
				dcin_cable.ma = 900;
				atomic_notifier_call_chain(
					&power_supply_notifier,
				  PSY_CABLE_EVENT, &dcin_cable);
				chc.vdcin_det = false;
			}
		}
	}
}

static void pmic_event_worker(struct work_struct *work)
{
	struct pmic_event *evt, *tmp;

	dev_dbg(chc.dev, "%s\n", __func__);

	list_for_each_entry_safe(evt, tmp, &chc.evt_queue, node) {
		list_del(&evt->node);

	dev_dbg(chc.dev, "%s pwrsrc=%X, spwrsrc=%x battirq=%x sbattirq=%x miscirq=%x smiscirq=%x wake thread\n",
			__func__, evt->pwrsrc_int,
			evt->pwrsrc_int_stat, evt->battemp_int,
			evt->battemp_int_stat, evt->misc_int,
			evt->misc_int_stat);

		if (evt->pwrsrc_int)
			handle_pwrsrc_interrupt(evt->pwrsrc_int,
						evt->pwrsrc_int_stat);
		kfree(evt);
	}
}

static irqreturn_t pmic_isr(int irq, void *data)
{
	return IRQ_WAKE_THREAD;
}
static irqreturn_t pmic_thread_handler(int id, void *data)
{
	int i, shift;
	u16 *pmic_int, *pmic_int_stat, off;
	u16 stat_reg = 0, int_reg = 0;
	u8 ireg_val = 0, sreg_val = 0, val;
	struct pmic_event *evt;

	evt = kzalloc(sizeof(struct pmic_event), GFP_KERNEL);
	if (!evt)
		return IRQ_NONE;

	pmic_int = &evt->pwrsrc_int;
	pmic_int_stat = &evt->pwrsrc_int_stat;

	for (i = 0; i < chc.intmap_size; ++i) {
		off = chc.intmap[i].pmic_int / 16;

		if (int_reg != chc.intmap[i].ireg) {
			pmic_read_reg(chc.intmap[i].ireg, &ireg_val);
			int_reg = chc.intmap[i].ireg;
		}
		val = ireg_val;
		dev_dbg(chc.dev, "%s:%d ireg=%x val = %x\n", __func__, __LINE__,
			chc.intmap[i].ireg, val);
		val &= chc.intmap[i].mask;

		shift = ffs(chc.intmap[i].mask) -
				ffs(BIT_POS(chc.intmap[i].pmic_int));
		if (shift < 0)
			val <<= abs(shift);
		else if (shift > 0)
			val >>= abs(shift);

		pmic_int[off] |= val;

		dev_dbg(chc.dev, "%s:%d ireg=%x\n", __func__, __LINE__,
				pmic_int[off]);

		if (stat_reg != chc.intmap[i].sreg) {
			pmic_read_reg(chc.intmap[i].sreg, &sreg_val);
			stat_reg = chc.intmap[i].sreg;
		}
		val = sreg_val;
		dev_dbg(chc.dev, "%s:%d sreg=%x\n",
				__func__, __LINE__, chc.intmap[i].sreg);
		val &= chc.intmap[i].mask;

		if (shift < 0)
			val <<= abs(shift);
		else if (shift > 0)
			val >>= abs(shift);

		pmic_int_stat[off] |= val;
		dev_dbg(chc.dev, "%s:%d stat=%x\n",
			__func__, __LINE__, pmic_int_stat[off]);
	}

	INIT_LIST_HEAD(&evt->node);
	list_add_tail(&evt->node, &chc.evt_queue);

	dev_dbg(chc.dev, "%s pwrsrc=%X, spwrsrc=%x battirq=%x sbattirq=%x miscirq=%x smiscirq=%x wake thread\n",
			__func__, evt->pwrsrc_int,
			evt->pwrsrc_int_stat, evt->battemp_int,
			evt->battemp_int_stat, evt->misc_int,
			evt->misc_int_stat);

	schedule_delayed_work(&chc.evt_work, msecs_to_jiffies(100));
	return IRQ_HANDLED;
}

static int pmic_check_initial_events(void)
{
	int ret = 0, i, shift;
	struct pmic_event *evt;
	u8 val, sreg_val = 0;
	u16 *pmic_int, *pmic_int_stat, off;
	u16 stat_reg = 0;
	struct extcon_dev *edev;

	evt = kzalloc(sizeof(struct pmic_event), GFP_KERNEL);
	if (!evt)
		return -ENOMEM;

	pmic_int = &evt->pwrsrc_int;
	pmic_int_stat = &evt->pwrsrc_int_stat;

	for (i = 0; i < chc.intmap_size; ++i) {
		off = chc.intmap[i].pmic_int / 16;

		if (stat_reg != chc.intmap[i].sreg) {
			pmic_read_reg(chc.intmap[i].sreg, &sreg_val);
			stat_reg = chc.intmap[i].sreg;
		}

		val = sreg_val;
		dev_dbg(chc.dev, "%s:%d reg=%x val = %x\n", __func__, __LINE__,
					chc.intmap[i].sreg, val);
		val &= chc.intmap[i].mask;
		dev_dbg(chc.dev, "%s:%d reg=%x val = %x\n", __func__, __LINE__,
					chc.intmap[i].sreg, val);

		shift = ffs(chc.intmap[i].mask) -
				ffs(BIT_POS(chc.intmap[i].pmic_int));
		if (shift < 0)
			val <<= abs(shift);
		else if (shift > 0)
			val >>= abs(shift);
		pmic_int[off] |= val;
		pmic_int_stat[off] |= val;
	}

	INIT_LIST_HEAD(&evt->node);
	list_add_tail(&evt->node, &chc.evt_queue);

	edev = extcon_get_extcon_dev("usb-typec");

	if (!edev)
		dev_err(chc.dev, "No edev found");
	else {
		chc.cable_state = extcon_get_cable_state(edev, "USB-Host");
		if (chc.cable_state)
			schedule_work(&chc.extcon_work);
	}

	schedule_delayed_work(&chc.evt_work, 0);

	return ret;
}

static int get_pmic_model(const char *name)
{
	if (!strncmp(name, "wcove_ccsm", strlen("wcove_ccsm")))
		return INTEL_PMIC_WCOVE;
	else if (!strncmp(name, "scove_ccsm", strlen("scove_ccsm")))
		return INTEL_PMIC_SCOVE;
	else if (!strncmp(name, "bcove_ccsm", strlen("bcove_ccsm")))
		return INTEL_PMIC_BCOVE;

	return INTEL_PMIC_UNKNOWN;
}

static void pmic_ccsm_extcon_host_work(struct work_struct *work)
{
	mutex_lock(&pmic_lock);
	if (chc.cable_state) {
		chc.otg_mode_enabled = chc.cable_state;
		intel_pmic_handle_otgmode(chc.otg_mode_enabled);
	}
	pmic_write_reg(chc.reg_map->pmic_usbphyctrl, chc.cable_state);
	mutex_unlock(&pmic_lock);
}

static int pmic_ccsm_usb_host_nb(struct notifier_block *nb,
		unsigned long event, void *data)
{
	struct extcon_dev *dev = (struct extcon_dev *)data;

	chc.cable_state = extcon_get_cable_state(dev, "USB-Host");
	schedule_work(&chc.extcon_work);
	return NOTIFY_OK;
}

/**
 * pmic_charger_probe - PMIC charger probe function
 * @pdev: pmic platform device structure
 * Context: can sleep
 *
 * pmic charger driver initializes its internal data
 * structure and other  infrastructure components for it
 * to work as expected.
 */
static int pmic_chrgr_probe(struct platform_device *pdev)
{
	int ret = 0, i = 0, irq;
	u8 val, chgr_ctrl0;

	if (!pdev)
		return -ENODEV;

	chc.batt_health = POWER_SUPPLY_HEALTH_UNKNOWN;
	chc.dev = &pdev->dev;

	while ((irq = platform_get_irq(pdev, i)) != -ENXIO)
		chc.irq[i++] = irq;

	chc.irq_cnt = i;
	chc.pdata = pdev->dev.platform_data;
	if (!chc.pdata) {
		dev_err(&pdev->dev, "Platform data not initialized\n");
		return -EFAULT;
	}

	platform_set_drvdata(pdev, &chc);
	chc.reg_map = chc.pdata->reg_map;
	chc.reg_cnt = sizeof(struct pmic_regs) / sizeof(u16);
	chc.intmap = chc.pdata->intmap;
	chc.intmap_size = chc.pdata->intmap_size;
	chc.vbus_state = VBUS_ENABLE;

	chc.pmic_model = get_pmic_model(pdev->name);
	dev_info(chc.dev, "PMIC model is %d\n", chc.pmic_model);

	if (chc.pmic_model == INTEL_PMIC_UNKNOWN)
		return -EINVAL;

	if (INTERNAL_PHY_SUPPORTED(chc.pmic_model)) {
		ret = pmic_read_reg(chc.reg_map->pmic_usbpath, &val);

		if (!ret && (val & USBPATH_USBSEL_MASK)) {
			dev_info(chc.dev, "SOC-Internal-USBPHY used\n");
			chc.is_internal_usb_phy = true;
			/* Enable internal detection */
			pmic_write_reg(chc.reg_map->pmic_usbphyctrl, 0x0);
		} else {
			dev_info(chc.dev, "External-USBPHY used\n");
		}
	}

	chgr_ctrl0 = intel_soc_pmic_readb(chc.reg_map->pmic_chgrctrl0);

	if (chgr_ctrl0 >= 0)
		chc.tt_lock = !!(chgr_ctrl0 & CHGRCTRL0_TTLCK_MASK);

	if (intel_soc_pmic_update(chc.reg_map->pmic_chgrctrl0,
			SWCONTROL_ENABLE|CHGRCTRL0_CCSM_OFF_MASK,
			CHGRCTRL0_SWCONTROL_MASK|CHGRCTRL0_CCSM_OFF_MASK))
		dev_err(chc.dev, "Error enabling sw control. Charging may continue in h/w control mode\n");

	chc.otg = usb_get_phy(USB_PHY_TYPE_USB2);
	if (!chc.otg || IS_ERR(chc.otg)) {
		dev_err(&pdev->dev, "Failed to get otg transceiver!!\n");
		ret = -EINVAL;
		goto otg_req_failed;
	}

	/* Disable VBUS if enable when booting. It will be enabled
	 * again if OTG ID event is detected later
	 */
	intel_pmic_enable_vbus(false);

	INIT_DELAYED_WORK(&chc.evt_work, pmic_event_worker);
	INIT_LIST_HEAD(&chc.evt_queue);

	INIT_WORK(&chc.extcon_work, pmic_ccsm_extcon_host_work);
	chc.cable_nb.notifier_call = pmic_ccsm_usb_host_nb;
	extcon_register_interest(&chc.host_cable, "usb-typec", "USB-Host",
						&chc.cable_nb);

	ret = pmic_check_initial_events();
	if (ret)
		goto otg_req_failed;

	/* register interrupt */
	for (i = 0; i < chc.irq_cnt; ++i) {
		ret = request_threaded_irq(chc.irq[i], pmic_isr,
				pmic_thread_handler,
				IRQF_ONESHOT|IRQF_NO_SUSPEND,
				DRIVER_NAME, &chc);
		if (ret) {
			dev_err(&pdev->dev, "Error in request_threaded_irq(irq(%d)!!\n",
				chc.irq[i]);
			while (i)
				free_irq(chc.irq[--i], &chc);
			goto otg_req_failed;
		}
	}

	ret = intel_soc_pmic_writeb(chc.reg_map->pmic_mthrmirq1,
					~MTHRMIRQ1_CCSM_MASK & 0xFF);
	if (ret)
		dev_warn(&pdev->dev, "Error writing to register: %x\n",
				chc.reg_map->pmic_mthrmirq1);

	ret = intel_soc_pmic_update(chc.reg_map->pmic_mchgrirq1,
				MPWRSRCIRQ_CCSM_VAL, MPWRSRCIRQ_CCSM_MASK);
	if (ret)
		dev_warn(&pdev->dev, "Error updating register: %x\n",
				chc.reg_map->pmic_mchgrirq1);

	chc.batt_health = POWER_SUPPLY_HEALTH_GOOD;
	return 0;

otg_req_failed:
	kfree(chc.bcprof);
	kfree(chc.actual_bcprof);
	kfree(chc.runtime_bcprof);
	return ret;
}

static void pmic_chrgr_do_exit_ops(struct pmic_chrgr_drv_context *chc)
{
	/* Empty */
}

/**
 * pmic_charger_remove - PMIC Charger driver remove
 * @pdev: PMIC charger platform device structure
 * Context: can sleep
 *
 * PMIC charger finalizes its internal data structure and other
 * infrastructure components that it initialized in
 * pmic_chrgr_probe.
 */
static int pmic_chrgr_remove(struct platform_device *pdev)
{
	int i, ret = 0;
	struct pmic_chrgr_drv_context *chc = platform_get_drvdata(pdev);

	if (chc) {
		if (IS_ERR_OR_NULL(chc->vbus_cdev))
			ret = PTR_ERR(chc->vbus_cdev);
		else
			thermal_cooling_device_unregister(chc->vbus_cdev);

		pmic_chrgr_do_exit_ops(chc);
		for (i = 0; i < chc->irq_cnt; ++i)
			free_irq(chc->irq[i], &chc);
		kfree(chc->bcprof);
		kfree(chc->actual_bcprof);
		kfree(chc->runtime_bcprof);
	}

	return ret;
}

/*********************************************************************
 *		Driver initialisation and finalization
 *********************************************************************/

static struct platform_device_id pmic_ccsm_device_ids[] = {
	{"bcove_ccsm", 0},
	{"scove_ccsm", 1},
	{"wcove_ccsm", 2},
	{},
};

static struct platform_driver intel_pmic_ccsm_driver = {
	.driver = {
		   .name = DRIVER_NAME,
		   .owner = THIS_MODULE,
		   .pm = &pmic_ccsm_pm,
		   },
	.probe = pmic_chrgr_probe,
	.remove = pmic_chrgr_remove,
	.id_table = pmic_ccsm_device_ids,
};


static int __init pmic_ccsm_init(void)
{
	return platform_driver_register(&intel_pmic_ccsm_driver);
}

static void __exit pmic_ccsm_exit(void)
{
	platform_driver_unregister(&intel_pmic_ccsm_driver);
}

late_initcall(pmic_ccsm_init);
module_exit(pmic_ccsm_exit);


MODULE_AUTHOR("Jenny TC <jenny.tc@intel.com>");
MODULE_DESCRIPTION("Intel PMIC CCSM Driver");
MODULE_LICENSE("GPL");
