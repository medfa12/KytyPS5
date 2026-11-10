# Retail local Home: storage dialog and missing auto-login

The user's screenshot from `retail-localhome1` shows the real guest M.2 SSD
Storage error screen: inaccessible M.2, with Reformat and Turn Off choices.
The screenshot window is at frame4149. This is not a Home success or evidence
of a physical host disk problem. Parent requested no button action and performed
no format, shutdown, navigation or store modification.

Fresh process query matches PID1004, candidate
`retail-localtest-boot1/kyty_emulator.exe`, creation FILETIME134386977204807014;
the window was responding at frame4381.

Frozen fixed byte prefixes are under `retail-localhome1/checkpoint1`:

- host.log: 6079682 bytes, SHA256
  `102dae580a2bfb1b04fad127488e163931ae0bcfab6846473c7e0aa956292266`
- guest.log: 153413547 bytes, SHA256
  `5d98017fb4b6f0881e33b5d1cc15b8637830cb9647540e7164ca96f1fd6c1512`
- launch.json: SHA256
  `525da2ab8f39fb0dad9eb08cbba841d9a8f076774fd2c57116d1ae3f7dd23dce`

Host line728 requests `m2-mount-error-with-selectable-format`, flags128.
Earlier M.2 work intentionally preserved the actual missing-controller open
failure without establishing the normal absent-device contract; see
`retail-m2-device-info-20261108.md`. A dedicated Astra medium task now owns
actual IDA analysis and production emulation of that absence path. It must
not substitute successful metadata verification or suppress the error screen.

Separately, host1247 reports `autoLoginUserExist: False` and IDffffffff;
auto-login ends at1248. The HLE store loaded one user and SYSTEM_initialize
read1, but that did not establish native auto-login eligibility. The existing
Astra medium profile task owns this native/HLE query mismatch, using the
frozen logs and original retail native contracts. User intent remains retail
local/test startup with six mounted titles and normal audio. No Home,
completed OOBE, or PSN authentication is claimed.

## User-triggered shutdown visual

The user subsequently reported selecting Turn Off PS5 and asked whether an
animation failed. Host54369 confirms the shutdown request;54393..94 loads and
focuses ShutdownScene. Guest2204563 opens `shutdown_ramp.gnf`, but2204598
reports `fread(72 x 1) -> 0 items`, followed by the native XF texture loader's
`failed file read` at2204599. The mounted original asset exists, is2304 bytes,
starts with GNF magic, and has SHA256
`090515e14a8ab18ee9a2b33f7f0676887f4172306a4a9b529d4596cb0c31bcf7`.
Guest2204745..49 separately opens, reads99600 bytes and closes
`shutdown_lightbar.fbxd` successfully.

This confirms a failed shutdown texture read after scene entry, rather than a
missing source texture. It does not establish the stdio root cause, whether
the entire animation failed, or a video decoder fault. Guest2352135 later
records `window closed, grace shutdown (500ms) before exit`. A process query
found PID1004 absent; parent performed no closure.

Final stable logs were hashed and bounded excerpts saved under
`retail-localhome1/shutdown-evidence`:
host6166339 bytes SHA256
`909c521052f25982b99a38ea746b32df729571a03b28fb55f678ed2b85950ccd`;
guest154676286 bytes SHA256
`8971f2570d4992cb3257094083b71d1a0451142b9f0027a18b1fcb05dedf2a1d`.
Earlier checkpoint prefixes remain unchanged. M.2 and saved-user auto-login
investigations continue independently of this shutdown observation.
