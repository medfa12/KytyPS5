# Socketpair provenance capture

The Windows probe is **default-off**. Set `KYTY_SOCKETPAIR_PROVENANCE=1` in the
intended capture process's environment before startup; other values disable it.
Initialization runs in `FileSystem::Initialize`, before guest execution. Setup
failure disables observation. The mapped buffer lives until process teardown.

For the main session's bounded capture, identify the file by its captured PID:

`%LOCALAPPDATA%\Temp\opencode\socketpair-provenance-<pid>-<QPC>.bin`

Collect after the process stops. There is no explicit probe flush; the file is
not a guarantee of crash-durable evidence. The decoder performs no process reads,
polling, debugger operations, network access, or emulation.

From the repository root:

```powershell
python -B tools/decode_socketpair_provenance.py <copied-artifact.bin>
python -B -m unittest discover -s tests -p test_socketpair_provenance.py -v
```

The ten stdlib-only tests use synthetic, firmware-free data in memory and write
no report artifacts. They validate decoding and bounds, not C++ or boot behavior.

## Bounds

- Binary: exactly **7,424 bytes**; decoder reads at most **7,425** to reject excess.
- Decoded UTF-8 text: at most **24,000 bytes**; binary plus text at most **31,424**.
- Banks: 32 relevant lifecycle transitions, 8 socketpair calls, 8 notification
  read dispatches, and 8 other relevant read dispatches.
- Reads: first two eligible 10-byte dispatches per File generation. Notification
  and GLS descriptors are watched at any fd; fd8 adds coverage of the prior run.

## Interpretation

Use fd, File pointer, generation, caller, thread, and FS context together.
Generation distinguishes pointer/slot reuse. A read-dispatch records the
requested count, not a completed transfer or EOF. Raw return addresses require
offline resolution against that capture's module bases; some can be host-wrapper
addresses. Raw addresses and FS-context pointers are **not credentials** or a
new ownership/authorization policy.

Sequence numbers order observation starts, not atomic guest operations or a
coherent cross-thread snapshot. Socketpair spans pre-copy, forwarded call, and
post-copy. Concurrent activity may affect sampled state. Missing, unreadable,
uncommitted, dropped, or budget-exhausted records are inconclusive; absence does
not establish that an operation did not occur.

Socketpair `flags` bit0 means pre-copy readable, bit1 post-copy readable, and
bit2 equal bytes with both readable. **`unchanged=True` proves only equality of
the two sampled endpoints**, not that no intermediate write occurred. The
baseline's separate source evidence was that the forwarded `KStub` returned zero
without writing the output pair. The current wrapper forwards once to the real
`Network::Net::Socketpair` implementation. Observation itself does not modify
the outputs or forwarded result; interpret each capture against its image identity.

The previously hypothesized chain is socketpair caller `0x9000049de` with sampled
pair `(8,0)` before/after, close caller `0x900004a70` on notification fd8, a new GLS
generation published at fd8, then read caller `0x900037cce` on that generation.
Addresses and descriptor reuse depend on the actual capture. This sequence must
be established from positive records, not forced or inferred from omissions.

## Observed baseline

The bounded `socketpairprovboot1` capture contained 35 committed records, with
no dropped or incomplete slots. Sequence 20 sampled `(8,0)` before and after a
zero socketpair return. Sequence 21, on the same host thread and FS context,
closed notification fd8 generation 20. Sequence 29 published a new notification
at fd8, generation 39; sequence 30 dispatched the button reader against that
new generation. GLS was published at **fd13**, so this capture establishes
notification reuse rather than the earlier hypothesized GLS reuse.

The binary SHA-256 is
`7873cfd857f42b793d44723daa0c6e9db6a63b8ff707d1f5c25d0aa3b711f3e`.
This demonstrates the descriptor-lifetime defect under the stub, not a working
socketpair or UI. The visible capture remained black with colored top lines.

The repaired `socketpairrealboot1` capture contained 17 committed records, no
dropped or incomplete slots. Sequences 10 and 11 returned zero with outputs
`(130,131)`, replacing `(8,0)` and `(4,4)` respectively. The notification reader
used fd12 generation 21; GLS was published at fd14. The probe does not record Net
socket retirement, so repeated numbers alone do not prove how the first pair
closed. Binary SHA-256:
`1f1c17ba32cf6b3d939632c2380de7e0a92621690a98ca57a3f61eb99661ffb1`.
The window remained black. This run exited on an invalid semaphore-handle
dereference in `KernelPollSema`; it is not successful setup/home boot evidence.
