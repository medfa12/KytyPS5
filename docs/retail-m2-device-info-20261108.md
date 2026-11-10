# Retail M.2 device-info failure (2026-11-08)

Strictretail4 reached actual native M2 and EHdd event creation, but the M2
probe invoked unresolved `NoXU3Apuy4I` and received a false success without
output initialization (guest 69945, 69950–69951). It then consumed stack
bytes as link type/model/vendor and called unresolved metadata verification;
the later `80f40047` is not evidence of a real M2 device or its capability.

Actual IDA Pro 9.3 analyzed original retail ShellCore (source SHA
`f2e569cebe324129a8be6a40a429e6f6652021f261ded3c02a5469cc81f5a434`,
private ELF-type-only analysis SHA `a4072f89b8c7d416afae8216c21a0bc718dc5aa853c40739bc1819035acdf669`)
and original retail `libSceFsInternalForVsh.sprx` (source SHA
`dc49cfc6a933ba910e021c57d4dfc24c27dba456a303d31ee19f774d6e89f4b8`,
private analysis SHA `0f9706a54cff393dcb3cdd87dba4c30fdc447cfb6c6eddace80d62f6cb96e73e`).
Independent ELF symbol metadata maps `NoXU3Apuy4I` to provider `+47b50`.

IDA provider `+47b50` rejects null mandatory info or nonzero reserved args
with `80020016`, opens `/dev/m2.ctl` read-only and directly returns a
negative open error without writing outputs. Only a successful open proceeds
to device ioctl `80283208` and kernel attachment/error handling. ShellCore
`+3ed320` calls that five-argument provider; `_m2Exists` at `+3e97f0` treats
success as present, and handles `80020013` specially using linkType. The
actual no-device ioctl's output semantics are **not established** by this
audit, so returning ENODEV with invented zero linkType is not justified.

The scoped implementation registers the real import identity, validates its
audited arguments, calls the actual emulator open on `/dev/m2.ctl`, and
preserves its failure and output bytes. If any host-backed path unexpectedly
opens, it closes the descriptor and returns `EOPNOTSUPP`: a host file is not
an emulated M2 controller. No device presence, metadata, mount, format, or
authentication success is synthesized. Device ioctl/attachment semantics
remain a future dependency. This change intentionally may expose an ENOENT
that the old unresolved success masked; it does not claim the guest's normal
absent-M2 branch will complete.

The second unresolved retail import, `V6i6yMSwqo4` (metadata verification),
is also admitted only through its native observed failure prefix: IDA provider
`+45c20` rejects flags >= 2 and its helper `+45c60` opens `/dev/m2.ctl` before
either verification ioctl. The implementation returns that open error, never
pretending to verify metadata; successful controller/ioctl behavior remains
unsupported.

The separate strictretail4 managed `SharedStoreUriManager.SaveResponse`
exception remains uncorrelated to a particular guest filesystem path; it is
not asserted to be caused by, or fixed by, this M2 correction.

## Isolated candidate and verification

Source `ef5a6855` on the retail branch; shared Windows build source
`5ee6a153`, no stale `vsh-integrated` inputs. The isolated builder recompiled
retail cookie/SSL/devctl consumers and `libFsSandbox.cpp`; link inputs were
hashed before and after, with maximum two jobs. Receipt and build artifacts
remain outside the repository.

Candidate `${USERPROFILE}\AppData\Local\Temp\opencode\retail-m2-info\kyty_emulator.exe`
SHA256 `313048d8ceb270d90c358127d7e0a252b5a3af690c61efb69537c6b38373d473`.

Production-object fixture passed both registered import identities, invalid
mandatory/reserved arguments and verification flags, actual missing controller
open-error propagation and unchanged info bytes. It also passed devctl
descriptor tests and metadbg/sshot smoke checks. Native SSL policy, HostHTTP,
HTTP/HTTP2 and RNPS curl loopback tests passed. Independent linked-PE audit
checked retail SSL hash/all 57 exports and retained RNPS resolver call targets.
The original strictretail4 unresolved calls at guest lines 69945/69967/69985
are the failing-old runtime control. Full filesystem test target was not built
in this incremental recipe. First fixture link failed due to its namespace;
commit `065f8df8` corrected it before the passing builds.

Launch unchanged from strictretail4 apart from the executable: original retail
providers/common-lib mount, fresh isolated store, native SSL/libc startup on,
smoke absent; no performance-strip change. No guest launch occurred here. A
real controller ioctl/attachment model, observed native M2 behavior, the
separate SharedStore exception, Home and PSN authentication remain unverified.

## Strict-retail live result

Parent tested the hash-verified candidate with the unchanged strict-retail
configuration and fresh store. M2 device-info calls now preserve the actual
`/dev/m2.ctl` open failure (`0x80020002`), rather than passing uninitialized
device metadata into capability checks. Native M2/EHdd events are still created
and ShellUI still logs `EvalAutoMounterFirst Complete`. This does not establish
normal absent-M2 controller semantics.

There is still no successful Home boot. Initial setup logs URL configuration
lookup failure `0x80BA8082` for `psinitialsetup:launched` and
`pssettings:play?function=initial_setup&phase=1&onPhaseFinish=false`, then
`InitialSetup.MenuScene.CreateRnpsPhase` throws an unhandled
`NullReferenceException`. The earlier SharedStore file-creation exception also
persists; its relationship to URL configuration is not established. Investigate
the retail app registration/configuration and prepared launch inputs next.
Manifest and immutable logs are retained locally in `retail-strict5/checkpoint1`.
Home and genuine PSN authentication remain unverified.
