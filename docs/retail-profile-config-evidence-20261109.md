# Frozen zero-profile config evidence

Following `f66f6900`, the parent captured `priv/home/00000000/config.dat`
from the retained font3, rnps-batch1 and rpc1 stores. All three are 16,384
bytes with SHA256
`cdacaa2c4a5938d27be1da2d1c540df075849e61d7b559a786815c12dda7b6dc`.
The latter two match their prelaunch store-seed hashes. Each capture checked
size/mtime before and after reading and repeated the read for byte equality.

Metadata-only recursive inventories for `config.dat*` and `user_list.dat*`
found only the zero-profile config and the two previously frozen list files
in each home directory. No other profile configs or matching backups were
found. Contents, names and credentials were not printed; originals were not
modified.

Frozen copies and path/size/hash receipt are under
`Temp/opencode/retail-user-config-evidence1`. The continuing Astra medium
LoginMgr task received this evidence for actual IDA contract analysis and
preservation-safe recovery design. No migration or setup success is established
by this capture.

A process query during this checkpoint found no process with the prior run's
PID27484. The parent did not close or restart an emulator in this checkpoint.
Any future process closure requires a fresh identity check.

## Recovery integration attempt

After `63fe92c0`, a fresh process query found no `kyty_emulator.exe` instances.
Running the recovery CLI against offline `retail-rpc1/sd` rejected the store
before creating output: `unexpected home directory`. A full metadata-only
inventory revealed the additional **empty** `00000000/nobackup` directory.
The earlier file-pattern inventory did not include directories. The three
artifact files remain the only files. This evidence was supplied to the same
Astra medium task for native-contract review and a scoped recovery adjustment.
No bypass or manual directory deletion was performed.

The external parent launcher now accepts `--store-recovery-receipt`, requires
its output/sd to match the seed, checks every recovered file's size/hash before
app imports, and requires the receipt's candidate hash. It records receipt
identity and explicitly sets `KYTY_USER_STORE` to the new run's copied store.
An isolated check using the agent's synthetic recovery fixture accepted its
exact inventory and rejected both a wrong candidate and changed inventory;
launcher syntax also passed. This was not an emulator boot or full-store
recovery success.
