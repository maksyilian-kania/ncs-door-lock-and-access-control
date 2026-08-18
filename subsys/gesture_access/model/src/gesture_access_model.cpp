/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <gesture_access_model/gesture_access_model.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <axon/nrf_axon_platform.h>
#include <drivers/axon/nrf_axon_driver.h>
#include <drivers/axon/nrf_axon_nn_infer.h>

#include <cerrno>
#include <cstring>

#define NRF_AXON_MODEL_ALLOCATE_PACKED_OUTPUT_BUFFER 1
#include "nrf_axon_model_fomo_.h"

namespace DoorLock::GestureAccess::Model {

namespace {

constexpr size_t kOutputWidth = 12;
constexpr size_t kOutputHeight = 12;
constexpr size_t kOutputChannels = 2;
constexpr size_t kOutputSize = kOutputWidth * kOutputHeight * kOutputChannels;
constexpr size_t kCellSize = kInputWidth / kOutputWidth;
constexpr uint16_t kDetectionThresholdMilli = 950;
constexpr size_t kHandChannel = 1;

static_assert(kOutputSize == NRF_AXON_MODEL_FOMO_PACKED_OUTPUT_SIZE);
static_assert(kInputWidth % kOutputWidth == 0);
static_assert(kInputHeight % kOutputHeight == 0);

int8_t sInput[kInputSize];
bool sInitialized;

bool ModelContractMatches()
{
	const nrf_axon_nn_compiled_model_input_s &input = model_fomo.inputs[model_fomo.external_input_ndx];

	return input.dimensions.width == kInputWidth && input.dimensions.height == kInputHeight &&
	       input.dimensions.channel_cnt == 1 && input.dimensions.byte_width == sizeof(int8_t) &&
	       input.quant_mult == 133693432 && input.quant_round == 19 && input.quant_zp == -128 &&
	       model_fomo.output_dimensions.width == kOutputWidth &&
	       model_fomo.output_dimensions.height == kOutputHeight &&
	       model_fomo.output_dimensions.channel_cnt == kOutputChannels &&
	       model_fomo.output_dimensions.byte_width == sizeof(int8_t) && model_fomo.output_dequant_mult == 2097152 &&
	       model_fomo.output_dequant_round == 29 && model_fomo.output_dequant_zp == -128;
}

/*
 * Dequantizes one "hand" channel cell to an unsigned activation in [0, 255].
 * The output tensor is CHW (channel-major): all kOutputWidth*kOutputHeight
 * background values come first, then all "hand" values.
 */
uint8_t OutputActivation(const int8_t *output, size_t row, size_t column)
{
	const size_t offset = kHandChannel * (kOutputWidth * kOutputHeight) + (row * kOutputWidth) + column;

	return static_cast<uint8_t>(static_cast<int16_t>(output[offset]) + 128);
}

bool IsDetection(uint8_t activation)
{
	return activation * 1000U >= kDetectionThresholdMilli * 256U;
}


void DecodeOutput(const int8_t *output, Result &result)
{
	uint8_t bestActivation = 0;

	for (size_t row = 0; row < kOutputHeight; row++) {
		for (size_t column = 0; column < kOutputWidth; column++) {
			const uint8_t activation = OutputActivation(output, row, column);

			bestActivation = MAX(bestActivation, activation);

#ifdef CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING
			if (IsDetection(activation) && result.detectionCount < kMaxReportedDetections) {
				result.detections[result.detectionCount++] = {
					static_cast<uint16_t>(column * kCellSize + kCellSize / 2),
					static_cast<uint16_t>(row * kCellSize + kCellSize / 2),
					static_cast<uint16_t>((activation * 1000U + 128U) / 256U),
				};
			}
#endif // CONFIG_DOOR_LOCK_GESTURE_ACCESS_FRAME_FORWARDING
		}
	}

	result.confidenceMilli = static_cast<uint16_t>((bestActivation * 1000U + 128U) / 256U);
	result.detected = IsDetection(bestActivation);
}

} // namespace

int Init()
{
	if (sInitialized) {
		return 0;
	}

	if (!ModelContractMatches()) {
		return -ENOTSUP;
	}

	nrf_axon_result_e axonResult = nrf_axon_platform_init();

	if (axonResult != NRF_AXON_RESULT_SUCCESS) {
		return -EIO;
	}

	axonResult = nrf_axon_nn_model_validate(&model_fomo);
	if (axonResult != NRF_AXON_RESULT_SUCCESS) {
		return -EIO;
	}

	sInitialized = true;
	return 0;
}

int Run(const uint8_t *frame, size_t frameSize, Result &result)
{
	if (!sInitialized || frame == nullptr || frameSize != kInputSize) {
		return -EINVAL;
	}

	std::memset(&result, 0, sizeof(result));
	for (size_t i = 0; i < kInputSize; i++) {
		sInput[i] = static_cast<int8_t>(static_cast<int16_t>(frame[i]) - 128);
	}

	const uint64_t startCycles = k_cycle_get_64();
	const nrf_axon_result_e axonResult =
		nrf_axon_nn_model_infer_sync(&model_fomo, sInput, model_fomo.packed_output_buf);
	result.inferenceTimeUs = static_cast<uint32_t>(k_cyc_to_us_floor64(k_cycle_get_64() - startCycles));

	if (axonResult != NRF_AXON_RESULT_SUCCESS) {
		return -EIO;
	}

	DecodeOutput(model_fomo.packed_output_buf, result);
	return 0;
}

} // namespace DoorLock::GestureAccess::Model
