/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include "credential_types.h"

#include <aliro/types.h>

#include <cstring>

/**
 * @brief Wire format for the `ConstData provisioningInput` byte blob
 * accepted by `Aliro::UserDeviceStack::ValidateCredential()`,
 * `::CreateCredential()`, and `::UpdateCredential()`.
 *
 * `ncs-aliro` documents this payload as "application-defined": no external
 * contract governs its byte layout, so this application defines the
 * simplest correct one — a fixed-layout struct built only by the CLI
 * staging transaction (`src/cli/cli.cpp`) and parsed only by
 * `credential_store.cpp`. A magic/version header guards against a stale or
 * garbage buffer being misinterpreted.
 */
namespace AliroUd::Credential::Provisioning {

constexpr uint32_t kMagic{ 0x414c5544 }; /* "ALUD" */
constexpr uint32_t kVersion{ 1 };

/** @brief The exact wire payload; POD, safe to reinterpret directly over `ConstData` bytes. */
struct Payload {
	uint32_t mMagic{ kMagic };
	uint32_t mVersion{ kVersion };
	bool mHasNewKeyInput{ false };
	std::array<uint8_t, ::Aliro::CryptoTypes::kEccP256KeyPrivateKeyLength> mNewKeyScalar{};
	bool mPolicySet{ false };
	::Aliro::UserDevice::AuthenticationPolicy mPolicy{ ::Aliro::UserDevice::AuthenticationPolicy::UserDeviceSetting };
	uint32_t mBindingCount{ 0 };
	std::array<Binding, kMaxBindingsPerCredential> mBindings{};
	MailboxConfig mMailbox{};
	bool mHasCredentialSignedTimestamp{ false };
	::Aliro::Timestamp mCredentialSignedTimestamp{};
	bool mHasRevocationSignedTimestamp{ false };
	::Aliro::Timestamp mRevocationSignedTimestamp{};
	OptionalDocument mAccessDocument{};
	OptionalDocument mRevocationDocument{};
};

/** @brief Builds a `Payload` from a `StagingCandidate`. */
inline Payload FromStagingCandidate(const StagingCandidate &candidate)
{
	Payload payload{};
	payload.mHasNewKeyInput = candidate.mHasNewKeyInput;
	payload.mNewKeyScalar = candidate.mNewKeyScalar;
	payload.mPolicySet = candidate.mPolicySet;
	payload.mPolicy = candidate.mPolicy;
	payload.mBindingCount = candidate.mBindingCount;
	payload.mBindings = candidate.mBindings;
	payload.mMailbox = candidate.mMailbox;
	payload.mHasCredentialSignedTimestamp = candidate.mHasCredentialSignedTimestamp;
	payload.mCredentialSignedTimestamp = candidate.mCredentialSignedTimestamp;
	payload.mHasRevocationSignedTimestamp = candidate.mHasRevocationSignedTimestamp;
	payload.mRevocationSignedTimestamp = candidate.mRevocationSignedTimestamp;
	payload.mAccessDocument = candidate.mAccessDocument;
	payload.mRevocationDocument = candidate.mRevocationDocument;
	return payload;
}

/** @brief Encodes `payload` as a `ConstData` view over it (valid only for `payload`'s lifetime). */
inline ::Aliro::ConstData AsConstData(const Payload &payload)
{
	return ::Aliro::ConstData{ reinterpret_cast<const uint8_t *>(&payload), sizeof(payload) };
}

/**
 * @brief Parses a `ConstData` provisioning input into a `Payload`.
 *
 * @return true if `input` is exactly `sizeof(Payload)` bytes with a matching
 * magic/version, false otherwise (malformed input).
 */
inline bool Parse(::Aliro::ConstData input, Payload &out)
{
	if (input.mData == nullptr || input.mLength != sizeof(Payload)) {
		return false;
	}

	Payload candidate{};
	std::memcpy(&candidate, input.mData, sizeof(candidate));

	if (candidate.mMagic != kMagic || candidate.mVersion != kVersion) {
		return false;
	}

	out = candidate;
	return true;
}

} // namespace AliroUd::Credential::Provisioning
