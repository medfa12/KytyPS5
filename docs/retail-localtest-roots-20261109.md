# Retail LOCAL/TEST user-root failure

The frozen `retail-full-rnps3` host log shows Theme.Apply's user-entry lookup
miss, followed by its fallback and null dereference. The guest log resolves the
local UserService Login export, but records its call for the saved user returning
`0x80960001` (internal error). Storage startup recovery succeeded, but the copied
seed had only `priv/home/<user>` of the three required user directories. The
UserService implementation refuses partial layouts and disables store writes;
the absence of a save-failure log is consistent with that pre-save rejection.
This is a supported causal chain, not proof of all subsequent Home behavior.

The prior `retail-localhome8` successful Home run had all three directories.
The retained local/test seed used for the failed runs lacks two empty roots.
`tools/complete_retail_localtest_roots.py` checks the receipt's full file hashes,
native roster/config and local user ID, then makes a separate verified copy and
adds only missing empty `user/home/<user>` and `eap/rodata/home/<user>` roots.
It preserves all original profile files and creates no new login or account.
The resulting external `retail-localtest-complete-roots1` has a new receipt and
matches the original file hashes. No source or live store was changed.

`python -B tests/test_complete_retail_localtest_roots.py` passed three offline
tests: byte-preserving copy, mismatch rejection, and no overwrite. A full-RNPS
retail comparison retry was prepared as `retail-complete-roots1`; runtime login,
Theme ordering, visible Home and apps remain unverified until observed. The
candidate and mixed RNPS stage remain unchanged, as do PSN limitations.

## Runtime retry

`retail-complete-roots1` launched PID 19020 with the same pinned executable,
verified full 1,703-file mixed RNPS stage, and a fresh copy of the completed
seed. Guest log records `userservice-store: login ... foreground=268501033` and
the actual `userservice-local` login export returned `0x0`, rather than the
previous `0x80960001`. Host log subsequently records `Theme.OnLogin` and user
entry initialization for `0x10010029`. At this checkpoint, the window was
responding, but `Theme.Apply`, visible Home and application execution had not
yet been confirmed. No authenticated PSN or pure-retail behavior is implied.

Additional offline regressions: 11 retail catalog-display tests, 11 RNPS-stage
tests, and 3 portable-path tests passed. Source firmware, source stores and
previous run logs remain untouched.

Later in the same run, host log confirms `Theme.Apply` **finished** for the
saved user without the previous null fallback, and an RNPS popup application
reached `MountApp runApplication`. The window was responding at frame 258 / 11
fps. This clears the observed Theme exception; visual Home and sustained
interactivity still require user confirmation.
