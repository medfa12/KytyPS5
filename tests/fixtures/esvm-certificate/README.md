# ESVM public service-certificate fixtures

No private/device credentials, client IDs, licenses, provisioning responses,
cookies, or captured application traffic are included.

## Authentic certificates

Public certificate literals from
[`pywidevine/cdm.py`, revision `25e03529f6bbfbf7701fd554f89b873fd398b0ef`](https://github.com/devine-dl/pywidevine/blob/25e03529f6bbfbf7701fd554f89b873fd398b0ef/pywidevine/cdm.py):
`common_privacy_cert` and `staging_privacy_cert`. Only certificate data was
copied, not the CDM implementation. The source file SHA-256 when retrieved was
`115bcd7d11649a85c43ff0f2871f46f4201db5f870ac89448f60e6442a147d26`.
Tests run offline against the checked-in literal bytes, never this URL.

| Decoded fixture | Bytes | SHA-256 |
| --- | --- | --- |
| CommonResponse | 716 | `6bca96e3371ebd0dbf181aee61d9059ea89c9f75ec3304e06b2400e9c716e0b1` |
| StagingResponse | 714 | `fdea053e3b3a8a67bbfff80743deca460ed3ac3a85dc41b6e305880fcb11d941` |
| SignedRoot | 802 | `d62fdabc9286648a81f7d3bedaf2f5a527bbad39bc38da034ba98a21569adb9b` |

The first two are type-5 SignedMessage responses containing direct-root-signed
SERVICE (type 3) certificates. Providers are `license.widevine.com` and
`staging.google.com`; public keys are 2048-bit RSA, PKCS#1 DER (270 bytes).
Their extracted signed wrappers are 711 / 709 bytes, SHA-256 respectively:

- `515f9fb59fb78ca86001a629b1120e56d681dbfd6f943ec044c7effe33ec54e2`
- `32aa703037e8944c6a34f97cbfc359d9062c63fb122a50be0972dfdd87446231`

SignedRoot was originally copied from the audited firmware's embedded **public**
root. On 2026-11-10, its full 802 bytes were independently matched to
`root_signed_cert` in the pinned public source above (without executing that
source). The production 398-byte DER key is an identical slice of that public
certificate. This establishes a public data source, not a license assessment
of the upstream implementation, which is not included. It has type 0 and is a
positive crypto vector but a **negative SERVICE acceptance** vector.

The production public pin is independently byte-identical in:

- 13.60 `libSceEsvmEngine.sprx.elf`: public DER file offset `0x35f1c1`, 398 bytes.
- 4.03 `libSceEsvmEngine.sprx`: public DER file offset `0x341631`, 398 bytes.

DER SHA-256:
`4d37517967d93c1fe39ea841f17a88da28c4aba7f3f09505224cafdf33f75c32`.
The production code embeds exactly those bytes, and checks this digest using
CNG SHA-256 before trust verification. Full firmware hashes and root provenance
are in `docs/direct-esvm-certificate-response-20261104.md`.

## Synthetic parameter-isolation vectors

`synthetic_pss.h` was generated offline by `generate_synthetic.py`, using Python
`cryptography` and an ephemeral, explicitly **test-only** 2048-bit RSA key.
The private key was not serialized, printed, installed or checked in. The
generator is not a runtime/build/test dependency. Rerunning it produces a new
test key/signatures and therefore requires deliberately replacing the header
and its hashes; it cannot recreate the discarded private key.

The payload deliberately uses noncanonical protobuf `08 83 00` for type 3 and
a binary provider containing a NUL. Every variant signs the same exact payload:

- Correct SHA-1 / MGF1-SHA-1 / salt20 PSS.
- Negative salt0, salt32, MGF1-SHA-256, SHA-256 digest, and PKCS#1-v1.5 variants.

| Decoded synthetic value | SHA-256 |
| --- | --- |
| PublicKey (270 bytes) | `5ac5b42402a7a0ca99d4c6e64519acec334e570747397ed9a32085e76ec35042` |
| Payload (304 bytes) | `09143bc8f6badf73d780aeb71e7a22e38134dc2d6018923daf8df777d65ffae2` |
| PssSha1Salt20 | `ef70709cf89e8124351e6ce979b6fc6f9d5afe910d08a1a491cf02cde28af109` |
| PssSha1Salt0 | `598acfa45cff2d6b47d8e99bc63e06c1f0abd0ae28df4f99e89c30f9381e9d74` |
| PssSha1Salt32 | `83b1d9486d77881b5f57374234ebcf484026cb0fcc489f26421d25934049eb17` |
| PssSha1Mgf256Salt20 | `b1b4dd04ec27d827b0a5d9470d81f223385ad10187c3b4f022a50c049f148616` |
| PssSha256Salt20 | `be69c1e6436699983eaf2a2dc843ba685aa7ec4cf566e79f7e628d73327f393a` |
| Pkcs1Sha1 | `582db61c209e52aa3f2752900c286992efc9c39b97980c87d71d344206824eff` |

Each signature is 256 bytes. The test-only signature seam returns a bare
status, **never** an authenticated-certificate object. Production has no root
parameter, provider callback, mutable trust store or test-key fallback, even
when compiled with test diagnostics enabled. Tests require production rejection
of the synthetic wrapper and subsequently recheck the authentic public fixture.

## Checks performed at creation (not C++ test execution)

Offline Python parsing confirmed literal sizes/hashes, matched the production
embedded key against both firmware files byte-for-byte, and independently
verified all three authentic signatures with SHA-1/MGF1-SHA-1/salt20.
Python `cryptography` also independently accepted only the correct synthetic
variant under those parameters and rejected the five alternate schemes.
Earlier public-root arithmetic independently checked EMSA-PSS padding/hash.

**The C++ implementation and standalone CNG tests were not built or run** under
the task's no-build/no-emulator/process-control restriction. Parent must compile
and execute the standalone tests before treating the Windows backend as tested.
