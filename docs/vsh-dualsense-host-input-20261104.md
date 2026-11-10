# Keyboard/mouse DualSense identity — 2026-11-04

Keyboard and mouse feed one guest controller through `HOST_INPUT_CONTROLLER_ID`.
The existing MBUS device type was already correct for PS5. The privileged pad
open previously accepted every device ID and port type, creating aliases under
unrelated device-class probes. It now accepts the enumerated ID 2, DualSense
port type 4, index 0. Capability queries also require that same device ID.
Physical-controller routing and public application pad opens are unchanged.

## Firmware identity

- 13.60 PSM `0x40eb0`, jump table `0x2c97c0`: MBUS type 24 maps through
  `0x40ef5` to managed `HardwareType.Bond=9` (DualSense).
- PSM `0x42625`, table `0x2c9a2c`: Bond uses privileged pad port type 4.
- Kernel USB identity 054c:0ce6 selects "DualSense Wireless Controller".
- Public controller information is 28 bytes; class 0 is the standard gamepad
  class used by DualSense too. Neither that class nor connection type is the
  controller-generation field. Reserved bytes are not model fields.
- Capability bits 0..4 mean motion, vibrator, lightbar, player LED and touchpad,
  per PSM `0x43a65..0x43aa7`. The existing supported mask 0x1f is retained;
  model identification comes from MBUS type 24.

Extended-open/48-byte VID/PID information remains a separate unfinished API;
firmware requires extended-open tracking before exposing that report. No model
bytes are invented in ordinary controller-information buffers.

## Verification

The integrated VSH input-focus fixture now checks the emitted MBUS record,
DualSense-only privileged open, rejection of other device classes and IDs,
capability output preservation on error, and keyboard buttons plus mouse-style
right-stick/touch data through the same focused controller. Build13 succeeded;
`vsh_input_focus`, `controller_settings`, and `usb_storage_enumeration` passed.
Run11's trace confirms device ID 2 opens twice on type 4 as keyboard/mouse PS5
DualSense, while type 5 no longer opens an alias. However, startup later exits
with an illegal-instruction fault in native libc (`base+0x34fdf`), before the
90-second screenshot. The full visible-input check remains blocked; causality
of that new exception is still being investigated.

Run12 with the RTTI fix boots Home, accepts keyboard Cross into Media Gallery,
Right switches to Favorites, PS opens the real Control Center, and Cross on its
Home action returns to Home. PID 29336 and HWND 3147136 remain identical. The
verified controller registration and live input path therefore work together.
Settings also opens; a later Settings-cleanup stall is tracked separately.
