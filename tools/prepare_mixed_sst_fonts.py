"""Create a new, explicitly mixed SST comparison overlay (requires fontTools).

Copies the hash-pinned retail PSM font dependency set from exact-name sources.
Never modifies firmware, an existing stage, or emulator runtime stores.
"""
import argparse
import hashlib
import io
import json
import re
from pathlib import Path
import zipfile

from fontTools.ttLib import TTFont
from elftools.elf.elffile import ELFFile


def sha(data):
    return hashlib.sha256(data).hexdigest()


def metadata(data):
    if data[:4] not in (b"OTTO", b"\0\1\0\0"):
        raise ValueError("Not a standalone scalable OpenType/TrueType font")
    with TTFont(io.BytesIO(data), checkChecksums=2) as font:
        if not ({"CFF ", "glyf"} & set(font.keys())):
            raise ValueError("Missing scalable outlines")
        names = {}
        for key, index in (("family", 1), ("subfamily", 2), ("full_name", 4),
                           ("version", 5), ("postscript_name", 6),
                           ("typographic_family", 16), ("typographic_subfamily", 17)):
            names[key] = sorted({n.toUnicode() for n in font["name"].names if n.nameID == index})
        # Force outline/table parsing as well as the SFNT header/name records.
        glyphs = font.getGlyphSet()
        return dict(signature=data[:4].hex(), tables=sorted(font.keys()), names=names,
                    glyph_count=len(glyphs), units_per_em=font["head"].unitsPerEm,
                    weight_class=font["OS/2"].usWeightClass,
                    italic_angle=font["post"].italicAngle,
                    cmap_entries=len(font.getBestCmap() or {}))


def alias_contract(psm):
    """Hash-pinned retail IDA +203760 initializer and +206e50 path consumer.

    Each 0xf0-byte alias has eight filename slots; directory kind is at +0xd0.
    SIMD constants below supply that field, not the filename's suffix/family.
    """
    if sha(psm) != "0d6fbc45680f4fd85ef10f98c6811411ea974b2e7be5fd90fb5e03a9114deff6":
        raise ValueError("Unexpected retail PSM source")
    elf = ELFFile(io.BytesIO(psm))
    loads = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
    def read(rva, size):
        segment = next(s for s in loads if s['p_vaddr'] <= rva and rva + size <= s['p_vaddr'] + s['p_filesz'])
        offset = segment['p_offset'] + rva - segment['p_vaddr']
        return psm[offset:offset + size]
    groups = [
        (0x445200, 'SST', 0, 0x2c6970),
        (0x4452f0, 'SIE-RDC-Pr5N-JPN', 0, 0x2c6490),
        (0x4453e0, 'SSTArabic', 0, 0x2c6f00),
        (0x4454d0, 'DFHEI5-SONY2022.ttf', 1, 0x2c6e90),
        (0x4455c0, 'SCEPS4Yoongd', 0, 0x2c6c10),
        (0x4456b0, 'SSTThai', 0, 0x2c6520),
        (0x4457a0, 'SSTVietnamese', 0, 0x2c6520),
        (0x445890, 'PS4Icon.ttf', 2, None),  # explicit dword store
        (0x445980, 'SSTAribStdB24-Regular.ttf', 0, None),  # vxorps zero store
    ]
    paths = {0: '/common/font', 1: '/common/font2', 2: '/common_ex/font2'}
    filenames = {
        'SST': [f'SST-{s}.otf' for s in ('Light', 'Medium', 'Roman', 'Bold', 'LightItalic', 'MediumItalic', 'Italic', 'BoldItalic')],
        'SIE-RDC-Pr5N-JPN': ['SIE-RDC-Pr5N-M-JPN.otf', 'SIE-RDC-Pr5N-B-JPN.otf'],
        'SCEPS4Yoongd': [f'SCEPS4Yoongd-{s}.otf' for s in ('Light', 'Medium', 'Bold')],
        **{family: [f'{family}-{s}.otf' for s in ('Light', 'Medium', 'Roman', 'Bold')]
           for family in ('SSTArabic', 'SSTThai', 'SSTVietnamese')},
    }
    result = []
    for table, family, kind, constant in groups:
        if constant is not None and int.from_bytes(read(constant, 4), 'little') != kind:
            raise ValueError("Alias kind constant mismatch")
        result.append(dict(table_rva=hex(table), kind_field_rva=hex(table + 0xd0),
                           family=family, directory_kind=kind, guest_directory=paths[kind],
                           filenames=filenames.get(family, [family]),
                           kind_constant_rva=hex(constant) if constant is not None else None))
    return result


def retail_font2_rows(directory, archive_data):
    """Verify authentic original directory against matching retail ZIP members."""
    rows = []
    with zipfile.ZipFile(io.BytesIO(archive_data)) as archive:
        if sorted(archive.namelist()) != ['DFHEI5-SONY2022.ttf', 'SIE-EmojiStyle-PS5.ttf']:
            raise ValueError("Unexpected retail archive members")
        for name in archive.namelist():
            source = directory / name
            data = source.read_bytes()
            if data != archive.read(name):
                raise ValueError(f"Original retail font2 differs from archive: {name}")
            rows.append(dict(name=name, source=str(source.resolve()), sha256=sha(data), bytes=len(data),
                             provenance='original retail system_b/common/font2; matching retail ZIP member',
                             selection='kind1 alias' if name.startswith('DFHEI') else 'retain retail font2 sibling',
                             guest_directory='/common/font2', metadata=metadata(data)))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--working-fonts", type=Path, required=True)
    parser.add_argument("--retail-font-zip", type=Path, required=True)
    parser.add_argument("--retail-psm", type=Path, required=True)
    parser.add_argument("--retail-icon", type=Path, required=True)
    parser.add_argument("--retail-font2", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        raise ValueError("Output must be a new isolated directory")
    psm = args.retail_psm.read_bytes()
    if sha(psm) != "0d6fbc45680f4fd85ef10f98c6811411ea974b2e7be5fd90fb5e03a9114deff6":
        raise ValueError("Unexpected retail PSM source")
    aliases = alias_contract(psm)
    # Retail IDA +0x203760..+0x204c99 initializes all alias/style slots.
    # Base SST names and PS4Icon are partly constructed with immediates;
    # remaining names are copied from non-executable ELF load segments.
    base = {f"SST-{style}.otf" for style in (
        "Roman", "Italic", "Light", "LightItalic", "Medium", "MediumItalic", "Bold", "BoldItalic")}
    elf = ELFFile(io.BytesIO(psm))
    references = {m.group(1).decode() for segment in elf.iter_segments()
                  if segment['p_type'] == 'PT_LOAD' and not segment['p_flags'] & 1
                  for m in re.finditer(rb'(?<![A-Za-z0-9_.-])([A-Za-z0-9_-]+\.(?:otf|ttf))\x00', segment.data())}
    retail_names = {"DFHEI5-SONY2022.ttf", "SIE-EmojiStyle-PS5.ttf"}
    selected = sorted((base | references) - retail_names)
    missing = [name for name in selected if not (args.working_fonts / name).is_file()]
    if missing:
        raise ValueError(f"Missing exact PSM font dependencies: {missing}")
    payloads = {}
    rows = []
    for name in selected:
        source = args.working_fonts / name
        data = source.read_bytes()
        info = metadata(data)
        expected_ps = {"SIE-RDC-Pr5N-B-JPN.otf": "SIERodinCattleyaJPNPr5N-B",
                       "SIE-RDC-Pr5N-M-JPN.otf": "SIERodinCattleyaJPNPr5N-M"}.get(name, Path(name).stem)
        if expected_ps not in info["names"]["postscript_name"] or not info["names"]["full_name"] or not info["names"]["subfamily"]:
            raise ValueError(f"Unexpected font name/style metadata: {name}")
        if name == "SST-Bold.otf" and (info["weight_class"] != 700 or info["italic_angle"] != 0):
            raise ValueError("SST-Bold metadata mismatch")
        if name == "SST-BoldItalic.otf" and (info["weight_class"] != 700 or info["italic_angle"] == 0):
            raise ValueError("SST-BoldItalic metadata mismatch")
        payloads[name] = data
        rows.append(dict(name=name, source=str(source.resolve()), sha256=sha(data), bytes=len(data),
                         provenance="working 13.60 source; NOT authenticated retail-derived",
                          selection="base SST style family" if name in base else "explicit retail PSM filename (all families)",
                         metadata=info))
    archive_data = args.retail_font_zip.read_bytes()
    if sha(archive_data) != "749222813eac78539b926099067b17af14642bee22d922be810eab35fdfb50ba":
        raise ValueError("Unexpected retail font archive")
    # Kind1 uses the authentic original directory directly. Placing these bytes
    # in the kind0 overlay cannot satisfy /<sandbox>/common/font2 lookups.
    font2_rows = retail_font2_rows(args.retail_font2, archive_data)
    # PSM table +0x445890 selects directory kind2 at +0x445960.
    # +0x206e50 uses /common_ex/font2/ for kind2, not /common/font/.
    if args.retail_icon.name != "PS4Icon.ttf":
        raise ValueError("Expected exact retail PS4Icon.ttf source")
    icon_data = args.retail_icon.read_bytes()
    icon_info = metadata(icon_data)
    if icon_info['names']['postscript_name'] != ['PS4Icon'] or icon_info['names']['full_name'] != ['PS4Icon']:
        raise ValueError("Unexpected icon font identity")
    target = args.output / "preinst/common/font"
    target.mkdir(parents=True)
    for name, data in payloads.items():
        (target / name).write_bytes(data)
        if (target / name).read_bytes() != data:
            raise ValueError(f"Copy verification failed: {name}")
    for row in rows:
        if "member" not in row and sha(Path(row["source"]).read_bytes()) != row["sha256"]:
            raise ValueError("Source changed during staging")
    if args.retail_font_zip.read_bytes() != archive_data:
        raise ValueError("Retail archive changed during staging")
    for row in font2_rows:
        if sha(Path(row['source']).read_bytes()) != row['sha256']:
            raise ValueError("Original font2 changed during staging")
    icon_target = args.output / "common_ex/font2/PS4Icon.ttf"
    icon_target.parent.mkdir(parents=True)
    icon_target.write_bytes(icon_data)
    if icon_target.read_bytes() != icon_data or args.retail_icon.read_bytes() != icon_data:
        raise ValueError("Icon source/copy changed")
    icon_row = dict(name="PS4Icon.ttf", source=str(args.retail_icon.resolve()), sha256=sha(icon_data),
                    bytes=len(icon_data), provenance="original retail system_ex_b/common_ex/font2",
                    selection="PSM constructed filename and directory kind2", metadata=icon_info)
    receipt = dict(classification="MIXED FONT COMPARISON; NOT authenticated retail-derived",
                    retail_psm_sha256=sha(psm), retail_archive_sha256=sha(archive_data),
                    alias_directory_contract=aliases,
                    font2_directory=str(args.retail_font2.resolve()), font2_files=font2_rows,
                    font_directory=str(target.resolve()), files=rows,
                    declared_font_count=len(rows), icon_directory=str(icon_target.parent.resolve()), icon_files=[icon_row],
                    dependency_inventory=dict(data_references=sorted(references), constructed_names=sorted(base | {"PS4Icon.ttf"}),
                                              missing_references=missing,
                                              unused_working_files=sorted(p.name for p in args.working_fonts.iterdir() if p.is_file() and p.name not in selected)),
                   mounts=[dict(guest=g, host=str(target.resolve()), read_only=True)
                           for g in ("/common/font", "/kyty/common/font", "/preinst/common/font")] +
                          [dict(guest="/common_ex/font2", host=str(icon_target.parent.resolve()), read_only=True),
                           dict(guest="/common/font2", host=str(args.retail_font2.resolve()), read_only=True)],
                   guest_runtime_verified=False)
    (args.output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(dict(directory=str(target.resolve()), common_fonts=len(rows), icon_fonts=1,
                         total_staged_files=len(rows) + 1, direct_retail_font2_files=len(font2_rows),
                         receipt=str((args.output / "receipt.json").resolve())), indent=2))


if __name__ == "__main__":
    main()
