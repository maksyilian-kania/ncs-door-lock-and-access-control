# AWP5 — PSA cryptography backend

Part of the per-AWP evidence log (`docs/evidence/`); see `docs/STATE.md` for
the current checkpoint and `../traceability.md` for requirement status.
Previous: `AWP4.md`. Next: `AWP6.md`.

## Baseline

- West topdir: `/home/mak5-local/gesture-access` (manifest file
  `west-aliro.yml`).
- `ncs-aliro` checked-out revision: unchanged from AWP4
  (`fa606452ab587bf7aaae85506962f1e340545471`, "user_device: complete WP5.5
  remediation"). No `west update` was run.

## Public contract inspected

- `include/aliro/user_device/interface.h` —
  `Aliro::Interface::UserDevice::Crypto` (`GenerateRandom`,
  `GenerateEphemeralKeyPair`, `RawKeyAgreement`, `DeriveSymmetricKey`,
  `DeriveRawKey`, `AeadEncrypt`/`AeadDecrypt`, `VerifySignature`, `Sha256`,
  `ValidateCertificate`, `DestroyKey`) and `::CredentialSigning::Sign`.
- `include/aliro/types.h` — `CryptoTypes::{KeyId, PublicKey, PrivateKey,
  Signature, Nonce, AuthenticationTag, Sha256Hash, SharedSecret}` sizes
  (P-256 uncompressed public key = 65 B, signature = 64 B `r||s`, shared
  secret = 32 B, symmetric key = 32 B, nonce = 12 B, tag = 16 B).
- Aliro 1.0 Specification and Test Plan, 26-42802-001, section 13
  ("Reader certificate compression") — the profile0000 DER scheme (fixed
  default `serialNumber`/`issuer`/`notBefore`/`notAfter`/`subject` values,
  `authorityKeyIdentifier` = SHA-1 of the issuer public key per RFC5280
  method 1) and section 14.1's four worked compression examples.

## External stack/pre-existing-gap observation (found during this AWP)

- `key_backend_psa.cpp` (AWP3) and its host-test fake both call
  `psa_import_key()`/`psa_sign_message()` with
  `PSA_KEY_TYPE_ECC_KEY_PAIR(SECP_R1)`/`PSA_ALG_ECDSA(SHA_256)`, but no
  `CONFIG_PSA_WANT_*` symbol for ECC/ECDSA/SHA-256 was ever added to
  `prj.conf` or any `Kconfig` reachable from the application (confirmed by
  inspecting `zephyr/.config` from an existing `build_awp4_dk` build
  directory: `CONFIG_PSA_WANT_ALG_ECDSA`/`_ECC_SECP_R1_256`/`_SHA_256` were
  all unset). This is a latent AWP3 gap, not something introduced here: on
  real hardware `nrf_security`'s `PSA_WANT_*` gating is not optional; only
  `native_sim`'s all-software mbedtls happened to have been exercised via
  test-only `prj.conf` files that already listed these symbols explicitly.
  Fixed in this AWP by adding a `CONFIG_ALIRO_UD_CREDENTIAL_PSA_WANT`
  Kconfig `select`-block to `src/storage/credential/Kconfig` (pattern
  copied from the pre-existing sibling implementation
  `subsys/aliro/crypto_utils/Kconfig`); confirmed corrected by inspecting
  the AWP5 DK build's `zephyr/.config` (see below).

## Deliverables completed

- `src/platform/crypto/crypto.cpp` — `Aliro::Interface::UserDevice::Crypto`:
  thin PSA Crypto bindings for `GenerateRandom` (`psa_generate_random`),
  `GenerateEphemeralKeyPair`/`RawKeyAgreement` (volatile
  `PSA_KEY_TYPE_ECC_KEY_PAIR(SECP_R1)` + `PSA_ALG_ECDH`),
  `DeriveSymmetricKey`/`DeriveRawKey` (`PSA_ALG_HKDF(PSA_ALG_SHA_256)`),
  `AeadEncrypt`/`AeadDecrypt` (`PSA_ALG_GCM`, single-part API),
  `VerifySignature` (import-then-`psa_verify_message`,
  `PSA_ALG_ECDSA(SHA_256)`), `Sha256` (`psa_hash_compute`), `DestroyKey`
  (idempotent `psa_destroy_key`), and `ValidateCertificate` (delegates to
  `certificate.cpp`). Every key here is `PSA_KEY_LIFETIME_VOLATILE`: no
  session/ephemeral key is ever persisted, so this file needs no
  hardware-backed secure-storage driver and runs unchanged on `native_sim`
  and the DK.
- `src/platform/crypto/certificate.{h,cpp}` — Aliro profile0000 Reader
  certificate decompression/validation (`APP_PLAN.md` AWP5, not a thin PSA
  binding): a hand-rolled minimal DER TLV reader/writer parses the
  compressed structure, reconstructs the implicit RFC5280
  `TBSCertificate` bytes from the spec's fixed default field values,
  regenerates the `authorityKeyIdentifier` extension
  (`SHA-1(issuerPublicKey)`), and verifies the reconstructed certificate's
  ECDSA-P256/SHA-256 signature against `issuerPublicKey` before returning
  the subject public key. No wall clock is added; `notBefore`/`notAfter`
  are decoded and shape-validated only, per `APP_PLAN.md`'s explicit
  instruction not to enforce certificate validity dates. Every non-success
  return path yields one of a small fixed set of error codes, independent
  of *why* trust material is absent (`ALIRO-UD-SYRS-P1-024`).
- `src/platform/crypto/credential_signing.cpp` —
  `Aliro::Interface::UserDevice::CredentialSigning::Sign()`: resolves the
  opaque `CredentialHandle` to its `PersistedCredential::mKeyId` via
  `AliroUd::Credential::Store::GetFullRecord()` (AWP3), then signs only
  through `AliroUd::Credential::KeyBackend::Sign()` — the private key
  scalar is never copied into this module or any other.
- `src/storage/credential/key_backend.h` /
  `key_backend_psa.cpp` — added `KeyBackend::Sign()` (real, PSA/CRACEN-KMU-
  or Oberon-backed persistent key, depending on target SoC's `nrf_security`
  driver) so `credential_signing.cpp` never needs to know how an
  application-facing `KeyId` maps to a usable PSA key handle.
- `tests/.../common/fake_key_backend.cpp` — added the matching
  `KeyBackend::Sign()` fake (maps the desired `KeyId` to its internal
  volatile PSA id, then `psa_sign_message()`), for host-test parity.
- `src/platform/crypto/Kconfig` — new `CONFIG_ALIRO_UD_CRYPTO` (`default
  y`) `select`-block declaring every `CONFIG_PSA_WANT_*` symbol this
  module's algorithms/key-types need (random generation; ECDH ephemeral
  key generation; HKDF/HMAC; AES-GCM/AES; ECDSA verify against an imported,
  not generated, public key; SHA-256; SHA-1 for the certificate AKI only).
- `src/storage/credential/Kconfig` — new `CONFIG_ALIRO_UD_CREDENTIAL_PSA_WANT`
  `select`-block for the pre-existing AWP3 gap described above (ECC key
  pair import/export, ECDSA, SHA-256, P-256).
- `applications/aliro-nfc-user-device/src/platform/Kconfig` — `rsource
  "crypto/Kconfig"`; `platform/crypto/CMakeLists.txt` — new module sources.
- `tests/functional/subsys/aliro_nfc_user_device/crypto/` — new host test
  target (17 cases across three suites):
  - `test_crypto.cpp` (9 cases): `GenerateRandom` (fills/varies, rejects
    null-with-nonzero-length, zero-length no-op); ephemeral ECDH agreement
    (both sides derive the identical shared secret — commutativity, not a
    hardcoded vector, since this is a thin binding over already-validated
    mbedtls/PSA primitives); `DeriveSymmetricKey` round-tripped through a
    full `AeadEncrypt`/`AeadDecrypt` cycle plus a tampered-ciphertext
    `ALIRO_INVALID_AUTHENTICATION_TAG` rejection case; `DeriveRawKey`
    determinism for identical `(ikm, salt, info)`; `VerifySignature`
    accepting a locally-generated-and-signed message and rejecting a
    tampered signature/tampered message; `Sha256` against the NIST FIPS
    180-2 known-answer vector `SHA-256("abc")`; `DestroyKey(0)` no-op.
  - `test_certificate.cpp` (5 cases): the Aliro 1.0 Specification's own
    section 14.1 "Demo 1" worked example (all-optional-fields-default
    profile0000 certificate, its issuer CA public key, and its expected
    subject public key) — independently cross-checked before being
    hardcoded into this test: the compressed DER's internal TLV lengths
    were confirmed to sum exactly to the outer `30 81 95` length,
    `SHA-1(issuerPublicKey)` was confirmed to equal the spec's own printed
    `authorityKeyIdentifier` value byte-for-byte, and the spec's
    uncompressed X.509 form of the same certificate was confirmed to have
    a genuinely valid ECDSA signature against that issuer key using
    Python's `cryptography` library, before trusting the vector for this
    test. Also covers: wrong issuer key (subject key cleared on failure),
    tampered signature byte (`ALIRO_INVALID_SIGNATURE`), truncated
    certificate and null certificate (`ALIRO_INVALID_DATA_FORMAT`).
  - `test_credential_signing.cpp` (3 cases): end-to-end against a real
    AWP3 credential provisioned through `credential_store::Create()` (host
    fakes for persistence/key-backend) — `CredentialSigning::Sign()`
    produces a signature independently checked against the credential's
    exported public key via `Crypto::VerifySignature()`; invalid handle and
    empty-data rejection with output cleared on failure.
- `applications/aliro-nfc-user-device/prj.conf` — no direct change needed:
  `CONFIG_PSA_CRYPTO`/`CONFIG_PSA_WANT_*` are now supplied transitively by
  the new `platform/crypto/Kconfig` and `storage/credential/Kconfig`
  `select`-blocks (`default y`, unconditional), consistent with how every
  other per-module `Kconfig` in this application already works.

## Commands run and results

Toolchain invoked via `ncs4`, from `/home/mak5-local/gesture-access` (west
topdir).

1. `west twister -T ncs-door-lock-and-access-control.git/tests/functional/subsys/aliro_nfc_user_device -p native_sim/native/64`
   — **Result: pass.** 6 of 6 test configurations, 59/59 test cases
   (`host_smoke`, `apdu_fragment_assembler`, `worker_lifecycle` [12],
   `authorization` [17], `cli_info` [5], `crypto` [17 — new]).
2. `west build -b nrf54lm20dk/nrf54lm20b/cpuapp -d build_awp5_app_dk ncs-door-lock-and-access-control.git/applications/aliro-nfc-user-device --pristine`
   — **Result: pass.** Final FLASH 156616 B (7.51%), RAM 82120 B (15.69%)
   (unchanged from AWP4's application RAM/FLASH baseline — the crypto
   module's added code is small and every key is volatile/stack-scoped, no
   new persistent RAM state).
3. `west build -b nrf54lm20dk/nrf54lm20b/cpuapp -d build_crypto_dk ncs-door-lock-and-access-control.git/tests/functional/subsys/aliro_nfc_user_device/crypto --pristine`, then `west flash`, then read the shell UART (`/dev/ttyACM1`, 115200-8N1) across a board reset (physical DK, serial `1051885995`, same board as prior AWPs) — **Result: pass, on real hardware.** All 17 host test cases (`aliro_ud_certificate` [5], `aliro_ud_credential_signing` [3], `aliro_ud_crypto` [9]) passed unchanged on-target, running against the DK's real `nrf_security` PSA driver (`libmbedcrypto`/`nrf_oberon` for the nRF54LM20B SoC — confirmed from the build's object list) rather than `native_sim`'s all-software mbedtls. This satisfies `APP_PLAN.md` AWP5 Verify: "Run the vectors on the target DK as well as the host-capable backend and confirm the intended PSA driver path from build/runtime evidence."
   - First attempt faulted on-target (`K_ERR_STACK_CHK_FAIL` in
     `test_sign_produces_signature_verifiable_against_credential_public_key`):
     `PersistedCredential` (two embedded 1024 B `OptionalDocument` buffers
     plus a 16-entry `Binding` array) on the ztest thread's stack overflowed
     `CONFIG_ZTEST_STACK_SIZE=8192`. Fixed by raising it to 16384 in the
     test's `prj.conf`; reconfirmed passing on `native_sim` and the DK
     after the fix.
4. `west build -b nrf54l15dk/nrf54l15/cpuapp -d build_awp5_dk_l15 ncs-door-lock-and-access-control.git/applications/aliro-nfc-user-device --pristine`
   — **Result: pass** (a second SoC family, CRACEN/KMU rather than
   nRF54LM20's Oberon-core driver, exercising the same `CONFIG_PSA_WANT_*`
   selection through a different `nrf_security` backend). Confirmed via
   `zephyr/.config` that `CONFIG_PSA_WANT_ALG_{ECDSA,ECDH,GCM,HKDF,SHA_1,
   SHA_256}`, `CONFIG_PSA_WANT_ECC_SECP_R1_256`, and
   `CONFIG_PSA_WANT_KEY_TYPE_ECC_{KEY_PAIR_IMPORT,KEY_PAIR_GENERATE,PUBLIC_KEY}`
   are all now `y` (previously unset for `ALIRO_UD_CREDENTIAL`'s needs on
   this build, per the pre-existing-gap note above; this build was not
   itself flashed, since the DK physically available in this environment is
   the nRF54LM20 board used in item 3).

## Verification-method mapping (SyRS codes: T/D/I/A)

- `ALIRO-UD-SYRS-P1-008` (session/ephemeral-key portion), `-018`, `-022`
  through `-027`, `-038` — **T**: the new `crypto`/`certificate`/
  `credential_signing` host suites (17 cases) directly exercise every
  primitive these codes require at the application/platform boundary. **D**:
  the same 17 cases were re-run unchanged on a physical nRF54LM20 DK
  against its real PSA driver (item 3 above). Updated to `app-implemented`
  (protocol *orchestration* — which bytes get signed/derived/encrypted and
  in what sequence — remains stack-owned/WP6-pending, per `APP_PLAN.md`'s
  own AWP5 scope note; see `docs/traceability.md`).

## External stack observations (not blocking AWP5)

- None beyond the pre-existing AWP3 `CONFIG_PSA_WANT_*` gap described
  above, which is application-side and has been fixed in this pass.

## Security check

- `grep`-reviewed every new/changed file: no private key scalar,
  `Kpersistent`, or session-key byte is ever logged (log lines carry only
  PSA status codes, key IDs, and lengths); `VerifySignature`/certificate
  validation return one of a small, non-key-dependent set of error codes on
  every failure path, matching `ALIRO-UD-SYRS-P1-024`'s non-disclosure
  requirement. `CredentialSigning::Sign()` never reads `PersistedCredential`
  fields other than `mKeyId`.

## Outstanding items

- None for AWP5's own scope: both Verify items (independently-checked
  known-answer/round-trip vectors, and DK + host execution confirming the
  intended PSA driver path) are complete.
- The DK build in item 4 (nRF54L15, CRACEN/KMU) was not physically flashed
  in this environment (only the nRF54LM20 DK from prior AWPs is attached);
  its `.config` inspection is build/static evidence only, not a runtime
  demonstration on that specific SoC.
- `west-aliro.yml` and `docs/Requirements.pdf` remain untracked/unstaged
  per explicit user instruction; not committed by this pass.
