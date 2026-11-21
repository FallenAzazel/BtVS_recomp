import json

with open("decompile/symbols.json", "r", encoding="utf-8") as f:
    syms = json.load(f)

print("=== LIBCMT & XAPILIB FUNCTIONS ===")
for s in syms:
    if s["library"] in ("LIBCMT", "xapilib") and s["is_func"]:
        low = s["name"].lower()
        if any(k in low for k in [
            "ptd", "tls", "malloc", "free", "realloc", "calloc", "new", "delete",
            "init", "heap", "createfile", "readfile", "writefile", "closehandle",
            "memorystatus", "lasterror", "launch", "device", "thread", "critical",
            "sleep", "tick", "fopen", "fclose", "fread", "fwrite", "fseek", "ftell",
            "fprintf", "findfirst", "findnext", "findclose", "diskfreespace", "mu"
        ]):
            print(f"{s['xbe_va_hex']} {s['library']:<8} {s['object']:<18} {s['name']}")
