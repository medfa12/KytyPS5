# Regular 13.60 dialog dimensions: numeric formatting

## Live evidence (psn19, PID 27852)

- Read-only bounded scan (4,515,524,608 bytes) found serialized UTF-16 UIManager
  constants at `0x2148926e2` (widthStandard field; mirrored mapping
  `0xfeeb16e6e2`). Both popup widths and all floating-point dimensions are
  serialized as `0.0`; integer Commands remain `focus:1024, blur:1025, close:1`.
  Evidence: `_TempData/psn19-serialized-dialog-values.json`.
- ReactNative.PUI `get_ExportedViewConstants` constructs boxed Singles of 928
  and 1456 at RVAs `0xd8d5` and `0xd922`. Earlier diagnostics established that
  the zero-width child is inside correctly sized ancestors.
- Newtonsoft.Json converts a Single through `System.Single.ToString`.
  mscorlib `System.Number.FormatSingle +0x58370` calls
  `System.Number.DoubleToNumber +0x5d7c0`, which constructs `%.40e` and invokes
  `.Sys.DoubleToString` with a 50-byte buffer at `+0x5d8a1`.
- Live mscorlib GOT `+0x180c240` resolves to `+0x304a10` (managed native-call
  wrapper); GOT `+0x181a598` resolves to monosgen `+0xe8140`. That shim shuffles
  the format/buffer/capacity arguments, sets AL=1 for the double in XMM0, and
  tail-calls snprintf.
- `guestPrintf.cpp` uses a 32-byte reverse mantissa buffer. For precision 40,
  it emits 31 padding zeros, then exhausts the buffer before writing the
  leading mantissa digits and decimal point. The resulting zero digit string
  is consumed by Number.DoubleToNumber, explaining zero JSON dimensions.
- A bounded live breakpoint at mscorlib `+0x5d8a6` captured five malformed
  results after Sys.DoubleToString, e.g. `50000000000000000000000000000000e+02`
  (36 bytes, no decimal point). Evidence:
  `_TempData/psn19-double-to-number.json`. Breakpoint restored and detached.

## Other findings

- The LoginMgrServer module-start event-open warning is specifically for
  `SceLoginMgrSharePlayEventFlag` (`+0x1d48`), not its IPMI connection event.
  Its constructor logs the warning and continues. It must not be treated as
  the demonstrated cause of CreateUser's `0x813e0001`.
- CommonAssets.rco is successfully opened and read in psn19. Missing profile
  icons are not yet explained by missing resource mounting.

## Validation status

Regression added to the existing guest_vsnprintf suite for the actual export,
firmware precision/buffer size, both popup widths, negative values, signed
zero, precision, padding, truncation, query length, and cursor preservation.
Before the fix, the regression fails at the first `%.40e` size-query assertion.
The conversion now uses the existing locale-independent fmt formatter with a
reconstructed numeric specifier and an extracted double; the guest SysV
argument cursor and outer snprintf truncation path are retained.
Post-fix `guest_vsnprintf` passes (existing mixed GP/FP/overflow regression plus
13 floating-point cases). The emulator target built successfully, including
the previously deferred filesystem guard. With user approval psn19 was stopped
and psn20 launched (PID 31368) using an isolated copy of psn19/sd and explicit
`Downloads/13.60/system_ex_b/vsh_asset`. The user confirmed the dialog is
visible and dismissible and that navigation resumes; no controller input was sent.

## Live post-fix result

- `_TempData/psn20-double-to-number.json`: 100 live observations now contain
  properly formed 40-digit scientific notation, with decimal points and signs.
  The diagnostic breakpoint restored itself and detached normally.
- `_TempData/psn20-boot-ready.png` captures a **visible** centered dialog over
  Add User: `Something went wrong.`, `NP-102933-8`, and a focused **OK** button.
  This is direct visual confirmation of the previously invisible error box.
  The user independently confirmed visibility with a screenshot. The matching
  log failure is `LoginMgr.pfAuth`, `0x80550002` (psn20.log line 5165), not
   CreateUser. The user confirmed OK dismisses the dialog and restores navigation.
   Account creation and PSN authentication
  remain separate outstanding failures.
- `_TempData/psn20-serialized-dialog-values-ready.json` confirms the full
  serialization fix: `widthStandard:928.0`, `widthWide:1456.0`,
  `minHeight:212.0`, `maxHeight:916.0`, and correct nonzero padding values.
- The final guest_vsnprintf regression also exercises the actual variadic
  snprintf export used by monosgen (double in XMM0), not just a constructed
  va_list. All checks pass.
