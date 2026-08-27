/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/ztest.h>

#include "platform/nfc/apdu_fragment_assembler.h"

#include <vector>

using AliroUd::Nfc::ApduFragmentAssembler;
using AliroUd::Nfc::kMaxApduLength;

namespace {

std::vector<uint8_t> ToVector(etl::span<const uint8_t> span)
{
	return { span.begin(), span.end() };
}

} // namespace

ZTEST_SUITE(aliro_nfc_apdu_fragment_assembler, nullptr, nullptr, nullptr, nullptr, nullptr);

/** @brief A single, unchained fragment (final, `more == false`) is immediately complete. */
ZTEST(aliro_nfc_apdu_fragment_assembler, test_single_fragment_is_complete)
{
	ApduFragmentAssembler assembler{};
	const std::vector<uint8_t> apdu{ 0x00, 0xA4, 0x04, 0x00, 0x02, 0xAB, 0xCD };

	const auto result = assembler.AddFragment(apdu.data(), apdu.size(), false);

	zassert_equal(ApduFragmentAssembler::Result::Complete, result, "Expected the single fragment to complete");
	zassert_true(ToVector(assembler.GetAssembled()) == apdu, "Assembled command APDU mismatch");
}

/** @brief Chained fragments (`more == true`, ... , `more == false`) assemble in order. */
ZTEST(aliro_nfc_apdu_fragment_assembler, test_chained_fragments_assemble_in_order)
{
	ApduFragmentAssembler assembler{};
	const std::vector<uint8_t> fragment1{ 0x00, 0xA4, 0x04, 0x00 };
	const std::vector<uint8_t> fragment2{ 0x09, 0xA0, 0x00, 0x00 };
	const std::vector<uint8_t> fragment3{ 0x09, 0x09, 0xAC, 0xCE, 0x55, 0x01 };

	zassert_equal(ApduFragmentAssembler::Result::Incomplete,
		      assembler.AddFragment(fragment1.data(), fragment1.size(), true), "Fragment 1 should be pending");
	zassert_equal(ApduFragmentAssembler::Result::Incomplete,
		      assembler.AddFragment(fragment2.data(), fragment2.size(), true), "Fragment 2 should be pending");
	const auto result = assembler.AddFragment(fragment3.data(), fragment3.size(), false);
	zassert_equal(ApduFragmentAssembler::Result::Complete, result, "Final fragment should complete assembly");

	std::vector<uint8_t> expected{};
	expected.insert(expected.end(), fragment1.begin(), fragment1.end());
	expected.insert(expected.end(), fragment2.begin(), fragment2.end());
	expected.insert(expected.end(), fragment3.begin(), fragment3.end());
	zassert_true(ToVector(assembler.GetAssembled()) == expected, "Assembled command APDU mismatch");
}

/** @brief A final, zero-length fragment (`more == false`, no data) completes whatever was already assembled. */
ZTEST(aliro_nfc_apdu_fragment_assembler, test_zero_length_final_fragment_completes)
{
	ApduFragmentAssembler assembler{};
	const std::vector<uint8_t> fragment1{ 0x00, 0xB0, 0x00, 0x00 };

	zassert_equal(ApduFragmentAssembler::Result::Incomplete,
		      assembler.AddFragment(fragment1.data(), fragment1.size(), true), "Fragment should be pending");
	const auto result = assembler.AddFragment(nullptr, 0, false);

	zassert_equal(ApduFragmentAssembler::Result::Complete, result, "Zero-length final fragment should complete");
	zassert_true(ToVector(assembler.GetAssembled()) == fragment1, "Assembled command APDU mismatch");
}

/** @brief A single fragment larger than the maximum command APDU length overflows and resets. */
ZTEST(aliro_nfc_apdu_fragment_assembler, test_single_oversized_fragment_overflows)
{
	ApduFragmentAssembler assembler{};
	const std::vector<uint8_t> oversized(kMaxApduLength + 1, 0x00);

	const auto result = assembler.AddFragment(oversized.data(), oversized.size(), false);

	zassert_equal(ApduFragmentAssembler::Result::Overflow, result, "Expected an overflow result");
	zassert_equal(0u, assembler.GetAssembled().size(), "Assembler did not reset after overflow");
}

/** @brief Several fragments that individually fit but cumulatively exceed the maximum overflow. */
ZTEST(aliro_nfc_apdu_fragment_assembler, test_cumulative_fragments_overflow)
{
	ApduFragmentAssembler assembler{};
	const std::vector<uint8_t> chunk(kMaxApduLength / 2, 0xAA);

	zassert_equal(ApduFragmentAssembler::Result::Incomplete, assembler.AddFragment(chunk.data(), chunk.size(), true),
		      "First chunk should be pending");
	zassert_equal(ApduFragmentAssembler::Result::Incomplete, assembler.AddFragment(chunk.data(), chunk.size(), true),
		      "Second chunk should be pending");
	const auto result = assembler.AddFragment(chunk.data(), chunk.size(), true);

	zassert_equal(ApduFragmentAssembler::Result::Overflow, result, "Expected a cumulative overflow");
	zassert_equal(0u, assembler.GetAssembled().size(), "Assembler did not reset after overflow");
}

/** @brief After an overflow, the assembler recovers and correctly assembles the next command. */
ZTEST(aliro_nfc_apdu_fragment_assembler, test_recovers_after_overflow)
{
	ApduFragmentAssembler assembler{};
	const std::vector<uint8_t> oversized(kMaxApduLength + 1, 0x00);
	zassert_equal(ApduFragmentAssembler::Result::Overflow,
		      assembler.AddFragment(oversized.data(), oversized.size(), false), "Expected an overflow result");

	const std::vector<uint8_t> nextCommand{ 0x00, 0xA4, 0x04, 0x00, 0x00 };
	const auto result = assembler.AddFragment(nextCommand.data(), nextCommand.size(), false);

	zassert_equal(ApduFragmentAssembler::Result::Complete, result, "Expected the next command to complete normally");
	zassert_true(ToVector(assembler.GetAssembled()) == nextCommand, "Assembled command APDU mismatch after recovery");
}

/** @brief `Reset()` discards a partially assembled command, e.g. on field loss mid-chain. */
ZTEST(aliro_nfc_apdu_fragment_assembler, test_reset_discards_partial_assembly)
{
	ApduFragmentAssembler assembler{};
	const std::vector<uint8_t> stale{ 0xDE, 0xAD, 0xBE, 0xEF };
	zassert_equal(ApduFragmentAssembler::Result::Incomplete, assembler.AddFragment(stale.data(), stale.size(), true),
		      "Stale fragment should be pending");

	assembler.Reset();

	const std::vector<uint8_t> fresh{ 0x00, 0xA4, 0x04, 0x00, 0x00 };
	const auto result = assembler.AddFragment(fresh.data(), fresh.size(), false);

	zassert_equal(ApduFragmentAssembler::Result::Complete, result, "Fresh command should complete normally");
	zassert_true(ToVector(assembler.GetAssembled()) == fresh, "Stale bytes leaked into the fresh assembly");
}
