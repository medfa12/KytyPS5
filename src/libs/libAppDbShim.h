#ifndef KYTY_LIBS_LIB_APP_DB_SHIM_H_
#define KYTY_LIBS_LIB_APP_DB_SHIM_H_

// PORT (kytyps5, 13.60 FW lineage): ported from voldemort-ref (4.03 FW lineage) libAppDbShim.h.
// Real SQLite implementation stays in voldemort-ref (Windows winsqlite3 + /system_data/priv/mms/app.db).
// OUR tree owns app.db lifecycle differently (offline DB seeding); this header carries ONLY the
// documented table inventory so future AppDb NID work writes against OUR sqlite wrapper instead
// of forking a second AppDb owner. No code, no behavior change.
#include <cstdint>

namespace Libs::AppDbShim {
// Observed app.db surface (table inventory, from the reference implementation):
//   tbl_contentinfo: titleId, titleName, metaDataPath, icon0Info (title icon path),
//     pprDeeplinkUri, contentStatus, contentLocation, installStatus, appinfoContentType,
//     categoryType, hddLocation, externalHddAppStatus, uninstallable, sortPriority,
//     pathInfo(2 cols), size
//   tbl_conceptmetadata: localConceptId, pathInfo(2), totalSize, sortPriority, hddLocation,
//     externalHddAppStatus, mergedContentsStatus, mergedOperationCapability
//   tbl_iconinfo_<10 digits>: localConceptId, titleId, titleName, conceptId, contentId,
//     visible, dispLocation (bitmask: 128 experience switcher | 8 library | 2 game space),
//     discStatus, platform (1 = CUSA game), installStatus, entEntitlement, entFilTypeEnt,
//     entHidden, lastAccessIndex (monotonic per-table), primaryTitleSort, hubAppUri, psNowFlag
//   tbl_concepticoninfo_<10 digits>: localConceptId, primaryTitleId/Name, conceptName,
//     primaryTitlePlatform, dispLocation, platform, validFlag, installedStatus,
//     entitlementStatus, metadataPath, icon0Info, pic0Info, lastAccessIndex,
//     totalPlayedTime, totalEstimatedSize, genreFlags, deeplinkUri, hubAppUri, psNowFlag
// Carrier rule: virtual-disc rows use concept id prefix "cid:virtualdisc:" and
// metadata "/mnt/disc/sce_sys"; an installed copy owns its content row (do not overwrite).
}

#endif
