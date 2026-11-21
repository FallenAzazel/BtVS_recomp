import json
import os
import re
import subprocess
import sys

DUMPBIN_DEFAULT = r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x86\dumpbin.exe"
PE_PATH = "decompile/XGameBuffy_xbox.exe"
SYMBOLS_PATH = "decompile/symbols.json"
OUT_DISASM_DIR = "decompile/disasm"

def load_symbol_maps():
    with open(SYMBOLS_PATH, "r", encoding="utf-8") as f:
        symbols = json.load(f)
    by_va = {s["xbe_va"]: s for s in symbols}
    by_obj = {}
    for s in symbols:
        if s["is_func"]:
            by_obj.setdefault(s["object"].lower(), []).append(s)
    return by_va, by_obj

def disassemble_range(start_va, end_va, by_va):
    cmd = [
        DUMPBIN_DEFAULT,
        "/DISASM:BYTES",
        f"/RANGE:0x{start_va:X},0x{end_va:X}",
        PE_PATH,
    ]
    raw = subprocess.check_output(cmd, stderr=subprocess.STDOUT).decode("latin1", errors="replace")
    addr_re = re.compile(r"^  ([0-9A-F]{8}):")
    target_re = re.compile(r"\b([0-9A-F]{8})h?\b")

    out_lines = []
    for line in raw.splitlines():
        m = addr_re.match(line)
        if m:
            va = int(m.group(1), 16)
            if va in by_va:
                s = by_va[va]
                out_lines.append("")
                out_lines.append(f"; ===========================================================================")
                out_lines.append(f"; {s['demangled']}")
                out_lines.append(f"; Mangled: {s['name']}")
                out_lines.append(f"; Module:  {s['library']}:{s['object']} | XBE VA: {s['xbe_va_hex']} | Orig PE VA: {s['pe_va_hex']} | Size: {s['size']} bytes")
                out_lines.append(f"; ===========================================================================")
            # Annotate referenced symbol VAs in instruction operands
            inst_part = line[12:]
            comments = []
            for tm in target_re.finditer(inst_part):
                tva = int(tm.group(1), 16)
                if tva in by_va and tva != va:
                    ts = by_va[tva]
                    comments.append(f"{ts['demangled']} ({ts['xbe_va_hex']})")
            if comments:
                line = f"{line:<72} ; -> {' | '.join(comments)}"
        out_lines.append(line)
    return "\n".join(out_lines)

def disassemble_object(obj_name, by_va, by_obj):
    key = obj_name.lower()
    if not key.endswith(".obj"):
        key += ".obj"
    funcs = by_obj.get(key, [])
    if not funcs:
        print(f"No functions found for module '{obj_name}'")
        return
    funcs.sort(key=lambda x: x["xbe_va"])
    os.makedirs(OUT_DISASM_DIR, exist_ok=True)
    out_path = os.path.join(OUT_DISASM_DIR, f"{key[:-4]}.asm")

    # Merge contiguous/nearby function ranges in the same section to invoke dumpbin efficiently
    ranges = []
    for fn in funcs:
        start = fn["xbe_va"]
        end = start + max(1, min(fn["size"], 0x10000)) - 1
        if ranges and start - ranges[-1][1] <= 0x40:
            ranges[-1] = (ranges[-1][0], max(ranges[-1][1], end))
        else:
            ranges.append((start, end))

    chunks = [
        f"; Annotated x86 Disassembly for Module: {obj_name}",
        f"; Functions: {len(funcs)} | Ranges: {len(ranges)}",
    ]
    for r_start, r_end in ranges:
        chunks.append(disassemble_range(r_start, r_end, by_va))

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(chunks) + "\n")
    print(f"Wrote annotated disassembly for {obj_name} ({len(funcs)} functions) -> {out_path}")

def main():
    by_va, by_obj = load_symbol_maps()
    targets = sys.argv[1:] if len(sys.argv) > 1 else [
        "xmain.obj",
        "xinstall.obj",
        "GameAPIMain.obj",
        "i_xinput.obj",
        "winsound.obj",
        "RenderLink.obj",
    ]
    for t in targets:
        disassemble_object(t, by_va, by_obj)

if __name__ == "__main__":
    main()
