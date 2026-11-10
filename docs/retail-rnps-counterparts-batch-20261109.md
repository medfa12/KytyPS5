# Requested RNPS counterpart batch: staged mixed comparison

## Scope and observed blocker

Parent reports retail-font3 advanced through language, internet, overscan,
disc, timezone, power-save and EULA before Render.NPXS40009 failed to load its
RNPS bundle (0x80940001). Setup progress and the running process remain parent-
owned. This task stages the user's requested 13.60 counterpart batch only;
it does not claim that these are all titles required for Home or PSN.

New stage:
`${USERPROFILE}/AppData/Local/Temp/opencode/retail-rnps-counterparts-batch1`

Receipt: `comparison-receipt.json`

SHA256:
`89cec3fff2edcd9ece88468e0514996997ec9a4ed9833b2dae60b573f22ee0e1`

Classification: **explicit mixed counterpart comparison; no authenticated
retail decryption or runtime compatibility is claimed**.

## Exact source/coherence decision

The 14 requested application titles have existing plaintext Hermes v89 bytes
in `Downloads/13.60/system_ex_b/rnps/apps/<title>/application.ps.bundle`.
Each file's declared HBC length equals its physical length and its footer SHA1
validates. Each accompanying `.orig` RNPSHEDR container is byte-identical to
the corresponding original retail container; its declared plaintext length
matches the counterpart. These are format/identity checks, not cryptographic
proof of decryption provenance.

For every title, manifests match byte-for-byte, including titleId, application
version, commit, backend, compatibility group and RNPS version. All sibling
assets/licenses also match exactly. Settings has six additional appdb files
(two descriptors, icons and ignore_cex.dat files); these were explicitly
reviewed as identical and retained from retail too. An initial preflight
stopped on that unreviewed directory before creating any output; the preparer
was then extended to audit and preserve appdb resources.

Therefore the stage keeps retail manifests, assets, licenses and appdb files,
replacing only application.ps.bundle with the exact counterpart bytes. There
is no need to pair these bundles with differing manifests/assets. Existing
NPXS40008 and NPXS40011 counterpart bytes were additionally verified identical
to `retail-apps-comparison-final`.

NPXS40141 is different: **all four files in the requested counterpart source
are still RNPSHEDR** and cannot be used directly as plaintext. They match the
retail containers byte-for-byte, including the four pins in
`prepare_retail_host_comparison.py`. The new stage reuses exact prior audited
plaintext from `retail-host-comparison-final`: host/base JavaScript and their
two Hermes siblings, with the matching retail manifest. All four prior
plaintext pins pass; both HBC siblings also pass the full header/size/footer
checks. This is reuse of existing mixed plaintext, not new decryption.

Original .orig containers, generated .hbc/.js sidecars and decompiler logs are
not staged. The receipt lists omitted local artifacts. The executable filenames
are the 14 application.ps.bundle files and all four host/base siblings.

## Mounts

For each title below, mount read-only at global scope:

`/system_ex/rnps/apps/<title>` ->
`<new-stage>/rnps/apps/<title>`

| Title | Application | Staged files |
| --- | --- | ---: |
| NPXS40009 | millennium-falcon | 93 |
| NPXS40002 | rnps-home | 4 |
| NPXS40003 | rnps-control-center | 39 |
| NPXS40007 | rnps-uam-fs | 7 |
| NPXS40008 | rnps-settings | 265 |
| NPXS40011 | rnps-notification-overlay | 69 |
| NPXS40021 | reactSystemModalDialog | 11 |
| NPXS40081 | rnps-millenniumfalcon-dialog | 93 |
| NPXS40147 | rnps-legal-docs | 4 |
| NPXS40260 | rnps-loginmgr-dialog | 30 |
| NPXS40013 | rnps-profile | 10 |
| NPXS40051 | rnps-web-launcher | 4 |
| NPXS40103 | rnps-netctlap-dialog | 4 |
| NPXS40182 | rnps-peripherals-onboarding | 194 |
| NPXS40141 | apennine host/base | 5 |

The receipt's `mounts` array contains all 15 absolute host paths. Register
these title mounts after existing mappings so same-point replacements select
the new stage. Keep the broader retail RNPS parent mount and unrelated title,
native, font and runtime-store mappings. No sparse-stage parent replacement.

## Verification

`tools/prepare_rnps_counterpart_batch.py` preflights the entire batch before
claiming a new output directory. It refuses existing/overlapping destinations,
unknown sibling resources, mismatched manifests/assets, encrypted counterparts,
unsupported/truncated HBC, corrupt HBC footer digests and mismatched preserved
containers. All copies are byte-for-byte; no bundle patching or rewriting.

The receipt records every staged file's source, classification, size and SHA256,
each counterpart-equivalent retail sibling, every audited source hash, original
container metadata, manifests and exact mounts. The independent external
`automounter-ida/verify_rnps_counterpart_batch.py` verified:

- Exact stage file set: 832 files (169779052 bytes), plus the receipt.
- Every staged file's bytes/hash/size match its selected source.
- All 1684 audited source hashes still match after staging.
- All 15 expected title manifests and all 18 executable bundle files present.
- 16 Hermes v89 files pass full header/length/footer checks; two host JS files
  decode as text; none of the executable bundles remains RNPSHEDR.
- Encrypted input, truncated bytecode and corrupted-footer controls rejected.
- Reusing the existing stage as output rejected without changing its receipt.

External verification receipt:
`Temp/opencode/retail-rnps-counterparts-batch1-validation/verification.json`.
No emulator build or guest process was launched. No store, controller, parent
launcher or previous stage was modified. Native candidate remains
`87cdf596b00141ab92ccd39f49f0ee2ccec9f88c5b6e0b1493bf06bfe88189cf`.

The parent owns orderly shutdown, cloning the progressed setup store, launcher
mount changes and subsequent user-controlled testing. Native route execution,
JS compatibility, usable Home and PSN authentication remain unverified here.

## Parent launch

The parent reviewed the preparer/coherence report and added pinned receipt
validation to the external launcher. It verifies all 832 staged files and
replaces the three prior title mappings while adding the other twelve, keeping
the broad retail RNPS parent and all native/font mappings. The effective batch
receipt is recorded in launch.json, explicitly classified as mixed plaintext.

`retail-rnps-batch1` launched with the unchanged Bond candidate, native SSL/libc,
replay0, normal audio and no debugger. Its store is a byte/hash-verified copy
of the closed `retail-font3/sd` store; the original is retained. The launcher
records all copied file hashes and sizes. Restoring the guest's exact setup
position is still a runtime question, not guaranteed by copying persisted data.

No emulator was running before launch. PID 13068, creation FILETIME
134386903692787163. A background observer waits for exit. NPXS40009 execution,
Home and genuine PSN sign-in remain pending guest/user verification.

## First batch runtime result

The copied store did not restore the prior EULA position: host output loads
and focuses language selection. NPXS40009's new mount is registered, but its
bundle execution is not observed before the run terminates. The counterpart
replacement is therefore still unverified at the previous blocking route.

A different fatal occurs in host Windows networking: `0xc0000005` at
`0x7ff9092d49f4`, in RPCRT4.dll (base `0x7ff909270000`), symbolized as
`RpcBindingFree`. The backtrace passes through DHCPv6/RPC and
`GetAdaptersAddresses`, then `NetCtlGetInfo` (emulator RVA `0x9093f9`) and
`NetCtlCheckCallback` (`0x908c0d`) on the co-ELF eboot thread. The recorded
instruction reads `[rax+0xa0]` with RAX `0x7ff900000000`. Root cause and the
accuracy of nearest-export host symbol labels require investigation; the batch
change is not established as causal. The previous PSM font fatal did not recur.

Immutable evidence: `retail-rnps-batch1/checkpoint1`; guest SHA256
`7ce962df0c0f9dd637960f317f1a9c4c5941a081398ce3be29916d644923cdb0`,
host SHA256 `2a684e45c5ef671f15c4f44bfbf9a4ee43a9f016ee1b9e01918d77bcf6722269`.
Astra medium is investigating the native network-call failure. Stores and
counterpart stages remain retained; Home/PSN success remains unverified.
