/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "platform/nfc/apdu_fragment_assembler.h"

#include <cstring>

namespace AliroUd::Nfc {

ApduFragmentAssembler::Result ApduFragmentAssembler::AddFragment(const uint8_t *data, size_t length, bool more)
{
	if (length > 0) {
		if ((mLength + length) > mBuffer.size()) {
			Reset();
			return Result::Overflow;
		}

		std::memcpy(&mBuffer[mLength], data, length);
		mLength += length;
	}

	return more ? Result::Incomplete : Result::Complete;
}

} // namespace AliroUd::Nfc
