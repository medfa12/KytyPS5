# Checkpoint commits

- Commit the current task's work at every checkpoint, as requested by the user.
- Record verified results and remaining blockers accurately in the checkpoint.
- Keep commits scoped to task files; preserve unrelated work and local runtime artifacts.

# Emulator process ownership

- The user authorizes closing and restarting emulator instances launched by the
  assistant; they leave these open for diagnosis. Do not require another closure
  confirmation for those instances.
- Verify launch identity before closing, prefer graceful shutdown, and preserve
  logs and authenticated stores. Controller navigation remains user-controlled.

# Retail development branch

- Keep retail-specific implementation and launch changes on `ps5-vsh-retail` in
  its own worktree. Synchronize shared advances from `ps5-vsh` without discarding
  retail commits or uncommitted work.
- Use isolated build outputs and runtime stores for retail experiments.
- Strict retail runs require retail-source firmware and derived assets. A file
  copied into the retail directory does not acquire retail provenance; exclude
  sidecars and replay payloads derived from the other firmware tree.
- Keep firmware comparisons, inventories, generated outputs and local launch
  receipts outside the shared repository. Record reusable findings and verified
  implementation results in scoped checkpoint commits.
