# Native admission dispatch and TEE contract: second static checkpoint

Continues8aac40eb (parent f98a711e). All firmware below is from Downloads/13.60.
Read-only disassembly; documentation-only changes. No process access, build,
credential inspection, payload execution or changes to decoder ownership.
All addresses are RVAs; integer offsets and lengths are hexadecimal.

## What this adds

The MediaFramework request is now mapped through **two distinct IPC layers**,
including the server's key-system suffix parser. The PlayReady4 path demonstrably
requires TEE initialization and trusted-application session admission **before**
its ordinary configuration-access export can succeed. The first required ioctls
and their user-side payload layouts are recoverable; their secure implementation
is not supplied by a generic successful ioctl return.

This does not establish a completely runnable native route. Remaining dynamic
registration/queue joins are identified below rather than drawn as proven calls.

## 1. MediaFramework request1234207a: exact server dispatch

Previously pinned interface df80 ->18310 ->240c0 ->4280 sends1234207a at4312.
NPXS40039 `eboot.bin` has a generated dispatcher at **cbe0**:

* cbea adds edcbe000 to the incoming32-bit request (subtract12342000).
* cbf0 bounds the resulting index to cc; cc06 reads signed32-bit offsets from
  table **1aa6a0**. Entry7a resolves to **d3d8**, not a guessed string match.
* d3eb tail-calls **b2a0**. b2aa..b2c3 unpack input descriptor0's u32 and byte+4,
  descriptor1's string pointer, and two output pointers. b2c7 calls **12d550**.
  b2cc/b2d4 set each output descriptor's actual length to4; response dispatch
  uses service vtable+30. The wire field's u32 is shared-configuration metadata,
  not proof of an already admitted Access handle.
* 12d622 bounds the key-system string to100; 12d651 obtains service identity,
  12d65a queries identity metadata, 12d66a..684 obtains its context association.
  12d6cd calls **106600** on the f4e50 singleton. Errors are translated at
  12d708..727 from817910xx through table1cb270; unexpected namespaces become
  81790fff. This is a real server method, not an EsvmEngine direct import.

106600 parses routing qualifiers via **115e10**. Helpers18780/187c0 split/remove
the final dot-separated suffix, so the application's full key-system string is
an input to routing. 115e10 processes at most4 suffixes and returns the remaining
base string. 115f71 explicitly recognizes `citroncore` and writes selector2;
115f31 recognizes a separate4-character qualifier and writes selector1 unless
citroncore already won. This checkpoint does not assign semantics to that other
qualifier. 116060 recognizes SystemVideoCore=101, CommonVideoCore=102,
CustomVideoCore=103, and corresponding music cores201/202/203.

For non-audio requests,1066ae rejects music-core types. 106718..7a8 applies
media-app policy: default core becomes102 for the media-app branch and101 for
the other branch; explicit SystemVideoCore is rejected in the media-app branch.
This is not an unrestricted user-selectable provider override.

1067e1 allocates a server-side request identifier;10680b associates its base
key-system/core metadata;106892 obtains a command object;1068ca/e7050/e7060
populate it;106905 queues it through f67e0. The asynchronously executed command
at **e7070** looks up the request and calls **e7180**. Its vtable/queue dispatch
join to f67e0 has not been independently pinned here; do not call this a captured
execution. e7180's diagnostics identify requestMediaKeySystemAccessProc.

Within e7180, core type102 and selector2 choose **citroncore** through table
**1c554c**, at e731b..333. Entries1/2/3 are becore_ext_esvm/citroncore/
becore_ext_esvm; zero/out-of-range uses becore_ext. e7370 passes this name to
24570, then e73bf calls the selected object's vtable+1a0. The object creation
and full vtable binding across d0050/24570 remain a bounded registration gap.

## 2. VideoCore IPC and citroncore engine entry

The server's video-specific command execution **8cbb0** calls **14a1b0** at
8cc2c. 14a2ab calls **6a60**, which sends **1234004e**, not1234207a:
three inputs (c-byte scalars,100-byte key-system buffer,8-byte callback data)
through client vtable+60 at6acd. This is an asynchronous VideoCore request.

`libSceVideoCoreInterface.sprx`:
* synchronous dispatcher2b20 deliberately has no4e case;
* asynchronous dispatcher **3280** subtracts12340007 at3288, bounds index8a,
  and uses table **27800**. Index47 (=4e-7) resolves to **3497**;
* 3497..34b4 unpack the descriptors and call **1b120** -> **bf10**;
* bf98 maps configuration shared memory via5e90; bfa8..bff8 checks its size;
  c0ad calls registered callback **[state+258]** with request ID, base key-system,
  mapped configurations, calculated count, completion callback c3a0 and context;
* missing provider callback explicitly returns **8179200a** at c0e2. Thus a
  transport service without the callback is still not native admission.

The callback registration is also pinned: citroncore+9d6 passes a table at
[rbp-428] to imported **tXNLLCUCfOE** at d80. Its entry+258 is [rbp-1d0],
written at b2b from the b1d LEA: **citroncore+4ad0**. That imported function is
VideoCoreInterface **80e0** ->88a0, which copies188 bytes followed by250 more
bytes into the singleton state. Therefore state+258 is exactly that callback,
not inferred from a method name. 4ad0 obtains the EME manager and calls6480.

6480 checks key-system length, associates request ownership through6910, and
allocates a401ca8-byte command. At6650 it writes command type0;6657 writes the
request ID;6693 stores configuration count;669a stores completion callback6b20;
66dd copies each401b8-byte configuration. 670a enqueues via10e80 on manager+c0.
The EME thread d890 waits on that same manager+c0 at d905, retrieves the queue
item at d91a..930, reads its type at d934, and dispatches table144ba8. Entry0 is
**d952**. This closes the callback/EME enqueue and dispatch join statically.

`citroncore.elf` then supplies the verified downstream segment: EME thread case
**d952..d9b6** retrieves the request-owning object and calls **11760**, which
converts configurations (including temporary/persistent-license/usage-record
session strings) then calls **16e10** at122bc. 16e17 imports
**hJLHJLp47qc** = EsvmEngine **250a0**, whose diagnostic names it
sceEsvmEngineRequestMediaKeySystemAccess. Engine250e9 obtains its manager via
f2d0 and25103 invokes the manager's first virtual method.

**Evidence limit:** the earlier server object's creation/vtable binding and the
engine manager's complete virtual-dispatch construction have not been pinned.
These must not be replaced with guessed direct calls. The mapped RPCs, selected
core name, registered citron callback/queue, engine import and provider methods
are verified static segments, not claims of execution in Apple's retained run.

## 3. Actual PlayReady4 meanings, configuration and initialization barrier

EsvmEngine's PlayReady implementation checks base string
`com.microsoft.playready.recommendation` at40500; it conditionally also accepts
`com.microsoft.playready` according to object byte88 (40518..552). This acceptance
flag does not select PlayReady4L. The call at40615 **always** passes the
recommendation string to the imported PlayReady4 export below.

| Engine call / following diagnostic | Imported NID | PlayReady4 RVA |
|---|---|---|
|40615 /40622 CreateMediaKeySystemAccess|Mgzn4F-H1A0|68280|
|406a5 /406b1 GetConfiguration|TAcqIuI0JjY|68550|
|40981 /4098e CreateMediaKeys|Cm8AZ5YHBTk|68630|

These meanings are pinned by adjacent call/result diagnostics, not guessed from
NIDs. Access method40560 first invokes lazy initializer **437e0** at40595.
43870 initializes a PlayReady argument block via Y1upNpVk2bU;438f6 invokes
**-hOylkSV7qY = PlayReady4+a00**. On success43907 invokes A7YXgKOV0sM with1.

PlayReady4+a00 validates magic50524459 and version4/size38 or version5/size48;
required callbacks at20/28/30 must be nonnull. Version5's optional38/40 values
must be supplied together. Initialization chain is:

`a00 ->85d0 ->24d10 ->25010 ->25210 ->TEEClient230`

25070 loads sysmodule **800000a2** via39iV5E1HoCk when needed. 25051 initializes
TEE context;25062 calls25280, which opens a TA session at253e4 using TEE export
FOXX1s3yDOg. Context/session failures translate to PlayReady **8004cd1c**.
Only after the initializer and handle manager return successfully does af0 set
the initialized byte. Both access682aa and GetConfiguration68586 check that byte
via b80, returning **8004dd03** when unset.

Access68280 itself constructs a native type8 handle and calls local configuration
validator69a60. Its input structure has count0, pointer-array8, string fields10/18,
u32 count20 and pointer28. 69b0a requires the recommendation key-system;69b75..
69c0a accepts supported init-data types (including keyids), and requires at least
one. It checks required/optional policy strings and session support. The local
policy helper35980 and67e00 return1; **these constants do not avoid the earlier
TEE session initialization barrier**. GetConfiguration clears30 output bytes
and retrieves only a valid native type8 object.

No4L selection was found in this audited provider path: EsvmEngine imports
libScePlayReady4 directly; its matching imports resolve in that image. Core choice
citroncore versus becore is a distinct decision from PlayReady4 versus4L.
Version4/5 initialization structures and TA `_r2` selection are likewise not4L.
Nothing here justifies substituting the4L exports or assuming compatible objects.

## 4. TEEClient first-required ABI (guest AMD64)

Recovered from actual call sites, not a proposed host interface. Numbers below
are exact request immediates. BSD-style size bits describe the **outer** argument;
OpenSession/InvokeCommand additionally carry pointed-to inner messages.

| Operation, site | ioctl / outer size | Contract observed |
|---|---|---|
|Capability,279|400cb400 / c|Three u32 response words. Word+8 bit0 required; word+4 bit1 enables a later alternate TA-path fallback. Other bits are not defined by this client inspection.|
|Register shared memory,3c1/162e|c020b40d /20|u64 size+0, guest VA+8, physical/direct-memory value+10; zeroed u64+18 before call; returned u32 ID+18.|
|OpenSession,bc9|c010b402 /10|u64 inner pointer+0, u64 inner size+8=c0. See inner layout below.|
|InvokeCommand,1504|c010b403 /10|u64 inner pointer+0, u64 inner size+8=98.|
|CloseSession,139c|c004b405 /4|u32 session ID, fd taken through session's context pointer.|
|Release shared memory,427|c004b40e /4|u32 registered ID; context fd. Finalize then releases pool and closes fd.|

Context object begins with u32 fd. Session object has context pointer+0 and
u32 session ID+8. These are **distinct** from MediaFramework/PlayReady handles.

Initialization230 opens `/dev/mp3` read/write at261. Capability failure or absent
required bit closes it and returns ffff0008. It creates a mutex and a10000-byte
direct-memory pool, registers the pool, and stores its ID. Pool has four4000-byte
slots. Observed initialization has weak error propagation on some pool failures;
a zero return is not sufficient evidence of initialized pool/session state.

OpenSession4a0 arguments: context, output session, public TA UUID, login method,
login data, optional operation, output error origin (seventh stack argument).
Nonzero login method is rejected with ffff000a; null context/session gives
ffff0006. The UUID selects a built-in path table; no credential data is involved
in that selector. **PlayReady4's selector at9ddd0 matches the TEE table entry
whose path begins at4a40: `/system/common/lib/ScePlayReady4.sbin`.**

The inner c0-byte open message:

| Offset | Observed field |
|---|---|
|00..0f|16-byte TA UUID input|
|10|u64 mapped TA file VA|
|18|u32 file length; do not infer initialized upper padding|
|20|u64 direct-memory allocation value|
|28|u32 login method=0|
|30|u32 returned session ID|
|34|u32 returned secure result|
|38|u32 returned error origin|
|3c|u32 parameter count=4|
|40..bf|four20-byte parameter descriptors|

The client reads a nonempty TA file <=100000 bytes, rounds allocation to4000,
maps and copies the **actual file**, submits the message, then unmaps/releases
that temporary TA allocation. It populates the output session only if returned
secure result=0 (c7f..ca3). Successful ioctl transport with a failed secure result
does not create a session. File-loading failures return ffff0000/origin1.
On ioctl failure errno c maps to ffff000c, otherwise ffff0000; origin=2.

InvokeCommand13f0 inner98 bytes: u32 command+0, session ID+4, secure result+c,
origin+10, parameter count4 at14, four20-byte descriptors at18. It uses the
context's fd; reads secure result/origin at1557/155e; postprocesses output and
releases temporary mappings regardless of secure result. Invalid session pointer
returns ffff0006/origin1; syscall failures map as OpenSession. Close logs transport
failure but has no success-result ABI that could establish secure deletion.

Each wire parameter is four u64 fields: type, value/offset, value/size, shared ID.
Marshal d20 interprets four4-bit types from operation+4 and four18-byte source
unions at operation+8. Types1/2/3 carry zero-extended u32 value pairs. Temporary
memory types5/6/7 use a registered pool slot or separately allocated memory;
whole/partial registered references use the owning shared-memory ID and offset.
1060 copies permitted output values/sizes back; bounded copies occur only when
returned size fits the caller's buffer. 11a0 handles temporary-resource release.
This describes the native contract, not permission to invent registrations.

## 5. Payload metadata and precisely bounded secure requirement

Correcting the earlier printable-span ambiguity: TEE's typed table entry proves
the initial path is `/system/common/lib/ScePlayReady4.sbin`; the preceding printable
bytes are UUID/table data, not part of the filename. `_r2` comes from hardware
value masks/comparisons at5a4..5cc and a400-byte path-table stride at7a4..7b3.
Neither path is a switch to4L. The referenced kernel NID0vTn5IDMU9A's full hardware
contract is not assigned a guessed name here.

Both supplied .sbin files remain non-ELF containers: zero16-byte prefix, `$PS1`
marker at10 and total-length field at14. Only structural header metadata was
inspected; no decryption, keys, certificate or credential extraction was attempted.
The client passes the container to the secure endpoint; it does not locally
interpret it as an ELF or implement its trusted commands.

Additional supplied files exist: decrypted_modules/mp3_dram.elf and mp3_sram.elf.
Their **wrapper headers declare** ELF64 EM_X86_64/type2/entry0, each with one RWX
PT_LOAD at file1000, VA0; no section table. Segment lengths are ff400 and1d00.
These wrapper declarations do not independently establish the embedded processor
ISA, boot protocol, or a runnable host `/dev/mp3` driver. The earlier absence of
a named source implementation must not be read as absence of these files.

A faithful endpoint must consume actual TA bytes, establish a genuine TA session,
own/register the shared allocations, execute trusted commands and return their
real result/origin and output sizes. Implementing the outer ioctl marshalling is
insufficient. **This is the bounded irreducible requirement for the audited4
route; no runnable native admission can be claimed before it is met.**

## Next step / validation

No source component is justified yet: a shim that merely satisfies these ioctls
would fabricate secure capability/session state. The next specific static task
is the server object's vtable+1a0 binding and engine manager construction, followed
by classifying the MP3 wrapper provenance/real executable format. The VideoCore
registration/EME queue is already pinned above; no binding diagnostic is needed
to rediscover the known HLE rejection.

First executable validation, **only once a genuine endpoint is available**, is
native TEE Initialize -> register -> OpenSession with the exact supplied TA ->
CloseSession -> unregister/finalize, checking ownership/error unwinding and
negative invalid-TA/missing-endpoint paths. It precedes native PlayReady access;
no licensed media, fabricated handles or mixed HLE/native Access objects needed.

Verified offline:14 structural assertions (two dispatcher table targets, five
ioctl immediates, three provider export RVAs, exact TA path, and three additional
callback/EME table/offset checks) passed. Bounded
disassembly establishes the call sites/layout above. Scratch inspection scripts
are outside the worktree; no tests/builds/processes were launched against firmware.

Hashes inherited from8aac40eb for server/engine/PlayReady4/TEE. Additional receipts:
* libSceVideoCoreInterface.sprx: `51903e06f307e39729d00ddd9f1b51a935767c69a9a7dc86475eb70e0c2aff51`
* citroncore.elf,1764880 bytes: `7ca3d010867a839f59e212e5e9e7cbde4ec92fb3499190821dabd94ad46292f6`
* mp3_dram.elf,1049600 bytes: `b0b5e5d9e09656c25a9e18eef92f136490ab50949999e90df61a054f761dcb6d`
* mp3_sram.elf,11520 bytes: `b7f1dfecc225cd9a4a8a249a46ed44242941f216e8e9410a2633b31d22369fbb`
