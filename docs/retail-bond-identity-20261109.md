# Retail Bond identity: observed onboarding rejection and correction

## Root cause

Parent's immutable `retail-query1/checkpoint1/peripheral-query.jsonl`, SHA256
`ffe7e5ca68bdd60f8446cd85b7aba8bb473f799cc9a73560b526c51a3a304c74`,
observes two complete queries: native return 0, initialOnboarding true,
device 2 / owner -1 / connection 0 / native type 24 / native subtype 2,
successful conversion, then zero assembled results. The other returned entry
is HDMI device -2 / owner 255 and is excluded by the explicit owner check.
The observer detached with zero decode errors and no outstanding queries.
The user still sees the controller prompt.

Actual retail IDA now connects those values to the producer error:

1. Eboot `+0x33fea0` reads MBUS subtype at record +12. Subtypes 23/24 dispatch
   to constructor `+0x3539d0`, vtable `+0x117bcc8`. Virtual type getter
   `+0x354870` returns native type 24 for subtype 24; subtype getter
   `+0x354890` returns 2. These are AstonR/right VR controller values.
2. MBUS subtype **22**, USB transport 0, dispatches to Bond constructor
   `+0x38f8c0`, vtable `+0x117ce30`. Type getter `+0x390310` returns
   `22 - IsUsb()`. Its real IsUsb getter `+0x390390` tests transport == 0,
   giving native type **21** (BondUsbCommunication). Subtype getter
   `+0x390330` returns **2** (Bond).
3. Native full-info conversion `+0x345fa0` calls those virtual getters at
   vtable +600/+608. The observed 24/2 is therefore an accurately converted
   VR identity, not a swapped field or failed managed conversion.
4. LoginMgr's actual managed converter constructor `+0x48020` maps native
   21 to `controller`, native 24 to `vrController`. The onboarding allowlist
   accepts the former and excludes the latter. This explains the capture's
   successful conversion followed by an empty list.

The previous conclusion that MBUS24 was Bond was wrong: it equated an internal
PSM hardware integer with the separately named managed Orbis HardwareType enum.
The shared header's misleading comments and the earlier event-consistency
report must not be used as identity evidence. Event consistency remains useful,
but both enumeration and events had consistently advertised the wrong device.

Actual retail PSM `+0x40eb0` maps MBUS22 to its internal value 10 and MBUS24 to
9. Its open-type table at `+0x2c9a2c`, indexed by hardware-1, maps these to
**0** and **4**, respectively. Correct both MBUS subtype and pad-open type;
otherwise PSM would discover a Bond but the HLE would reject its real open.

## Implementation

The shared virtual-pad constants now specify MBUS22 and standard pad-open type
0. Existing enumeration and added/owner events consume those constants. The
keyboard/mouse pad accepts that real device/port combination and rejects the
former VR-right port. Owner, state, event timing and firmware code are not
changed. There is no allowlist/JS edit or forced assignment/completion.

Production-export tests now query the actual registered MBUS initializer and
enumerator, assert the explicit retail wire contract, exclude VR enumeration,
and check pad-open identity validation separately from controller lifecycle.
They also retain the real added/owner wire-record consistency checks.

`tools/test_retail_bond_native_1360.py` takes a production-export record and
executes copied, hash-pinned PSM/native getter leaf code in isolated test-process
scratch memory. It audits the managed dictionary and allowed strings from the
original AOT. An old-type24 control must yield VR-right/type24/port4/rejected;
the corrected record must yield Bond USB/type21/subtype2/port0/allowed.
It does not execute the entire guest query, constructor or JS response delivery.

## Sources

- Original eboot: `f3068cd9769711f9962367026f0bbba28ac2fac24e34fad1f40714979c89e060`.
- ELF-type-only IDA copy: `0ea43b4cc6b323a5ffa53e9db8c6c4473ac72abe9da65455c87b049e81a2dd79`.
- Original PSM: `0d6fbc45680f4fd85ef10f98c6811411ea974b2e7be5fd90fb5e03a9114deff6`.
- Original LoginMgr AOT: `72a74dce1d5d8fad59d3fb9872cbd23472dd432d2e3d99fd209391af9b8f5088`.
- Original Peripheral AOT: `8892e5107bb68c917fbf952cdc11978292ed1bbfa82797364cda14dde9083afa`.

Private IDA reports: `retail-device-constructor.txt`,
`retail-device-type-map.txt`, `retail-device-native-types.txt`,
`retail-psm-map.txt` in the existing external analysis directory.

## Verified candidate

Implementation checkpoint `8fde5dc4` was built in isolated
`Temp/opencode/retail-bond-identity`, using read-only shared build source base
`5ee6a153` and at most two compile/link jobs. All three consumers of the identity
header (`libC`, `libShellHits`, `libPad`) were rebuilt. No integrated-tree inputs
were used. The initial test link exposed a missing `ShellHits` namespace in
the fixture declaration; that test-only declaration was corrected, the tests
recompiled, and linking/testing completed. Production sources were verified
unchanged from the implementation checkpoint before reusing their objects.

Candidate:
`${USERPROFILE}\AppData\Local\Temp\opencode\retail-bond-identity\kyty_emulator.exe`

SHA256:
`87cdf596b00141ab92ccd39f49f0ee2ccec9f88c5b6e0b1493bf06bfe88189cf`.

Verified results:

- Production-export MBUS/Bond tests passed, including real enumeration,
  exclusion from VR-right queries, correct pad-open dispatch and event records.
- Actual native leaf execution on the production record passed: corrected
  MBUS22 -> PSM internal10/open0 -> native21/subtype2 -> mapped `controller`,
  allowed. Old MBUS24 -> internal9/open4 -> native24/subtype2 -> mapped
  `vrController`, rejected. The native functions were executed; dictionary and
  allowlist conclusions were audited from the pinned original managed AOT.
- Identical fixture linked with the previous `libShellHits` object failed the
  explicit Bond wire-identity assertion. A separate old `libPad` control failed
  the standard-port lifecycle assertion. Both returned expected exit code 1.
- State-flag, co-ELF context, devctl/M2, SSL, HostHTTP, HTTP/HTTP2 and RNPS curl
  regressions passed. Linked-PE audit verified all 57 SSL exports and RNPS
  resolver targets. The complete filesystem CMake target was not built.

External receipts: `receipt.json`, `native-consumer.json`,
`negative-controls.json`; the PE audit emitted its results to the tool log.
Production record SHA256:
`0dcbe3f749ca2afd7674bce47e87cb852838ea9ec79c15033ddb488979ab69d0`.

Parent should review/run this candidate with the same isolated inputs and
early query capture. Expected observations are event MBUS subtype22, pad-open
type0, query device2/owner-1/connection0/native21/subtype2, successful conversion
and one accepted result. Actual guest acceptance, language navigation and later
Home/PSN remain unverified until that run. No parent process, store or controller
was accessed during this work.

## Parent validation launch

The parent reviewed the corrected constants, three rebuilt consumers and
native-consumer controls, and started `retail-bond1` with this candidate and
the same mixed overlays/native SSL/libc/cache/replay-disabled configuration.
No emulator was running at the prelaunch check. A fresh disposable store and
the early query capture are used to compare the actual native type and result
count against `retail-query1`. The capture is timing-perturbing and scheduled
in the background; query output, clean detach and visual language-screen
progress remain pending. The prior statement to the user that subtype24
identified DualSense has been explicitly corrected.

## Parent runtime result

The early query capture observes native return 0, device 2 / owner -1 /
connection 0 / native type **21** / subtype 2. Conversion succeeds and the
initial-onboarding result count is **1**, versus 0 for the old identity.
Host output then loads and focuses
`ps5:settings:onboarding:select system language`. This verifies controller
query acceptance and a logged setup-scene advance; user-visible language
rendering and navigation remain to be confirmed.

The process subsequently exits with unhandled exception `0xc000001d` at
`0x96030a4f0`. The observer ends with reason `process_exit`, four hits,
zero decode unknowns and no outstanding queries. `cleanup_verified=false`
because the process exited while attached; orderly detach is not claimed.
The role, if any, of capture in the crash is unestablished. Home and genuine
PSN authentication remain unverified.

Immutable evidence: `retail-bond1/checkpoint1`. Guest SHA256
`ccb8f12431495b5dd195bb8174e22816280c644007662313cad249d615fae3d9`;
host SHA256 `9acb57f1fa2aaaf66eb914ba9165ae9fd15b55c8c7c259d21fd59641662869bc`;
query JSONL SHA256 `b8a761034797a909ea04dabbf7d5f989ceea6da5ba668e186b16389a6abf6ddf`.
