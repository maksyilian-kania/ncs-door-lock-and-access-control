/*
 * Copyright (c) 2023 Arducam Technology Co., Ltd. <www.arducam.com>
 * Copyright The Zephyr Project Contributors
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Zephyr `video` class driver for the Arducam Mega SPI camera module used by
 * the gesture access feature. Ported from the upstream in-tree driver
 * (zephyr/drivers/video/video_arducam_mega.c), keeping the register
 * map/protocol identical and renaming arducam_mega_* -> spi_cam_*.
 *
 * Notable differences from upstream:
 *  - Capture/FIFO-drain work runs on the dedicated gesture-access work queue
 *    (gesture_access_workqueue) instead of a private k_sys_work_q-style
 *    queue, so it never competes with the system work queue used by
 *    BLE/Matter/Aliro/NFC/UWB - see subsys/gesture_access/
 *    gesture_access_workqueue for the rationale.
 *  - Adds a driver-controlled capture cadence
 *    (VIDEO_CID_SPI_CAM_CAPTURE_INTERVAL_MS) with an enforced minimum
 *    interval, instead of a fixed 30 ms polling period.
 *  - The camera contract is intentionally fixed to the gesture model input:
 *    96x96 VIDEO_PIX_FMT_GREY. The sensor has no native grayscale mode, so a
 *    complete YUYV frame is captured into reusable driver storage and its Y
 *    bytes are copied into the caller's video_buffer.
 */

#define DT_DRV_COMPAT door_lock_spi_cam

#include <spi_cam.h>

#include <gesture_access_workqueue/gesture_access_workqueue.h>

#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/video-controls.h>
#include <zephyr/drivers/video.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>

#include "video_common.h"
#include "video_ctrls.h"
#include "video_device.h"

LOG_MODULE_REGISTER(door_lock_spi_cam, CONFIG_DOOR_LOCK_CAMERA_SPI_LOG_LEVEL);

/* Configure camera contrast level */
enum spi_cam_contrast_level {
	SPI_CAM_CONTRAST_LEVEL_NEGATIVE_3 = 6,
	SPI_CAM_CONTRAST_LEVEL_NEGATIVE_2 = 4,
	SPI_CAM_CONTRAST_LEVEL_NEGATIVE_1 = 2,
	SPI_CAM_CONTRAST_LEVEL_DEFAULT = 0,
	SPI_CAM_CONTRAST_LEVEL_1 = 1,
	SPI_CAM_CONTRAST_LEVEL_2 = 3,
	SPI_CAM_CONTRAST_LEVEL_3 = 5,
};

/* Configure camera EV level */
enum spi_cam_ev_level {
	SPI_CAM_EV_LEVEL_NEGATIVE_3 = 6,
	SPI_CAM_EV_LEVEL_NEGATIVE_2 = 4,
	SPI_CAM_EV_LEVEL_NEGATIVE_1 = 2,
	SPI_CAM_EV_LEVEL_DEFAULT = 0,
	SPI_CAM_EV_LEVEL_1 = 1,
	SPI_CAM_EV_LEVEL_2 = 3,
	SPI_CAM_EV_LEVEL_3 = 5,
};

/* Configure camera saturation level */
enum spi_cam_saturation_level {
	SPI_CAM_SATURATION_LEVEL_NEGATIVE_3 = 6,
	SPI_CAM_SATURATION_LEVEL_NEGATIVE_2 = 4,
	SPI_CAM_SATURATION_LEVEL_NEGATIVE_1 = 2,
	SPI_CAM_SATURATION_LEVEL_DEFAULT = 0,
	SPI_CAM_SATURATION_LEVEL_1 = 1,
	SPI_CAM_SATURATION_LEVEL_2 = 3,
	SPI_CAM_SATURATION_LEVEL_3 = 5,
};

/* Configure camera brightness level */
enum spi_cam_brightness_level {
	SPI_CAM_BRIGHTNESS_LEVEL_NEGATIVE_4 = 8,
	SPI_CAM_BRIGHTNESS_LEVEL_NEGATIVE_3 = 6,
	SPI_CAM_BRIGHTNESS_LEVEL_NEGATIVE_2 = 4,
	SPI_CAM_BRIGHTNESS_LEVEL_NEGATIVE_1 = 2,
	SPI_CAM_BRIGHTNESS_LEVEL_DEFAULT = 0,
	SPI_CAM_BRIGHTNESS_LEVEL_1 = 1,
	SPI_CAM_BRIGHTNESS_LEVEL_2 = 3,
	SPI_CAM_BRIGHTNESS_LEVEL_3 = 5,
	SPI_CAM_BRIGHTNESS_LEVEL_4 = 7,
};

/* Configure camera Sharpness level (3MP sensor variants only) */
enum spi_cam_sharpness_level {
	SPI_CAM_SHARPNESS_LEVEL_AUTO = 0,
	SPI_CAM_SHARPNESS_LEVEL_1,
	SPI_CAM_SHARPNESS_LEVEL_2,
	SPI_CAM_SHARPNESS_LEVEL_3,
	SPI_CAM_SHARPNESS_LEVEL_4,
	SPI_CAM_SHARPNESS_LEVEL_5,
	SPI_CAM_SHARPNESS_LEVEL_6,
	SPI_CAM_SHARPNESS_LEVEL_7,
	SPI_CAM_SHARPNESS_LEVEL_8,
};

/* Configure camera auto focus level (5MP sensor variants only) */
enum spi_cam_auto_focus_level {
	SPI_CAM_AUTO_FOCUS_ON = 0,
	SPI_CAM_AUTO_FOCUS_SINGLE,
	SPI_CAM_AUTO_FOCUS_CONT,
	SPI_CAM_AUTO_FOCUS_PAUSE,
	SPI_CAM_AUTO_FOCUS_OFF,
};

/* Configure special effects */
enum spi_cam_color_fx {
	SPI_CAM_COLOR_FX_NONE = 0,
	SPI_CAM_COLOR_FX_BLUEISH,
	SPI_CAM_COLOR_FX_REDISH,
	SPI_CAM_COLOR_FX_BW,
	SPI_CAM_COLOR_FX_SEPIA,
	SPI_CAM_COLOR_FX_NEGATIVE,
	SPI_CAM_COLOR_FX_GRASS_GREEN,
	SPI_CAM_COLOR_FX_OVER_EXPOSURE,
	SPI_CAM_COLOR_FX_SOLARIZE,
};

/* Configure white balance mode */
enum spi_cam_white_balance {
	SPI_CAM_WHITE_BALANCE_MODE_DEFAULT = 0,
	SPI_CAM_WHITE_BALANCE_MODE_SUNNY,
	SPI_CAM_WHITE_BALANCE_MODE_OFFICE,
	SPI_CAM_WHITE_BALANCE_MODE_CLOUDY,
	SPI_CAM_WHITE_BALANCE_MODE_HOME,
};

enum {
	SPI_CAM_SENSOR_5MP_1 = 0x81,
	SPI_CAM_SENSOR_3MP_1 = 0x82,
	SPI_CAM_SENSOR_5MP_2 = 0x83, /* 2592x1936 */
	SPI_CAM_SENSOR_3MP_2 = 0x84,
};

/* Configure camera pixel format, as sent over the wire to the sensor. */
enum spi_cam_pixelformat {
	SPI_CAM_PIXELFORMAT_YUV = 0x03,
};

/* Configure camera available features */
enum spi_cam_features {
	SPI_CAM_HAS_DEFAULT = 0,
	SPI_CAM_HAS_SHARPNESS = 1 << 0,
	SPI_CAM_HAS_FOCUS = 1 << 1,
	SPI_CAM_HAS_COLORFX = 1 << 2,
};

#define ARDUCHIP_FIFO   0x04 /* FIFO and I2C control */
#define ARDUCHIP_FIFO_2 0x07 /* FIFO and I2C control */

#define FIFO_CLEAR_ID_MASK 0x01
#define FIFO_START_MASK    0x02

#define ARDUCHIP_TRIG 0x44 /* Trigger source */
#define VSYNC_MASK    0x01
#define SHUTTER_MASK  0x02
#define CAP_DONE_MASK 0x04

#define FIFO_SIZE1 0x45 /* Camera write FIFO size[7:0] for burst to read */
#define FIFO_SIZE2 0x46 /* Camera write FIFO size[15:8] */
#define FIFO_SIZE3 0x47 /* Camera write FIFO size[18:16] */

#define BURST_FIFO_READ  0x3C /* Burst FIFO read operation */
#define SINGLE_FIFO_READ 0x3D /* Single FIFO read operation */

/* DSP register bank FF=0x00 */
#define CAM_REG_POWER_CONTROL                 0x02
#define CAM_REG_SENSOR_RESET                  0x07
#define CAM_REG_FORMAT                        0x20
#define CAM_REG_CAPTURE_RESOLUTION            0x21
#define CAM_REG_BRIGHTNESS_CONTROL            0x22
#define CAM_REG_CONTRAST_CONTROL              0x23
#define CAM_REG_SATURATION_CONTROL            0x24
#define CAM_REG_EV_CONTROL                    0x25
#define CAM_REG_WHITEBALANCE_CONTROL          0x26
#define CAM_REG_COLOR_EFFECT_CONTROL          0x27
#define CAM_REG_SHARPNESS_CONTROL             0x28
#define CAM_REG_AUTO_FOCUS_CONTROL            0x29
#define CAM_REG_EXPOSURE_GAIN_WHITEBAL_ENABLE 0x30
#define CAM_REG_MANUAL_GAIN_BIT_9_8           0x31
#define CAM_REG_MANUAL_GAIN_BIT_7_0           0x32
#define CAM_REG_MANUAL_EXPOSURE_BIT_19_16     0x33
#define CAM_REG_MANUAL_EXPOSURE_BIT_15_8      0x34
#define CAM_REG_MANUAL_EXPOSURE_BIT_7_0       0x35
#define CAM_REG_BURST_FIFO_READ_OPERATION     0x3C
#define CAM_REG_SINGLE_FIFO_READ_OPERATION    0x3D
#define CAM_REG_SENSOR_ID                     0x40
#define CAM_REG_YEAR_SDK                      0x41
#define CAM_REG_MONTH_SDK                     0x42
#define CAM_REG_DAY_SDK                       0x43
#define CAM_REG_SENSOR_STATE                  0x44
#define CAM_REG_FPGA_VERSION_NUMBER           0x49
#define CAM_REG_DEBUG_DEVICE_ADDRESS          0x0A
#define CAM_REG_DEBUG_REGISTER_HIGH           0x0B
#define CAM_REG_DEBUG_REGISTER_LOW            0x0C
#define CAM_REG_DEBUG_REGISTER_VALUE          0x0D

#define SENSOR_STATE_IDLE   (1 << 1)
#define SENSOR_RESET_ENABLE (1 << 6)

#define CTR_WHITEBALANCE 0x02
#define CTR_EXPOSURE     0x01
#define CTR_GAIN         0x00

#define SPI_CAM_CAPTURE_TRIES 200
#define SPI_CAM_FRAME_WIDTH 96U
#define SPI_CAM_FRAME_HEIGHT 96U
#define SPI_CAM_GRAY_FRAME_BYTES (SPI_CAM_FRAME_WIDTH * SPI_CAM_FRAME_HEIGHT)
#define SPI_CAM_YUYV_FRAME_BYTES (SPI_CAM_GRAY_FRAME_BYTES * 2U)

/*
 * Fixed overhead added on top of the raw SPI transfer time when computing the
 * minimum allowed capture interval: register I/O for the capture trigger and
 * FIFO-size read-back, plus polling CAM_REG_SENSOR_STATE/ARDUCHIP_TRIG.
 * Deliberately conservative; see spi_cam_min_capture_interval_ms().
 */
#define SPI_CAM_CAPTURE_OVERHEAD_MS 20

struct spi_cam_config {
	struct spi_dt_spec spi;
};

struct spi_cam_ctrls {
	struct video_ctrl reset;
	struct video_ctrl brightness;
	struct video_ctrl contrast;
	struct video_ctrl saturation;
	struct video_ctrl ev;
	struct video_ctrl whitebal;
	struct video_ctrl colorfx;
	struct video_ctrl lowpower;
	struct video_ctrl whitebalauto;
	struct video_ctrl sharpness;
	struct {
		struct video_ctrl exp_auto;
		struct video_ctrl exposure;
	};
	struct {
		struct video_ctrl gain;
		struct video_ctrl gain_auto;
	};
	struct video_ctrl focus_auto;
	struct video_ctrl capture_interval_ms;
};

struct spi_cam_data {
	struct spi_cam_ctrls ctrls;
	struct video_format fmt;

	const struct device *dev;
	struct k_fifo fifo_in;
	struct k_fifo fifo_out;
	struct k_work buf_work;
	struct k_timer stream_schedule_timer;
	struct k_poll_signal *signal;
	uint8_t stream_on;
	uint32_t features;
	uint32_t camera_id;
	uint32_t capture_interval_ms;
	uint32_t frame_count;
	uint8_t frame_buf[SPI_CAM_YUYV_FRAME_BYTES] __aligned(4);
};

#define SPI_CAM_VIDEO_FORMAT_CAP(width, height, format)                                          \
	{.pixelformat = (format),                                                                 \
	 .width_min = (width),                                                                    \
	 .width_max = (width),                                                                    \
	 .height_min = (height),                                                                  \
	 .height_max = (height),                                                                  \
	 .width_step = 0,                                                                         \
	 .height_step = 0}

static const struct video_format_cap fmts[] = {
	SPI_CAM_VIDEO_FORMAT_CAP(SPI_CAM_FRAME_WIDTH, SPI_CAM_FRAME_HEIGHT,
				 VIDEO_PIX_FMT_GREY),
	{0},
};

enum spi_cam_resolution {
	SPI_CAM_RESOLUTION_96X96 = 0x0a,
};

/* -------------------------------------------------------------------------
 * Low-level register I/O
 * ---------------------------------------------------------------------- */

static int spi_cam_write_reg(const struct spi_dt_spec *spec, uint8_t reg_addr, uint8_t value)
{
	int ret = 0;

	reg_addr |= 0x80;

	struct spi_buf tx_buf[2] = {
		{.buf = &reg_addr, .len = 1},
		{.buf = &value, .len = 1},
	};

	struct spi_buf_set tx_bufs = {.buffers = tx_buf, .count = 2};

	for (int tries = 3;; tries--) {
		ret = spi_write_dt(spec, &tx_bufs);
		if (ret < 0 && tries == 0) {
			LOG_ERR("failed to write 0x%x to 0x%x", value, reg_addr);
			return ret;
		}

		if (ret == 0) {
			break;
		}

		/* If writing failed wait 5ms before next attempt */
		k_msleep(5);
	}

	return 0;
}

static int spi_cam_read_reg(const struct spi_dt_spec *spec, uint8_t reg_addr, uint32_t *val)
{
	uint8_t value;
	int ret = 0;

	reg_addr &= 0x7F;

	struct spi_buf tx_buf[] = {
		{.buf = &reg_addr, .len = 1},
	};

	struct spi_buf_set tx_bufs = {.buffers = tx_buf, .count = 1};

	struct spi_buf rx_buf[] = {
		{.buf = &value, .len = 1},
		{.buf = &value, .len = 1},
		{.buf = &value, .len = 1},
	};

	struct spi_buf_set rx_bufs = {.buffers = rx_buf, .count = 3};

	for (int tries = 3;; tries--) {
		ret = spi_transceive_dt(spec, &tx_bufs, &rx_bufs);
		if (ret < 0 && tries == 0) {
			LOG_ERR("failed to read 0x%x register", reg_addr);
			return ret;
		}

		if (ret >= 0) {
			break;
		}

		/* If reading failed wait 5ms before next attempt */
		k_msleep(5);
	}
	*val = value;

	return 0;
}

static int spi_cam_read_block(const struct spi_dt_spec *spec, uint8_t *img_buff, uint32_t img_len,
			      uint8_t first)
{
	uint8_t cmd_fifo_read[] = {BURST_FIFO_READ, 0x00};
	uint8_t buf_len = first == 0 ? 1 : 2;

	struct spi_buf tx_buf[] = {
		{.buf = cmd_fifo_read, .len = buf_len},
	};

	struct spi_buf_set tx_bufs = {.buffers = tx_buf, .count = 1};

	struct spi_buf rx_buf[2] = {
		{.buf = cmd_fifo_read, .len = buf_len},
		{.buf = img_buff, .len = img_len},
	};
	struct spi_buf_set rx_bufs = {.buffers = rx_buf, .count = 2};

	return spi_transceive_dt(spec, &tx_bufs, &rx_bufs);
}

static int spi_cam_await_bus_idle(const struct spi_dt_spec *spec, int tries)
{
	uint32_t reg_data;
	int ret = 0;

	for (; tries > 0; tries--) {
		ret = spi_cam_read_reg(spec, CAM_REG_SENSOR_STATE, &reg_data);
		if (ret < 0) {
			return ret;
		}

		if ((reg_data & 0x03) == SENSOR_STATE_IDLE) {
			return 0;
		}

		k_msleep(2);
	}

	return 0;
}

static int spi_cam_write_reg_wait(const struct spi_dt_spec *spec, uint16_t reg, uint8_t value,
				  uint32_t idle_timeout_ms)
{
	int ret = 0;

	ret = spi_cam_await_bus_idle(spec, idle_timeout_ms);
	if (ret < 0) {
		LOG_ERR("Bus idle wait failed before writing register 0x%04x", reg);
		return ret;
	}

	ret = spi_cam_write_reg(spec, reg, value);
	if (ret < 0) {
		LOG_ERR("Failed to write register 0x%04x)", reg);
	}

	return ret;
}

/* -------------------------------------------------------------------------
 * Control-value helpers and setters
 * ---------------------------------------------------------------------- */

static enum spi_cam_ev_level spi_cam_get_ev_level(int value)
{
	static const enum spi_cam_ev_level ev_level_map[] = {
		SPI_CAM_EV_LEVEL_NEGATIVE_3, SPI_CAM_EV_LEVEL_NEGATIVE_2,
		SPI_CAM_EV_LEVEL_NEGATIVE_1, SPI_CAM_EV_LEVEL_DEFAULT,
		SPI_CAM_EV_LEVEL_1,          SPI_CAM_EV_LEVEL_2,
		SPI_CAM_EV_LEVEL_3,
	};

	int index = value + 3;

	if (index >= 0 && index < ARRAY_SIZE(ev_level_map)) {
		return ev_level_map[index];
	} else {
		return SPI_CAM_EV_LEVEL_DEFAULT;
	}
}

static int spi_cam_set_brightness(const struct device *dev, enum spi_cam_brightness_level level)
{
	const struct spi_cam_config *cfg = dev->config;

	return spi_cam_write_reg_wait(&cfg->spi, CAM_REG_BRIGHTNESS_CONTROL, level, 3);
}

static int spi_cam_set_saturation(const struct device *dev, enum spi_cam_saturation_level level)
{
	const struct spi_cam_config *cfg = dev->config;

	return spi_cam_write_reg_wait(&cfg->spi, CAM_REG_SATURATION_CONTROL, level, 3);
}

static int spi_cam_set_contrast(const struct device *dev, enum spi_cam_contrast_level level)
{
	const struct spi_cam_config *cfg = dev->config;

	return spi_cam_write_reg_wait(&cfg->spi, CAM_REG_CONTRAST_CONTROL, level, 3);
}

static int spi_cam_set_ev(const struct device *dev, int level)
{
	const struct spi_cam_config *cfg = dev->config;

	return spi_cam_write_reg_wait(&cfg->spi, CAM_REG_EV_CONTROL, spi_cam_get_ev_level(level),
				      3);
}

static int spi_cam_set_sharpness(const struct device *dev, enum spi_cam_sharpness_level level)
{
	const struct spi_cam_config *cfg = dev->config;

	return spi_cam_write_reg_wait(&cfg->spi, CAM_REG_SHARPNESS_CONTROL, level, 3);
}

static int spi_cam_set_auto_focus(const struct device *dev, bool enable)
{
	const struct spi_cam_config *cfg = dev->config;
	enum spi_cam_auto_focus_level level =
		enable ? SPI_CAM_AUTO_FOCUS_ON : SPI_CAM_AUTO_FOCUS_OFF;

	return spi_cam_write_reg_wait(&cfg->spi, CAM_REG_AUTO_FOCUS_CONTROL, level, 3);
}

static int spi_cam_set_special_effects(const struct device *dev, enum video_colorfx effect)
{
	const struct spi_cam_config *cfg = dev->config;
	static const struct {
		enum video_colorfx video_fx;
		enum spi_cam_color_fx spi_cam_fx;
	} fx_map[] = {
		{VIDEO_COLORFX_NONE, SPI_CAM_COLOR_FX_NONE},
		{VIDEO_COLORFX_BW, SPI_CAM_COLOR_FX_BW},
		{VIDEO_COLORFX_SEPIA, SPI_CAM_COLOR_FX_SEPIA},
		{VIDEO_COLORFX_NEGATIVE, SPI_CAM_COLOR_FX_NEGATIVE},
		{VIDEO_COLORFX_SKY_BLUE, SPI_CAM_COLOR_FX_BLUEISH},
		{VIDEO_COLORFX_GRASS_GREEN, SPI_CAM_COLOR_FX_GRASS_GREEN},
		{VIDEO_COLORFX_VIVID, SPI_CAM_COLOR_FX_OVER_EXPOSURE},
	};

	uint8_t spi_cam_effect = SPI_CAM_COLOR_FX_NONE;
	bool supported = false;

	for (size_t i = 0; i < ARRAY_SIZE(fx_map); ++i) {
		if (fx_map[i].video_fx == effect) {
			spi_cam_effect = fx_map[i].spi_cam_fx;
			supported = true;
			break;
		}
	}

	if (!supported) {
		LOG_ERR("Unsupported color effect: %d", effect);
		return -ENOTSUP;
	}

	return spi_cam_write_reg_wait(&cfg->spi, CAM_REG_COLOR_EFFECT_CONTROL, spi_cam_effect, 3);
}

static int spi_cam_set_output_format(const struct device *dev)
{
	const struct spi_cam_config *cfg = dev->config;
	int ret = 0;

	/* The sensor has no native grayscale mode. It always supplies the fixed
	 * 96x96 model frame as YUYV; the driver exposes only its Y samples.
	 */
	ret = spi_cam_write_reg_wait(&cfg->spi, CAM_REG_FORMAT, SPI_CAM_PIXELFORMAT_YUV, 3);
	if (ret < 0) {
		return ret;
	}

	ret = spi_cam_await_bus_idle(&cfg->spi, 30);
	if (ret < 0) {
		LOG_ERR("Bus idle wait failed after setting output format");
	}

	return ret;
}

static int spi_cam_set_white_bal_enable(const struct device *dev, int enable)
{
	const struct spi_cam_config *cfg = dev->config;
	int ret = 0;
	uint8_t reg = CTR_WHITEBALANCE;

	if (enable) {
		reg |= 0x80;
	}

	ret = spi_cam_write_reg_wait(&cfg->spi, CAM_REG_EXPOSURE_GAIN_WHITEBAL_ENABLE, reg, 3);
	if (ret < 0) {
		return ret;
	}

	ret = spi_cam_await_bus_idle(&cfg->spi, 10);
	if (ret < 0) {
		LOG_ERR("Bus idle wait failed after setting white balance");
	}

	return ret;
}

static int spi_cam_set_white_bal(const struct device *dev, enum spi_cam_white_balance level)
{
	const struct spi_cam_config *cfg = dev->config;

	return spi_cam_write_reg_wait(&cfg->spi, CAM_REG_WHITEBALANCE_CONTROL, level, 3);
}

static int spi_cam_set_gain_enable(const struct device *dev, int enable)
{
	const struct spi_cam_config *cfg = dev->config;
	int ret = 0;
	uint8_t reg = CTR_GAIN;

	if (enable) {
		reg |= 0x80;
	}

	ret = spi_cam_write_reg_wait(&cfg->spi, CAM_REG_EXPOSURE_GAIN_WHITEBAL_ENABLE, reg, 3);
	if (ret < 0) {
		return ret;
	}

	ret = spi_cam_await_bus_idle(&cfg->spi, 10);
	if (ret < 0) {
		LOG_ERR("Bus idle wait failed after setting gain enable");
	}

	return ret;
}

static int spi_cam_set_lowpower_enable(const struct device *dev, int enable)
{
	const struct spi_cam_config *cfg = dev->config;
	const struct spi_cam_data *drv_data = dev->data;

	/* Quirk carried over from the upstream Arducam Mega driver: the
	 * power-control register polarity is inverted on some sensor
	 * variants.
	 */
	if (drv_data->camera_id == SPI_CAM_SENSOR_5MP_2 ||
	    drv_data->camera_id == SPI_CAM_SENSOR_3MP_2) {
		enable = !enable;
	}

	uint8_t reg_val = enable ? 0x07 : 0x05;

	return spi_cam_write_reg_wait(&cfg->spi, CAM_REG_POWER_CONTROL, reg_val, 3);
}

#ifdef CONFIG_PM_DEVICE
/*
 * Zephyr device PM hookup for the sensor's own low-power mode (as opposed to
 * &spi22's bus-level runtime PM, set up via zephyr,pm-device-runtime-auto in
 * the board overlay - that suspends the SPI controller, this suspends the
 * sensor IC itself). Reuses the same register write as the
 * VIDEO_CID_SPI_CAM_LOWPOWER control; see spi_cam_set_lowpower_enable() for
 * the sensor-ID polarity quirk.
 *
 * Intended caller: DoorLock::GestureAccess::Stop()/Start() via
 * pm_device_action_run(cam_dev, PM_DEVICE_ACTION_SUSPEND/RESUME) - see the
 * doc comment on VIDEO_CID_SPI_CAM_LOWPOWER in spi_cam.h.
 */
static int spi_cam_pm_action(const struct device *dev, enum pm_device_action action)
{
	switch (action) {
	case PM_DEVICE_ACTION_RESUME:
		return spi_cam_set_lowpower_enable(dev, 0);
	case PM_DEVICE_ACTION_SUSPEND:
		return spi_cam_set_lowpower_enable(dev, 1);
	default:
		return -ENOTSUP;
	}
}
#endif /* CONFIG_PM_DEVICE */

static int spi_cam_set_gain(const struct device *dev, uint16_t value)
{
	const struct spi_cam_config *cfg = dev->config;
	int ret = 0;

	ret = spi_cam_write_reg_wait(&cfg->spi, CAM_REG_MANUAL_GAIN_BIT_9_8, (value >> 8) & 0xFF,
				     3);
	if (ret < 0) {
		return ret;
	}

	ret = spi_cam_write_reg_wait(&cfg->spi, CAM_REG_MANUAL_GAIN_BIT_7_0, value & 0xFF, 10);
	if (ret < 0) {
		return ret;
	}

	ret = spi_cam_await_bus_idle(&cfg->spi, 10);
	if (ret < 0) {
		LOG_ERR("Bus idle wait failed after setting gain");
	}

	return ret;
}

static int spi_cam_set_exposure_enable(const struct device *dev, int enable)
{
	const struct spi_cam_config *cfg = dev->config;
	int ret = 0;
	uint8_t reg = CTR_EXPOSURE;

	if (enable) {
		reg |= 0x80;
	}

	ret = spi_cam_write_reg_wait(&cfg->spi, CAM_REG_EXPOSURE_GAIN_WHITEBAL_ENABLE, reg, 3);
	if (ret < 0) {
		return ret;
	}

	ret = spi_cam_await_bus_idle(&cfg->spi, 10);
	if (ret < 0) {
		LOG_ERR("Bus idle wait failed after setting exposure enable");
	}

	return ret;
}

static int spi_cam_set_exposure(const struct device *dev, uint32_t value)
{
	const struct spi_cam_config *cfg = dev->config;
	int ret = 0;

	ret = spi_cam_write_reg_wait(&cfg->spi, CAM_REG_MANUAL_EXPOSURE_BIT_19_16,
				     (value >> 16) & 0xFF, 3);
	if (ret < 0) {
		return ret;
	}

	ret = spi_cam_write_reg_wait(&cfg->spi, CAM_REG_MANUAL_EXPOSURE_BIT_15_8,
				     (value >> 8) & 0xFF, 10);
	if (ret < 0) {
		return ret;
	}

	ret = spi_cam_write_reg_wait(&cfg->spi, CAM_REG_MANUAL_EXPOSURE_BIT_7_0, value & 0xFF, 10);
	if (ret < 0) {
		return ret;
	}

	ret = spi_cam_await_bus_idle(&cfg->spi, 10);
	if (ret < 0) {
		LOG_ERR("Bus idle wait failed after setting exposure");
	}

	return ret;
}

static int spi_cam_set_resolution(const struct device *dev, enum spi_cam_resolution resolution)
{
	const struct spi_cam_config *cfg = dev->config;
	int ret = 0;

	ret = spi_cam_write_reg_wait(&cfg->spi, CAM_REG_CAPTURE_RESOLUTION, resolution, 10);
	if (ret < 0) {
		return ret;
	}

	ret = spi_cam_await_bus_idle(&cfg->spi, 10);
	if (ret < 0) {
		LOG_ERR("Bus idle wait failed after setting resolution");
	}

	return ret;
}

/* -------------------------------------------------------------------------
 * Connection check / sensor identification
 * ---------------------------------------------------------------------- */

static int spi_cam_check_connection(const struct device *dev)
{
	uint32_t cam_id;
	const struct spi_cam_config *cfg = dev->config;
	struct spi_cam_data *drv_data = dev->data;
	int ret = 0;

	ret = spi_cam_await_bus_idle(&cfg->spi, 255);
	if (ret < 0) {
		LOG_ERR("Bus idle wait failed during connection check");
		return ret;
	}

	ret = spi_cam_read_reg(&cfg->spi, CAM_REG_SENSOR_ID, &cam_id);
	if (ret < 0) {
		LOG_ERR("Failed to read sensor ID");
		return ret;
	}
	if (!(cam_id & 0x87)) {
		LOG_ERR("SPI camera not detected, 0x%x", cam_id);
		return -ENODEV;
	}
	drv_data->features = SPI_CAM_HAS_DEFAULT;

	switch (cam_id) {
	case SPI_CAM_SENSOR_5MP_1:
		drv_data->features |= SPI_CAM_HAS_FOCUS | SPI_CAM_HAS_COLORFX;
		break;
	case SPI_CAM_SENSOR_3MP_1:
	case SPI_CAM_SENSOR_3MP_2:
		drv_data->features |= SPI_CAM_HAS_SHARPNESS | SPI_CAM_HAS_COLORFX;
		break;
	case SPI_CAM_SENSOR_5MP_2:
		drv_data->features |= SPI_CAM_HAS_FOCUS | SPI_CAM_HAS_COLORFX;
		break;
	default:
		return -ENODEV;
	}

	drv_data->camera_id = cam_id;

	return ret;
}

/* -------------------------------------------------------------------------
 * Format get/set
 * ---------------------------------------------------------------------- */

static int spi_cam_set_format(const struct device *dev, struct video_format *fmt)
{
	struct spi_cam_data *drv_data = dev->data;
	int ret = 0;

	if (!memcmp(&drv_data->fmt, fmt, sizeof(drv_data->fmt))) {
		/* nothing to do */
		return 0;
	}

	if (fmt->type != VIDEO_BUF_TYPE_OUTPUT || fmt->pixelformat != VIDEO_PIX_FMT_GREY ||
	    fmt->width != SPI_CAM_FRAME_WIDTH || fmt->height != SPI_CAM_FRAME_HEIGHT) {
		LOG_ERR("Unsupported pixel format or resolution %s %ux%u",
			VIDEO_FOURCC_TO_STR(fmt->pixelformat), fmt->width, fmt->height);
		return -ENOTSUP;
	}

	ret = video_estimate_fmt_size(fmt);
	if (ret < 0) {
		return ret;
	}

	ret = spi_cam_set_output_format(dev);
	if (ret < 0) {
		return ret;
	}

	ret = spi_cam_set_resolution(dev, SPI_CAM_RESOLUTION_96X96);
	if (ret < 0) {
		return ret;
	}

	drv_data->fmt = *fmt;

	return 0;
}

static int spi_cam_get_format(const struct device *dev, struct video_format *fmt)
{
	struct spi_cam_data *drv_data = dev->data;

	*fmt = drv_data->fmt;

	return 0;
}

/* -------------------------------------------------------------------------
 * Capture cadence
 * ---------------------------------------------------------------------- */

/*
 * Enforce a real minimum capture interval (not just a documented one): the
 * driver-controlled cadence must never be shorter than the worst-case time
 * to capture a frame and drain it out of the FIFO over SPI, for whatever
 * format/resolution is currently configured. This is deliberately
 * conservative; see SPI_CAM_CAPTURE_OVERHEAD_MS.
 */
static uint32_t spi_cam_min_capture_interval_ms(const struct device *dev)
{
	const struct spi_cam_config *cfg = dev->config;
	const struct spi_cam_data *drv_data = dev->data;
	uint32_t freq_hz = cfg->spi.config.frequency;
	uint32_t transfer_ms;

	if (freq_hz == 0 || drv_data->fmt.size == 0) {
		return CONFIG_DOOR_LOCK_CAMERA_SPI_CAPTURE_INTERVAL_MS_DEFAULT;
	}

	/* The video output is one byte per pixel, but two YUYV bytes per pixel
	 * must be transferred from the sensor.
	 */
	transfer_ms = (uint32_t)(((uint64_t)SPI_CAM_YUYV_FRAME_BYTES * 8ULL * 1000ULL +
				  freq_hz - 1) /
				 freq_hz);

	return transfer_ms + SPI_CAM_CAPTURE_OVERHEAD_MS;
}

static void spi_cam_stream_schedule(struct k_timer *timer)
{
	struct spi_cam_data *drv_data = timer->user_data;

	GestureAccessWorkqueueSubmit(&drv_data->buf_work);
}

static int spi_cam_apply_capture_interval(const struct device *dev, uint32_t requested_ms)
{
	struct spi_cam_data *drv_data = dev->data;
	uint32_t min_ms = spi_cam_min_capture_interval_ms(dev);
	uint32_t applied_ms = requested_ms;

	if (applied_ms < min_ms) {
		LOG_WRN("Requested capture interval %u ms below minimum %u ms for current "
			"format, clamping",
			requested_ms, min_ms);
		applied_ms = min_ms;
	}

	drv_data->capture_interval_ms = applied_ms;

	if (drv_data->stream_on) {
		k_timer_start(&drv_data->stream_schedule_timer, K_MSEC(applied_ms),
			      K_MSEC(applied_ms));
	}

	return 0;
}

/* -------------------------------------------------------------------------
 * Stream start/stop, flush
 * ---------------------------------------------------------------------- */

static int spi_cam_stream_start(const struct device *dev, bool enable, enum video_buf_type type)
{
	struct spi_cam_data *drv_data = dev->data;

	ARG_UNUSED(type);

	if (drv_data->stream_on == enable) {
		return 0;
	}

	if (enable) {
		drv_data->stream_on = 1;
		k_timer_start(&drv_data->stream_schedule_timer,
			      K_MSEC(drv_data->capture_interval_ms),
			      K_MSEC(drv_data->capture_interval_ms));
	} else {
		drv_data->stream_on = 0;
		k_timer_stop(&drv_data->stream_schedule_timer);
	}

	return 0;
}

static int spi_cam_flush(const struct device *dev, bool cancel)
{
	struct spi_cam_data *drv_data = dev->data;
	struct video_buffer *vbuf;

	ARG_UNUSED(cancel);

	/* Clear fifo cache */
	while (!k_fifo_is_empty(&drv_data->fifo_out)) {
		vbuf = k_fifo_get(&drv_data->fifo_out, K_USEC(10));
		if (vbuf != NULL) {
			k_fifo_put(&drv_data->fifo_in, vbuf);
		}
	}
	return 0;
}

static int spi_cam_soft_reset(const struct device *dev)
{
	const struct spi_cam_config *cfg = dev->config;
	struct spi_cam_data *drv_data = dev->data;
	int ret = 0;

	if (drv_data->stream_on) {
		spi_cam_stream_start(dev, false, VIDEO_BUF_TYPE_OUTPUT);
	}
	/* Initiate system reset */
	ret = spi_cam_write_reg(&cfg->spi, CAM_REG_SENSOR_RESET, SENSOR_RESET_ENABLE);
	if (ret < 0) {
		LOG_ERR("Failed to reset the sensor (%d)", ret);
		return ret;
	}

	k_msleep(1000);

	return ret;
}

/* -------------------------------------------------------------------------
 * Capture + FIFO read
 * ---------------------------------------------------------------------- */

static int spi_cam_capture(const struct device *dev, uint32_t *length)
{
	const struct spi_cam_config *cfg = dev->config;
	bool capture_done = false;
	int ret;
	uint32_t reg_data;

	ret = spi_cam_write_reg(&cfg->spi, ARDUCHIP_FIFO, FIFO_CLEAR_ID_MASK);
	if (ret < 0) {
		return ret;
	}

	ret = spi_cam_write_reg(&cfg->spi, ARDUCHIP_FIFO, FIFO_START_MASK);
	if (ret < 0) {
		return ret;
	}

	for (int tries = SPI_CAM_CAPTURE_TRIES; tries > 0; tries--) {
		ret = spi_cam_read_reg(&cfg->spi, ARDUCHIP_TRIG, &reg_data);
		if (ret < 0) {
			LOG_ERR("Capture timeout!");
			return ret;
		}

		if (reg_data & CAP_DONE_MASK) {
			capture_done = true;
			break;
		}

		k_msleep(2);
	}

	if (!capture_done) {
		LOG_ERR("Capture timeout");
		return -ETIMEDOUT;
	}

	ret = spi_cam_read_reg(&cfg->spi, FIFO_SIZE1, &reg_data);
	if (ret < 0) {
		LOG_ERR("Failed to read the fifo1 size (%d)", ret);
		return ret;
	}
	*length = reg_data;
	ret = spi_cam_read_reg(&cfg->spi, FIFO_SIZE2, &reg_data);
	if (ret < 0) {
		LOG_ERR("Failed to read the fifo2 size (%d)", ret);
		return ret;
	}
	*length |= reg_data << 8;
	ret = spi_cam_read_reg(&cfg->spi, FIFO_SIZE3, &reg_data);
	if (ret < 0) {
		LOG_ERR("Failed to read the fifo3 size (%d)", ret);
		return ret;
	}
	*length |= reg_data << 16;
	return 0;
}

static int spi_cam_fifo_read(const struct device *dev, struct video_buffer *buf,
			     uint32_t fifo_length)
{
	const struct spi_cam_config *cfg = dev->config;
	struct spi_cam_data *drv_data = dev->data;
	int ret;

	if (fifo_length != SPI_CAM_YUYV_FRAME_BYTES) {
		LOG_ERR("Unexpected YUYV frame size %u (expected %u)", fifo_length,
			SPI_CAM_YUYV_FRAME_BYTES);
		return -EMSGSIZE;
	}

	if (buf->size < SPI_CAM_GRAY_FRAME_BYTES) {
		LOG_ERR("Video buffer too small: %u (need %u)", (uint32_t)buf->size,
			SPI_CAM_GRAY_FRAME_BYTES);
		return -ENOBUFS;
	}

	ret = spi_cam_read_block(&cfg->spi, drv_data->frame_buf, SPI_CAM_YUYV_FRAME_BYTES, true);
	if (ret < 0) {
		LOG_ERR("Failed to read block (%d)", ret);
		return ret;
	}

	drv_data->frame_count++;
	if (drv_data->frame_count <= 8U) {
		uint32_t even_sum = 0;
		uint32_t odd_sum = 0;
		uint8_t even_min = UINT8_MAX;
		uint8_t even_max = 0;
		uint8_t odd_min = UINT8_MAX;
		uint8_t odd_max = 0;

		for (size_t i = 0; i < SPI_CAM_GRAY_FRAME_BYTES; i++) {
			uint8_t even = drv_data->frame_buf[i * 2U];
			uint8_t odd = drv_data->frame_buf[i * 2U + 1U];

			even_min = MIN(even_min, even);
			even_max = MAX(even_max, even);
			odd_min = MIN(odd_min, odd);
			odd_max = MAX(odd_max, odd);
			even_sum += even;
			odd_sum += odd;
		}

		LOG_INF("Raw frame %u even[min=%u max=%u avg=%u] odd[min=%u max=%u avg=%u]",
			drv_data->frame_count, even_min, even_max,
			even_sum / SPI_CAM_GRAY_FRAME_BYTES, odd_min, odd_max,
			odd_sum / SPI_CAM_GRAY_FRAME_BYTES);
	}

	/* YUYV stores one luminance byte for every pixel at each even index.
	 * Compact those bytes in place; the next capture overwrites this storage.
	 */
	for (size_t i = 0; i < SPI_CAM_GRAY_FRAME_BYTES; i++) {
		drv_data->frame_buf[i] = drv_data->frame_buf[i * 2U];
	}

	memcpy(buf->buffer, drv_data->frame_buf, SPI_CAM_GRAY_FRAME_BYTES);
	buf->bytesused = SPI_CAM_GRAY_FRAME_BYTES;

	return 0;
}

static void spi_cam_buffer_work(struct k_work *work)
{
	struct spi_cam_data *drv_data = CONTAINER_OF(work, struct spi_cam_data, buf_work);
	struct video_buffer *vbuf;
	uint32_t fifo_length;
	int ret;

	if (!drv_data->stream_on) {
		return;
	}

	vbuf = k_fifo_get(&drv_data->fifo_in, K_NO_WAIT);
	if (vbuf == NULL) {
		return;
	}

	ret = spi_cam_capture(drv_data->dev, &fifo_length);
	if (ret < 0) {
		LOG_ERR("Failed to capture frame (%d)", ret);
		k_fifo_put(&drv_data->fifo_in, vbuf);
		return;
	}

	vbuf->timestamp = k_uptime_get_32();

	ret = spi_cam_fifo_read(drv_data->dev, vbuf, fifo_length);
	if (ret < 0) {
		LOG_ERR("Failed to read frame (%d)", ret);
		k_fifo_put(&drv_data->fifo_in, vbuf);
		return;
	}

	k_fifo_put(&drv_data->fifo_out, vbuf);
}

/* -------------------------------------------------------------------------
 * Buffer enqueue/dequeue, capabilities
 * ---------------------------------------------------------------------- */

static int spi_cam_enqueue(const struct device *dev, struct video_buffer *buf)
{
	struct spi_cam_data *data = dev->data;

	k_fifo_put(&data->fifo_in, buf);

	LOG_DBG("enqueue buffer %p", buf->buffer);

	return 0;
}

static int spi_cam_dequeue(const struct device *dev, struct video_buffer **buf,
			   k_timeout_t timeout)
{
	struct spi_cam_data *data = dev->data;

	*buf = k_fifo_get(&data->fifo_out, timeout);
	if (*buf == NULL) {
		return -EAGAIN;
	}

	LOG_DBG("dequeue buffer %p", (*buf)->buffer);

	return 0;
}

static int spi_cam_get_caps(const struct device *dev, struct video_caps *caps)
{
	ARG_UNUSED(dev);

	/* This sensor, in capture mode, needs only one buffer allocated
	 * before starting.
	 */
	caps->min_vbuf_count = 1;
	caps->format_caps = fmts;
	return 0;
}

/* -------------------------------------------------------------------------
 * Controls
 * ---------------------------------------------------------------------- */

static int spi_cam_set_ctrl(const struct device *dev, uint32_t id)
{
	struct spi_cam_data *drv_data = dev->data;
	int ret = 0;

	switch (id) {
	case VIDEO_CID_EXPOSURE_AUTO:
		return spi_cam_set_exposure_enable(dev, drv_data->ctrls.exp_auto.val);
	case VIDEO_CID_EXPOSURE:
		return spi_cam_set_exposure(dev, drv_data->ctrls.exposure.val);
	case VIDEO_CID_AUTOGAIN:
		return spi_cam_set_gain_enable(dev, drv_data->ctrls.gain_auto.val);
	case VIDEO_CID_GAIN:
		return spi_cam_set_gain(dev, drv_data->ctrls.gain.val);
	case VIDEO_CID_BRIGHTNESS:
		return spi_cam_set_brightness(dev, drv_data->ctrls.brightness.val);
	case VIDEO_CID_SATURATION:
		return spi_cam_set_saturation(dev, drv_data->ctrls.saturation.val);
	case VIDEO_CID_AUTO_WHITE_BALANCE:
		return spi_cam_set_white_bal_enable(dev, drv_data->ctrls.whitebalauto.val);
	case VIDEO_CID_WHITE_BALANCE_TEMPERATURE:
		return spi_cam_set_white_bal(dev, drv_data->ctrls.whitebal.val);
	case VIDEO_CID_CONTRAST:
		return spi_cam_set_contrast(dev, drv_data->ctrls.contrast.val);
	case VIDEO_CID_AUTO_EXPOSURE_BIAS:
		return spi_cam_set_ev(dev, drv_data->ctrls.ev.val);
	case VIDEO_CID_SHARPNESS:
		return spi_cam_set_sharpness(dev, drv_data->ctrls.sharpness.val);
	case VIDEO_CID_FOCUS_AUTO:
		return spi_cam_set_auto_focus(dev, drv_data->ctrls.focus_auto.val);
	case VIDEO_CID_COLORFX:
		return spi_cam_set_special_effects(dev, drv_data->ctrls.colorfx.val);
	case VIDEO_CID_SPI_CAM_LOWPOWER:
		return spi_cam_set_lowpower_enable(dev, drv_data->ctrls.lowpower.val);
	case VIDEO_CID_SPI_CAM_CAPTURE_INTERVAL_MS:
		return spi_cam_apply_capture_interval(
			dev, (uint32_t)drv_data->ctrls.capture_interval_ms.val);
	case VIDEO_CID_SPI_CAM_RESET: {
		drv_data->ctrls.reset.val = 0;
		ret = spi_cam_soft_reset(dev);
		if (ret < 0) {
			return ret;
		}
		return spi_cam_check_connection(dev);
	}
	default:
		return -ENOTSUP;
	}
}

static DEVICE_API(video, spi_cam_driver_api) = {
	.set_format = spi_cam_set_format,
	.get_format = spi_cam_get_format,
	.set_ctrl = spi_cam_set_ctrl,
	.get_caps = spi_cam_get_caps,
	.set_stream = spi_cam_stream_start,
	.flush = spi_cam_flush,
	.enqueue = spi_cam_enqueue,
	.dequeue = spi_cam_dequeue,
};

static int spi_cam_init_controls(const struct device *dev)
{
	int ret = 0;
	struct spi_cam_data *drv_data = dev->data;
	struct spi_cam_ctrls *ctrls = &drv_data->ctrls;

	ret = video_init_ctrl(&ctrls->reset, dev, VIDEO_CID_SPI_CAM_RESET,
			      (struct video_ctrl_range){.min = 0, .max = 1, .step = 1, .def = 0});
	if (ret < 0) {
		return ret;
	}
	ret = video_init_ctrl(&ctrls->brightness, dev, VIDEO_CID_BRIGHTNESS,
			      (struct video_ctrl_range){.min = 0, .max = 8, .step = 1, .def = 0});
	if (ret < 0) {
		return ret;
	}
	ret = video_init_ctrl(&ctrls->contrast, dev, VIDEO_CID_CONTRAST,
			      (struct video_ctrl_range){.min = 0, .max = 6, .step = 1, .def = 0});
	if (ret < 0) {
		return ret;
	}
	ret = video_init_ctrl(&ctrls->saturation, dev, VIDEO_CID_SATURATION,
			      (struct video_ctrl_range){.min = 0, .max = 6, .step = 1, .def = 0});
	if (ret < 0) {
		return ret;
	}
	ret = video_init_ctrl(&ctrls->ev, dev, VIDEO_CID_AUTO_EXPOSURE_BIAS,
			      (struct video_ctrl_range){.min = 0, .max = 6, .step = 1, .def = 0});
	if (ret < 0) {
		return ret;
	}
	ret = video_init_ctrl(&ctrls->whitebal, dev, VIDEO_CID_WHITE_BALANCE_TEMPERATURE,
			      (struct video_ctrl_range){.min = 0, .max = 4, .step = 1, .def = 0});
	if (ret < 0) {
		return ret;
	}
	if (drv_data->features & SPI_CAM_HAS_COLORFX) {
		ret = video_init_ctrl(
			&ctrls->colorfx, dev, VIDEO_CID_COLORFX,
			(struct video_ctrl_range){.min = 0, .max = 14, .step = 1, .def = 0});
		if (ret < 0) {
			return ret;
		}
	}
	ret = video_init_ctrl(&ctrls->exp_auto, dev, VIDEO_CID_EXPOSURE_AUTO,
			      (struct video_ctrl_range){.min = 0, .max = 1, .step = 1, .def = 0});
	if (ret < 0) {
		return ret;
	}
	ret = video_init_ctrl(&ctrls->gain_auto, dev, VIDEO_CID_AUTOGAIN,
			      (struct video_ctrl_range){.min = 0, .max = 1, .step = 1, .def = 0});
	if (ret < 0) {
		return ret;
	}
	ret = video_init_ctrl(&ctrls->whitebalauto, dev, VIDEO_CID_AUTO_WHITE_BALANCE,
			      (struct video_ctrl_range){.min = 0, .max = 1, .step = 1, .def = 0});
	if (ret < 0) {
		return ret;
	}
	if (drv_data->features & SPI_CAM_HAS_SHARPNESS) {
		ret = video_init_ctrl(
			&ctrls->sharpness, dev, VIDEO_CID_SHARPNESS,
			(struct video_ctrl_range){.min = 0, .max = 8, .step = 1, .def = 0});
		if (ret < 0) {
			return ret;
		}
	}
	ret = video_init_ctrl(
		&ctrls->gain, dev, VIDEO_CID_GAIN,
		(struct video_ctrl_range){.min = 0, .max = 1023, .step = 1, .def = 0});
	if (ret < 0) {
		return ret;
	}
	ret = video_init_ctrl(
		&ctrls->exposure, dev, VIDEO_CID_EXPOSURE,
		(struct video_ctrl_range){.min = 0, .max = 30000, .step = 1, .def = 0});
	if (ret < 0) {
		return ret;
	}
	ret = video_init_ctrl(
		&ctrls->lowpower, dev, VIDEO_CID_SPI_CAM_LOWPOWER,
		(struct video_ctrl_range){.min = 0, .max = 65535, .step = 1, .def = 0});
	if (ret < 0) {
		return ret;
	}
	ret = video_init_ctrl(&ctrls->capture_interval_ms, dev,
			      VIDEO_CID_SPI_CAM_CAPTURE_INTERVAL_MS,
			      (struct video_ctrl_range){
				      .min = 0,
				      .max = INT32_MAX,
				      .step = 1,
				      .def = CONFIG_DOOR_LOCK_CAMERA_SPI_CAPTURE_INTERVAL_MS_DEFAULT});
	if (ret < 0) {
		return ret;
	}
	if (drv_data->features & SPI_CAM_HAS_FOCUS) {
		ret = video_init_ctrl(
			&ctrls->focus_auto, dev, VIDEO_CID_FOCUS_AUTO,
			(struct video_ctrl_range){.min = 0, .max = 1, .step = 1, .def = 0});
		if (ret < 0) {
			return ret;
		}
	}
	return 0;
}

/* -------------------------------------------------------------------------
 * Init
 * ---------------------------------------------------------------------- */

static int spi_cam_init(const struct device *dev)
{
	const struct spi_cam_config *cfg = dev->config;
	struct spi_cam_data *drv_data = dev->data;
	struct video_format fmt = {0};
	int ret = 0;
	uint32_t reg_data;

	if (!spi_is_ready_dt(&cfg->spi)) {
		LOG_ERR("SPI bus not ready");
		return -ENODEV;
	}

	drv_data->dev = dev;
	drv_data->capture_interval_ms = CONFIG_DOOR_LOCK_CAMERA_SPI_CAPTURE_INTERVAL_MS_DEFAULT;
	k_fifo_init(&drv_data->fifo_in);
	k_fifo_init(&drv_data->fifo_out);

	k_timer_init(&drv_data->stream_schedule_timer, spi_cam_stream_schedule, NULL);
	drv_data->stream_schedule_timer.user_data = (void *)drv_data;

	k_work_init(&drv_data->buf_work, spi_cam_buffer_work);

	ret = spi_cam_soft_reset(dev);
	if (ret < 0) {
		LOG_ERR("SPI camera reset failed");
		return ret;
	}

	ret = spi_cam_check_connection(dev);
	if (ret < 0) {
		LOG_ERR("SPI camera not connected");
		return ret;
	}

	ret = spi_cam_read_reg(&cfg->spi, CAM_REG_YEAR_SDK, &reg_data);
	if (ret < 0) {
		LOG_ERR("Failed to read year (%d)", ret);
		return ret;
	}

	uint8_t year = reg_data & 0x3F;

	ret = spi_cam_read_reg(&cfg->spi, CAM_REG_MONTH_SDK, &reg_data);
	if (ret < 0) {
		LOG_ERR("Failed to read month (%d)", ret);
		return ret;
	}

	uint8_t month = reg_data & 0x0F;

	ret = spi_cam_read_reg(&cfg->spi, CAM_REG_DAY_SDK, &reg_data);
	if (ret < 0) {
		LOG_ERR("Failed to read day (%d)", ret);
		return ret;
	}

	uint8_t day = reg_data & 0x1F;

	ret = spi_cam_read_reg(&cfg->spi, CAM_REG_FPGA_VERSION_NUMBER, &reg_data);
	if (ret < 0) {
		LOG_ERR("Failed to read version number (%d)", ret);
		return ret;
	}

	uint8_t version = reg_data & 0xFF;

	LOG_INF("SPI camera sensor 0x%x, fw %d-%d-%d v%x", drv_data->camera_id, year, month, day,
		version);

	/* set default/init format */
	fmt.type = VIDEO_BUF_TYPE_OUTPUT;
	fmt.pixelformat = VIDEO_PIX_FMT_GREY;
	fmt.width = SPI_CAM_FRAME_WIDTH;
	fmt.height = SPI_CAM_FRAME_HEIGHT;

	ret = spi_cam_set_format(dev, &fmt);
	if (ret < 0) {
		LOG_ERR("Unable to configure default format");
		return ret;
	}
	ret = spi_cam_init_controls(dev);
	if (ret < 0) {
		LOG_ERR("Unable to initialize controls");
		return ret;
	}
	return 0;
}

#define SPI_CAM_INIT(inst)                                                                        \
	static const struct spi_cam_config spi_cam_config_##inst = {                             \
		.spi = SPI_DT_SPEC_INST_GET(                                                      \
			inst,                                                                     \
			SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_LINES_SINGLE | SPI_LOCK_ON),   \
	};                                                                                         \
                                                                                                   \
	static struct spi_cam_data spi_cam_data_##inst;                                          \
                                                                                                   \
	PM_DEVICE_DT_INST_DEFINE(inst, spi_cam_pm_action);                                        \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, &spi_cam_init, PM_DEVICE_DT_INST_GET(inst),                   \
			      &spi_cam_data_##inst, &spi_cam_config_##inst, POST_KERNEL,          \
			      CONFIG_VIDEO_INIT_PRIORITY, &spi_cam_driver_api);                  \
                                                                                                   \
	VIDEO_DEVICE_DEFINE(spi_cam_##inst, DEVICE_DT_INST_GET(inst), NULL);

DT_INST_FOREACH_STATUS_OKAY(SPI_CAM_INIT)
