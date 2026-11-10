# Instruction-fragment disposition

## B16 failure resolved

Verified implementation `7ab81b2c`. The decoded operands were correct; the
dormant test expected lowercase opcode text and obsolete register-based IR.
It also had no observable output, allowing typed SSA to eliminate its arithmetic.
The test now loads runtime buffer inputs, initializes dynamic vcc_lo, and stores
the result of each of the 21 original instructions before it can be overwritten.
All original fixture instruction words remain unchanged and external.

Assertions now follow each stored value's typed dataflow: required arithmetic
or conversion, partial-register insertion offsets/widths (including byte two),
half-word extraction, sign extension, half-float rounding, and packed clamping.
Decoded operand checks remain. SPIR-V checks retain the original operation
coverage with two corrections: OpConvertSToF is the named enum, not integer 124
(OpBitcast); DX10 NaN-to-zero saturation emits NClamp, not FClamp. The resulting
module passes SPIR-V validation. Production decoder/emitter code was unchanged.

Both shader targets rebuilt. `--boot-b16-only` and `--retained-shaders-only`
returned 0 with no skips. Missing fixture root returned 2. Evidence:
`retail-shader-build-20261110/b16-resolved-build.log` and
`runs/b16-resolved/results.json` plus stdout/stderr logs. Earlier failures below
are retained as diagnostic history, not current blockers. This validates CPU
translation/emission, not GPU execution or firmware boot.

The large-array review is resolved as follows: keep numeric input/output
sentinels and constructed SPIR-V serialization expectations; externalize the
mixed boot B16 instruction collection; retain individual ISA encoding cases used
to assert decoded opcode/operand fields. Individual instruction encodings are
technical test inputs, not bundled firmware modules or extracted shader programs.
Their historical observation in a capture is not being relabeled as independent
authorship. This is the source-distribution boundary used for this review, not
a claim that no instruction word can match firmware.

The MoreAluFamilies finding mixes encoder-built cases and individual raw
conversion/move encodings with explicit operand expectations. The nearby FREXP
cases similarly test a single instruction plus its modifier word. Those cases
remain, as do decoder rejection inputs and their assertions. The 40-DWORD boot
B16 collection is instead preserved as a checked external 160-byte fixture;
its original literal initializer is removed. No fixture payload is committed.

## Verification and newly exposed failure

The extracted collection includes exactly the old literal words and the results
of its two existing EncodeVop1 calls. SHA-256:
`0ade771803e79bcc3124cba0f499588f53abfcf205d4808ed55d0584d08da8d0`.
Both shader targets built from committed source `6e428062`.

Previously the B16 test function was not invoked from main. Executing it after
externalization failed its first existing assertion:
`new decoder did not decode low-half V_LSHRREV_B16` (exit 3221226505).
The assertion is preserved. A separate `--boot-b16-only` mode makes the failure
reproducible without changing the scope of the established retained CFG suite.
Logs: `Temp/opencode/retail-shader-build-20261110/runs/boot-b16/`.
This newly exposed regression remains a runtime/test blocker; build success does
not resolve it. The conversion is not claimed to fix decoder behavior.

The refreshed candidate includes dependency-notice fix `5ba114bd`. Fresh-user
boot inputs and compatible GPU execution remain outstanding. No release-ready
claim or GitHub push is made by this checkpoint.

## Refreshed candidate and final checks

Candidate `a5b50ccdad8ad0d50add417150096f3374cc6f0c` is a parentless commit
with the exact tree `53db94883829f0e414c80f953af65003e4831c8b` from
`f8ff6a08d6a7977f79c3ce85c158e12dc8fcc79e`. Fetched only this ref into
`Temp/opencode/retail-publication-candidate-v2-20261110`; verified one commit,
identical recursive tree/modes/gitlinks, and `git fsck --full` success.
It includes the verified fetched-dependency notice correction.

Reachability check: zero matches against 1,164 removed/historical blob IDs,
including the prior CFG source. Scanned 1,549 tracked top-level blobs for all
19 external payloads in binary and hexadecimal DWORD forms: zero matches.
Receipt: `retail-assets-cleanup-20261110/candidate-v2-verification.json`.

Both shader targets rebuilt at `f8ff6a08`. Established focused retained CFG
mode returned 0. Separate B16 mode reproduced the assertion failure, and that
mode without the external fixture root returned 2. Logs and result JSON:
`retail-shader-build-20261110/runs/fragment-final/`; build log:
`retail-shader-build-20261110/fragment-build.log`.

The earlier fresh full emulator/launcher build tested candidate v1. Production
runtime source is unchanged by this refresh; changes are test data loading,
notice installation, and documentation. This is not a new full-build claim for
v2. This later evidence checkpoint is outside the candidate tree above.
