/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <aliro/types.h>

#include <array>
#include <cstddef>
#include <cstdint>

/**
 * @brief In-memory PSA key-object model implementing
 * `storage/key/persistent_key_backend.h`, with leak and double-destroy
 * accounting.
 *
 * Every key object is one of:
 * - an input key the test creates to stand in for the stack's transient
 *   `Kpersistent` handle passed to `Replace()`;
 * - a durable copy created by `Backend::DurablyOwn()` at the store-chosen
 *   persistent key ID; or
 * - a temporary copy minted by `Backend::MintVolatileHandle()` for `Lookup()`.
 *
 * Input and temporary objects are caller-owned and released with
 * `DestroyCallerKey()`, standing in for `Crypto::DestroyKey()`. Durable
 * objects are released only by `Backend::Destroy()`. Durable objects survive
 * `AliroUd::PersistentKey::Store::Init()`, simulating a reboot. Durable
 * creation and destruction honor `fake_power_loss.h`.
 *
 * Key material is only ever compared, never returned or formatted.
 */
namespace AliroUd::PersistentKey::FakePsa {

using Material = std::array<uint8_t, 32>;

enum class Kind : uint8_t {
	Input,
	Durable,
	Temporary,
};

enum class Fault : uint8_t {
	None,
	DurablyOwn,
	MintVolatileHandle,
	Exists,
	Destroy,
};

/** @brief Destroys every object and clears every counter and armed fault (first boot). */
void Reset();

/** @brief Simulates a reboot: input and temporary objects vanish, durable objects and counters survive. */
void Reboot();

/** @brief Arms one failure of the next call to the given backend operation. */
void FailNext(Fault fault);

/** @brief Creates a caller-owned input key holding `material`; returns 0 when the object table is full. */
::Aliro::CryptoTypes::KeyId CreateInputKey(const Material &material);

/** @brief Seeds a durable object at `persistedKeyId`, as if left by an earlier boot. */
bool PreloadDurable(::Aliro::CryptoTypes::KeyId persistedKeyId, const Material &material);

/**
 * @brief Releases a live input or temporary object. Returns false, and
 * counts an invalid destroy, if `keyId` is not a live caller-owned object.
 */
bool DestroyCallerKey(::Aliro::CryptoTypes::KeyId keyId);

/** @brief Returns true if `keyId` names a live object of `kind`. */
bool IsLive(::Aliro::CryptoTypes::KeyId keyId, Kind kind);

/** @brief Returns true if `keyId` names a live object whose material equals `material`. */
bool MaterialEquals(::Aliro::CryptoTypes::KeyId keyId, const Material &material);

/** @brief Number of live objects of every kind. */
size_t LiveCount();

/** @brief Number of live objects of `kind`. */
size_t LiveCount(Kind kind);

/** @brief Number of `Backend::Destroy()` calls that released a live durable object. */
size_t DurableDestroyCount();

/** @brief Number of durable objects `Backend::DurablyOwn()` created. */
size_t DurableCreateCount();

/** @brief Number of destroy attempts on an object that was not live (double destroy or unknown ID). */
size_t InvalidDestroyCount();

} // namespace AliroUd::PersistentKey::FakePsa
