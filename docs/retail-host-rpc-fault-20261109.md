# Adapter query RPC fault: evidence and capture boundary

## Observed failure

Parent retail-rnps-batch1 crashed before observed NPXS40009 execution. Mount
registration is not execution evidence. The language scene loaded again; this
analysis does not infer that store bytes were reset or explain setup resumption.
The original process is exited and was not accessed.

Immutable logs were rehashed:

- guest: `7ce962df0c0f9dd637960f317f1a9c4c5941a081398ce3be29916d644923cdb0`
- host: `2a684e45c5ef671f15c4f44bfbf9a4ee43a9f016ee1b9e01918d77bcf6722269`

Host lines 2604–2647 capture a Windows RPC fault, not the prior PSM font null
cleanup. PC `0x7ff9092d49f4` maps to RPCRT4 base `0x7ff909270000` +0x649f4.

## Actual Windows DLL IDA and corrected labels

Private copies of installed System32 DLLs were analyzed in IDA; originals
were not modified. Exact source SHA256:

- RPCRT4.dll: `c060f02c11fb19c9c8a30b5077a87cdb17d4be40f08070a539ec73eb53402775`
- dhcpcsvc6.dll: `8f552e6e00a13bdb439180fad0277fe8cb664d2e8d38e7f58fbabdf0e67976a8`

RPCRT4's export table and IDA both identify `RpcBindingFree` starting at
+0x649b0. The bytes at +0x649f4 match the captured instruction:
`mov rax,[rax+0xa0]`. This is a read, irrespective of diagnostic access-enum
numbering. IDA shows the preceding operations:

1. Load the binding handle through the supplied pointer.
2. Check handle non-null, signature at +8 equals 0x89abcdef, and type at +12
   intersects 0x130010.
3. Load the object's first qword (vtable), then dispatch virtual slot +0xa0.

The captured object at RCX `0xffe1b93860` passes signature/type checks:
its +8 qword is `0x0001000089abcdef`. Its first qword is instead
`0x00007ff900000000`; loading slot +0xa0 faults at `0x7ff9000000a0`.
The low 32 bits of that vtable value are zero. The logs do not show who wrote
it or establish whether it was already wrong at allocation/bind time.

The dhcpcsvc6 frame label `Dhcpv6FreeLeaseInfoArray` was a nearest-export
label. Exact matching captured bytes and IDA identify +0x1df0..+0x1e10 as
`STRING_HANDLE_unbind`. It stores the binding received in RDX in a local slot,
passes its address to RpcBindingFree at +0x1dfe, and returns through +0x1e05.
This is RPC binding cleanup, not evidence of freeing a lease array twice.
`Dhcpv6QueryLeaseInfoArray` is the real export at +0x1120 and calls
NdrClientCall3 at +0x133b, with captured return PC +0x1342.

The candidate map corrects another nearest-symbol label: emulator +0x9093f9
is in `QueryHostNetworkInfo` (+0x909320), not NetCtlGetInfo (+0x909110).
It is the return site of the **second**, output-buffer GetAdaptersAddresses
call. NetCtlCheckCallback (+0x908bc0) calls this query through NetCtlConnected;
the observed return +0x908c0d is consistent with callback polling.

Disassembly of the candidate call site shows AF_INET=2 in ECX, flags0x86 in
EDX, null reserved R8, the allocated vector in R9, and the ULONG-size address
in the fifth stack argument. Windows shadow space and 16-byte call alignment
are present. No supported argument/prototype fix follows from this evidence.

## Stack/TEB hypothesis tested, not assumed

The production guest-entry assembly clears GS:8/GS:16 (TEB StackBase/StackLimit)
while using its alternate guest stack. Host adapter discovery also enters RPC.
This is a concrete boundary worth testing, but not proof of the corrupt vtable.

An isolated offline native probe performed 100 iterations per mode:

| Query path | Normal host stack | Alternate stack, zero TEB bounds | Alternate stack, matching bounds |
| --- | --- | --- | --- |
| Direct two-call GetAdaptersAddresses, same AF_INET/flags | 100 passed | 100 passed | 100 passed |
| Actual candidate NetCtlCheckCallback + NetCtlGetState | 100 passed | 100 passed | 100 passed |

The production path uses the existing candidate objects and real host networking,
without registering guest callbacks. State returned IPOBTAINED (3), and all
direct queries returned NO_ERROR. The alternate stack is a separate fully
committed 16 MiB allocation at 0x700000000. All modes verified TEB restoration.
The probe has no guest loader, firmware, stores or controller access and is not
a substitute for a full guest-runtime reproduction or concurrent-GC test.

All six child runs exited zero; no corruption reproduced. This establishes
that the tested stack placement/TEB bounds alone are insufficient to reproduce
the failure. It does not exonerate every boundary or concurrency interaction.
Moving queries to workers, changing IPv6 flags or catching this fault is not
an evidence-supported root correction at this checkpoint.

Probe source and recipe are external:
`automounter-ida/adapter_stack_probe.cpp`, `run_adapter_stack_probe.py`.
Results: `adapter-stack-probe/results.json`, SHA256
`129bd5c68d92e0fb9af4532dc778ea4bdc359cd33ff4f54203efecfaf7bf710a`.
Probe executable SHA256:
`1f7e48cd0bfde81288e04b650651f54cc94dde7505b73506aefcc32c2f39e294`.
Build output was isolated, linker jobs=2, receipt-pinned production link inputs
verified before/after. Shared base5ee6a153 outputs remained read-only.

## Targeted parent capture needed

The missing evidence is the binding object's initial vtable and the instruction
that first changes it to the invalid value. The original fault registers show
the failure endpoint only. Use module-relative locations and verify the DLL
hashes above; do not reuse old ASLR addresses or the old heap-object address.

IDA establishes this bounded capture plan for the next parent-owned run:

1. dhcpcsvc6 +0x1eb2 is immediately after RpcBindingFromStringBindingW in
   STRING_HANDLE_bind_helper (+0x1e40). On EAX=0, the new binding pointer is
   `[RSP+0x48]`. Record pointer, first 0x40 bytes, thread, call stack, RSP and
   TEB limits. This identifies the object's state at creation.
2. If the vtable is valid then, arm a write watch on that aligned first qword
   for the tracked binding through unbind, covering possible writer threads
   as well as the allocating thread. Record the first writing instruction,
   pre/post value, registers and native/guest stack. If already invalid here,
   capture must move inside binding creation rather than assuming a later write.
3. Useful boundary snapshots: +0x1ee8 after SetOption, +0x1f1c after
   SetAuthInfo, and +0x1f47 before bind helper returns (RAX is binding or null).
   The bind/unbind table at +0xb0e8 contains +0x1f80 and +0x1df0; +0x1f80
   tail-jumps to +0x1e40. At unbind +0x1df0 the binding is in RDX.
4. Record whether the watch fires before unbind. Preserve existing debug
   registers, restore capture state and document whether cleanup was orderly
   or the process exited. Capture remains parent-operated and navigation
   remains user-controlled.

External IDA reports: `host-rpc-binding-fault.txt`,
`host-dhcp-rpc-boundary.txt`, `host-dhcp-binding-create.txt`,
`host-dhcp-binding-helper.txt` in the existing analysis directory.

## Checkpoint status

The invalid RPC binding vtable and exact native path are verified. The first
bad write/initialization cause remains unresolved. No production code change
or new emulator candidate is justified yet. Bond/state/GC/network behavior,
all stages, setup stores and parent launcher are unchanged. Candidate remains
`87cdf596b00141ab92ccd39f49f0ee2ccec9f88c5b6e0b1493bf06bfe88189cf`.
