import ctypes
import json
import os
import re
import struct
from collections import defaultdict

XBE_PATH = "extracted/Default.xbe"
MAP_PATH = "extracted/final.map"
OUT_DIR = "decompile"

XBE_SECTION_VAS = {
    1:  ("0001", ".text",    0x00011000, 0x00580390),
    2:  ("0002", "D3D",      0x005913A0, 0x00011614),
    3:  ("0003", "D3DX",     0x005A29C0, 0x000021E8),
    4:  ("0004", "XGRPH",    0x005A4BC0, 0x00002018),
    5:  ("0005", "DSOUND",   0x005A6BE0, 0x0001CCC4),
    6:  ("0006", "WMADECXM", 0x005C38C0, 0x00000838),
    7:  ("0007", "WMADEC",   0x005C4100, 0x00018D80),
    8:  ("0008", "WMVDEC",   0x005DCE80, 0x000328B4),
    9:  ("0009", "XPP",      0x0060F740, 0x0000762C),
    10: ("000a", ".rdata",   0x00616D80, 0x0013A104),
    11: ("000b", ".data",    0x00750EA0, 0x001C5514),
    12: ("000c", "DOLBY",    0x009163C0, 0x00007180),
}

dbghelp = ctypes.windll.dbghelp
UnDecorateSymbolName = dbghelp.UnDecorateSymbolName
UnDecorateSymbolName.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint]
UnDecorateSymbolName.restype = ctypes.c_uint

def demangle(name: str) -> str:
    if name.startswith("??_C@_0"):
        # Extract readable part from MSVC mangled string literal if possible
        return f"[string literal] {name}"
    if not name.startswith("?"):
        if name.startswith("__imp_"):
            inner = name[6:]
            if inner.startswith("?"):
                return "__imp_ " + demangle(inner)
        return name
    buf = ctypes.create_string_buffer(2048)
    res = UnDecorateSymbolName(name.encode("ascii", errors="ignore"), buf, 2048, 0)
    if res > 0:
        return buf.value.decode("ascii", errors="replace")
    return name

def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    # Read XBE kernel thunk table ordinals from Default.xbe (.rdata at raw offset 0x609000)
    with open(XBE_PATH, "rb") as f:
        xbe_data = f.read()

    kt_raw_off = 0x609000
    kt_va_base = 0x00616D80
    xbe_ordinals = {}
    idx = 0
    while True:
        val = struct.unpack_from("<I", xbe_data, kt_raw_off + idx * 4)[0]
        if val == 0:
            break
        xbe_ordinals[kt_va_base + idx * 4] = val & 0x7FFFFFFF
        idx += 1

    sym_re = re.compile(
        r"^\s*([0-9a-fA-F]{4}):([0-9a-fA-F]{8})\s+(\S+)\s+([0-9a-fA-F]{8})\s+(f\s+i|f\s+|\s+i|\s+)\s*(\S.*)$"
    )

    by_va = {}
    kernel_imports = []
    module_stats = defaultdict(lambda: {"symbols": 0, "functions": 0, "unique_funcs": 0, "library": ""})
    in_statics = False

    with open(MAP_PATH, "r", encoding="latin1") as f:
        for line in f:
            if "Static symbols" in line:
                in_statics = True
                continue
            m = sym_re.match(line)
            if not m:
                continue
            sec_str, off_str, name, pe_va_str, flags, lib_obj = m.groups()
            sec_idx = int(sec_str, 16)
            sec_off = int(off_str, 16)
            pe_va = int(pe_va_str, 16)
            is_func = "f" in flags
            is_inline = "i" in flags
            lib_obj = lib_obj.strip()
            if ":" in lib_obj:
                lib, obj = lib_obj.split(":", 1)
            else:
                lib, obj = "SlateEngine", lib_obj

            module_stats[lib_obj]["symbols"] += 1
            module_stats[lib_obj]["library"] = lib
            if is_func:
                module_stats[lib_obj]["functions"] += 1

            if sec_idx not in XBE_SECTION_VAS:
                continue

            _, sec_name, sec_base_va, _ = XBE_SECTION_VAS[sec_idx]
            xbe_va = sec_base_va + sec_off

            # Check if this is a kernel import thunk entry in .rdata (0x00616D80 .. 0x00616F5C)
            if sec_idx == 10 and sec_off < 0x1E0 and xbe_va in xbe_ordinals:
                if name.startswith("__imp_"):
                    clean = name.replace("__imp__", "").replace("__imp_", "")
                    kernel_imports.append({
                        "thunk_va": f"0x{xbe_va:08X}",
                        "thunk_va_int": xbe_va,
                        "ordinal": xbe_ordinals[xbe_va],
                        "mangled_imp": name,
                        "symbol": clean,
                    })

            if xbe_va not in by_va:
                demangled_name = demangle(name)
                by_va[xbe_va] = {
                    "xbe_va": xbe_va,
                    "xbe_va_hex": f"0x{xbe_va:08X}",
                    "pe_va_hex": f"0x{pe_va:08X}",
                    "section": sec_name,
                    "sec_idx": sec_idx,
                    "sec_offset": sec_off,
                    "name": name,
                    "demangled": demangled_name,
                    "is_func": is_func,
                    "is_inline": is_inline,
                    "is_static": in_statics,
                    "library": lib,
                    "object": obj,
                    "aliases": [],
                }
                if is_func:
                    module_stats[lib_obj]["unique_funcs"] += 1
            else:
                entry = by_va[xbe_va]
                # Prefer non-inline or shorter canonical symbol as primary
                if (entry["is_inline"] and not is_inline) or (entry["name"].startswith("_$E") and not name.startswith("_$E")):
                    entry["aliases"].append(entry["name"])
                    entry["name"] = name
                    entry["demangled"] = demangle(name)
                    entry["is_inline"] = is_inline
                    entry["library"] = lib
                    entry["object"] = obj
                else:
                    if name != entry["name"] and name not in entry["aliases"]:
                        entry["aliases"].append(name)

    # Sort all unique symbols by XBE VA and compute inferred byte size within each section
    sorted_vas = sorted(by_va.keys())
    for i, va in enumerate(sorted_vas):
        entry = by_va[va]
        sec_idx = entry["sec_idx"]
        _, _, sec_base, sec_size = XBE_SECTION_VAS[sec_idx]
        sec_end = sec_base + sec_size
        if i + 1 < len(sorted_vas) and by_va[sorted_vas[i + 1]]["sec_idx"] == sec_idx:
            entry["size"] = sorted_vas[i + 1] - va
        else:
            entry["size"] = max(0, sec_end - va)

    # Write kernel_imports.json
    kernel_imports.sort(key=lambda x: x["thunk_va_int"])
    with open(os.path.join(OUT_DIR, "kernel_imports.json"), "w", encoding="utf-8") as f:
        json.dump(kernel_imports, f, indent=2)

    # Write functions.tsv
    funcs = [by_va[va] for va in sorted_vas if by_va[va]["is_func"]]
    with open(os.path.join(OUT_DIR, "functions.tsv"), "w", encoding="utf-8") as f:
        f.write("xbe_va\tpe_va\tsize\tsection\tlibrary\tobject\tis_inline\tis_static\tmangled_name\tdemangled_signature\n")
        for fn in funcs:
            f.write(
                f"{fn['xbe_va_hex']}\t{fn['pe_va_hex']}\t{fn['size']}\t{fn['section']}\t"
                f"{fn['library']}\t{fn['object']}\t{int(fn['is_inline'])}\t{int(fn['is_static'])}\t"
                f"{fn['name']}\t{fn['demangled']}\n"
            )

    # Write symbols.json (compact list of all unique VA entries)
    with open(os.path.join(OUT_DIR, "symbols.json"), "w", encoding="utf-8") as f:
        json.dump([by_va[va] for va in sorted_vas], f, indent=2)

    # Write modules_summary.json
    mod_list = [
        {"module": k, **v}
        for k, v in sorted(module_stats.items(), key=lambda x: (-x[1]["unique_funcs"], -x[1]["symbols"]))
    ]
    with open(os.path.join(OUT_DIR, "modules_summary.json"), "w", encoding="utf-8") as f:
        json.dump(mod_list, f, indent=2)

    # Write Ghidra and IDA symbol import scripts
    ghidra_script = '''# Ghidra Python script to import all 75,668 Buffy the Vampire Slayer (Xbox) symbols from symbols.json
# Run via Ghidra Script Manager on XGameBuffy_xbox.exe or Default.xbe
import json
import os
from ghidra.program.model.symbol import SourceType

sym_path = os.path.join(os.path.dirname(__file__), "symbols.json")
with open(sym_path, "r") as f:
    symbols = json.load(f)

st = currentProgram.getSymbolTable()
fm = currentProgram.getFunctionManager()
af = currentProgram.getAddressFactory()

count = 0
for s in symbols:
    addr = af.getAddress(s["xbe_va_hex"])
    name = s["name"]
    st.createLabel(addr, name, SourceType.IMPORTED)
    if s["is_func"] and fm.getFunctionAt(addr) is None:
        createFunction(addr, name)
    if s["demangled"] != name:
        setPlateComment(addr, f"{s['demangled']}\\nModule: {s['library']}:{s['object']} (size: {s['size']} bytes)")
    count += 1

print(f"Imported {count} symbols into Ghidra.")
'''
    with open(os.path.join(OUT_DIR, "import_symbols_ghidra.py"), "w", encoding="utf-8") as f:
        f.write(ghidra_script)

    ida_script = '''# IDAPython script to import all 75,668 Buffy the Vampire Slayer (Xbox) symbols from symbols.json
import json
import os
import idc
import ida_funcs
import ida_name

sym_path = os.path.join(os.path.dirname(__file__), "symbols.json")
with open(sym_path, "r") as f:
    symbols = json.load(f)

for s in symbols:
    ea = s["xbe_va"]
    ida_name.set_name(ea, s["name"], ida_name.SN_NOWARN | ida_name.SN_NOCHECK)
    if s["is_func"]:
        ida_funcs.add_func(ea)
    if s["demangled"] != s["name"]:
        idc.set_cmt(ea, f"{s['demangled']} [{s['library']}:{s['object']}]", 1)

print(f"Imported {len(symbols)} symbols into IDA.")
'''
    with open(os.path.join(OUT_DIR, "import_symbols_ida.py"), "w", encoding="utf-8") as f:
        f.write(ida_script)

    print(f"Processed {len(sorted_vas):,} unique virtual addresses ({len(funcs):,} unique functions).")
    print(f"Resolved {len(kernel_imports)} Xbox Kernel import thunks.")
    print(f"Cataloged {len(mod_list)} object modules.")

if __name__ == "__main__":
    main()
