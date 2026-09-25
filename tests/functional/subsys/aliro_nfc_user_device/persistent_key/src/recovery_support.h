/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include "fake_psa_key_backend.h"

#include <storage/key/persistent_key_types.h>

#include <aliro/user_device/interface.h>

/**
 * @brief Shared helpers for the recovery and credential-cascade suites.
 *
 * "Boot" means `Store::Init()` over the durable state the fakes retain;
 * "reboot" additionally restores power and drops every volatile fake PSA
 * object first. Assertion messages never format key material.
 */
namespace AliroUd::PersistentKey::RecoverySupport {

namespace PK = ::Aliro::Interface::UserDevice::PersistentKey;

struct Stored {
	PK::RecordHandle mRecord{ PK::kInvalidRecordHandle };
	FakePsa::Material mMaterial{};
};

FakePsa::Material MakeMaterial(uint8_t seed);
::Aliro::UserDevice::ReaderGroupSubIdentifier MakeSub(uint8_t seed);

::Aliro::CryptoTypes::KeyId CommittedKeyId(size_t slotIndex);
::Aliro::CryptoTypes::KeyId StagedKeyId(size_t slotIndex);

/** @brief A well-formed record for direct preloading. */
Record MakeRecord(PK::RecordHandle handle, ::Aliro::UserDevice::CredentialHandle credential,
		  const ::Aliro::UserDevice::ReaderGroupSubIdentifier &sub, ::Aliro::CryptoTypes::KeyId keyId);

/** @brief Clears every fake (first boot) without initializing the store. */
void ClearAll();

/** @brief Restores power, drops volatile PSA objects, and boots; returns `Store::Init()`'s result. */
::AliroError Reboot();

/** @brief Commits one record, then releases the input key as the stack does after `Replace()`. */
Stored Insert(::Aliro::UserDevice::CredentialHandle credential,
	      const ::Aliro::UserDevice::ReaderGroupSubIdentifier &sub, uint8_t seed);

void ExpectResolves(::Aliro::UserDevice::CredentialHandle credential,
		    const ::Aliro::UserDevice::ReaderGroupSubIdentifier &sub, const Stored &stored);
void ExpectMiss(::Aliro::UserDevice::CredentialHandle credential,
		const ::Aliro::UserDevice::ReaderGroupSubIdentifier &sub);

/** @brief Returns true if the pair resolves to `stored`; fails unless it resolves so or misses. */
bool ResolvesOrMisses(::Aliro::UserDevice::CredentialHandle credential,
		      const ::Aliro::UserDevice::ReaderGroupSubIdentifier &sub, const Stored &stored);

/**
 * @brief Checks the recovered invariant: every persisted record is valid,
 * resolves with its own handle, and owns a live durable key at one of its
 * slot's IDs, and no other durable key exists.
 */
void ExpectConsistent();

/** @brief Reboots again and checks that recovery makes no durable change. */
void ExpectStableReboot();

/** @brief Factory-resets the store and checks that nothing survives. */
void ExpectCleanTeardown();

} // namespace AliroUd::PersistentKey::RecoverySupport
