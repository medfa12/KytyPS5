"""IDA batch script: export named RVA windows, xrefs and available decompilation.

Run only against private, hash-verified firmware copies. IDA receives a JSON
specification through idc.ARGV: image SHA256, output path and RVA windows.
This script never attaches to a guest or patches source firmware.
"""
import hashlib
import json
import traceback

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_lines
import ida_nalt
import ida_ua
import idautils
import idc


def main():
    spec = json.load(open(idc.ARGV[1], encoding='utf-8'))
    source = ida_nalt.get_input_file_path()
    digest = hashlib.sha256()
    with open(source, 'rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            digest.update(block)
    actual = digest.hexdigest()
    if actual != spec['sha256']:
        raise RuntimeError('input hash does not match specification')
    base = ida_nalt.get_imagebase()
    ida_auto.auto_wait()
    has_hexrays = ida_hexrays.init_hexrays_plugin()
    with open(spec['output'], 'w', encoding='utf-8', newline='\n') as out:
        def line(text=''):
            print(text, file=out)
        line(f'IDA input_sha256={actual} imagebase={base:#x} hexrays={has_hexrays}')
        for item in spec.get('data_refs', []):
            address = base + int(item['rva'], 16)
            line(f'\n### DATA {item["name"]} RVA {address-base:#x}')
            for ref in idautils.XrefsTo(address, 0):
                owner = ida_funcs.get_func(ref.frm)
                line(f'XREF {ref.frm-base:#x} type={ref.type} function=' +
                     (f'{owner.start_ea-base:#x}' if owner else 'unknown') + ' ' +
                     ida_lines.tag_remove(idc.generate_disasm_line(ref.frm, 0) or ''))
        for window in spec['windows']:
            rva, size = int(window['rva'], 16), int(window['size'], 16)
            start, end = base + rva, base + rva + size
            line(f'\n### {window["name"]} RVA {rva:#x} SIZE {size:#x}')
            if not ida_bytes.is_mapped(start) or not ida_bytes.is_mapped(end - 1):
                line('UNMAPPED'); continue
            # Managed AOT functions may have no symbol/function extent. Define
            # only at explicitly selected starts when IDA has not identified one.
            if not ida_funcs.get_func(start):
                ida_ua.create_insn(start)
                ida_funcs.add_func(start)
                ida_auto.auto_wait()
            fn = ida_funcs.get_func(start)
            line('function=' + (f'{fn.start_ea - base:#x}..{fn.end_ea - base:#x}' if fn else 'unknown'))
            line('xrefs_to_start: ' + ', '.join(f'{x.frm - base:#x}/{x.type}' for x in idautils.XrefsTo(start, 0)))
            ea = start
            while ea < end:
                if not ida_bytes.is_code(ida_bytes.get_full_flags(ea)):
                    ida_ua.create_insn(ea)
                text = ida_lines.tag_remove(idc.generate_disasm_line(ea, 0) or '')
                length = idc.get_item_size(ea)
                line(f'{ea-base:08x} {text}')
                ea += max(1, length)
            if window.get('decompile') and has_hexrays and fn and fn.start_ea == start:
                try:
                    line('--- HEX-RAYS ---')
                    for item in ida_hexrays.decompile(start).get_pseudocode():
                        line(ida_lines.tag_remove(item.line))
                except Exception as error:
                    line('DECOMPILE_ERROR ' + str(error))
    idc.qexit(0)


try:
    main()
except Exception:
    traceback.print_exc()
    idc.qexit(1)
