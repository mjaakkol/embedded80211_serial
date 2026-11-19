/*
 * Copyright (c) 2025 Mikko Jaakkola
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(usbd_app_init, CONFIG_USBD_LOG_LEVEL);

/*
 * Explicit USB device stack initialization for the modern stack
 * (CONFIG_USB_DEVICE_STACK_NEXT).
 *
 * The CDC ACM class driver (CONFIG_USBD_CDC_ACM_CLASS) creates a serial
 * device for every `zephyr,cdc-acm-uart` device tree node, but the USBD
 * context, string descriptors, and class registration must be set up by
 * the application, and the stack started with usbd_init() + usbd_enable().
 * This mirrors the SDK reference implementation in
 * zephyr/subsys/usb/device_next/app/cdc_acm_serial.c.
 */

/* Temporary USB VID/PID for testing. */
#define USB_DEVICE_VID 0x2FE3
#define USB_DEVICE_PID 0x0001

USBD_DEVICE_DEFINE(embedded80211_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   USB_DEVICE_VID, USB_DEVICE_PID);

USBD_DESC_LANG_DEFINE(usbd_lang);
USBD_DESC_MANUFACTURER_DEFINE(usbd_mfr, "Test Vendor");
USBD_DESC_PRODUCT_DEFINE(usbd_product, "My Zephyr CDC-ACM Device");
IF_ENABLED(CONFIG_HWINFO, (USBD_DESC_SERIAL_NUMBER_DEFINE(usbd_sn)));

USBD_DESC_CONFIG_DEFINE(usbd_fs_cfg_desc, "FS Configuration");

USBD_CONFIGURATION_DEFINE(usbd_fs_config, 0, 125, &usbd_fs_cfg_desc);

static int usbd_stack_init(void)
{
	int err;

	err = usbd_add_descriptor(&embedded80211_usbd, &usbd_lang);
	if (err) {
		LOG_ERR("Failed to add language descriptor (%d)", err);
		return err;
	}

	err = usbd_add_descriptor(&embedded80211_usbd, &usbd_mfr);
	if (err) {
		LOG_ERR("Failed to add manufacturer descriptor (%d)", err);
		return err;
	}

	err = usbd_add_descriptor(&embedded80211_usbd, &usbd_product);
	if (err) {
		LOG_ERR("Failed to add product descriptor (%d)", err);
		return err;
	}

	IF_ENABLED(CONFIG_HWINFO, (
		err = usbd_add_descriptor(&embedded80211_usbd, &usbd_sn);
	))
	if (err) {
		LOG_ERR("Failed to add serial number descriptor (%d)", err);
		return err;
	}

	err = usbd_add_configuration(&embedded80211_usbd, USBD_SPEED_FS,
				     &usbd_fs_config);
	if (err) {
		LOG_ERR("Failed to add configuration (%d)", err);
		return err;
	}

	/*
	 * Register the first zephyr,cdc-acm-uart instance ("cdc_acm_0").
	 * That is the cdc_serial_uart node in the board app overlay, which
	 * serial_usb.c registers as the multiplexer's USB backend.
	 */
	err = usbd_register_class(&embedded80211_usbd, "cdc_acm_0",
				  USBD_SPEED_FS, 1);
	if (err) {
		LOG_ERR("Failed to register CDC ACM class (%d)", err);
		return err;
	}

	err = usbd_device_set_code_triple(&embedded80211_usbd, USBD_SPEED_FS,
					  USB_BCC_MISCELLANEOUS, 0x02, 0x01);
	if (err) {
		LOG_ERR("Failed to set code triple (%d)", err);
		return err;
	}

	err = usbd_init(&embedded80211_usbd);
	if (err) {
		LOG_ERR("Failed to initialize USB device stack (%d)", err);
		return err;
	}

	err = usbd_enable(&embedded80211_usbd);
	if (err) {
		LOG_ERR("Failed to enable USB device stack (%d)", err);
		return err;
	}

	LOG_INF("USB device stack enabled (VID 0x%04X, PID 0x%04X)",
		USB_DEVICE_VID, USB_DEVICE_PID);
	return 0;
}

SYS_INIT(usbd_stack_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
