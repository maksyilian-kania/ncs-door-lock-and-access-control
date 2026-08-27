/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include "platform/nfc/nfc_worker.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include <etl/span.h>

namespace AliroUd::Nfc {

/**
 * @brief Reassembles chained raw ISO-DEP fragments into one complete
 * command APDU (APP_PLAN.md AWP1: "Keep transport fragment assembly in this
 * module. Do not parse Aliro APDUs.").
 *
 * Pure, host-testable logic with no `nfc_t4t_lib`/hardware dependency:
 * `nfc_transport.cpp` is the only caller and owns everything hardware
 * specific (the callback, sending the response).
 */
class ApduFragmentAssembler {
public:
	enum class Result {
		/** @brief More fragments are expected; nothing to do yet. */
		Incomplete,
		/** @brief The command APDU is fully assembled; see `GetAssembled()`. */
		Complete,
		/**
		 * @brief More raw fragment bytes were received than any valid
		 * short-length command APDU can hold. The assembler has already
		 * reset itself; the caller is responsible for responding to the
		 * transport-layer framing violation.
		 */
		Overflow,
	};

	/** @brief Discards any partially assembled command. */
	void Reset() { mLength = 0; }

	/**
	 * @brief Adds one raw ISO-DEP fragment.
	 *
	 * @param data Fragment bytes; may be `nullptr` if `length` is 0.
	 * @param length Number of bytes in `data`.
	 * @param more Whether the transport signaled that more fragments
	 * follow (`NFC_T4T_DI_FLAG_MORE`).
	 */
	Result AddFragment(const uint8_t *data, size_t length, bool more);

	/**
	 * @brief Gets the fully assembled command APDU.
	 *
	 * Only valid to call after `AddFragment()` returned `Result::Complete`
	 * and before the next `AddFragment()`/`Reset()` call.
	 */
	etl::span<const uint8_t> GetAssembled() const { return { mBuffer.data(), mLength }; }

private:
	std::array<uint8_t, kMaxApduLength> mBuffer{};
	size_t mLength{ 0 };
};

} // namespace AliroUd::Nfc
