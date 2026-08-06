/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/*
 * TODO: implement the SPI camera driver against Zephyr's `video` driver
 * class (zephyr/include/zephyr/drivers/video.h)
 *   - set_format() / get_format()
 *   - set_stream() (start/stop capture)
 *   - enqueue() / dequeue() of video_buffer for captured frames
 *   - get_caps() advertising supported pixel formats/resolutions
 *
 */

#include <spi_cam.h>

#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/video.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(door_lock_spi_cam, CONFIG_DOOR_LOCK_CAMERA_SPI_LOG_LEVEL);

#define DT_DRV_COMPAT door_lock_spi_cam

struct spi_cam_config {
	struct spi_dt_spec spi;
};

struct spi_cam_data {
	/* TODO: current format, buffer queues, sensor state, etc. */
};

static int spi_cam_set_format(const struct device *dev, struct video_format *fmt)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(fmt);

	/* TODO: validate and apply the requested format. */
	return -ENOSYS;
}

static int spi_cam_get_format(const struct device *dev, struct video_format *fmt)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(fmt);

	/* TODO: report the currently configured format. */
	return -ENOSYS;
}

static int spi_cam_set_stream(const struct device *dev, bool enable, enum video_buf_type type)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(enable);
	ARG_UNUSED(type);

	/* TODO: start/stop the capture stream. */
	return -ENOSYS;
}

static int spi_cam_enqueue(const struct device *dev, struct video_buffer *buf)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(buf);

	/* TODO: queue a buffer to receive/hold captured frame data. */
	return -ENOSYS;
}

static int spi_cam_dequeue(const struct device *dev, struct video_buffer **buf, k_timeout_t timeout)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(buf);
	ARG_UNUSED(timeout);

	/* TODO: return the next filled buffer. */
	return -ENOSYS;
}

static int spi_cam_get_caps(const struct device *dev, struct video_caps *caps)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(caps);

	/* TODO: advertise supported pixel formats/resolutions. */
	return -ENOSYS;
}

static DEVICE_API(video, spi_cam_driver_api) = {
	.set_format = spi_cam_set_format,
	.get_format = spi_cam_get_format,
	.set_stream = spi_cam_set_stream,
	.enqueue = spi_cam_enqueue,
	.dequeue = spi_cam_dequeue,
	.get_caps = spi_cam_get_caps,
};

static int spi_cam_init(const struct device *dev)
{
	const struct spi_cam_config *config = dev->config;

	if (!spi_is_ready_dt(&config->spi)) {
		LOG_ERR("SPI bus not ready");
		return -ENODEV;
	}

	/* TODO: probe the sensor and bring it to a known idle state. */
	return 0;
}

#define SPI_CAM_INIT(inst)                                                                        \
	static const struct spi_cam_config spi_cam_config_##inst = {                              \
		.spi = SPI_DT_SPEC_INST_GET(inst, SPI_WORD_SET(8), 0),                            \
	};                                                                                         \
	static struct spi_cam_data spi_cam_data_##inst;                                          \
	DEVICE_DT_INST_DEFINE(inst, spi_cam_init, NULL, &spi_cam_data_##inst,                     \
			      &spi_cam_config_##inst, POST_KERNEL, CONFIG_VIDEO_INIT_PRIORITY,    \
			      &spi_cam_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SPI_CAM_INIT)
