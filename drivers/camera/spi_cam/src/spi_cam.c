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
 *  - Adds a grayscale pixel format (VIDEO_PIX_FMT_GREY). The sensor has no
 *    native grayscale mode, so this selects the YUYV wire format under the
 *    hood and strips the Y bytes while filling the video_buffer.
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

/* Configure JPEG image quality */
enum spi_cam_image_quality {
	SPI_CAM_IMAGE_QUALITY_HIGH = 0,
	SPI_CAM_IMAGE_QUALITY_DEFAULT = 1,
	SPI_CAM_IMAGE_QUALITY_LOW = 2,
};

enum {
	SPI_CAM_SENSOR_5MP_1 = 0x81,
	SPI_CAM_SENSOR_3MP_1 = 0x82,
	SPI_CAM_SENSOR_5MP_2 = 0x83, /* 2592x1936 */
	SPI_CAM_SENSOR_3MP_2 = 0x84,
};

/* Configure camera pixel format, as sent over the wire to the sensor. */
enum spi_cam_pixelformat {
	SPI_CAM_PIXELFORMAT_JPG = 0x01,
	SPI_CAM_PIXELFORMAT_RGB565 = 0x02,
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
#define CAM_REG_IMAGE_QUALITY                 0x2A
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
	struct video_ctrl quality;
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
	/* Read only registers */
	struct video_ctrl linkfreq;
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
	uint8_t fifo_first_read;
	uint32_t fifo_length;
	uint8_t stream_on;
	uint32_t features;
	uint32_t camera_id;
	uint32_t capture_interval_ms;
};

#define SPI_CAM_VIDEO_FORMAT_CAP(width, height, format)                                          \
	{.pixelformat = (format),                                                                 \
	 .width_min = (width),                                                                    \
	 .width_max = (width),                                                                    \
	 .height_min = (height),                                                                  \
	 .height_max = (height),                                                                  \
	 .width_step = 0,                                                                         \
	 .height_step = 0}

#define SPI_CAM_FMT_BLOCK(format)                                                                 \
	SPI_CAM_VIDEO_FORMAT_CAP(96, 96, format),                                                 \
	SPI_CAM_VIDEO_FORMAT_CAP(128, 128, format),                                               \
	SPI_CAM_VIDEO_FORMAT_CAP(320, 240, format),                                               \
	SPI_CAM_VIDEO_FORMAT_CAP(320, 320, format),                                               \
	SPI_CAM_VIDEO_FORMAT_CAP(640, 480, format),                                               \
	SPI_CAM_VIDEO_FORMAT_CAP(1280, 720, format),                                              \
	SPI_CAM_VIDEO_FORMAT_CAP(1600, 1200, format),                                             \
	SPI_CAM_VIDEO_FORMAT_CAP(1920, 1080, format),                                             \
	{0} /* replaced with the sensor-dependent max resolution in spi_cam_check_connection() */

/*
 * Each format block below has exactly SUPPORT_RESOLUTION_NUM (9) entries, so
 * `index % SUPPORT_RESOLUTION_NUM` always yields the right resolution enum
 * for spi_cam_set_resolution() regardless of which block matched.
 *
 * VIDEO_PIX_FMT_GREY has no native sensor mode: it reuses the YUYV wire
 * format (see spi_cam_set_output_format()) and the Y plane is extracted in
 * spi_cam_fifo_read().
 */
static struct video_format_cap fmts[] = {
	SPI_CAM_FMT_BLOCK(VIDEO_PIX_FMT_RGB565),
	SPI_CAM_FMT_BLOCK(VIDEO_PIX_FMT_JPEG),
	SPI_CAM_FMT_BLOCK(VIDEO_PIX_FMT_YUYV),
	SPI_CAM_FMT_BLOCK(VIDEO_PIX_FMT_GREY),
	{0},
};

#define SPI_CAM_FMT_RGB565_MAX_IDX 8
#define SPI_CAM_FMT_JPEG_MAX_IDX   17
#define SPI_CAM_FMT_YUYV_MAX_IDX   26
#define SPI_CAM_FMT_GREY_MAX_IDX   35

enum spi_cam_resolution {
	SPI_CAM_RESOLUTION_QQVGA = 0x00,
	SPI_CAM_RESOLUTION_QVGA = 0x01,
	SPI_CAM_RESOLUTION_VGA = 0x02,
	SPI_CAM_RESOLUTION_SVGA = 0x03,
	SPI_CAM_RESOLUTION_HD = 0x04,
	SPI_CAM_RESOLUTION_SXGAM = 0x05,
	SPI_CAM_RESOLUTION_UXGA = 0x06,
	SPI_CAM_RESOLUTION_FHD = 0x07,
	SPI_CAM_RESOLUTION_QXGA = 0x08,
	SPI_CAM_RESOLUTION_WQXGA2 = 0x09,
	SPI_CAM_RESOLUTION_96X96 = 0x0a,
	SPI_CAM_RESOLUTION_128X128 = 0x0b,
	SPI_CAM_RESOLUTION_320X320 = 0x0c,
	SPI_CAM_RESOLUTION_12 = 0x0d,
	SPI_CAM_RESOLUTION_13 = 0x0e,
	SPI_CAM_RESOLUTION_14 = 0x0f,
	SPI_CAM_RESOLUTION_15 = 0x10,
	SPI_CAM_RESOLUTION_NONE,
};

#define SUPPORT_RESOLUTION_NUM 9

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

static int spi_cam_set_auto_focus(const struct device *dev, enum spi_cam_auto_focus_level level)
{
	const struct spi_cam_config *cfg = dev->config;

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

static int spi_cam_set_output_format(const struct device *dev, uint32_t pixelformat)
{
	const struct spi_cam_config *cfg = dev->config;
	uint8_t format_val;
	int ret = 0;

	switch (pixelformat) {
	case VIDEO_PIX_FMT_JPEG:
		format_val = SPI_CAM_PIXELFORMAT_JPG;
		break;
	case VIDEO_PIX_FMT_RGB565:
		format_val = SPI_CAM_PIXELFORMAT_RGB565;
		break;
	case VIDEO_PIX_FMT_YUYV:
	case VIDEO_PIX_FMT_GREY:
		/* No native grayscale sensor mode: capture YUYV on the wire
		 * and strip the Y bytes in spi_cam_fifo_read().
		 */
		format_val = SPI_CAM_PIXELFORMAT_YUV;
		break;
	default:
		LOG_ERR("Image format not supported");
		return -ENOTSUP;
	}

	ret = spi_cam_write_reg_wait(&cfg->spi, CAM_REG_FORMAT, format_val, 3);
	if (ret < 0) {
		return ret;
	}

	ret = spi_cam_await_bus_idle(&cfg->spi, 30);
	if (ret < 0) {
		LOG_ERR("Bus idle wait failed after setting output format");
	}

	return ret;
}

static int spi_cam_set_jpeg_quality(const struct device *dev, enum spi_cam_image_quality qc)
{
	const struct spi_cam_config *cfg = dev->config;
	struct spi_cam_data *drv_data = dev->data;

	if (drv_data->fmt.pixelformat != VIDEO_PIX_FMT_JPEG) {
		LOG_ERR("Image format does not support setting JPEG quality");
		return -ENOTSUP;
	}

	return spi_cam_write_reg_wait(&cfg->spi, CAM_REG_IMAGE_QUALITY, qc, 3);
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

	uint32_t rgb565_w, rgb565_h, jpeg_w, jpeg_h, yuyv_w, yuyv_h;

	switch (cam_id) {
	case SPI_CAM_SENSOR_5MP_1:
		rgb565_w = jpeg_w = yuyv_w = 2592;
		rgb565_h = jpeg_h = yuyv_h = 1944;
		drv_data->features |= SPI_CAM_HAS_FOCUS | SPI_CAM_HAS_COLORFX;
		break;
	case SPI_CAM_SENSOR_3MP_1:
	case SPI_CAM_SENSOR_3MP_2:
		rgb565_w = jpeg_w = yuyv_w = 2048;
		rgb565_h = jpeg_h = yuyv_h = 1536;
		drv_data->features |= SPI_CAM_HAS_SHARPNESS | SPI_CAM_HAS_COLORFX;
		break;
	case SPI_CAM_SENSOR_5MP_2:
		rgb565_w = jpeg_w = yuyv_w = 2592;
		rgb565_h = jpeg_h = yuyv_h = 1936;
		drv_data->features |= SPI_CAM_HAS_FOCUS | SPI_CAM_HAS_COLORFX;
		break;
	default:
		return -ENODEV;
	}

	fmts[SPI_CAM_FMT_RGB565_MAX_IDX] =
		(struct video_format_cap)SPI_CAM_VIDEO_FORMAT_CAP(rgb565_w, rgb565_h,
								   VIDEO_PIX_FMT_RGB565);
	fmts[SPI_CAM_FMT_JPEG_MAX_IDX] = (struct video_format_cap)SPI_CAM_VIDEO_FORMAT_CAP(
		jpeg_w, jpeg_h, VIDEO_PIX_FMT_JPEG);
	fmts[SPI_CAM_FMT_YUYV_MAX_IDX] = (struct video_format_cap)SPI_CAM_VIDEO_FORMAT_CAP(
		yuyv_w, yuyv_h, VIDEO_PIX_FMT_YUYV);
	fmts[SPI_CAM_FMT_GREY_MAX_IDX] = (struct video_format_cap)SPI_CAM_VIDEO_FORMAT_CAP(
		yuyv_w, yuyv_h, VIDEO_PIX_FMT_GREY);

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
	size_t i = 0;

	if (!memcmp(&drv_data->fmt, fmt, sizeof(drv_data->fmt))) {
		/* nothing to do */
		return 0;
	}

	ret = video_estimate_fmt_size(fmt);
	if (ret < 0) {
		return ret;
	}

	ret = video_format_caps_index(fmts, fmt, &i);
	if (ret < 0) {
		LOG_ERR("Unsupported pixel format or resolution %s %ux%u",
			VIDEO_FOURCC_TO_STR(fmt->pixelformat), fmt->width, fmt->height);
		return ret;
	}

	drv_data->fmt = *fmt;

	ret = spi_cam_set_output_format(dev, fmt->pixelformat);
	if (ret < 0) {
		return ret;
	}

	ret = spi_cam_set_resolution(dev, i % SUPPORT_RESOLUTION_NUM);
	if (ret < 0) {
		return ret;
	}

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

	/* Single SPI data line, 8 bits per clock: ceil(bytes * 8 * 1000 / Hz). */
	transfer_ms = (uint32_t)(((uint64_t)drv_data->fmt.size * 8ULL * 1000ULL + freq_hz - 1) /
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
		drv_data->fifo_length = 0;
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
	struct spi_cam_data *drv_data = dev->data;
	int ret = 0;
	uint32_t reg_data;

	spi_cam_write_reg(&cfg->spi, ARDUCHIP_FIFO, FIFO_CLEAR_ID_MASK);
	spi_cam_write_reg(&cfg->spi, ARDUCHIP_FIFO, FIFO_START_MASK);

	for (int tries = SPI_CAM_CAPTURE_TRIES; tries > 0; tries--) {
		ret = spi_cam_read_reg(&cfg->spi, ARDUCHIP_TRIG, &reg_data);
		if (ret < 0) {
			LOG_ERR("Capture timeout!");
			return ret;
		}

		if (reg_data & CAP_DONE_MASK) {
			break;
		}

		k_msleep(2);
	}

	ret = spi_cam_read_reg(&cfg->spi, FIFO_SIZE1, &reg_data);
	if (ret < 0) {
		LOG_ERR("Failed to read the fifo1 size (%d)", ret);
		return ret;
	}
	drv_data->fifo_length = reg_data;
	ret = spi_cam_read_reg(&cfg->spi, FIFO_SIZE2, &reg_data);
	if (ret < 0) {
		LOG_ERR("Failed to read the fifo2 size (%d)", ret);
		return ret;
	}
	drv_data->fifo_length |= reg_data << 8;
	ret = spi_cam_read_reg(&cfg->spi, FIFO_SIZE3, &reg_data);
	if (ret < 0) {
		LOG_ERR("Failed to read the fifo3 size (%d)", ret);
		return ret;
	}
	drv_data->fifo_length |= reg_data << 16;

	drv_data->fifo_first_read = 1;
	*length = drv_data->fifo_length;
	return 0;
}

/*
 * Scratch chunk size used to read raw YUYV wire bytes off the SPI bus when
 * extracting a grayscale (Y-only) frame. Kept small and on-stack: it does
 * not need to hold a whole frame, only enough to make the burst reads
 * efficient. The gesture-access work queue stack
 * (CONFIG_DOOR_LOCK_GESTURE_ACCESS_WORKQUEUE_STACK_SIZE) must be able to fit
 * this on top of the rest of spi_cam_buffer_work()'s call chain.
 */
#define SPI_CAM_GRAY_CHUNK_WIRE_BYTES 256

static int spi_cam_fifo_read_gray(const struct device *dev, struct video_buffer *buf)
{
	const struct spi_cam_config *cfg = dev->config;
	struct spi_cam_data *drv_data = dev->data;
	uint8_t wire[SPI_CAM_GRAY_CHUNK_WIRE_BYTES];
	/* Sensor emits YUYV (2 bytes/pixel); only the Y byte of every pixel
	 * is kept, so at most 2x the caller's remaining buffer is consumed
	 * from the FIFO.
	 */
	uint32_t max_wire_len = (uint32_t)buf->size * 2;
	uint32_t total_wire_len =
		drv_data->fifo_length > max_wire_len ? max_wire_len : drv_data->fifo_length;
	uint32_t wire_read = 0;
	uint32_t pixels_written = 0;

	while (wire_read < total_wire_len) {
		uint32_t chunk_len = total_wire_len - wire_read;
		int ret;

		if (chunk_len > sizeof(wire)) {
			chunk_len = sizeof(wire);
		}
		/* Burst reads must consume an even number of wire bytes to
		 * stay pixel (Y,U/V pair) aligned; any odd remainder is
		 * picked up by the final, smaller chunk.
		 */
		chunk_len &= ~1U;
		if (chunk_len == 0) {
			break;
		}

		ret = spi_cam_read_block(&cfg->spi, wire, chunk_len,
					 drv_data->fifo_first_read && wire_read == 0);
		if (ret < 0) {
			LOG_ERR("Failed to read block (%d)", ret);
			return ret;
		}

		for (uint32_t i = 0; i < chunk_len / 2; i++) {
			buf->buffer[pixels_written++] = wire[i * 2];
		}

		wire_read += chunk_len;
	}

	drv_data->fifo_length -= wire_read;
	buf->bytesused = pixels_written;

	return 0;
}

static int spi_cam_fifo_read(const struct device *dev, struct video_buffer *buf)
{
	int ret = 0;
	int32_t rlen;
	const struct spi_cam_config *cfg = dev->config;
	struct spi_cam_data *drv_data = dev->data;

	if (drv_data->fmt.pixelformat == VIDEO_PIX_FMT_GREY) {
		ret = spi_cam_fifo_read_gray(dev, buf);
		if (ret < 0) {
			return ret;
		}
		drv_data->fifo_first_read = 0;
		return 0;
	}

	rlen = buf->size > drv_data->fifo_length ? drv_data->fifo_length : buf->size;

	LOG_DBG("read fifo: %u. - fifo_length %u", buf->size, drv_data->fifo_length);

	ret = spi_cam_read_block(&cfg->spi, buf->buffer, rlen, drv_data->fifo_first_read);
	if (ret < 0) {
		LOG_ERR("Failed to read block (%d)", ret);
		return ret;
	}

	drv_data->fifo_length -= rlen;
	buf->bytesused = rlen;
	if (drv_data->fifo_first_read) {
		drv_data->fifo_first_read = 0;
	}

	return 0;
}

static void spi_cam_buffer_work(struct k_work *work)
{
	struct spi_cam_data *drv_data = CONTAINER_OF(work, struct spi_cam_data, buf_work);
	static uint32_t f_timestamp, f_length;
	struct video_buffer *vbuf;
	int ret = 0;

	vbuf = k_fifo_get(&drv_data->fifo_in, K_NO_WAIT);
	if (vbuf == NULL) {
		GestureAccessWorkqueueSubmit(&drv_data->buf_work);
		return;
	}

	if (drv_data->fifo_length == 0) {
		spi_cam_capture(drv_data->dev, &f_length);
		f_timestamp = k_uptime_get_32();
	}

	ret = spi_cam_fifo_read(drv_data->dev, vbuf);
	if (ret < 0) {
		LOG_ERR("failed to read a buffer (%d)", ret);
		return;
	}

	if (drv_data->fifo_length != 0) {
		GestureAccessWorkqueueSubmit(&drv_data->buf_work);
	}

	vbuf->timestamp = f_timestamp;
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
	case VIDEO_CID_JPEG_COMPRESSION_QUALITY:
		return spi_cam_set_jpeg_quality(dev, drv_data->ctrls.quality.val);
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

#define SPI_CAM_640_480_LINK_FREQ      120000000
#define SPI_CAM_640_480_LINK_FREQ_ID   0
#define SPI_CAM_1600_1200_LINK_FREQ    240000000
#define SPI_CAM_1600_1200_LINK_FREQ_ID 1
static const int64_t spi_cam_link_frequency[] = {
	SPI_CAM_640_480_LINK_FREQ,
	SPI_CAM_1600_1200_LINK_FREQ,
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
		&ctrls->quality, dev, VIDEO_CID_JPEG_COMPRESSION_QUALITY,
		(struct video_ctrl_range){.min = 0, .max = 65535, .step = 1, .def = 0});
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
			(struct video_ctrl_range){.min = 0, .max = 65535, .step = 1, .def = 0});
		if (ret < 0) {
			return ret;
		}
	}
	/* Read only controls */
	ret = video_init_int_menu_ctrl(&ctrls->linkfreq, dev, VIDEO_CID_LINK_FREQ,
				       SPI_CAM_640_480_LINK_FREQ_ID, spi_cam_link_frequency,
				       ARRAY_SIZE(spi_cam_link_frequency));
	if (ret < 0) {
		return ret;
	}
	ctrls->linkfreq.flags |= VIDEO_CTRL_FLAG_READ_ONLY;

	return 0;
}

/* -------------------------------------------------------------------------
 * Init
 * ---------------------------------------------------------------------- */

static int spi_cam_init(const struct device *dev)
{
	const struct spi_cam_config *cfg = dev->config;
	struct spi_cam_data *drv_data = dev->data;
	struct video_format fmt;
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
	fmt.pixelformat = VIDEO_PIX_FMT_RGB565;
	fmt.width = 320;
	fmt.height = 240;

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
			SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_LINES_SINGLE | SPI_LOCK_ON,    \
			0),                                                                       \
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
