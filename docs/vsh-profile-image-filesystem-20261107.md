# Regular 13.60 profile-image filesystem boundary

## Captured failure (psn32)

User screenshots from psn31/psn32 show **User1** correctly after native byte
reversal was restored, but the avatar remains a circular missing-image
placeholder. Home also remained background-only in psn31.

The byte-validated image observer captured eight hits and restored/detached
normally. `_TempData/psn32-image-contract.jsonl` records:

- ProfileCacheAssetLoader.LoadImageAsync receives
  `profilecache:image/face/default?userid=268500996&size=m&salt=1`.
- LoadImageRequest.Load runs on its worker.
- Utilities.GetLocalIconPath constructs
  `/<sandbox>/priv_ex/etc/default_avatar_260.dds`.
- ReadImageFile first attempts
  `/<sandbox>/priv/cache/profile/0x10010004/avatar260.dds`, then that firmware
  fallback path. Both paths are correct products of the genuine loader.
- FileCommonUtility.ReadBinary's File.Exists return boundary reports true for
  both. Neither normal ReadBinary return nor ReadImageFile return was captured;
  MakeImageObject sees the request's result at+0x28 still null. This is consistent
  with the reader's exception/catch path, not a successfully decoded image.

The fallback asset exists in regular firmware at
`system_ex_b/priv_ex/etc/default_avatar_260.dds`. The bootstrap previously bound
only sandbox `/priv/mms` and `/priv/home`, omitting both image namespaces.

## Metadata-provider defect

Managed File.Exists (mscorlib+0x4483c0) calls FileSystem.FileExists+0x1ffa50.
That first calls Stat, then falls back to LStat on failure. The native Mono
LStat shim+0x3d3910 calls import `DRGXpDDh8Ng` through PLT+0x3d5580. Its success
path copies the native stack stat fields into the managed result.

The import was registered to KStub in libShellCoreFloor.cpp. Its zero return
does not initialize the output: absent files can thus appear to exist with
uninitialized attributes. psn32's relocation log establishes that provider;
live GOT inspection identifies the Mono call chain (the runtime call-tracing
thunk can sit between the GOT and the host provider).

## Implementation

- Route lstat through the current guest VFS metadata and POSIX errno conversion,
  replacing success-without-output. The VFS currently exposes ordinary files
  and directories; this does not add guest symbolic-link support.
- Bind sandbox `/priv/cache/profile` to the selected system-data cache and
  `/priv_ex/etc` to the selected system-ex firmware assets. Preserve the source
  mount's read-only policy and share storage rather than copying images into
  user records or substituting a UI-specific picture.
- Add the `profile_cache_filesystem` fixture: actual registered lstat, missing
  paths/intermediates with unchanged output, file/directory attributes, alias
  identity, actual byte reads, read-only firmware and writable shared cache.
- Update the old filesystem-identity fixture's obsolete
  BindSysCoreMmsSandbox call to the already-renamed BindSysCoreServiceSandbox;
  that stale call was the first build's compile failure.

Visible avatar completion still requires the rebuilt live run. File reads,
decoding and presentation are separate verification boundaries; no image/UI
success is claimed solely from these source changes.

## Validation checkpoint

`kyty_emulator` and `kernel_file_system_tests` built successfully. CTest
`posix_open` and `filesystem_identity` passed. The new fixture initially omitted
the separate Floor export registration, then reached KernelRead without its
required render context; these were test setup failures. It now registers the
kernel Floor and uses the existing hidden Vulkan fixture, and
`profile_cache_filesystem` passes, including the real read/copy boundary.

psn32's observer detached before its window was closed. psn33 PID22816 launched
with the same regular firmware, selected shared store and standard flags, with
the image observer queued on ProfileCache mapping. Visible image verification
is pending user input. The production build also includes the independently
committed database transport checkpoint5038fa4f; native Home lifecycle activation
and upstream NP startup remain separate unresolved work.

## psn33 visible acceptance

The user's subsequent psn33 screenshot shows the default smiling avatar and
the correct **User1** label on "Who's logging in?". This verifies the demonstrated
selector name/image failures are resolved in the rebuilt run.

`_TempData/psn33-image-contract.jsonl` also records the genuine fallback chain:

- The request retains user ID 268500996 (`0x10010004`).
- The missing per-user avatar now returns File.Exists=false and normal
  binary/image read status `0x811e0008`, allowing fallback without false metadata.
- The firmware default avatar returns File.Exists=true; both ReadBinary and
  ReadImageFile return zero. The captured managed byte array starts with the
  `DDS ` signature and contains a 260-by-260 DDS header.
- MakeImageObject receives a request with a non-null result at offset `0x28`.
  The screenshot establishes successful visible presentation beyond these
  captured read/completion boundaries.

Home completion, new persistent-user creation/restart, guest Quick Play and
genuine PSN sign-in remain unverified by this selector screenshot. The agent
continues the independent NP startup/database lifecycle work. The running psn33
process and its observer were left undisturbed for the next user-driven check.

## psn33 post-login result

The next user screenshot, after selecting User1, shows only the shell background
with no usable Home. PID22816 is still the psn33 process (its window retains the
older embedded `2b5dbf5-dirty` build label).

`_Logs/psn33.log:12976` records LoginMgr `instanceCompleted`, `result=success`,
`retCode=0`, `userId=268500996`. The same log records rnps-home/NPXS40002 instance
promotion at line12975 and `MountApp runApplication` at line13169. These establish
login handoff and Home launch, but the screenshot fails visible Home acceptance.

The current run still records NP server creation failure `0x8002004e` at
`_Runs/psn33/_kyty.txt:42686` and repeated unresolved IsEmpty/dequeue calls at
lines42715-42718, followed by AppBrowse corruption-marker access. It predates the
agent's in-progress NP EventQueue implementation. This evidence was handed to
that agent; no database reset, restart or automated controller input was made.
