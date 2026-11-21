import json

with open("decompile/symbols.json", "r", encoding="utf-8") as f:
    syms = json.load(f)

print("=== D3D8 LIBRARY FUNCTIONS ===")
for s in syms:
    if s["library"] == "d3d8" and s["is_func"]:
        print(f"  {s['xbe_va_hex']} {s['object']:<16} {s['name']:<45} (size={s['size']})")

print("\n=== SLATE ENGINE D3D_*.OBJ FUNCTIONS ===")
for s in syms:
    if s["object"].lower().startswith("d3d_") and s["is_func"]:
        print(f"  {s['xbe_va_hex']} {s['object']:<20} {s['name']:<45} -> {s['demangled']}")
