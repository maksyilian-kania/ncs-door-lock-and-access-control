/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <gesture_access_model/gesture_access_model.h>

#include <zephyr/kernel.h>

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
constexpr char kDetectionLabel[] = "hand";

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

uint8_t OutputActivation(const int8_t *output, size_t row, size_t column)
{
	constexpr size_t kHandChannel = 1;
	const size_t offset = kHandChannel * (kOutputWidth * kOutputHeight) + (row * kOutputWidth) + column;

	return static_cast<uint8_t>(static_cast<int16_t>(output[offset]) + 128);
}

bool IsDetection(const int8_t *output, size_t row, size_t column)
{
	return OutputActivation(output, row, column) * 1000U >= kDetectionThresholdMilli * 256U;
}

void DecodeOutput(const int8_t *output, Result &result)
{
	bool visited[kOutputWidth * kOutputHeight] = {};
	uint8_t pending[kOutputWidth * kOutputHeight];

	for (size_t startRow = 0; startRow < kOutputHeight; startRow++) {
		for (size_t startColumn = 0; startColumn < kOutputWidth; startColumn++) {
			const size_t startIndex = startRow * kOutputWidth + startColumn;

			if (visited[startIndex] || !IsDetection(output, startRow, startColumn)) {
				continue;
			}

			size_t pendingRead = 0;
			size_t pendingWrite = 0;
			size_t minRow = startRow;
			size_t maxRow = startRow;
			size_t minColumn = startColumn;
			size_t maxColumn = startColumn;
			uint8_t maxActivation = OutputActivation(output, startRow, startColumn);

			visited[startIndex] = true;
			pending[pendingWrite++] = static_cast<uint8_t>(startIndex);

			while (pendingRead < pendingWrite) {
				const size_t index = pending[pendingRead++];
				const int row = static_cast<int>(index / kOutputWidth);
				const int column = static_cast<int>(index % kOutputWidth);

				for (int rowDelta = -1; rowDelta <= 1; rowDelta++) {
					for (int columnDelta = -1; columnDelta <= 1; columnDelta++) {
						if (rowDelta == 0 && columnDelta == 0) {
							continue;
						}

						const int adjacentRow = row + rowDelta;
						const int adjacentColumn = column + columnDelta;

						if (adjacentRow < 0 || adjacentRow >= static_cast<int>(kOutputHeight) ||
						    adjacentColumn < 0 ||
						    adjacentColumn >= static_cast<int>(kOutputWidth)) {
							continue;
						}

						const size_t adjacentIndex =
							static_cast<size_t>(adjacentRow) * kOutputWidth +
							static_cast<size_t>(adjacentColumn);

						if (visited[adjacentIndex] ||
						    !IsDetection(output, adjacentRow, adjacentColumn)) {
							continue;
						}

						visited[adjacentIndex] = true;
						pending[pendingWrite++] = static_cast<uint8_t>(adjacentIndex);

						const size_t adjacentRowSize = static_cast<size_t>(adjacentRow);
						const size_t adjacentColumnSize = static_cast<size_t>(adjacentColumn);
						minRow = MIN(minRow, adjacentRowSize);
						maxRow = MAX(maxRow, adjacentRowSize);
						minColumn = MIN(minColumn, adjacentColumnSize);
						maxColumn = MAX(maxColumn, adjacentColumnSize);
						maxActivation = MAX(maxActivation, OutputActivation(output, adjacentRow,
												    adjacentColumn));
					}
				}
			}

			BoundingBox &box = result.boxes[result.boxCount++];

			box.label = kDetectionLabel;
			box.x = static_cast<uint16_t>(minColumn * kCellSize);
			box.y = static_cast<uint16_t>(minRow * kCellSize);
			box.width = static_cast<uint16_t>((maxColumn - minColumn + 1) * kCellSize);
			box.height = static_cast<uint16_t>((maxRow - minRow + 1) * kCellSize);
			box.confidenceMilli = static_cast<uint16_t>((maxActivation * 1000U + 128U) / 256U);
		}
	}
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
