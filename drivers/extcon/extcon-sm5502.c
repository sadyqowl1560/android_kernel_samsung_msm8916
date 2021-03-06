/*
 * driver/misc/sm5502.c - SM5502 micro USB switch device driver
 *
 * Copyright (C) 2013 Samsung Electronics
 * Jeongrae Kim <jryu.kim@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/extcon/extcon-sm5502.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/mfd/pmic8058.h>
#include <linux/input.h>
#include <linux/switch.h>
#include <linux/extcon.h>
#include <linux/sec_class.h>
#if defined(CONFIG_OF)
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#endif

#if defined(CONFIG_USB_HOST_NOTIFY)
#include <linux/host_notify.h>
#endif

#define INT_MASK1					0x5C
#define INT_MASK2					0x20

/* DEVICE ID */
#define SM5502_DEV_ID				0x0A
#define SM5502_DEV_ID_REV			0x12

/* SM5502 I2C registers */
#define REG_DEVICE_ID				0x01
#define REG_CONTROL					0x02
#define REG_INT1					0x03
#define REG_INT2					0x04
#define REG_INT_MASK1				0x05
#define REG_INT_MASK2				0x06
#define REG_ADC						0x07
#define REG_TIMING_SET1				0x08
#define REG_TIMING_SET2				0x09
#define REG_DEVICE_TYPE1			0x0a
#define REG_DEVICE_TYPE2			0x0b
#define REG_BUTTON1					0x0c
#define REG_BUTTON2					0x0d
#define REG_MANUAL_SW1				0x13
#define REG_MANUAL_SW2				0x14
#define REG_DEVICE_TYPE3			0x15
#define REG_RESET					0x1B
#define REG_TIMER_SET				0x20
#define REG_VBUSINVALID		        0x1D
#define REG_OCP_SET			        0x22
#define REG_CHGPUMP_SET			    0x3A
#define REG_CARKIT_STATUS			0x0E

#define DATA_NONE					0x00

/* Control */
#define CON_SWITCH_OPEN		(1 << 4)
#define CON_RAW_DATA		(1 << 3)
#define CON_MANUAL_SW		(1 << 2)
#define CON_WAIT			(1 << 1)
#define CON_INT_MASK		(1 << 0)
#define CON_MASK		(CON_SWITCH_OPEN | CON_RAW_DATA | \
				CON_MANUAL_SW | CON_WAIT)

/* Device Type 1 */
#define DEV_USB_OTG			(1 << 7)
#define DEV_DEDICATED_CHG	(1 << 6)
#define DEV_USB_CHG			(1 << 5)
#define DEV_CAR_KIT			(1 << 4)
#define DEV_UART			(1 << 3)
#define DEV_USB				(1 << 2)
#define DEV_AUDIO_2			(1 << 1)
#define DEV_AUDIO_1			(1 << 0)

#define DEV_T1_USB_MASK		(DEV_USB_OTG | DEV_USB_CHG | DEV_USB)
#define DEV_T1_UART_MASK	(DEV_UART)
#define DEV_T1_CHARGER_MASK	(DEV_DEDICATED_CHG | DEV_CAR_KIT)
#define DEV_CARKIT_CHARGER1_MASK	(1 << 1)
#define MANSW1_OPEN_RUSTPROOF	((0x0 << 5) | (0x3 << 2) | (1 << 0))

/* Device Type 2 */

#define DEV_LANHUB		(1 << 9)

#define DEV_AUDIO_DOCK		(1 << 8)
#define DEV_SMARTDOCK		(1 << 7)
#define DEV_AV			(1 << 6)
#define DEV_TTY			(1 << 5)
#define DEV_PPD			(1 << 4)
#define DEV_JIG_UART_OFF	(1 << 3)
#define DEV_JIG_UART_ON		(1 << 2)
#define DEV_JIG_USB_OFF		(1 << 1)
#define DEV_JIG_USB_ON		(1 << 0)

#define DEV_T2_USB_MASK		(DEV_JIG_USB_OFF | DEV_JIG_USB_ON)
#define DEV_T2_UART_MASK	(DEV_JIG_UART_OFF)
#define DEV_T2_JIG_MASK		(DEV_JIG_USB_OFF | DEV_JIG_USB_ON | \
				DEV_JIG_UART_OFF)
#define DEV_T2_JIG_ALL_MASK	(DEV_JIG_USB_OFF | DEV_JIG_USB_ON | \
				DEV_JIG_UART_OFF | DEV_JIG_UART_ON)

/* Device Type 3 */
#define DEV_MHL				(1 << 0)
#define DEV_VBUSIN_VALID	(1 << 1)
#define DEV_NON_STANDARD	(1 << 2)
#define DEV_AV_VBUS			(1 << 4)
#define DEV_U200_CHARGER	(1 << 6)

#define DEV_T3_CHARGER_MASK	(DEV_U200_CHARGER)

/*
 * Manual Switch
 * D- [7:5] / D+ [4:2]
 * 000: Open all / 001: USB / 010: AUDIO / 011: UART / 100: V_AUDIO
 */
#define SW_VAUDIO		((4 << 5) | (4 << 2) | (1 << 1) | (1 << 0))
#define SW_UART			((3 << 5) | (3 << 2))
#define SW_AUDIO		((2 << 5) | (2 << 2) | (1 << 0))
#define SW_DHOST		((1 << 5) | (1 << 2) | (1 << 0))
#define SW_AUTO			((0 << 5) | (0 << 2))
#define SW_USB_OPEN		(1 << 0)
#define SW_ALL_OPEN		(0)
#define SW_ALL_OPEN_WITH_VBUS	((0 << 5) | (0 << 2) | (1 << 0))

/* Interrupt 1 */
#define INT_OXP_DISABLE			(1 << 7)
#define INT_OCP_ENABLE			(1 << 6)
#define INT_OVP_ENABLE			(1 << 5)
#define INT_LONG_KEY_RELEASE	(1 << 4)
#define INT_LONG_KEY_PRESS		(1 << 3)
#define INT_KEY_PRESS			(1 << 2)
#define INT_DETACH				(1 << 1)
#define INT_ATTACH				(1 << 0)

/* Interrupt 2 */
#define INT_VBUSOUT_ON          (1 << 7)
#define INT_OTP_ENABLE			(1 << 6)
#define INT_CONNECT				(1 << 5)
#define INT_STUCK_KEY_RCV		(1 << 4)
#define INT_STUCK_KEY			(1 << 3)
#define INT_ADC_CHANGE			(1 << 2)
#define INT_RESERVED_ATTACH		(1 << 1)
#define INT_VBUSOUT_OFF			(1 << 0)
/* ADC VALUE */
#define	ADC_OTG				0x00
#define	ADC_MHL				0x01
#define ADC_SMART_DOCK			0x10
#define ADC_AUDIO_DOCK			0x12
#define	ADC_JIG_USB_OFF			0x18
#define	ADC_JIG_USB_ON			0x19
#define	ADC_DESKDOCK			0x1a
#define	ADC_JIG_UART_OFF		0x1c
#define	ADC_JIG_UART_ON			0x1d
#define	ADC_CARDOCK			0x1d
#define	ADC_OPEN			0x1f
#define ADC_LANHUB			0x13

int uart_sm5502_connecting;
EXPORT_SYMBOL(uart_sm5502_connecting);

struct sm5502_usbsw {
	struct i2c_client		*client;
	struct sm5502_platform_data	*pdata;
	struct extcon_dev		*edev;
	int				dev1;
	int				dev2;
	int				dev3;
	int				mansw;
	int				vbus;
	int				dev_id;
	int				carkit_dev;
	int				jig_state;
	struct delayed_work		init_work;
	struct mutex			mutex;
	int				adc;
	enum extcon_cable_name	cable_name;
#if defined(CONFIG_MUIC_SUPPORT_RUSTPROOF)
	bool				is_rustproof;
#endif
};

static struct sm5502_usbsw *local_usbsw;

static int sm5502_attach_dev(struct sm5502_usbsw *usbsw);
static int sm5502_detach_dev(struct sm5502_usbsw *usbsw);

static void sm5502_disable_interrupt(void)
{
	struct i2c_client *client = local_usbsw->client;
	int value, ret;

	value = i2c_smbus_read_byte_data(client, REG_CONTROL);
	value |= CON_INT_MASK;

	ret = i2c_smbus_write_byte_data(client, REG_CONTROL, value);
	if (ret < 0)
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);

}

static void sm5502_enable_interrupt(void)
{
	struct i2c_client *client = local_usbsw->client;
	int value, ret;

	value = i2c_smbus_read_byte_data(client, REG_CONTROL);
	value &= (~CON_INT_MASK);

	ret = i2c_smbus_write_byte_data(client, REG_CONTROL, value);
	if (ret < 0)
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);

}

static void sm5502_dock_control(struct sm5502_usbsw *usbsw,
	int dock_type, int state, int path)
{
	struct i2c_client *client = usbsw->client;
	int ret;

	if (state) {
		usbsw->mansw = path;
		ret = extcon_set_cable_state(usbsw->edev,
				extcon_cable_name[dock_type], state);
		ret = i2c_smbus_write_byte_data(client, REG_MANUAL_SW1, path);
		if (ret < 0)
			dev_err(&client->dev, "%s: err %d\n", __func__, ret);
		ret = i2c_smbus_read_byte_data(client, REG_CONTROL);
		if (ret < 0)
			dev_err(&client->dev, "%s: err %d\n", __func__, ret);
		else {
			ret = i2c_smbus_write_byte_data(client,
					REG_CONTROL, ret & ~CON_MANUAL_SW);
		}
		if (ret < 0)
			dev_err(&client->dev, "%s: err %x\n", __func__, ret);
	} else {
		ret = extcon_set_cable_state(usbsw->edev,
				extcon_cable_name[dock_type], state);
		ret = i2c_smbus_read_byte_data(client, REG_CONTROL);
		if (ret < 0)
			dev_err(&client->dev, "%s: err %d\n", __func__, ret);
		ret = i2c_smbus_write_byte_data(client, REG_CONTROL,
			ret | CON_MANUAL_SW | CON_RAW_DATA);
		if (ret < 0)
			dev_err(&client->dev, "%s: err %d\n", __func__, ret);
	}
}

static void sm5502_reg_init(struct sm5502_usbsw *usbsw)
{
	struct i2c_client *client = usbsw->client;
	unsigned int ctrl = CON_MASK;
	int ret;

	pr_info("sm5502_reg_init is called\n");

	usbsw->dev_id = i2c_smbus_read_byte_data(client, REG_DEVICE_ID);
	local_usbsw->dev_id = usbsw->dev_id;
	if (usbsw->dev_id < 0)
		dev_err(&client->dev, "%s: err %d\n", __func__, usbsw->dev_id);

	dev_info(&client->dev, " sm5502_reg_init dev ID: 0x%x\n",
			usbsw->dev_id);

	ret = i2c_smbus_write_byte_data(client, REG_INT_MASK1, INT_MASK1);
	if (ret < 0)
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);

	ret = i2c_smbus_write_byte_data(client,	REG_INT_MASK2, INT_MASK2);
	if (ret < 0)
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);

	ret = i2c_smbus_write_byte_data(client, REG_CONTROL, ctrl);
	if (ret < 0)
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);
    /*set timing1 to 300ms */
	ret = i2c_smbus_write_byte_data(client, REG_TIMING_SET1, 0x04);
	if (ret < 0)
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);

    /*Manual SW2 bit2 : JIG_ON '1' */
	ret = i2c_smbus_write_byte_data(client, REG_MANUAL_SW2, 0x04);
	if (ret < 0)
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);
}

static ssize_t sm5502_show_control(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct sm5502_usbsw *usbsw = dev_get_drvdata(dev);
	struct i2c_client *client = usbsw->client;
	int value;

	value = i2c_smbus_read_byte_data(client, REG_CONTROL);
	if (value < 0)
		dev_err(&client->dev, "%s: err %d\n", __func__, value);

	return snprintf(buf, 13, "CONTROL: %02x\n", value);
}

static ssize_t sm5502_show_device_type(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct sm5502_usbsw *usbsw = dev_get_drvdata(dev);
	struct i2c_client *client = usbsw->client;
	int value;

	value = i2c_smbus_read_byte_data(client, REG_DEVICE_TYPE1);
	if (value < 0)
		dev_err(&client->dev, "%s: err %d\n", __func__, value);

	return snprintf(buf, 11, "DEV_TYP %02x\n", value);
}

static ssize_t sm5502_show_manualsw(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct sm5502_usbsw *usbsw = dev_get_drvdata(dev);
	struct i2c_client *client = usbsw->client;
	int value;

	value = i2c_smbus_read_byte_data(client, REG_MANUAL_SW1);
	if (value < 0)
		dev_err(&client->dev, "%s: err %d\n", __func__, value);

	if (value == SW_VAUDIO)
		return snprintf(buf, 7, "VAUDIO\n");
	else if (value == SW_UART)
		return snprintf(buf, 5, "UART\n");
	else if (value == SW_AUDIO)
		return snprintf(buf, 6, "AUDIO\n");
	else if (value == SW_DHOST)
		return snprintf(buf, 6, "DHOST\n");
	else if (value == SW_AUTO)
		return snprintf(buf, 5, "AUTO\n");
	else
		return snprintf(buf, 4, "%x", value);
}

static ssize_t sm5502_set_manualsw(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct sm5502_usbsw *usbsw = dev_get_drvdata(dev);
	struct i2c_client *client = usbsw->client;
	int value, ret;
	unsigned int path = 0;

	value = i2c_smbus_read_byte_data(client, REG_CONTROL);
	if (value < 0)
		dev_err(&client->dev, "%s: err %d\n", __func__, value);

	if ((value & ~CON_MANUAL_SW) !=
			(CON_SWITCH_OPEN | CON_RAW_DATA | CON_WAIT))
		return 0;

	if (!strncmp(buf, "VAUDIO", 6)) {
		path = SW_VAUDIO;
		value &= ~CON_MANUAL_SW;
	} else if (!strncmp(buf, "UART", 4)) {
		path = SW_UART;
		value &= ~CON_MANUAL_SW;
	} else if (!strncmp(buf, "AUDIO", 5)) {
		path = SW_AUDIO;
		value &= ~CON_MANUAL_SW;
	} else if (!strncmp(buf, "DHOST", 5)) {
		path = SW_DHOST;
		value &= ~CON_MANUAL_SW;
	} else if (!strncmp(buf, "AUTO", 4)) {
		path = SW_AUTO;
		value |= CON_MANUAL_SW;
	} else {
		dev_err(dev, "Wrong command\n");
		return 0;
	}

	usbsw->mansw = path;

	ret = i2c_smbus_write_byte_data(client, REG_MANUAL_SW1, path);
	if (ret < 0)
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);

	ret = i2c_smbus_write_byte_data(client, REG_CONTROL, value);
	if (ret < 0)
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);

	return count;
}

static ssize_t sm5502_show_usb_state(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct sm5502_usbsw *usbsw = dev_get_drvdata(dev);
	struct i2c_client *client = usbsw->client;
	int device_type1, device_type2;

	device_type1 = i2c_smbus_read_byte_data(client, REG_DEVICE_TYPE1);
	if (device_type1 < 0) {
		dev_err(&client->dev, "%s: err %d ", __func__, device_type1);
		return (ssize_t)device_type1;
	}
	device_type2 = i2c_smbus_read_byte_data(client, REG_DEVICE_TYPE2);
	if (device_type2 < 0) {
		dev_err(&client->dev, "%s: err %d ", __func__, device_type2);
		return (ssize_t)device_type2;
	}

	if (device_type1 & DEV_T1_USB_MASK || device_type2 & DEV_T2_USB_MASK)
		return snprintf(buf, 22, "USB_STATE_CONFIGURED\n");

	return snprintf(buf, 25, "USB_STATE_NOTCONFIGURED\n");
}

static ssize_t sm5502_show_adc(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct sm5502_usbsw *usbsw = dev_get_drvdata(dev);
	struct i2c_client *client = usbsw->client;
	int adc;

	adc = i2c_smbus_read_byte_data(client, REG_ADC);
	if (adc < 0) {
		dev_err(&client->dev,
			"%s: err at read adc %d\n", __func__, adc);
		return snprintf(buf, 9, "UNKNOWN\n");
	}

	return snprintf(buf, 4, "%x\n", adc);
}

static ssize_t sm5502_reset(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct sm5502_usbsw *usbsw = dev_get_drvdata(dev);
	struct i2c_client *client = usbsw->client;
	if (!strncmp(buf, "1", 1)) {
		dev_info(&client->dev,
			"sm5502 reset after delay 1000 msec.\n");
		msleep(1000);
		i2c_smbus_write_byte_data(client, REG_RESET, 0x01);

	dev_info(&client->dev, "sm5502_reset_control done!\n");
	} else {
		dev_info(&client->dev,
			"sm5502_reset_control, but not reset_value!\n");
	}

#ifdef CONFIG_MUIC_SUPPORT_RUSTPROOF
	usbsw->is_rustproof = false;
#endif

	sm5502_reg_init(usbsw);

	return count;
}

#ifdef CONFIG_MUIC_SUPPORT_RUSTPROOF
static void muic_rustproof_feature(struct i2c_client *client, int state);
/* Keystring "*#0*#" sysfs implementation */
static ssize_t uart_en_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct sm5502_usbsw *usbsw = dev_get_drvdata(dev);
	/*is_rustproof is false then UART can be enabled*/
	return snprintf(buf, 4, "%d\n", !(usbsw->is_rustproof));
}

static ssize_t uart_en_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t size)
{
	struct sm5502_usbsw *usbsw = dev_get_drvdata(dev);
	struct i2c_client *client = usbsw->client;
	if (!strncmp(buf, "1", 1)) {
		dev_info(&client->dev,
			"[MUIC]Runtime enabling the UART.\n");
		usbsw->is_rustproof = false;
		muic_rustproof_feature(client, detach);

	} else {
		dev_info(&client->dev,
			"[MUIC]Runtime disabling the UART.\n");
		usbsw->is_rustproof = true;
	}
	/* reinvoke the attach detection function to set proper paths */
	sm5502_attach_dev(usbsw);

	return size;
}

static DEVICE_ATTR(uart_en, S_IRUGO | S_IWUSR ,
				uart_en_show, uart_en_store);

static ssize_t uart_sel_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	/*for sm5502 paths are always switch to AP*/
	return snprintf(buf, 3, "AP\n");
;
}

static ssize_t uart_sel_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t size)
{
	struct sm5502_usbsw *usbsw = dev_get_drvdata(dev);
	struct i2c_client *client = usbsw->client;
	dev_info(&client->dev, "[MUIC]Enabling AP UART Path, dummy Call\n");
	return size;
}

static DEVICE_ATTR(uart_sel, S_IRUGO | S_IWUSR ,
				uart_sel_show, uart_sel_store);

static ssize_t usbsel_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, 4, "PDA\n");
}

static ssize_t usbsel_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t size)
{
	struct sm5502_usbsw *usbsw = dev_get_drvdata(dev);
	struct i2c_client *client = usbsw->client;
	dev_info(&client->dev, "[MUIC]Enabling AP UART Path, dummy Call\n");
	return size;
}

static DEVICE_ATTR(usb_sel, S_IRUGO | S_IWUSR ,
				usbsel_show, usbsel_store);
#endif

static DEVICE_ATTR(control, S_IRUGO, sm5502_show_control, NULL);
static DEVICE_ATTR(device_type, S_IRUGO, sm5502_show_device_type, NULL);
static DEVICE_ATTR(switch, S_IRUGO | S_IWUSR,
		sm5502_show_manualsw, sm5502_set_manualsw);
static DEVICE_ATTR(usb_state, S_IRUGO, sm5502_show_usb_state, NULL);
static DEVICE_ATTR(adc, S_IRUGO, sm5502_show_adc, NULL);
static DEVICE_ATTR(reset_switch, S_IWUSR | S_IWGRP, NULL, sm5502_reset);

static struct attribute *sm5502_attributes[] = {
	&dev_attr_control.attr,
	&dev_attr_device_type.attr,
	&dev_attr_switch.attr,
	&dev_attr_usb_state.attr,
	&dev_attr_adc.attr,
	&dev_attr_reset_switch.attr,
	NULL
};

static const struct attribute_group sm5502_group = {
	.attrs = sm5502_attributes,
};

#if defined(CONFIG_USB_HOST_NOTIFY)
static void sm5502_set_otg(struct sm5502_usbsw *usbsw, int state)
{
	int ret;
	struct i2c_client *client = usbsw->client;

	if (state == attach) {
		ret = i2c_smbus_write_byte_data(client, REG_MANUAL_SW1, 0x25);
		if (ret < 0)
			dev_info(&client->dev, "%s: err %d\n", __func__, ret);
		/*	Disconnecting the MUIC_ID & ITBP Pins	*/
		ret = i2c_smbus_write_byte_data(client, REG_MANUAL_SW2, 0x00);
		if (ret < 0)
			dev_info(&client->dev, "%s: err %d\n", __func__, ret);
		ret = i2c_smbus_read_byte_data(client, REG_CONTROL);
		if (ret < 0)
			dev_info(&client->dev, "%s: err %d\n", __func__, ret);
		ret = ret & 0xFB; /*Manual Connection S/W enable*/
		ret = i2c_smbus_write_byte_data(client, REG_CONTROL, ret);
		if (ret < 0)
			dev_info(&client->dev, "%s: err %d\n", __func__, ret);
	} else {
		ret = i2c_smbus_write_byte_data(client, REG_MANUAL_SW2, 0x00);
		if (ret < 0)
			dev_info(&client->dev, "%s: err %d\n", __func__, ret);
		ret = i2c_smbus_write_byte_data(client, REG_MANUAL_SW1,
				SW_ALL_OPEN);
		if (ret < 0)
			dev_info(&client->dev, "%s: err %d\n", __func__, ret);
		ret = i2c_smbus_read_byte_data(client, REG_CONTROL);
		if (ret < 0)
			dev_info(&client->dev, "%s: err %d\n", __func__, ret);
		ret = ret | 0x04; /*Manual Connection S/W Disable*/
		ret = i2c_smbus_write_byte_data(client, REG_CONTROL, ret);
		if (ret < 0)
			dev_info(&client->dev, "%s: err %d\n", __func__, ret);
	}
}
#endif

#ifndef CONFIG_USB_HOST_NOTIFY
enum sec_otg_dummy_defines {
	HNOTIFY_MODE = 1,
	NOTIFY_TEST_MODE = 3,
};
#endif

int check_sm5502_jig_state(void)
{
	return local_usbsw->jig_state;
}
EXPORT_SYMBOL(check_sm5502_jig_state);

#if defined(CONFIG_MUIC_SUPPORT_RUSTPROOF)
static void muic_rustproof_feature(struct i2c_client *client, int state)
{
	int val;
	if (state) {
		val = i2c_smbus_write_byte_data(client, REG_MANUAL_SW1,
							SW_ALL_OPEN_WITH_VBUS);
		if (val < 0)
			dev_info(&client->dev, "%s:MANUAL SW1,err %d\n",
				__func__, val);
		val = i2c_smbus_read_byte_data(client, REG_CONTROL);
		if (val < 0)
			dev_info(&client->dev, "%s:CTRL REG,err %d\n",
				__func__, val);
		val &= 0xFB;
		val = i2c_smbus_write_byte_data(client, REG_CONTROL, val);
		if (val < 0)
			dev_info(&client->dev, "%s:CTRL REG,err %d\n",
				__func__, val);
	} else {
		val = i2c_smbus_write_byte_data(client, REG_MANUAL_SW2, 0x00);
		if (val < 0)
			dev_info(&client->dev, "%s: MANUAL SW2,err %d\n",
				__func__, val);
		val = i2c_smbus_write_byte_data(client, REG_MANUAL_SW1,
			SW_ALL_OPEN);
		if (val < 0)
			dev_info(&client->dev, "%s: MANUAL SW1,err %d\n",
				__func__, val);
		val = i2c_smbus_read_byte_data(client, REG_CONTROL);
		if (val < 0)
			dev_info(&client->dev, "%s: CTRL REG,err %d\n",
				__func__, val);
		val = val | 0x04; /*Automatic Connection S/W enable*/
		val = i2c_smbus_write_byte_data(client, REG_CONTROL, val);
		if (val < 0)
			dev_info(&client->dev, "%s: CTRL REG,err %d\n",
				__func__, val);

	}
}
#endif

static int sm5502_attach_dev(struct sm5502_usbsw *usbsw)
{
	int adc;
	int val1, val2, val3, val4, vbus;
	struct i2c_client *client = usbsw->client;
#if defined(CONFIG_VIDEO_MHL_V2)
	u8 mhl_ret = 0;
#endif
	val1 = i2c_smbus_read_byte_data(client, REG_DEVICE_TYPE1);
	if (val1 < 0) {
		dev_err(&client->dev, "%s: err %d\n", __func__, val1);
		return val1;
	}

	val2 = i2c_smbus_read_byte_data(client, REG_DEVICE_TYPE2);
	if (val2 < 0) {
		dev_err(&client->dev, "%s: err %d\n", __func__, val2);
		return val2;
	}
	usbsw->jig_state =  (val2 & DEV_T2_JIG_ALL_MASK) ? 1 : 0;

	val3 = i2c_smbus_read_byte_data(client, REG_DEVICE_TYPE3);
	if (val3 < 0) {
		dev_err(&client->dev, "%s: err %d\n", __func__, val3);
		return val3;
	}

	val4 = i2c_smbus_read_byte_data(client, REG_CARKIT_STATUS);
	if (val4 < 0) {
		dev_err(&client->dev, "%s: err %d\n", __func__, val4);
		return val4;
	}

	vbus = i2c_smbus_read_byte_data(client, REG_VBUSINVALID);
	if (vbus < 0) {
		dev_err(&client->dev, "%s: err %d\n", __func__, vbus);
		return vbus;
	}
	adc = i2c_smbus_read_byte_data(client, REG_ADC);

#if defined(CONFIG_USB_HOST_NOTIFY)
	if (adc == ADC_AUDIO_DOCK) {
		val2 = DEV_AUDIO_DOCK;
		val1 = 0;
	}
#endif
	dev_err(&client->dev,
			"dev1: 0x%x,dev2: 0x%x,dev3: 0x%x,Carkit: 0x%x,ADC: 0x%x,Jig: %s\n",
			val1, val2, val3, val4, adc,
			(check_sm5502_jig_state() ? "ON" : "OFF"));

	/* USB */
	if (val1 & DEV_USB || val2 & DEV_T2_USB_MASK ||
			val4 & DEV_CARKIT_CHARGER1_MASK) {
		pr_info("[MUIC] USB Connected\n");
		extcon_set_cable_state(usbsw->edev,
			extcon_cable_name[EXTCON_USB], attach);
	/* D+,D-open */
	} else if (val3 & DEV_NON_STANDARD) {
		pr_info("[MUIC] D+,D-open Connected\n");
		extcon_set_cable_state(usbsw->edev,
			extcon_cable_name[EXTCON_USB], attach);
	/* USB_CDP */
	} else if (val1 & DEV_USB_CHG) {
		pr_info("[MUIC] CDP Connected\n");
		extcon_set_cable_state(usbsw->edev,
			extcon_cable_name[EXTCON_CHARGE_DOWNSTREAM],
			attach);
	/* UART */
	} else if (val1 & DEV_T1_UART_MASK || val2 & DEV_T2_UART_MASK) {
		uart_sm5502_connecting = 1;
#if defined(CONFIG_MUIC_SUPPORT_RUSTPROOF)
		if (usbsw->is_rustproof) {
			pr_info("[MUIC] RustProof mode, close UART Path\n");
			muic_rustproof_feature(client, attach);
		} else
#endif
		{
			pr_info("[MUIC] UART OFF Connected\n");
			i2c_smbus_write_byte_data(client, REG_MANUAL_SW1,
				SW_UART);
			if (vbus & DEV_VBUSIN_VALID)
				extcon_set_cable_state(usbsw->edev,
					extcon_cable_name[EXTCON_JIG_UARTOFF_VB],
					attach);
			else
				extcon_set_cable_state(usbsw->edev,
					extcon_cable_name[EXTCON_JIG_UARTOFF],
					attach);
		}
	/* CHARGER */
	} else if ((val1 & DEV_T1_CHARGER_MASK) ||
			(val3 & DEV_T3_CHARGER_MASK)) {
		pr_info("[MUIC] Charger Connected\n");
		extcon_set_cable_state(usbsw->edev,
			extcon_cable_name[EXTCON_TA], attach);
#if defined(CONFIG_USB_HOST_NOTIFY)
	/* for SAMSUNG OTG */
	} else if (val1 & DEV_USB_OTG && adc == ADC_OTG) {
		pr_info("[MUIC] OTG Connected\n");

		sm5502_set_otg(usbsw, attach);
		extcon_set_cable_state(usbsw->edev,
				extcon_cable_name[EXTCON_USB_HOST], attach);
#endif
	/* Desk Dock */
	} else if ((val2 & DEV_AV) || (val3 & DEV_AV_VBUS)) {
		pr_info("[MUIC] Deskdock Connected\n");
		if (vbus & DEV_VBUSIN_VALID)
			sm5502_dock_control(usbsw, EXTCON_DESKDOCK_VB,
				attach, SW_AUDIO);
		else
			sm5502_dock_control(usbsw, EXTCON_DESKDOCK,
				attach, SW_AUDIO);
#if defined(CONFIG_VIDEO_MHL_V2)
	/* MHL */
	} else if (val3 & DEV_MHL) {
		pr_info("[MUIC] MHL Connected\n");
		sm5502_disable_interrupt();
		if (!poweroff_charging)
			mhl_ret = mhl_onoff_ex(1);
		else
			pr_info("LPM mode, skip MHL sequence\n");
		sm5502_enable_interrupt();
#endif
	/* Car Dock */
	} else if (val2 & DEV_JIG_UART_ON) {
#if defined(CONFIG_SEC_FACTORY)
		pr_info("[MUIC] Cardock Connected\n");
		sm5502_dock_control(usbsw, EXTCON_CARDOCK,
			attach, SW_UART);
#elif defined(CONFIG_MUIC_SUPPORT_RUSTPROOF)
		if (usbsw->is_rustproof) {
			pr_info("[MUIC] RustProof mode, close UART Path\n");
			muic_rustproof_feature(client, attach);
		}
#else
		pr_info("[MUIC] UART ON Connected\n");
		extcon_set_cable_state(usbsw->edev,
			extcon_cable_name[EXTCON_JIG_UARTON],
			attach);
#endif
#if defined(CONFIG_USB_HOST_NOTIFY)
	/* Audio Dock */
	} else if (val2 & DEV_AUDIO_DOCK) {
		pr_info("[MUIC] Audiodock Connected\n");
		sm5502_dock_control(usbsw, EXTCON_AUDIODOCK,
			attach, SW_DHOST);
#endif
	/* Incompatible */
	} else if (vbus & DEV_VBUSIN_VALID) {
		pr_info("[MUIC] Incompatible Charger Connected\n");
		extcon_set_cable_state(usbsw->edev,
				extcon_cable_name[EXTCON_INCOMPATIBLE], attach);
	}

	usbsw->dev1 = val1;
	usbsw->dev2 = val2;
	usbsw->dev3 = val3;
	usbsw->adc = adc;
	usbsw->vbus = vbus;
	usbsw->carkit_dev = val4;

	return adc;
}

static int sm5502_detach_dev(struct sm5502_usbsw *usbsw)
{
	/* USB */
	if (usbsw->dev1 & DEV_USB ||
			usbsw->dev2 & DEV_T2_USB_MASK ||
				usbsw->carkit_dev & DEV_CARKIT_CHARGER1_MASK) {
		pr_info("[MUIC] USB Disonnected\n");
		extcon_set_cable_state(usbsw->edev,
				extcon_cable_name[EXTCON_USB], detach);
	} else if (usbsw->dev1 & DEV_USB_CHG) {
		extcon_set_cable_state(usbsw->edev,
				extcon_cable_name[EXTCON_CHARGE_DOWNSTREAM],
				detach);
	/* D+,D-open */
	} else if (usbsw->dev3 & DEV_NON_STANDARD) {
		pr_info("[MUIC] D+,D-open Disonnected\n");
		extcon_set_cable_state(usbsw->edev,
				extcon_cable_name[EXTCON_USB], detach);
	/* UART */
	} else if (usbsw->dev1 & DEV_T1_UART_MASK ||
			usbsw->dev2 & DEV_T2_UART_MASK) {
#if defined(CONFIG_MUIC_SUPPORT_RUSTPROOF)
		if (usbsw->is_rustproof) {
			pr_info("[MUIC] RustProof mode Disconnected Event\n");
			muic_rustproof_feature(usbsw->client, detach);
		} else
#endif
		{
			pr_info("[MUIC] UART OFF Disonnected\n");
			if (usbsw->vbus & DEV_VBUSIN_VALID)
				extcon_set_cable_state(usbsw->edev,
					extcon_cable_name[EXTCON_JIG_UARTOFF_VB],
					detach);
			else
				extcon_set_cable_state(usbsw->edev,
					extcon_cable_name[EXTCON_JIG_UARTOFF],
					detach);
				uart_sm5502_connecting = 0;
		}
	/* CHARGER */
	} else if ((usbsw->dev1 & DEV_T1_CHARGER_MASK) ||
			(usbsw->dev3 & DEV_T3_CHARGER_MASK)) {
		pr_info("[MUIC] Charger Disonnected\n");
		extcon_set_cable_state(usbsw->edev,
				extcon_cable_name[EXTCON_TA], detach);
#if defined(CONFIG_USB_HOST_NOTIFY)
	/* for SAMSUNG OTG */
	} else if (usbsw->dev1 & DEV_USB_OTG) {
		pr_info("[MUIC] OTG Disonnected\n");
		sm5502_set_otg(usbsw, detach);
		extcon_set_cable_state(usbsw->edev,
				extcon_cable_name[EXTCON_USB_HOST], detach);
#endif
	/* Desk Dock */
	} else if ((usbsw->dev2 & DEV_AV) ||
			(usbsw->dev3 & DEV_AV_VBUS)) {
		pr_info("[MUIC] Deskdock Disonnected\n");
		if (usbsw->vbus & DEV_VBUSIN_VALID)
			sm5502_dock_control(usbsw, EXTCON_DESKDOCK_VB,
				detach, SW_ALL_OPEN);
		else
			sm5502_dock_control(usbsw, EXTCON_DESKDOCK,
				detach, SW_ALL_OPEN);
#if defined(CONFIG_MHL_D3_SUPPORT)
	/* MHL */
	} else if (usbsw->dev3 & DEV_MHL) {
		pr_info("[MUIC] MHL Disonnected\n");
		mhl_onoff_ex(false);
		extcon_set_cable_state(usbsw->edev,
				extcon_cable_name[EXTCON_MHL], detach);
#endif
	/* Car Dock */
	} else if (usbsw->dev2 & DEV_JIG_UART_ON) {
#if defined(CONFIG_SEC_FACTORY)
		pr_info("[MUIC] Cardock Disonnected\n");
		sm5502_dock_control(usbsw, EXTCON_CARDOCK,
			detach, SW_ALL_OPEN);
#elif defined(CONFIG_MUIC_SUPPORT_RUSTPROOF)
		if (usbsw->is_rustproof) {
			pr_info("[MUIC] RustProof mode disconneted Event\n");
			muic_rustproof_feature(usbsw->client, detach);
		}
#else
		pr_info("[MUIC] UART ON Disonnected\n");
		extcon_set_cable_state(usbsw->edev,
			extcon_cable_name[EXTCON_JIG_UARTON],
			detach);
#endif
#if defined(CONFIG_USB_HOST_NOTIFY)
	/* Audio Dock */
	} else if (usbsw->dev2 & DEV_AUDIO_DOCK) {
		pr_info("[MUIC] Audiodock Disonnected\n");
		sm5502_dock_control(usbsw, EXTCON_AUDIODOCK,
			detach, SW_ALL_OPEN);
#endif
	/* Incompatible */
	} else if (usbsw->vbus & DEV_VBUSIN_VALID) {
		pr_info("[MUIC] Incompatible Charger Disonnected\n");
		extcon_set_cable_state(usbsw->edev,
				extcon_cable_name[EXTCON_INCOMPATIBLE], detach);
	}

	i2c_smbus_write_byte_data(usbsw->client, REG_CONTROL, CON_MASK);

	usbsw->dev1 = 0;
	usbsw->dev2 = 0;
	usbsw->dev3 = 0;
	usbsw->adc = 0;
	usbsw->vbus = 0;
	usbsw->carkit_dev = 0;
	usbsw->jig_state = 0;

	return 0;
}

static irqreturn_t sm5502_irq_thread(int irq, void *data)
{
	struct sm5502_usbsw *usbsw = data;
	struct i2c_client *client = usbsw->client;
	int intr1, intr2;
	int val1, val3, adc , vbus;
	/* SM5502 : Read interrupt -> Read Device */
	pr_info("sm5502_irq_thread is called\n");

	mutex_lock(&usbsw->mutex);
	sm5502_disable_interrupt();
	intr1 = i2c_smbus_read_byte_data(client, REG_INT1);
	intr2 = i2c_smbus_read_byte_data(client, REG_INT2);
	sm5502_enable_interrupt();

	adc = i2c_smbus_read_byte_data(client, REG_ADC);
	dev_info(&client->dev, "%s: intr1 : 0x%x,intr2 : 0x%x, adc : 0x%x\n",
					__func__, intr1, intr2, adc);

	/* device detection */
	/* interrupt both attach and detach */
	if (intr1 == (INT_ATTACH + INT_DETACH)) {
		val1 = i2c_smbus_read_byte_data(client, REG_DEVICE_TYPE1);
		val3 = i2c_smbus_read_byte_data(client, REG_DEVICE_TYPE3);
		vbus = i2c_smbus_read_byte_data(client, REG_VBUSINVALID);

		if ((adc == ADC_OPEN) && (val1 == DATA_NONE) &&
				((val3 == DATA_NONE) || (vbus == 0x00)))
			sm5502_detach_dev(usbsw);
		else
			sm5502_attach_dev(usbsw);
	}
	/* interrupt attach */
	else if ((intr1 & INT_ATTACH) || (intr2 & INT_RESERVED_ATTACH)) {
		sm5502_attach_dev(usbsw);
	/* interrupt detach */
	} else if (intr1 & INT_DETACH) {
		sm5502_detach_dev(usbsw);
	} else if ((intr2 == INT_VBUSOUT_ON)) {
		pr_info("sm5502: VBUSOUT_ON\n");
#if defined(CONFIG_USB_HOST_NOTIFY)
		sec_otg_notify(HNOTIFY_OTG_POWER_ON);
#endif
		if (adc == ADC_JIG_UART_OFF)	/*JIG UART OFF VBUS Change*/
			sm5502_attach_dev(usbsw);
		else if (adc == ADC_DESKDOCK)	/*DESKDOCK VBUS Change*/
			extcon_set_cable_state(usbsw->edev,
				extcon_cable_name[EXTCON_TA], attach);
	} else if (intr2 == INT_VBUSOUT_OFF) {
		pr_info("sm5502: VBUSOUT_OFF\n");
#if defined(CONFIG_USB_HOST_NOTIFY)
		sec_otg_notify(HNOTIFY_OTG_POWER_OFF);
#endif
		if (adc == ADC_JIG_UART_OFF)	/*JIG UART OFF VBUS Change*/
			sm5502_detach_dev(usbsw);
		else if (adc == ADC_DESKDOCK)	/*DESKDOCK VBUS Change*/
			extcon_set_cable_state(usbsw->edev,
				extcon_cable_name[EXTCON_TA], detach);
	}

	mutex_unlock(&usbsw->mutex);
	pr_info("sm5502_irq_thread,end\n");
	return IRQ_HANDLED;
}

static int sm5502_irq_init(struct sm5502_usbsw *usbsw)
{
	struct i2c_client *client = usbsw->client;
	int ret;

	if (client->irq) {
		ret = request_threaded_irq(client->irq, NULL,
			sm5502_irq_thread, IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
			"sm5502 micro USB", usbsw);
		if (ret) {
			dev_err(&client->dev, "failed to reqeust IRQ\n");
			return ret;
		}
		enable_irq_wake(client->irq);
	}

	return 0;
}

static void sm5502_init_detect(struct work_struct *work)
{
	struct sm5502_usbsw *usbsw = container_of(work,
			struct sm5502_usbsw, init_work.work);
	int ret;
	int int_reg1, int_reg2;

	dev_info(&usbsw->client->dev, "%s\n", __func__);

	mutex_lock(&usbsw->mutex);
	sm5502_attach_dev(usbsw);
	mutex_unlock(&usbsw->mutex);

	ret = sm5502_irq_init(usbsw);
	if (ret)
		dev_info(&usbsw->client->dev,
				"failed to enable  irq init %s\n", __func__);

	int_reg1 = i2c_smbus_read_byte_data(usbsw->client, REG_INT1);
	dev_info(&usbsw->client->dev, "%s: intr1 : 0x%x\n",
		__func__, int_reg1);

	int_reg2 = i2c_smbus_read_byte_data(usbsw->client, REG_INT2);
	dev_info(&usbsw->client->dev, "%s: intr2 : 0x%x\n",
		__func__, int_reg2);
}

#if defined(CONFIG_OF)
static int sm5502_parse_dt(struct device *dev,
		struct sm5502_platform_data *pdata)
{
	struct device_node *np = dev->of_node;

	/* reset, irq gpio info */
	pdata->gpio_scl = of_get_named_gpio_flags(np, "sm5502,gpio-scl",
			0, &pdata->scl_gpio_flags);
	pdata->gpio_uart_on = of_get_named_gpio_flags(np, "sm5502,uarton-gpio",
			0, &pdata->uarton_gpio_flags);
	pdata->gpio_sda = of_get_named_gpio_flags(np, "sm5502,gpio-sda",
			0, &pdata->sda_gpio_flags);
	pdata->gpio_int = of_get_named_gpio_flags(np, "sm5502,irq-gpio",
			0, &pdata->irq_gpio_flags);
	pr_info("%s: irq-gpio: %u\n", __func__, pdata->gpio_int);

	return 0;
}
#endif

static int sm5502_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	struct sm5502_usbsw *usbsw;
	int ret = 0;
	struct sm5502_platform_data *pdata;

	dev_info(&client->dev, "%s:sm5502 probe called\n", __func__);
	if (client->dev.of_node) {
		pdata = devm_kzalloc(&client->dev,
			sizeof(struct sm5502_platform_data), GFP_KERNEL);
		if (!pdata) {
			dev_err(&client->dev, "Failed to allocate memory\n");
				return -ENOMEM;
		}
		ret = sm5502_parse_dt(&client->dev, pdata);
		if (ret < 0)
			return ret;
	} else
		pdata = client->dev.platform_data;

	if (!pdata)
		return -EINVAL;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -EIO;

	usbsw = kzalloc(sizeof(struct sm5502_usbsw), GFP_KERNEL);
	if (!usbsw) {
		dev_err(&client->dev, "failed to allocate driver data\n");
		kfree(usbsw);
		return -ENOMEM;
	}

	usbsw->client = client;
	if (client->dev.of_node)
		usbsw->pdata = pdata;
	else
		usbsw->pdata = client->dev.platform_data;
	if (!usbsw->pdata)
		goto fail1;

	usbsw->edev = devm_kzalloc(&client->dev, sizeof(struct extcon_dev),
			GFP_KERNEL);
	if (!usbsw->edev) {
		dev_err(&client->dev, "failed to allocate memory for extcon\n");
		ret = -ENOMEM;
		goto fail2;
	}
	usbsw->edev->name = EXTCON_DEV_NAME;
	usbsw->edev->supported_cable = extcon_cable_name;

	ret = extcon_dev_register(usbsw->edev, NULL);
	if (ret) {
		dev_err(&client->dev, "failed to register extcon device\n");
		goto fail2;
	}

	i2c_set_clientdata(client, usbsw);

	mutex_init(&usbsw->mutex);

	local_usbsw = usbsw;

#if defined(CONFIG_MUIC_SUPPORT_RUSTPROOF)
	ret = i2c_smbus_read_byte_data(client, REG_MANUAL_SW1);
	if (ret < 0)
		dev_err(&client->dev,
			"failed to read MANUAL SW1 Reg, err:%d\n", ret);

	/* Keep the feature disabled by default */
	usbsw->is_rustproof = false;
	/* RUSTPROOF:
	 * disable UART connection if MANSW1 from BL is OPEN_RUSTPROOF*/
	if (ret == MANSW1_OPEN_RUSTPROOF)
		usbsw->is_rustproof = true;
#endif

	sm5502_reg_init(usbsw);

	ret = sysfs_create_group(&switch_dev->kobj, &sm5502_group);
	if (ret) {
		dev_err(&client->dev,
				"failed to create sm5502 attribute group\n");
		goto fail2;
	}

#if defined(CONFIG_MUIC_SUPPORT_RUSTPROOF)
	ret = device_create_file(switch_dev, &dev_attr_uart_en);
	if (ret < 0) {
		pr_err("[SM5502] Failed to create file (uart_en)!\n");
		goto err_create_file_uart_en;
	}
	ret = device_create_file(switch_dev, &dev_attr_uart_sel);
	if (ret < 0) {
		pr_err("[SM5502] Failed to create file (uart_sel)!\n");
		goto err_create_file_uart_sel;
	}
	ret = device_create_file(switch_dev, &dev_attr_usb_sel);
	if (ret < 0) {
		pr_err("[SM5502] Failed to create file (usb_sel)!\n");
		goto err_create_file_usb_sel;
	}

#endif
	dev_set_drvdata(switch_dev, usbsw);
	/* initial cable detection */
	INIT_DELAYED_WORK(&usbsw->init_work, sm5502_init_detect);
	schedule_delayed_work(&usbsw->init_work, msecs_to_jiffies(2700));

	return 0;
#if defined(CONFIG_MUIC_SUPPORT_RUSTPROOF)
err_create_file_usb_sel:
	device_remove_file(switch_dev, &dev_attr_usb_sel);
err_create_file_uart_sel:
	device_remove_file(switch_dev, &dev_attr_uart_sel);
err_create_file_uart_en:
	device_remove_file(switch_dev, &dev_attr_uart_en);
#endif
fail2:
	if (client->irq)
		free_irq(client->irq, usbsw);
fail1:
	mutex_destroy(&usbsw->mutex);
	i2c_set_clientdata(client, NULL);
	kfree(usbsw);
	return ret;
}

static int sm5502_remove(struct i2c_client *client)
{
	struct sm5502_usbsw *usbsw = i2c_get_clientdata(client);
	cancel_delayed_work(&usbsw->init_work);
	if (client->irq) {
		disable_irq_wake(client->irq);
		free_irq(client->irq, usbsw);
	}
	mutex_destroy(&usbsw->mutex);
	i2c_set_clientdata(client, NULL);

	sysfs_remove_group(&client->dev.kobj, &sm5502_group);
	kfree(usbsw);
	return 0;
}

static int sm5502_resume(struct i2c_client *client)
{
	struct sm5502_usbsw *usbsw = i2c_get_clientdata(client);
	int ldev1, ldev2, ldev3;

	pr_info("%s: resume\n", __func__);

	ldev1 = i2c_smbus_read_byte_data(client, REG_DEVICE_TYPE1);
	if (ldev1 < 0) {
		pr_err("%s: Dev reg 1 read err! %d\n", __func__, ldev1);
		goto safe_exit;
	}
	ldev2 = i2c_smbus_read_byte_data(client, REG_DEVICE_TYPE2);
	if (ldev2 < 0) {
		pr_err("%s: Dev reg 2 read err! %d\n", __func__, ldev2);
		goto safe_exit;
	}
	ldev3 = i2c_smbus_read_byte_data(client, REG_DEVICE_TYPE3);
	if (ldev3 < 0) {
		pr_err("%s: Dev reg 3 read err! %d\n", __func__, ldev3);
		goto safe_exit;
	}

	i2c_smbus_read_byte_data(client, REG_INT1);
	i2c_smbus_read_byte_data(client, REG_INT2);

	if ((usbsw->dev1 != ldev1) || (usbsw->dev2 != ldev2) ||
			(usbsw->dev3 != ldev3)) {
		/* device detection */
		mutex_lock(&usbsw->mutex);
		sm5502_attach_dev(usbsw);
		mutex_unlock(&usbsw->mutex);
	}

safe_exit:
	return 0;
}


static const struct i2c_device_id sm5502_id[] = {
	{"sm5502", 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, sm5502_id);
static struct of_device_id sm5502_i2c_match_table[] = {
	{ .compatible = "sm5502,i2c",},
	{},
};
MODULE_DEVICE_TABLE(of, sm5502_i2c_match_table);

static struct i2c_driver sm5502_i2c_driver = {
	.driver = {
		.name = "sm5502",
		.owner = THIS_MODULE,
		.of_match_table = sm5502_i2c_match_table,
	},
	.probe = sm5502_probe,
	.remove = sm5502_remove,
	.resume = sm5502_resume,
	.id_table = sm5502_id,
};

static int __init sm5502_init(void)
{
	return i2c_add_driver(&sm5502_i2c_driver);
}
module_init(sm5502_init);

static void __exit sm5502_exit(void)
{
	i2c_del_driver(&sm5502_i2c_driver);
}
module_exit(sm5502_exit);

MODULE_AUTHOR("Jeongrae Kim <Jryu.kim@samsung.com>");
MODULE_DESCRIPTION("SM5502 Micro USB Switch driver for EXTCON");
MODULE_LICENSE("GPL");

