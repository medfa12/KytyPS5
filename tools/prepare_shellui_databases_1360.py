"""Initialize the missing offline ShellUI databases for the 13.60 dump.

This is a port of voldemort's ``prepare_shellui_databases.py`` (the 4.03/devkit
reference) to the 13.60 firmware layout. This is local/test reader metadata,
not native AppDb schema admission or installed-title delivery. Sce.Vsh.Accessor.Db
assembly is loaded by Sce.Vsh.ShellUI.ReactNativeApps and
Sce.Vsh.AppInstUtilWrapper and reads these tables directly. It imports genuine
installed-app descriptors and images, never substitutes a UI or JS bundle.
Existing rows are preserved. dnfile reads PE metadata, not method code.

Intentional differences from the 4.03 reference (all 13.60-driven):

  * Firmware layout: apps under ``system_ex_b/rnps/apps``, the av-content
    template under ``system_b/priv/mms_ro/template_content.db`` (the devkit
    dump used ``system_ex/system_ex/...`` and ``system/system/priv/...``).
  * Schema contract: 13.60's Accessor.Db does not carry the 4.03 literal
    ``cicon.localConceptId AS LocalConceptId,`` projection; it composes
    lowercase join fragments instead. The schema columns are still derived
    from the assembly's own query strings, and the post-seed verification runs
    the same join shape the reference used, projecting ``cicon.localConceptId``
    when the literal projection is absent.
  * Row shape: ``insert_missing`` lazily adds any column it is about to write,
    so the genuine descriptor values are stored even where the 13.60 assembly
    references a smaller column set than the 4.03 one.
  * Hub auto-open is suppressed unless the title is staged in
    ``staged_titles.json``: the 13.60 RNPS bundles are Hermes bytecode, not the
    plaintext staging this harness can load, and SceneFactory does not catch an
    unavailable URL configuration. Genuine tiles/artwork are kept regardless.
"""
from __future__ import annotations

import argparse
from contextlib import closing
import json
import re
import shutil
import sqlite3
from functools import lru_cache
from pathlib import Path

import dnfile

WORKSPACE = Path(__file__).resolve().parent.parent
from local_paths import configured_path


@lru_cache(maxsize=4)
def user_strings(image: Path) -> list[str]:
    """Return the #US (user string) heap of a managed PE image.

    dnfile reads the metadata stream; it does not decode method bodies.
    """
    pe = dnfile.dnPE(str(image))
    stream = pe.net.metadata.streams[b"#US"]
    data = stream.get_data_at_offset(0, stream.sizeof())
    offset = 1
    strings: list[str] = []
    while offset < len(data):
        first = data[offset]
        if first < 128:
            length, prefix = first, 1
        elif first < 192:
            length, prefix = ((first & 63) << 8) | data[offset + 1], 2
        else:
            length = ((first & 31) << 24) | int.from_bytes(data[offset + 1:offset + 4], "big")
            prefix = 4
        strings.append(data[offset + prefix:offset + prefix + max(0, length - 1)].decode("utf-16-le"))
        offset += prefix + length
    return strings


def columns(strings: list[str], aliases: list[str]) -> list[str]:
    """Union of ``alias.column`` references, preserving first-seen spelling."""
    names: dict[str, str] = {}
    joined = "\n".join(strings)
    for alias in aliases:
        for name in re.findall(r"\b" + alias + r"\.([A-Za-z_][A-Za-z0-9_]*)", joined):
            names.setdefault(name.lower(), name)
    return list(names.values())


def ensure_table(conn: sqlite3.Connection, name: str, names: list[str], key: str) -> None:
    # NULL leaves unavailable metadata absent; CLR readers retain their default
    # values. Unknown dates must not become bogus zero dates or empty strings.
    deduped: dict[str, str] = {}
    for col in names:
        deduped.setdefault(col.lower(), col)
    names = list(deduped.values())
    definitions = [f'"{col}"' + (" TEXT PRIMARY KEY" if col.lower() == key.lower() else "") for col in names]
    conn.execute(f'CREATE TABLE IF NOT EXISTS "{name}" ({", ".join(definitions)})')
    existing = {row[1].lower() for row in conn.execute(f'PRAGMA table_info("{name}")')}
    for col in names:
        if col.lower() not in existing:
            conn.execute(f'ALTER TABLE "{name}" ADD COLUMN "{col}"')


def insert_missing(conn: sqlite3.Connection, table: str, key: str, values: dict) -> None:
    if conn.execute(f'SELECT 1 FROM "{table}" WHERE "{key}"=?', (values[key],)).fetchone():
        return
    # A few genuine descriptor fields may not appear in this assembly's query
    # strings; store them anyway rather than dropping real metadata.
    existing = {row[1] for row in conn.execute(f'PRAGMA table_info("{table}")')}
    for name in values:
        if name not in existing:
            conn.execute(f'ALTER TABLE "{table}" ADD COLUMN "{name}"')
    cols = ",".join(f'"{name}"' for name in values)
    conn.execute(f'INSERT INTO "{table}" ({cols}) VALUES ({",".join("?" for _ in values)})', tuple(values.values()))


def descriptor_display_location(param: dict, external: bool) -> int:
    if external and "displayLocation" not in param and param["applicationCategoryType"] == 0:
        # LOCAL/TEST extracted-game import policy, not native admission. Retail
        # Accessor.Db constants separate Game space (1 << 1), Library (1 << 3)
        # and the Home experience switcher (1 << 7). Game space alone (2) is
        # excluded by both latter readers. Supplied descriptor flags stay exact.
        return (1 << 1) | (1 << 3) | (1 << 7)
    return param["displayLocation"]


def repair_default_game_display(conn: sqlite3.Connection, user_id: int, param: dict) -> int:
    """Repair only this importer's former Game-space-only local-game default.

    Caller owns the transaction and copy/staging boundary. Preserve customized
    flags, all install/entitlement fields, versions, recovery markers and rows.
    """
    if "displayLocation" in param or param["applicationCategoryType"] != 0:
        return 0
    if not 0x10000000 < user_id < 0x20000000:
        raise ValueError("An available persistent user ID is required")
    title = param["titleId"]
    if not re.fullmatch(r"[A-Z]{4}[0-9]{5}", title):
        raise ValueError("Invalid game title")
    concept = "cid:local:" + title
    metadata = "/user/appmeta/" + title
    flags = descriptor_display_location(param, True)
    # Do not mutate an unrelated/user-customized row merely sharing a title.
    content = conn.execute('SELECT categoryType,metaDataPath FROM tbl_contentinfo WHERE titleId=?', (title,)).fetchone()
    if content is None or tuple(content) != (0, metadata):
        return 0
    changed = conn.execute(f'UPDATE "tbl_iconinfo_{user_id:010d}" SET dispLocation=? '
                           "WHERE titleId=? AND localConceptId=? AND dispLocation=2 AND typeof(dispLocation)='integer'",
                           (flags, title, concept)).rowcount
    changed += conn.execute(f'UPDATE "tbl_concepticoninfo_{user_id:010d}" SET dispLocation=? '
                            "WHERE primaryTitleId=? AND localConceptId=? AND metadataPath=? AND dispLocation=2 AND typeof(dispLocation)='integer'",
                            (flags, title, concept, metadata)).rowcount
    return changed


def diagnostic_hub_uri(managed: Path, app_root: Path, directory: Path, param: dict) -> str:
    uri = param.get("hubAppUri") or ""
    app_title = directory.relative_to(app_root).parts[0]
    if not uri or app_title not in {"NPXS40075", "NPXS40097"}:
        return uri
    # HomeUI opens these embedded hubs on focus, before the user launches the
    # tile. SceneFactory does not catch an unavailable URL configuration, so do
    # not auto-open a hub whose script the harness cannot load. The 13.60 RNPS
    # bundles are Hermes bytecode (not the plaintext staging this loader path
    # accepts), so only a staged title is considered loadable here.
    staged_titles = json.loads((managed / "rnps/staged_titles.json").read_text())
    if app_title not in staged_titles:
        return ""
    bundle_root = managed / "rnps/apps" if app_title in staged_titles else app_root
    bundle = bundle_root / app_title / "application.ps.bundle"
    if not bundle.is_file():
        return ""
    with bundle.open("rb") as stream:
        header = stream.read(8)
    return uri if len(header) == 8 and header != b"RNPSHEDR" else ""


def prepare(destination: Path, managed: Path, firmware: Path, user_id: int,
            locale: str, development_apps: bool = False, external_app_root: Path | None = None,
            system_apps: tuple[str, ...] = ()) -> int:
    if not 0x10000000 < user_id < 0x20000000:
        raise ValueError("An available persistent user ID is required")
    strings = user_strings(managed / "common/Sce.Vsh.Accessor.Db.dll")
    # 4.03 carries a literal projection; 13.60 composes join fragments. Both
    # drive the same ConceptBrowseItemAccessor join, so fall back to the key.
    concept_select = next((s for s in strings
                           if s.startswith("cicon.localConceptId AS LocalConceptId,")), None)
    app_strings = [s for s in strings if re.search(r"\b(cicon|ticon|icon|content|metadata)\.", s)]
    schemas = {
        "tbl_conceptmetadata": (columns(app_strings, ["metadata"]) + ["localConceptId"], "localConceptId"),
        "tbl_contentinfo": (columns(app_strings, ["content"]), "titleId"),
        f"tbl_iconinfo_{user_id:010d}": (columns(app_strings, ["icon", "ticon"]), "titleId"),
        f"tbl_concepticoninfo_{user_id:010d}": (columns(app_strings, ["cicon"]), "localConceptId"),
    }
    if not all("localConceptId" in names for names, key in schemas.values() if key == "localConceptId"):
        raise ValueError("Missing concept key in managed query contract")
    if "titleId" not in schemas["tbl_contentinfo"][0]:
        raise ValueError("Missing content key in managed query contract")
    descriptors = []
    app_root = firmware / "system_ex_b/rnps/apps"
    descriptor_paths = ["NPXS40071/appdb/default", "NPXS40071/appdb/NPXS40139"]
    if development_apps:
        # Import the dump's actual built-in app records. Disc placeholders are
        # conditional media entries, not installed applications. Do not seed them.
        descriptor_paths += [
            "NPXS40008/appdb/NPXS20993", "NPXS40008/appdb/NPXS40165",
            "NPXS40041/appdb/default", "NPXS40047/appdb/default",
            "NPXS40037/appdb/default", "NPXS40063/appdb/default",
            "NPXS40075/appdb/default", "NPXS40097/appdb/default",
        ]
    supported_system_apps = {"NPXS40037", "NPXS40047"}
    if any(title not in supported_system_apps for title in system_apps):
        raise ValueError("Unsupported system app descriptor")
    for title in dict.fromkeys(system_apps):
        relative_path = title + "/appdb/default"
        if relative_path not in descriptor_paths:
            descriptor_paths.append(relative_path)
    for relative_path in descriptor_paths:
        directory = app_root / relative_path
        param = json.loads((directory / "param.json").read_text(encoding="utf-8"))
        if not (directory / "icon0.png").is_file():
            raise ValueError(f"App icon is missing: {directory}")
        metadata = "/system_ex/rnps/apps/" + directory.relative_to(app_root).as_posix()
        descriptors.append((directory, param, metadata, False))
    if external_app_root is not None:
        paths = sorted(external_app_root.glob("*/sce_sys/param.json"))
        if not paths:
            raise ValueError(f"No extracted app descriptors: {external_app_root}")
        titles = {param["titleId"] for _, param, _, _ in descriptors}
        for path in paths:
            param = json.loads(path.read_text(encoding="utf-8-sig"))
            title = param["titleId"]
            if not re.fullmatch(r"[A-Z]{4}[0-9]{5}", title) or title in titles:
                raise ValueError(f"Invalid or duplicate app title: {title}")
            if not (path.parent / "icon0.png").is_file():
                raise ValueError(f"App icon is missing: {path.parent}")
            titles.add(title)
            descriptors.append((path.parent, param, f"/user/appmeta/{title}", True))
    destination.mkdir(parents=True, exist_ok=True)
    with closing(sqlite3.connect(destination / "app.db")) as conn, conn:
        for table, (names, key) in schemas.items():
            ensure_table(conn, table, names, key)
        for order, (directory, param, metadata, external) in enumerate(descriptors):
            title = param["titleId"]
            concept = "cid:local:" + title
            locales = param["localizedParameters"]
            label = locales.get(locale, locales[locales["defaultLanguage"]])["titleName"]
            icon = metadata + "/icon0.png"
            hub_uri = (param.get("hubAppUri") or "") if external else diagnostic_hub_uri(managed, app_root, directory, param)
            deeplink = param.get("deeplinkUri", "")
            # 13.60 ShellCore +0xfb4140 preserves a supplied DEEPLINK_URI,
            # otherwise +0xfb428b/+0xfb4333 generates this for native categories.
            if external and not deeplink and param["applicationCategoryType"] in (0, 0x10000, 0x20000):
                deeplink = "psgm:play?id=" + title
            display_location = descriptor_display_location(param, external)
            shared = dict(localConceptId=concept, dispLocation=display_location, platform=0,
                          hubAppUri=hub_uri, psNowFlag=0, lastAccessTime=None)
            insert_missing(conn, "tbl_contentinfo", "titleId", dict(
                titleId=title, titleName=label, metaDataPath=metadata, icon0Info=icon,
                contentStatus=0, contentLocation=0, installStatus=0, appinfoContentType=1 if external else 4,
                categoryType=param["applicationCategoryType"], bgMatteType=param.get("backgroundBasematType"),
                pprDeeplinkUri=deeplink, hddLocation=0, externalHddAppStatus=0,
                uninstallable=0, sortPriority=100, pathInfo=0, pathInfo2=0, size=0))
            insert_missing(conn, "tbl_conceptmetadata", "localConceptId", dict(
                localConceptId=concept, pathInfo=0, pathInfo2=0, totalSize=0, sortPriority=100,
                hddLocation=0, externalHddAppStatus=0, mergedContentsStatus=0, mergedOperationCapability=0))
            insert_missing(conn, f"tbl_iconinfo_{user_id:010d}", "titleId", dict(
                **shared, titleId=title, titleName=label, conceptId=concept,
                contentId=param["contentId"], visible=1, installStatus=2 if external else 0, entEntitlement=0,
                entFilTypeEnt=0, entHidden=0, lastAccessIndex=order, primaryTitleSort=0))
            insert_missing(conn, f"tbl_concepticoninfo_{user_id:010d}", "localConceptId", dict(
                **shared, primaryTitleId=title, primaryTitleName=label, conceptName=label,
                primaryTitlePlatform=0, primaryTitleBgMatteType=param.get("backgroundBasematType"),
                validFlag=1, installedStatus=0, entitlementStatus=0, metadataPath=metadata,
                icon0Info=icon, lastAccessIndex=order, totalPlayedTime=0, totalEstimatedSize=0,
                genreFlags=0, deeplinkUri=deeplink))
            if external:
                repair_default_game_display(conn, user_id, param)
                # AppBrowseInstallStatus differs from AppInfo.InstallStatus:
                # 13.60 AppBrowseItem::IsInstalled (+0x6090) compares the
                # icon-row status to 2. Content-row installed remains 0.
                conn.execute(f'UPDATE "tbl_iconinfo_{user_id:010d}" SET installStatus=2 '
                             'WHERE titleId=? AND localConceptId=? AND installStatus=0',
                             (title, concept))
                if deeplink:
                    conn.execute('UPDATE tbl_contentinfo SET pprDeeplinkUri=? '
                                 'WHERE titleId=? AND coalesce(pprDeeplinkUri,\'\')=\'\'',
                                 (deeplink, title))
                    conn.execute(f'UPDATE "tbl_concepticoninfo_{user_id:010d}" SET deeplinkUri=? '
                                 'WHERE localConceptId=? AND coalesce(deeplinkUri,\'\')=\'\'',
                                 (deeplink, concept))
            # Repair earlier diagnostic imports too; preserve customized URIs
            # and restrict the update to the exact local descriptor identity.
            original_hub = param.get("hubAppUri") or ""
            if original_hub:
                for table, title_column in [
                    (f"tbl_iconinfo_{user_id:010d}", "titleId"),
                    (f"tbl_concepticoninfo_{user_id:010d}", "primaryTitleId"),
                ]:
                    conn.execute(f'UPDATE "{table}" SET hubAppUri=? '
                                 f'WHERE localConceptId=? AND "{title_column}"=? '
                                 'AND hubAppUri IN (?, ?)',
                                 (hub_uri, concept, title, original_hub, ""))
        # Execute the ConceptBrowseItemAccessor projection and joins. Use the
        # assembly's own projection when present; otherwise its key column.
        query = ("SELECT " + (concept_select or "cicon.localConceptId")
                 + f" FROM tbl_concepticoninfo_{user_id:010d} AS cicon "
                 "LEFT OUTER JOIN tbl_conceptmetadata AS metadata USING(localConceptId) "
                 f"LEFT OUTER JOIN tbl_iconinfo_{user_id:010d} AS ticon USING(localConceptId) "
                 "LEFT OUTER JOIN tbl_contentinfo AS content USING(titleId) "
                 "WHERE cicon.validFlag=1 GROUP BY cicon.localConceptId LIMIT ? OFFSET ?")
        rows = conn.execute(query, (100, 0)).fetchall()
        if not all(any(row[0] == "cid:local:" + param["titleId"] for row in rows) for _, param, _, _ in descriptors):
            raise ValueError("Installed-app records were not returned by the managed query")
        if conn.execute("PRAGMA integrity_check").fetchone()[0] != "ok":
            raise ValueError("App database integrity check failed")
    av = destination / "av_content.db"
    if not av.exists() or av.stat().st_size == 0:
        shutil.copyfile(firmware / "system_b/priv/mms_ro/template_content.db", av)
    counters = ["download_inprogress", "download_pause", "download_error", "download_complete",
                "upload_inprogress", "upload_error", "upload_complete", "upload_inprogress_crashreport",
                "upload_error_crashreport", "upload_complete_crashreport"]
    with closing(sqlite3.connect(destination / "bgft.db")) as conn, conn:
        conn.execute("CREATE TABLE IF NOT EXISTS tbl_summary (" + ",".join(name + " INTEGER NOT NULL DEFAULT 0" for name in counters) + ")")
        if not conn.execute("SELECT 1 FROM tbl_summary").fetchone():
            conn.execute("INSERT INTO tbl_summary DEFAULT VALUES")
        conn.execute("SELECT " + ",".join(counters) + " FROM tbl_summary LIMIT 1").fetchone()
    print(f"Offline ShellUI databases: app descriptors={len(descriptors)}, user=0x{user_id:08x}")
    return len(descriptors)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, default=WORKSPACE / "_SystemData/priv/mms")
    parser.add_argument("--managed", type=Path, default=WORKSPACE / "_ShellUIManaged")
    parser.add_argument("--firmware", type=Path, help="Extracted 13.60 firmware root (or KYTY_FIRMWARE_ROOT)")
    parser.add_argument("--user-id", type=lambda s: int(s, 0), default=0x10000001)
    parser.add_argument("--user-store", type=Path,
                        help="Prepare app-list tables for saved local users and available creation slots")
    parser.add_argument("--locale", default="en-US")
    parser.add_argument("--development-apps", action="store_true",
                        help="Import genuine built-in and development app descriptors from the dump")
    parser.add_argument("--system-app", choices=("NPXS40037", "NPXS40047"), action="append", default=[],
                        help="Include a selected genuine Plus or Store descriptor; does not establish admission or PSN")
    parser.add_argument("--external-app-root", type=Path,
                        help="Extracted apps with <directory>/sce_sys/param.json; mount at /user/app/<titleId>")
    args = parser.parse_args()
    args.firmware = configured_path(args.firmware)
    if not (args.firmware / "system_ex_b/rnps/apps").is_dir():
        raise SystemExit(f"Not a 13.60 firmware root: {args.firmware}")
    if not (args.managed / "common/Sce.Vsh.Accessor.Db.dll").is_file():
        raise SystemExit(f"Managed ShellUI staging required first: {args.managed}")
    user_ids = {args.user_id}
    if args.user_store:
        data = {"version": 1, "users": [], "next_user_number": 1}
        if args.user_store.exists():
            data = json.loads(args.user_store.read_text(encoding="utf-8"))
        if data.get("version") != 1 or len(data["users"]) > 16:
            raise ValueError("Invalid local user store")
        user_ids.update(record["id"] for record in data["users"])
        next_number = data["next_user_number"]
        if not 0 < next_number < 0x10000000:
            raise ValueError("Invalid next local user ID")
        # Tables supply app metadata only; they do not register or log in users.
        # Pre-create slots so a new profile can open HomeUI in the same session.
        user_ids.update(0x10000000 | number for number in
                        range(next_number, min(next_number + 16, 0x10000000)))
    for user_id in sorted(user_ids):
        prepare(args.destination, args.managed, args.firmware, user_id,
                args.locale, args.development_apps, args.external_app_root, tuple(args.system_app))
