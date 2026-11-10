# `kern.rng_pseudo`: bounded native-contract repair

## Native evidence

Local 13.60 `x86_kernel.elf` OID flags `0x80040005` select a read-only handler
at `ffffffff80481060 -> ffffffff80aaac10`. It generates64 bytes under the RNG
mutex, then supplies exactly `0x40` to the sysctl output callback. Generation
failure returns EIO before output. Internal20-byte refill blocks do not expose
an arbitrarily sized sysctl stream. Queries also generate before returning size.

The output callback at `ffffffff80768130` copies at most available capacity and
returns ENOMEM for truncation. Syscall202 at `ffffffff807681e0` reads the8-byte
length, validates output then new-input ranges, enforces read-only permission,
and finally stores length only on success or ENOMEM (`ffffffff8076852f..85ae`).
An output copy fault leaves length unchanged; final length-store faults occur
after output may have been copied. Copies can partially write before failing.

`MhC53TKmjVA` is SysV `int(const char*, void*, size_t*, void*, size_t)`;
`libkernel+11550` / `libkernel_sys+12260` preserve errno on success. The syscall
error trampoline stores positive BSD errno and returns-1. Native wrappers
directly load a non-null length pointer before the syscall (`+11445`/`+12155`):
an unreadable pointer can fault in userspace, rather than return EFAULT.

SSL setup `libSceSsl+3d69` requests `0x28` (40) seed bytes. Helper `+f2050`
requests64 into a zeroed64-byte temporary at `[rbp-80,rbp-40)` with canary at
`rbp-30`; after sysctl success it copies the caller's40 bytes without checking
returned length. The previous16-byte successful fill therefore left24 zero
seed bytes. Filling64 into that temporary does not overwrite its canary.
The observed private sysctl accepts larger capacities but still outputs only64.

## Implemented ordering and outcomes

Only the named RNG path is dispatched to `RngSysctl::Sysctl`, before the old
generic null-length rejection/writable-length probe. Other knobs retain their
existing implementations. The old private xorshift generator is removed.

`Run` implements this order using explicit trusted host callbacks:

1. Read supplied length (otherwise capacity0).
2. Validate the entire advertised non-null output range, then any non-null input
   range. Invalid ranges return EFAULT14.
3. Non-null newp returns EPERM1, including zero newlen. Null newp ignores newlen.
4. Generate exactly64 bytes into a fixed local array. Failure returns EIO5.
5. Copy `min(capacity,64)` to non-null output; a failure returns EFAULT without
   attempting the length store or undoing a partial write.
6. Store resulting length, if supplied. A store failure returns EFAULT, overriding
   a pending ENOMEM. This store follows output even when pointers alias.
7. Non-null output with capacity below64 returns ENOMEM12; otherwise success.

Null output reports length64 regardless of input capacity. Both pointers null
still generate and succeed without stores. Non-null output with capacity0 still
generates, does not copy, stores0 if possible, and returns ENOMEM. Sizes1..63
copy that many bytes, report that same length, and return ENOMEM; sizes64+ copy
and report64. No success path changes `Posix::GetErrorAddr()`; failures set its
thread-local positive errno and return-1. The injected core has no global setter,
test mode or production fallback to controlled bytes.

## Host adapter and explicit limits

- Windows entropy: `BCryptGenRandom(nullptr, ...,64,
  BCRYPT_USE_SYSTEM_PREFERRED_RNG)`; every nonzero status becomes EIO.
- Linux: `getrandom` until64 bytes, retrying EINTR and handling short reads;
  zero/other failures become EIO. Apple: `arc4random_buf`.
- Unsupported platforms fail closed. There is no timestamp/PRNG fallback.
- Windows copies use explicit `NtReadVirtualMemory`/`NtWriteVirtualMemory`
  resolved from already-loaded ntdll, exact status/count checks, and real mapping
  permissions. No WriteProcessMemory protection-changing retry or host memcpy
  into guest pointers. This follows the private filesystem NpDrmCopy model;
  that helper is not exported. Missing transfer APIs return EFAULT.
- Linux copies use `process_vm_readv/writev`; Apple uses Mach checked transfers.
  Preflight cannot prevent subsequent protection races; the OS copy catches them
  and partial output is not rolled back. Read-only length storage is read first
  and rejected only when the final store is attempted.
- No owned locks/global mutable RNG state or broad host-critical guards are added.

All copies are at most64 bytes (length copies8). Validation rejects pointer
arithmetic overflow and advertised non-null ranges greater than `INT32_MAX`
with EFAULT, not EINVAL. It visits at most256 mapping regions. Linux reads at
most4096 fixed512-byte `/proc/self/maps` chunks; inability to establish coverage
within the budget is EFAULT. Null-output query capacities are not range-checked.
Zero-length ranges are treated as empty and require no mapped byte.

These are explicit host safety policies, **not complete native huge-range or
zero-length-invalid-pointer parity**. The audited native range helper narrows
length to signed32; this adapter rejects the ambiguous oversized domain instead
of reproducing wraparound or doing unbounded work. Checked initial length reads
return EFAULT rather than reproducing the native wrapper's userspace load fault.
Mapping validation is host-backed, not a new native guest-VM ownership model.

## Integration and fixtures

The existing source glob includes `rngSysctl.cpp` in production and full-emulator
fixtures. Shared Windows link libraries include **bcrypt**. The memory fixture
includes the header and `RngSysctlTests.inc`; CTest `rng_sysctl` dispatches
`TestRngSysctl` through `--rng-sysctl-only` after InitSubsystems.

The fixture checks typed production exact/NID registration, real host entropy
status/copy bounds, all capacities0/1/16/40/63/64/65/128, queries/null combinations,
errno preservation/isolation, late read-only length errors, alias stores,
controlled partial-copy failures, entropy errors, ordered validation and
bounded oversized inputs. An SSL-shaped64-byte temporary supplies all40 seed
bytes with intact canaries. Controlled bytes are injected only in core fixtures;
production fixtures call the actual registered implementation and impose no
statistical or deterministic assertion on random values.

The combined Windows emulator/fixture build passed after making three fixture
`std::min` calls immune to the Windows macro. `rng_sysctl`, `process_sdk`,
`net_pool_config` and `np_heap` passed; independent source review found no blocker.
The binding fixture initializes only ShellHits: full-library NID fallback can
still select an earlier libc logging stub. Native SSL requires separate actual
binding evidence. Entropy/partial-copy failures use injected callbacks; Linux
and Apple adapters were source-reviewed rather than runtime-tested here.

The bounded `shinerngprovider1` boot (executable SHA256
`4be0456cf7bf1b1892c72aa36738ffe3ded7aceb86d0623ef4d09ae7175cc2ec`)
retained the observed SaveData SSL quota failure at `ShellCore+ada246` and black
output. Loader records select the repaired exact libkernel sysctl binding;
this capture does not prove native SSL setup called it. The actual provider's
threading gate remained zero, blocking a native allocator smoke independently
of this RNG repair.
