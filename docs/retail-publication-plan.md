# Retail publication plan — approval required

Update: `retail-agc-extraction-20261110.md` establishes that all 67 embedded base
register/type arrays can be extracted exactly from the user's existing retail
AGC module. The request below for additional AGC source inputs is resolved for
that matched profile. Runtime/setup integration and register/type literal removal
are implemented and ABI-verified in `d948a709`; see that document for validation
scope and remaining boot/publication work.

## Verified starting point

- Retail work is on `ps5-vsh-retail`; regular `ps5-vsh` remains separate.
- Read-only `git ls-remote --heads origin` on 2026-11-10 returned only
  `782a85c621bd849762599a6a0e790596be119505 refs/heads/main`.
- Destination is `https://github.com/kytyps5/kytyps5`.
- Reachable retail history contains all 18 removed captured shader inputs.
  See `retail-history-provenance-20261110.md` for object-level evidence.
- Both shader targets build. Focused CFG tests pass; GPU execution is blocked
  by host capabilities. This does not establish first-user boot readiness.

## 1. Asset replacement scope

Request approval to replace these four assets with original, reproducibly
generated artwork/audio, with source generators and attribution committed:

| Asset | Integration to preserve | Proposed replacement |
| --- | --- | --- |
| `assets/sounds/trophy-unlock.wav` | CMake copy/install and `systemOverlay.cpp` audio loading | Short procedurally synthesized notification tone |
| `assets/cursors/hand.png` | Touch cursor loading in `systemOverlay.cpp` | Original neutral pointer graphic |
| `assets/cursors/drawing.png` | Touch cursor loading in `systemOverlay.cpp` | Original crosshair graphic |
| `assets/images/hidden-trophy.png` | `src/launcher/resources.qrc` and trophy viewer | Original generic hidden-achievement symbol |

Preserve the existing files and hashes in an external backup before replacement.
Do not infer artwork licensing from the enclosing repository license. Review
the generated images visually and verify dimensions, transparency, audio format,
Qt resource compilation, installed paths and notification playback.

Request approval to omit `docs/screenshots/` from the publication tree and remove
or rewrite its Markdown/HTML references, including README screenshots. Keep the
originals in existing local history/backups. Textual diagnostic evidence may
remain after its own content review. Do not replace screenshots with invented
emulator output. Scan all tracked documentation for broken image links afterward.

## 2. Register-default table resolution

`src/libs/agc.cpp` includes `agcRegisterDefaults.inc` and builds persistent pointer
tables from it. Both public/internal dispatch cover versions 0–13; higher input
versions currently normalize to 13. A replacement must preserve register counts,
pointer offsets/aliasing, type flags, storage lifetime, locking and version
behavior. Zero-filled or empty substitute tables are not a functional solution.

Choose one route:

**A — establish provenance:** provide the original table generator/source,
the relevant revision, authorship and applicable redistribution terms. Validate
the generated values against the current table and record the evidence.

**B — externalize source-bound data (recommended if A is unavailable):** approve
removal of the embedded table and an additional setup prerequisite. Supply the
matching user-owned decrypted AGC module(s), or a documented extraction process
and independently attributable input records for both table families/versions.
First inspect those inputs to determine whether a reliable extractor is feasible;
the current firmware directory alone is not proof of available table coverage.
Implement a bounded, versioned external data format, explicit input identity,
validated counts/offsets, stable owned storage and clear missing-input diagnostics.
Package the extractor/loader, never the captured table payload. Validate all
supported versions, malformed input rejection and native callers with external
fixtures. Do not claim version support where source inputs are missing.

This is production behavior, so build-only evidence is insufficient. Preserve
the old table externally for comparison; execution acceptance still needs a
compatible GPU. This route may add prerequisites to the first-user workflow.

## 3. Clean-history publication

Request approval for a **new root-history branch `retail-release`**, keeping
remote `main`, existing local branches, authenticated stores and backups intact.
No force-push, deletion of remote refs, default-branch change, or release tag is
included in this approval. A new root branch does not erase content already on
GitHub's main branch, tags, forks or caches; those require separate review.

After approved tip cleanup and acceptance:

1. Record the exact approved source commit, tree ID, dependency pins, test results
   and retained local backup refs. Verify a Git bundle of relevant local history
   outside the checkout. Keep runtime files and firmware out of the bundle/tree.
2. Audit the entire proposed tree, including documentation, scripts, binary
   assets, notices and submodule distribution requirements. Resolve remaining
   opcode/reference findings. Confirm no external fixtures or personal stores
   are tracked. Removing ancestor history does not clean the tip automatically.
3. Create a root commit from that approved Git tree with no parents in an external
   publication workflow. Preserve Git file modes and exact submodule gitlinks;
   do not recursively copy a working directory. Retain required copyright,
   contributor and dependency notices and record the original source revision.
4. Fetch only the new root branch into a fresh external publication repository.
   Assert `git rev-list --count HEAD` is 1, the parent list is empty, and its tree
   equals the approved tree. Enumerate all reachable objects and verify that the
   known captured historical blobs are absent. Audit any later publication
   commits too. A count of one alone is not a content check.
5. From a fresh recursive clone, build/install and run packaged setup with a new
   external writable directory. Verify pinned dependencies and license/source
   obligations. Complete the runtime acceptance below before labeling it ready.
6. Recheck remote refs and authentication. If `retail-release` already exists,
   stop and compare rather than overwrite it. Dry-run the explicit refspec,
   then push only `HEAD:refs/heads/retail-release`; never use `--all` or `--mirror`.
   Verify `git ls-remote` reports the exact published commit and that main's hash
   is unchanged. Record the URL and commit; do not call a dry-run a publication.

## 4. Inputs and manual acceptance still required

For the alternate retail firmware, provide paths to matching:

- URL configuration (`urlconfig_cache.json`).
- Decrypted per-title RNPS overlay directories.
- Source-bound 136-byte Core and UI admission records accepted by the current
  setup workflow. Their required hashes are documented in the existing setup
  and admission code; copied authenticated stores do not substitute for them.
- AGC extraction inputs if table route B is selected.

Provide access to a Vulkan device satisfying the shader harness, including
fragment barycentrics and 64-bit shared/image atomics. The current Intel Arc
reports `shaderSharedInt64Atomics = false`. Run both focused modes with all
18 fixtures and retain passing logs; do not relax the capability checks to
manufacture acceptance.

After fresh preparation and launch, the user controls controller navigation to
confirm Home, application selection/launch and relevant playback behavior.
Online authentication requires user-controlled sign-in; never supply credentials
in commits or reports. Mark unsupported/unverified features explicitly.

GitHub write authentication must work at push time. No token needs to be pasted
into chat; use the local Git credential manager. Current read-only remote access
does not establish write permission.

## Approval response requested

1. Approve the four original asset replacements and screenshot omission, or
   provide source/permission evidence for retaining specific originals.
2. Select table route A or B and supply the corresponding input locations.
3. Approve new root-history `retail-release` publication with main preserved.
4. Supply missing retail inputs and a compatible GPU execution environment;
   controller acceptance will follow preparation.

Until these decisions/inputs are available, this document is a concrete plan,
not authorization to rewrite history or evidence of a release-ready push.
