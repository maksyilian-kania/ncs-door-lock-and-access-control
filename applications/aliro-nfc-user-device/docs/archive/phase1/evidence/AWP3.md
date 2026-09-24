# AWP3 — Credential and trust persistence (stopped, then resolved)

Part of the per-AWP evidence log (`docs/evidence/`); see `docs/STATE.md` for
the current checkpoint and `../traceability.md` for requirement status.
Previous: `AWP2.md`. Next: `AWP4.md`.

## Baseline

- `ncs-aliro` checked-out revision unchanged since AWP2:
  `a7e99c21` ("WP5: add credential, trust, and authentication-policy
  semantics"), confirmed a descendant of the minimum baseline `b8bed857`
  (`git -C <west-topdir>/ncs-aliro merge-base --is-ancestor b8bed857... HEAD`).
  No `west update` was run; only branch present besides the detached HEAD is
  `manifest-rev`.

## Public contract inspected

- `include/aliro/user_device/interface.h` —
  `Aliro::Interface::UserDevice::Trust::GetReaderPublicKey(CredentialHandle,
  CryptoTypes::PublicKey&)` and `::GetReaderIssuerPublicKey(CredentialHandle,
  CryptoTypes::PublicKey&)`: both take only a `CredentialHandle`, with no
  `reader_group_identifier`/binding index parameter.
- `stack/src/user_device/credential_manager.{h,cpp}` —
  `CredentialManager::ResolveReaderTrust(CredentialHandle handle)` is the
  only caller of either function in the checked-out stack, and it forwards
  only the handle it received, at lines 72 and 79 of `credential_manager.cpp`.
  `ResolveReaderTrust()` is not yet called from anywhere else (WP5 declares
  it; WP6 is expected to wire it into AUTH1 orchestration).
- Confirmed no other `ncs-aliro` branch or later commit changes this
  signature: `git branch -a` shows only `manifest-rev` besides the detached
  `a7e99c21` HEAD; `git log --all --oneline | grep -i "trust\|binding"`
  finds only `a7e99c21` itself and `4cf8bc57` ("aliro: added trust
  framework"), an already-ancestored Reader-role commit unrelated to the
  User Device `Trust` contract.

## Blocker: `Trust` contract is not binding-aware

`APP_PLAN.md` AWP3 requires modeling every trust binding as
`reader_group_identifier + trust_type + reader_group_identifier_key`, and
explicitly states "Multiple bindings on one credential may use different
keys." A `CredentialHandle`-only `Trust::GetReaderPublicKey()`/
`GetReaderIssuerPublicKey()` signature gives the application-owned backend
no way to know which of a credential's (up to 16) bindings matched the
`reader_group_identifier` received in AUTH0, so it cannot return the correct
per-binding key once a credential has more than one binding with different
keys. `APP_PLAN.md` explicitly forbids working around this ("Do not cache a
'last resolved identifier' or force bindings to share a key") and instructs:
"Stop AWP3 without committing and report the required binding-aware stack
contract unless the checked-out public API has been corrected." The public
API has not been corrected since this exact gap was first flagged in the
AWP2 evidence entry (`docs/traceability.md` row `-010`).

**At the time this blocker was hit, no `src/storage/credential`,
`src/storage/mailbox`, or CLI-provisioning code was implemented for AWP3,
and no files were changed or committed.** See "Resolution" below for how
this was subsequently unblocked and AWP3 completed in full.

## Required external contract change

`Aliro::Interface::UserDevice::Trust::GetReaderPublicKey()` and
`::GetReaderIssuerPublicKey()` need a way to identify which binding the
stack is resolving trust for — for example, accepting the
`ReaderGroupIdentifier` (already available to `CredentialManager` from the
AUTH0 command) in addition to the `CredentialHandle`, so the application
backend can look up the specific `reader_group_identifier_key` provisioned
for that `reader_group_identifier + trust_type` pair on that credential,
rather than an ambiguous "the" key for the whole credential.

## Resolution (out of band, before AWP4)

`ncs-aliro` moved to `37c465e8` ("WP5.5: partial pre-WP6 User Device
remediation (D1-D9, unblock AWP3)") and then `fa606452` ("WP5.5: complete
WP5.5 remediation"), both confirmed descendants of the required baseline
`b8bed857`. WP5.5 changed `Trust::GetReaderPublicKey()`/
`GetReaderIssuerPublicKey()` to accept a `ReaderGroupIdentifier` alongside
the `CredentialHandle` (decision D9), resolving the exact blocker recorded
above. AWP3 was subsequently implemented in full against the corrected
contract: `src/storage/credential/{credential_store,credential_persistence_settings,key_backend_psa,credential_types,provisioning,credential_persistence,key_backend}`
(Zephyr settings/NVS-backed metadata, PSA/CRACEN/KMU-backed private keys, a
four-phase crash-safe journal, per-binding `{reader_group_identifier,
trust_type, reader_group_identifier_key}` trust storage, preferred-credential
tracking), `src/lifecycle/lifecycle.h` (the mutating-operation coordinator),
and the `aliro-ud credential *` CLI staging/management commands, verified
with a DK build and the `worker_lifecycle`/`cli_info` host suites (commit
"Implement AWP3: credential and trust persistence"). `docs/traceability.md`
rows `-004` through `-010` were corrected to their actual AWP3-implemented
status in the AWP4 evidence pass (`AWP4.md`); this note is that correction's
source record.

## Outstanding items

- None remaining for AWP3 itself; it completed after the WP5.5 unblock
  described above. See `AWP4.md`'s baseline section for the immediately
  following AWP's `ncs-aliro` revision.
