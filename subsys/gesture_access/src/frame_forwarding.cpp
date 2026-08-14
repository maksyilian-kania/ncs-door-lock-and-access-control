/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "frame_forwarding.h"

#include <cerrno>
#include <cstring>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/usb/usb_ch9.h>
#include <zephyr/usb/usbd.h>

LOG_MODULE_DECLARE(door_lock_gesture_access);

BUILD_ASSERT(!IS_ENABLED(CONFIG_USBD_CDC_ACM_WORKQUEUE),
	     "Frame forwarding's TX handler requires CDC ACM on the system work queue");

namespace DoorLock::GestureAccess::FrameForwarding {

namespace {

constexpr uint8_t kProtocolVersion = 1;
constexpr uint8_t kPixFmtGrey8 = 1;

constexpr size_t kHeaderSize = 16;
constexpr size_t kCrcSize = 2;
constexpr uint16_t kCrcSeed = 0xffff;

constexpr size_t kStageSize = 512;

enum class TxStage {
	kHeader,
	kMeta,
	kPixels,
	kCrc,
	kDone,
	kAbandoned,
};

K_SEM_DEFINE(sTxDone, 0, 1);
K_MUTEX_DEFINE(sTxLock);

struct {
	const char *meta;
	size_t metaLen;
	size_t metaOff;
	const uint8_t *pixels;
	size_t pixelLen;
	size_t pixelOff;
	uint16_t width;
	uint16_t height;
	uint16_t crc;
	TxStage stage;
	uint8_t buf[kStageSize];
	size_t bufLen;
	size_t bufOff;
} sTx;

atomic_t sHostReady;

const struct device *const sFrameUart = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_frames));

USBD_DEVICE_DEFINE(sFrameUsbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING_USB_VID,
		   CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING_USB_PID);

USBD_DESC_LANG_DEFINE(sFrameLang);
USBD_DESC_MANUFACTURER_DEFINE(sFrameMfr, "Nordic Semiconductor ASA");
USBD_DESC_PRODUCT_DEFINE(sFrameProduct, "Gesture access frames");
USBD_DESC_CONFIG_DEFINE(sFrameFsCfgDesc, "FS Configuration");
USBD_DESC_CONFIG_DEFINE(sFrameHsCfgDesc, "HS Configuration");

USBD_CONFIGURATION_DEFINE(sFrameFsConfig, 0, 125, &sFrameFsCfgDesc);
USBD_CONFIGURATION_DEFINE(sFrameHsConfig, 0, 125, &sFrameHsCfgDesc);

void BuildHeader()
{
	const uint32_t dataLen = (uint32_t)sTx.pixelLen;

	sTx.buf[0] = 'G';
	sTx.buf[1] = 'A';
	sTx.buf[2] = 'F';
	sTx.buf[3] = 'F';
	sTx.buf[4] = kProtocolVersion;
	sTx.buf[5] = kPixFmtGrey8;
	sys_put_le16(sTx.width, &sTx.buf[6]);
	sys_put_le16(sTx.height, &sTx.buf[8]);
	sys_put_le16((uint16_t)sTx.metaLen, &sTx.buf[10]);
	sys_put_le32(dataLen, &sTx.buf[12]);

	sTx.bufLen = kHeaderSize;
	sTx.bufOff = 0;
}

void StageMeta()
{
	const size_t chunk = MIN(sTx.metaLen - sTx.metaOff, kStageSize);

	memcpy(sTx.buf, &sTx.meta[sTx.metaOff], chunk);
	sTx.metaOff += chunk;
	sTx.crc = crc16_ccitt(sTx.crc, sTx.buf, chunk);
	sTx.bufLen = chunk;
	sTx.bufOff = 0;
}

void StagePixels()
{
	const size_t chunk = MIN(sTx.pixelLen - sTx.pixelOff, kStageSize);

	memcpy(sTx.buf, &sTx.pixels[sTx.pixelOff], chunk);
	sTx.pixelOff += chunk;
	sTx.crc = crc16_ccitt(sTx.crc, sTx.buf, chunk);
	sTx.bufLen = chunk;
	sTx.bufOff = 0;
}

/*
 * Stage the next slice of the frame. Returns false once the last byte has
 * been staged, which is how the TX handler learns the frame is complete.
 */
bool StageRefill()
{
	while (true) {
		switch (sTx.stage) {
		case TxStage::kHeader:
			BuildHeader();
			sTx.stage = TxStage::kMeta;
			return true;

		case TxStage::kMeta:
			if (sTx.metaOff < sTx.metaLen) {
				StageMeta();
				return true;
			}

			sTx.stage = TxStage::kPixels;
			break;

		case TxStage::kPixels:
			if (sTx.pixelOff < sTx.pixelLen) {
				StagePixels();
				return true;
			}

			sTx.stage = TxStage::kCrc;
			break;

		case TxStage::kCrc:
			sys_put_le16(sTx.crc, sTx.buf);
			sTx.bufLen = kCrcSize;
			sTx.bufOff = 0;
			sTx.stage = TxStage::kDone;
			return true;

		case TxStage::kDone:
		default:
			return false;
		}
	}
}

void FrameTxHandler(const struct device *dev, void *userData)
{
	ARG_UNUSED(userData);

	if (sTx.stage == TxStage::kAbandoned) {
		/* The sender gave up on this frame; stop without signalling it. */
		uart_irq_tx_disable(dev);
		return;
	}

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (!uart_irq_tx_ready(dev)) {
			break;
		}

		if (sTx.bufOff == sTx.bufLen && !StageRefill()) {
			/* The whole frame is queued in the driver's TX FIFO. */
			uart_irq_tx_disable(dev);
			k_sem_give(&sTxDone);
			return;
		}

		const int written =
			uart_fifo_fill(dev, &sTx.buf[sTx.bufOff], (int)(sTx.bufLen - sTx.bufOff));

		if (written <= 0) {
			break;
		}

		sTx.bufOff += (size_t)written;
	}
}

void UsbdMsgHandler(struct usbd_context *const ctx, const struct usbd_msg *const msg)
{
	if (usbd_can_detect_vbus(ctx)) {
		if (msg->type == USBD_MSG_VBUS_READY) {
			if (usbd_enable(ctx)) {
				LOG_ERR("Failed to enable USB device support");
			}
		} else if (msg->type == USBD_MSG_VBUS_REMOVED) {
			atomic_set(&sHostReady, 0);
			if (usbd_disable(ctx)) {
				LOG_ERR("Failed to disable USB device support");
			}
		}
	}

	switch (msg->type) {
	case USBD_MSG_CDC_ACM_CONTROL_LINE_STATE: {
		uint32_t dtr = 0;

		(void)uart_line_ctrl_get(msg->dev, UART_LINE_CTRL_DTR, &dtr);
		atomic_set(&sHostReady, dtr ? 1 : 0);
		LOG_INF("Frame viewer %s", dtr ? "attached" : "detached");
		break;
	}
	case USBD_MSG_RESET:
		atomic_set(&sHostReady, 0);
		break;
	default:
		break;
	}
}

int UsbdSetup()
{
	int err;

	err = usbd_add_descriptor(&sFrameUsbd, &sFrameLang);
	if (err) {
		LOG_ERR("Failed to add language descriptor (err %d)", err);
		return err;
	}

	err = usbd_add_descriptor(&sFrameUsbd, &sFrameMfr);
	if (err) {
		LOG_ERR("Failed to add manufacturer descriptor (err %d)", err);
		return err;
	}

	err = usbd_add_descriptor(&sFrameUsbd, &sFrameProduct);
	if (err) {
		LOG_ERR("Failed to add product descriptor (err %d)", err);
		return err;
	}

	if (USBD_SUPPORTS_HIGH_SPEED && usbd_caps_speed(&sFrameUsbd) == USBD_SPEED_HS) {
		err = usbd_add_configuration(&sFrameUsbd, USBD_SPEED_HS, &sFrameHsConfig);
		if (err) {
			LOG_ERR("Failed to add high-speed configuration (err %d)", err);
			return err;
		}

		err = usbd_register_all_classes(&sFrameUsbd, USBD_SPEED_HS, 1, NULL);
		if (err) {
			LOG_ERR("Failed to register high-speed classes (err %d)", err);
			return err;
		}

		/* CDC ACM spans two interfaces, so advertise the IAD triple. */
		usbd_device_set_code_triple(&sFrameUsbd, USBD_SPEED_HS, USB_BCC_MISCELLANEOUS,
					    0x02, 0x01);
	}

	err = usbd_add_configuration(&sFrameUsbd, USBD_SPEED_FS, &sFrameFsConfig);
	if (err) {
		LOG_ERR("Failed to add full-speed configuration (err %d)", err);
		return err;
	}

	err = usbd_register_all_classes(&sFrameUsbd, USBD_SPEED_FS, 1, NULL);
	if (err) {
		LOG_ERR("Failed to register full-speed classes (err %d)", err);
		return err;
	}

	usbd_device_set_code_triple(&sFrameUsbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 0x02, 0x01);
	usbd_self_powered(&sFrameUsbd, false);

	err = usbd_msg_register_cb(&sFrameUsbd, UsbdMsgHandler);
	if (err) {
		LOG_ERR("Failed to register message callback (err %d)", err);
		return err;
	}

	return 0;
}

} // namespace

int Init()
{
	int err;

	if (!device_is_ready(sFrameUart)) {
		LOG_ERR("CDC ACM device not ready");
		return -ENODEV;
	}

	uart_irq_tx_disable(sFrameUart);
	uart_irq_callback_user_data_set(sFrameUart, FrameTxHandler, NULL);

	err = UsbdSetup();
	if (err) {
		return err;
	}

	err = usbd_init(&sFrameUsbd);
	if (err) {
		LOG_ERR("Failed to initialize USB device support (err %d)", err);
		return err;
	}

	if (!usbd_can_detect_vbus(&sFrameUsbd)) {
		err = usbd_enable(&sFrameUsbd);
		if (err) {
			LOG_ERR("Failed to enable USB device support (err %d)", err);
			return err;
		}
	}

	LOG_INF("Frame forwarding ready on USB CDC ACM");

	return 0;
}

bool HostReady()
{
	return atomic_get(&sHostReady) != 0;
}

int Send(uint16_t width, uint16_t height, const uint8_t *pixels, const char *meta, size_t metaLength)
{
	int err;

	if (!HostReady()) {
		return -ENOTCONN;
	}

	k_mutex_lock(&sTxLock, K_FOREVER);

	sTx.width = width;
	sTx.height = height;
	sTx.pixels = pixels;
	sTx.pixelLen = (size_t)width * height;
	sTx.pixelOff = 0;
	sTx.meta = meta;
	sTx.metaLen = (meta != nullptr) ? metaLength : 0;
	sTx.metaOff = 0;
	sTx.crc = kCrcSeed;
	sTx.stage = TxStage::kHeader;
	sTx.bufLen = 0;
	sTx.bufOff = 0;

	k_sem_reset(&sTxDone);
	uart_irq_tx_enable(sFrameUart);

	err = k_sem_take(&sTxDone, K_MSEC(CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING_TX_TIMEOUT_MS));
	if (err) {
		/*
		 * The host stopped draining the port. Abandon the frame; the
		 * viewer discards the truncated data and resynchronises on
		 * the next frame magic.
		 */
		sTx.stage = TxStage::kAbandoned;
		uart_irq_tx_disable(sFrameUart);
		LOG_WRN("Frame transfer timed out");
		err = -ETIMEDOUT;
	}

	k_mutex_unlock(&sTxLock);

	return err;
}

} // namespace DoorLock::GestureAccess::FrameForwarding
