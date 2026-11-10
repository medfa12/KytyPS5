# Retail native SSL prerequisite (2026-11-08)

## Evidence and implementation checkpoint

Parent strictretail2 passed the cookie/SNS startup prerequisite, then exposed
`8095f008` from PlayGoCore, BGFT, ProfileCache and other SSL consumers. HLE
`Network::SslInit` has 32 slots and returns this error when exhausted. This
does not by itself prove a leak or establish SSL as the only Home blocker.

The existing native route rejected original retail SSL because the old pinned
provider has a different hash and export layout. This retail branch replaces
the pin and all 57 export address/extent entries with the actual retail image:

- SSL: `c4cd758bb1f9aa25ffef517e89e029ef4b3f6f3b22b7e525c1e4bf591b8fb3a7`.
- libc: `8fa838791061e19ca81ddd4912798bfd1d074f8127ce8c5fb25bb708eab66935`
  (already matched the admitted profile).
- Core: `f2e569cebe324129a8be6a40a429e6f6652021f261ded3c02a5469cc81f5a434`.

Actual IDA Pro 9.3 analysis used a private ELF-type-only adapted SSL image
(`35adf5e4da0473d5531d9a82cb00bebd77c11c2034fd7bfa700c4974cfc564fa`).
Original firmware bytes were unchanged. IDA disassembly/Hex-Rays establishes:

- `+340` maps backing and creates a native allocator; `+420` allocates a
  `0x1000` context and links it at root `+169188`.
- `+15f0` calls retirement helper `+3ab0`, which marks the context retired,
  unlinks its next/previous pointers under the native mutex, and retains a
  reference for cleanup.
- `+1a60` decrements the reference count and releases only when retired and
  unreferenced. It frees children/context, destroys allocator backing and
  calls the imported kernel unmap at `+1b75`.
- Retained state roots `+169188`, `+16a060`, `+16a458` remain applicable.
  Changed public exports include CA getter `+2240` and `w1+L-27nYas`
  `+2a10`, extent `0xa0`; a uniform address adjustment is invalid.

Independent ELF metadata verification (not IDA evidence) passed via
`tools/test_retail_ssl_profile.py`: three pinned hashes, all 57 SSL exports,
21 Core imports and their library/module identities, 15 required libc import
NIDs, initializer addresses and writable state ranges. Empty, wrong-address,
wrong-extent and exact old-profile controls were rejected. The C++ production
fixture now checks retail addresses/extents as well as existing admission policy.

## Launch and outstanding gates

Use `KYTY_NATIVE_SSL=1`, `KYTY_NATIVE_LIBC_STARTUP=1`, with
`KYTY_NATIVE_SSL_SMOKE` absent. Existing `-NativeSsl` sets these. The loader
selects original retail SSL/libc before dependency relocation, loading missing
providers through `/common/lib/`; that mount must resolve to original retail
`system_b/common/lib`. An explicit original retail SSL co-ELF is also supported;
duplicate providers are rejected. Same-image hash, export, GOT, initializer
order and fresh-state validation remain mandatory.

At this checkpoint no retail native SSL executable has been built or launched.
Host production-policy fixture execution is pending. Parent has now declared
the shared Windows build stable at source `5ee6a153` (latest shared branch
`7c248aa0`, documentation only). Preserve this checkpoint before merging shared
advances; the next isolated link must record that updated base.

Runtime gates remain: successful native libc/SSL initializers, real native
context allocation/lifetime, disappearance of capacity failures, actual EHdd
and M2 producer creation/signaling, and Home progression. Parent owns the live
strictretail2 process; this work has not accessed it.

## Verified candidate after shared merge

Retail implementation checkpoint `b6db3c50` was committed before merging
shared `ps5-vsh` at `7c248aa0`; merge commit `547a9eb6` preserves both histories.
Built isolated candidate against the completed Windows build at **5ee6a153**,
with two compile/link jobs maximum. All five HTTP-header consumers and native
SSL, runtime linker, symbol database and libKernel were recompiled locally.
Every linked object/library was hashed before and after linking; none changed.
The executable contains base marker `5ee6a15`.

Candidate: `${USERPROFILE}\AppData\Local\Temp\opencode\retail-native-ssl\kyty_emulator.exe`

SHA256: `bb6aab472487d6f1b58ccc27243c238ea42658dde8baefef5dbf4191203d5016`

Private build inputs/options, map, test executables and `receipt.json` are beside
the candidate. The first wrapper compilation missed `loader/elf.h`; adding that
include fixed the wrapper, and the subsequent build and all tests passed:

- `retail_ssl_tests.exe`: exact modes, native initializer admission/order,
  raw failure preservation, all 57 export identities, 21 Core imports and
  rejection controls. Host fixture only; no native firmware execution.
- `test_host_http.py host_http_tests.exe`: all loopback cases passed.
- `test_host_http.py network_http_tests.exe`: exported HTTP/HTTP2 sync/async,
  partial read, EOF, offline and cookie prerequisite cases passed.
- `test_host_http.py rnps_curl_tests.exe`: real loopback transport, ABI,
  callbacks, completion, cancellation and wakeup passed.
- Actual-retail metadata audit and exact-old-profile rejection passed again.
- Linked PE audit verified retail hash at RVA `1babe10`, all 57 actual linked
  profile entries at `1ba7460`, and retained direct RNPS resolver calls at
  `7a49e1`, `7a51eb`, `7a6086` to `3a1e70`.

Current shared media object SHA256 values recorded in the receipt:
decoder `3f960501e250bb3a7f243f2fa40d25af3b0d843393464ab5b2dedddeeefb58a5`;
source `dce47610e05407fe7952afbeedb1f0b685b23aab4a0457fb78cdc3e549ed9021`.

Launch differences from strictretail2: candidate executable above, native SSL
and libc startup enabled as specified above, original retail SSL provider
available through the retail common-lib mount (or explicit original co-ELF).
This candidate has not been guest-launched; the native startup, EHdd/M2 and
Home gates above remain for the parent's strict-retail run.

## Strict-retail live result

The parent launched the verified candidate with a fresh store, original retail
providers through an explicit `/common/lib` mount, `KYTY_NATIVE_SSL=1` and
`KYTY_NATIVE_LIBC_STARTUP=1`; the smoke switch was absent. Native libc startup
completed and both recorded initializers returned zero. The loader reported
native SSL routing ready and continued normal boot without a test SSL context.
The retained runtime snapshot contains no `0x8095f008` capacity errors.

Home remains blocked. AutoMount cannot open `/dev/devctl` (`0x80020002`), and
MediaEventListener then reports a null event sorter (`0x80f40009`). The EHdd
event open still fails; `GetM2SystemModalDialogTypeFlags()` still throws
`0x80020003`. Native device/event initialization and mount-result publication
remain unresolved. PsnCore logs `1062 msec` and later `PsnCore OK, 0 sec`;
these are startup observations, not authenticated PSN sign-in.

The launch manifest and immutable log snapshot are retained locally under
`retail-strict3/checkpoint1`. The next investigation is the actual `/dev/devctl`
contract and its relationship to native AutoMounter event producers.
