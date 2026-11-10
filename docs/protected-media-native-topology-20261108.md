# Protected media: native firmware topology checkpoint

Read-only inspection of Downloads/13.60 and current parent source. Docs-only;
no builds, process access, firmware writes, credential reads or codec changes.
Home remains paused. Addresses below are image RVAs unless explicitly file offsets.

## Conclusion

The supplied firmware **contains native PlayReady providers**. In particular,
EsvmEngine imports37 actual PlayReady4 exports. This corrects any interpretation
of “no DRM backend” as “no firmware provider files.” The current HLE has no
production admission/backend route. Neither disk presence nor native import
resolution demonstrates a working trusted execution environment.

First demonstrated gap: `mediaFrameworkEsvmKeys.cpp` EsvmRequestKeySystemAccess
copies bounded inputs, then completes with handle0/status8179000a. Production
Access objects are not admitted. Retained Apple access/session evidence in
`media-app-playback-triage-20261106.md` supports this boundary; an Unable to Play
screenshot alone cannot distinguish it from codec/readiness failures.

## Verified call and transport edges

* Apple's retained native Esvm session path: public+a470 -> internal+1acb0;
  +1acf6 calls MediaKeys lookup+1a1a0. Missing keys reject at+1ad02..1ad09 with
  8a790004, before backend session call+1ad51. This is not evidence of a failed
  license exchange: the owning keys object was missing first.
* Actual MediaFrameworkInterface exports (both OS export-library tags checked):
  Bn34VVQIB-g access=df80; UMuQN3ysX9o create-keys=f110;
  Mh8bbtUvx1Y session=e2f0; AooiBF0kst8 decrypt=f7f0.
  Export library is `libSceMediaFrameworkInterface`, version1. Current HLE
  registers these NIDs under `MediaFrameworkInterface`; NID fallback matters.
* Native access: dfba obtains singleton via fbd0; dfd8 calls18310.
  18310 bounds configuration count<=16 and key-system length<=256; copies
  count*401b8 bytes through shared-storage preparation13530. At18448 it calls
  240c0, which calls4280 at24138. At4312 the request ID is1234207a;
  432a dispatches through transport object's vtable+58 with two input/two output
  descriptors. This is an actual IPC-shaped admission request, not a direct
  PlayReady call. Interface imports Ipmi and names SceMediaFwIfIpmiClientThread.
  Native create-keys f15a calls1a560; session/decrypt exports exist but their
  full downstream RPC mappings are not established in this checkpoint.
* MediaCoreServer NPXS40039 contains MediaKeys/session/create/decrypt command
  strings and shared-memory names `/SceMediaFwEsvmShm`,
  `/SceWebCore/SceMediaFwEsvmShm`, SceMediaFwEsvmUpSema/DownSema.
  Those strings support topology, not proof that the above request was serviced.
  The exact server dispatch-to-EsvmEngine selection edge remains unresolved.
* `libSceEsvmEngine.sprx` imports library L=`libScePlayReady4`, module M;
  all37 imported NIDs matched actual provider definitions. Selected exact pairs:
  kLERpSLEraQ->b90, Mgzn4F-H1A0->68280, TAcqIuI0JjY->68550,
  Cm8AZ5YHBTk->68630, e7nwve2CgyQ->687f0, XGcNn1LamZk->689f0,
  JhBOQYY6iu0->68fc0, dxbGiL38h+4->69750, qkidjMT42sQ->75670.
  These are NID identities, not guessed semantic export names. Engine also
  contains scePlayReadyCdmiGetConfiguration/Update/CreateMediaKeys-related
  diagnostics and CencDecryptorPlayReady; diagnostics alone do not assign NIDs.
* PlayReady4 imports TEEClient NIDs d-OLk3nvFIA, VaBqpbmaWpc,
  Xn5kfzvPht4, FOXX1s3yDOg, uZCi5aYi35w, RSaXQ+J0c5s, w1yWDjuh0I0
  at PLT89c20..89c80. TEEClient+253 addresses `/dev/mp3`, +261 calls
  open NIDwuCroIGjt2g with flags2; +279 ioctl PfccT7qURYE command400cb400.
  It requires a response capability bit at [rbp-38], otherwise closes and
  returns ffff0008. Successful open alone is insufficient.
* TEEClient references ScePlayReady4.sbin/_r2.sbin paths (file offsets8a3c,
  8e40; the first printable span starts with `/:Tk`, so do not infer its whole
  prefix is a valid pathname). The payload variants are not x86 shared libraries.
* Older routes are different: PlayReady/2/4L reference helper paths at client
  RVAs8c361/20449/98703 respectively. PlayReady2+2046d calls SystemService
  t5ShV0jWEFE (GetAppStatus), then+2050b calls0cl8SuwosPQ
  (AddLocalProcess) with the formatted ScePlayReady2.self path and size90 options.
  Current `libMediaSrvSysSvc.cpp:24–33,48,65,81` provides log-and-zero stubs,
  with no process creation or status-output population. Thus registering the NID
  is not implementing the helper launch contract.
* ScePlayReady2.self+2e46d addresses `/dev/sbl_srv`; +2e498 calls KernelOpen
  1G3lF1Gg1k8; +2e4b1 issues ioctl c0245001. `/dev/srtc` references begin2db78;
  `/dev/encdec` references2e2a4/2e3a4. ScePlayReady4L.self references srtc at
  2eff8 and sbl_srv at2f6fd. These are prerequisites of alternate providers,
  not proof Apple selects either alternate provider.

## Binding and availability distinctions

`runtimeLinker.cpp:4658–4674` checks HLE before native for NID fallback;
4808–4828 checks HLE before ordinary native exact resolution. MediaFramework is
not in cohost whitelist4728–4730. Consequently co-loading the interface does
not automatically replace current access/session/decrypt HLEs. Global
KYTY_REAL_MODULE_WINS changes much more than this family and is not a scoped fix.

Retained docs establish Apple reaching HLE admission, not PlayReady4/TEE
execution. This inspection establishes providers on disk and static dependency
edges, **not an actively loaded PlayReady module in that attempt**. No new live
load claim is made. Source searches found no named TEEClient or handlers for
/dev/mp3, /dev/sbl_srv, /dev/srtc, /dev/encdec. Generic open/ioctl providers exist;
their presence is not implementation of these trusted contracts. A physical
secure endpoint, its credential provisioning and compatible protected buffers
have not been demonstrated. The full ioctl/protocol contract is still missing.

## Containers and hash receipts

All four PlayReady libraries and three `.self` helpers are readable ELF64 in
this supplied tree, despite filename suffixes. Export libraries are respectively
libScePlayReady, libScePlayReady2, libScePlayReady4, libScePlayReady4L (version1).
Import sets include kernel, LibcInternal, SystemService, Sysmodule and Rtc;
PlayReady4 additionally imports TEEClient. MediaFramework imports Ipmi/Composite;
VideoCoreInterface imports Ipmi/SystemService/Sysmodule/RegMgr/kernel/LibcInternal.

Both .sbin files are110864 bytes: first16 bytes zero, `$PS1` at offset10(hex),
little-endian1af10 at14 equals file length; no embedded ELF magic was found.
This establishes a structured non-ELF container, **not** a decoded trusted
application or verified encryption/signature format. No opaque payload dumped.
sm_npdrm.elf is ELF32 little-endian EM_ARM(40), not x86; both supplied copies
are identical36724-byte files. Game NPDRM is not evidence for media PlayReady.

SHA256 (common library directory unless noted):

| File | SHA256 |
|---|---|
| libScePlayReady.sprx | 291459b404545925de1b6771439f0334b9c37540cd94858aa714772ab18037bc |
| libScePlayReady2.sprx | 36e83dc0369a14711a1602732675c1c785ce71e21dfca6a19d947cac2d7167c4 |
| libScePlayReady4.sprx | c440d9d5a0a60cba8cfbb478d0f6147f0eb1888acf258de4dd38bf07c2bc5da2 |
| libScePlayReady4L.sprx | b51a64e18c24dec2eeb8d81a3875fd5e0ede9431eab2ad504c3c14613c27ec07 |
| ScePlayReady.self | 795fc08b7620530cb26b30285de9cbee49764913005882217a964a9c244b7c94 |
| ScePlayReady2.self | 1974514bfc1070b34f4770d2da5629816941f88b3274424fd0b9ce154da8e646 |
| ScePlayReady4L.self | 06742e0e4d59a67bebd3c1a5084c99ffec6bf423a57d93d8acaf766170166414 |
| ScePlayReady4.sbin | 789793482b2077b8ed8e13137ee961846201962a7b3157c70d5dc8b0725a12e4 |
| ScePlayReady4_r2.sbin | b780094323900f599a8ca2e8acd920adb1f7dd6df03d7c609c82a0d1abc54041 |
| libSceTEEClient.sprx | 8c120adf911533ae9e08b7e2a1ee2ddada91c527d621bde6d03eba5b4625f663 |
| libSceMediaFrameworkInterface.sprx | fc380ac0e792b13b19191154ed9de00f1690b2b168434664c71c09f692a7c270 |
| libSceEsvmEngine.sprx | 95df64b49dedd751913b423106ff547ed6b56d81b356d3d27025e3cfa90f2b9b |
| NPXS40039/eboot.bin | 9781fec935ce6a53087ea6919a6a9db6fc78c5facf2e8416731498ca0fe1b14f |
| secure_modules and decrypted_modules sm_npdrm.elf | 1aa110be22de4c494d557ff0ef6ce13be264aebec3b06e6422c62c3f794f205b |

## Small next implementation and remaining work

Recommend a bounded **binding/admission diagnostic**, owned by parent: for the
four listed NIDs record selected provider identity (HLE/native plus module hash),
numeric access completion status and whether an owned keys handle was created.
Do not log configurations, licenses, identifiers or buffers. Offline controls
should verify co-loaded native exports still lose to HLE under current policy,
and unsupported admission never manufactures keys. This directly tests the
first demonstrated interception without introducing mixed native/HLE objects.

A native playback switch is not yet evidence-backed. Next static milestone is
mapping request1234207a to server dispatch and the engine provider selector,
then defining the TEE capability/session/command contract. A future native route
must keep Access/MediaKeys/session lifetime ownership coherent and satisfy real
secure transport; replacing only access with a success callback cannot do that.
Helper-launch implementation is a separate prerequisite only if evidence selects
the older helper route. Legitimate authenticated playback remains unverified.

Validation: read full two retained Apple investigation documents; inspect actual
ELF dynamic metadata, cross-match37 engine imports against provider exports,
disassemble bounded windows above, inspect container headers and SHA256, and
audit current resolver/launcher sources. No runtime success or tests claimed.
