#ifndef KYTY_LIBS_KIT_PROFILE_H_
#define KYTY_LIBS_KIT_PROFILE_H_

// PORT (kytyps5, 13.60 FW lineage): ported from voldemort-ref (4.03 FW lineage) kitProfile.h.
// His file documents observed SceShellCore references to SceKitProfile::* (SystemName,
// IsAttached / LoadStartStatus / SaveStartStatus LoadSaveDataMount). Ours currently resolves
// none of those names (grep of src/libs finds no SceKitProfile at all), so the file lands as
// documentation-only; when the next absent-NID survey names the blocking NID, port the exact
// observed algorithm here rather than inventing kit behavior.
// Plain header, intentionally no code: included for searchability alongside the other shims.

namespace Libs::KitProfile {
// Observed SceKitProfile::* surface (names only; no behavior ported yet):
//   SystemName (SceShellCore references; backing store unknown on retail)
//   IsAttached / LoadStartStatus / SaveStartStatus LoadSaveDataMount
// Carrier rule: ShellCore decides save-pack availability off IsAttached-ish paths;
// do NOT stub these as always-false without a blocking NID in hand.
}

#endif
