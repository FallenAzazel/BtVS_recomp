import struct
import re
from collections import defaultdict

XBE_PATH = "extracted/Default.xbe"
MAP_PATH = "extracted/final.map"

# Retail & Debug XOR keys for XBE entry point and kernel thunk
EP_XOR_RETAIL = 0xA8FC57AB
EP_XOR_DEBUG  = 0x94859D4B
KT_XOR_RETAIL = 0x5B6D40B6
KT_XOR_DEBUG  = 0xEFB1F152

XBOX_KERNEL_ORDINALS = {
    1: "AvGetSavedDataAddress", 2: "AvSendTVEncoderOption", 3: "AvSetDisplayMode", 4: "AvSetSavedDataAddress",
    14: "ExAllocatePool", 15: "ExAllocatePoolWithTag", 16: "ExEventObjectType", 17: "ExFreePool",
    24: "ExQueryNonVolatileSetting", 37: "HalReadSMCTrayState", 40: "HalReturnToFirmware",
    47: "HalWriteSMBusValue", 49: "InterlockedCompareExchange", 50: "InterlockedDecrement",
    51: "InterlockedIncrement", 52: "InterlockedExchange", 53: "InterlockedExchangeAdd",
    61: "IoCreateFile", 62: "IoCreateSymbolicLink", 64: "IoDeleteSymbolicLink",
    67: "IoDismountVolumeByName", 83: "KeClearEvent", 95: "KeDelayExecutionThread",
    99: "KeInitializeDpc", 100: "KeInitializeEvent", 107: "KeInitializeTimerEx",
    119: "KeQueryPerformanceCounter", 120: "KeQueryPerformanceFrequency", 121: "KeQuerySystemTime",
    125: "KeRaiseIrqlToDpcLevel", 128: "KeResetEvent", 145: "KeSetEvent", 149: "KeSetTimerEx",
    153: "KeSynchronizeExecution", 158: "KeTickCount", 159: "KeWaitForMultipleObjects",
    160: "KeWaitForSingleObject", 161: "KfRaiseIrql", 162: "KfLowerIrql",
    164: "LaunchDataPage", 165: "MmAllocateContiguousMemory", 166: "MmAllocateContiguousMemoryEx",
    167: "MmAllocateSystemMemory", 168: "MmClaimGpuInstanceMemory", 171: "MmFreeContiguousMemory",
    172: "MmFreeSystemMemory", 173: "MmGetPhysicalAddress", 175: "MmLockUnlockBufferPages",
    176: "MmLockUnlockPhysicalPage", 178: "MmPersistContiguousMemory", 179: "MmQueryAddressProtect",
    180: "MmQueryAllocationSize", 181: "MmQueryStatistics", 182: "MmSetAddressProtect",
    184: "NtAllocateVirtualMemory", 187: "NtClose", 189: "NtCreateEvent", 190: "NtCreateFile",
    192: "NtCreateMutant", 193: "NtCreateSemaphore", 195: "NtDeleteFile",
    196: "NtDeviceIoControlFile", 197: "NtDuplicateObject", 198: "NtFlushBuffersFile",
    199: "NtFreeVirtualMemory", 202: "NtOpenDirectoryObject", 203: "NtOpenFile",
    207: "NtQueryDirectoryFile", 210: "NtQueryFullAttributesFile", 211: "NtQueryInformationFile",
    215: "NtQueryVirtualMemory", 216: "NtQueryVolumeInformationFile", 218: "NtReadFile",
    221: "NtReleaseMutant", 222: "NtReleaseSemaphore", 224: "NtResumeThread",
    225: "NtSetEvent", 226: "NtSetInformationFile", 228: "NtSetSystemTime",
    231: "NtSuspendThread", 233: "NtWaitForMultipleObjectsEx", 234: "NtWaitForSingleObject",
    235: "NtWaitForSingleObjectEx", 236: "NtWriteFile", 238: "NtYieldExecution",
    246: "ObReferenceObjectByHandle", 254: "PsCreateSystemThreadEx", 258: "PsTerminateSystemThread",
    259: "PsThreadObjectType", 260: "RtlAnsiStringToUnicodeString", 261: "RtlAppendStringToString",
    269: "RtlEnterCriticalSection", 277: "RtlInitializeCriticalSection", 279: "RtlLeaveCriticalSection",
    289: "RtlNtStatusToDosError", 291: "RtlRaiseException", 301: "RtlTimeFieldsToTime",
    302: "RtlTimeToTimeFields", 304: "RtlTryEnterCriticalSection", 308: "RtlUnicodeStringToAnsiString",
    312: "RtlUnwind", 322: "XboxHardwareInfo", 323: "XboxHDKey", 324: "XboxKrnlVersion",
    325: "XboxSignatureKey", 326: "XeImageFileName", 327: "XeLoadSection", 328: "XeUnloadSection",
    355: " HalBootSMCVideoMode", 356: "IdexChannelObject",
}

def inspect_xbe():
    with open(XBE_PATH, "rb") as f:
        data = f.read()

    magic = data[:4]
    assert magic == b"XBEH", f"Unexpected magic: {magic}"
    base_addr, size_headers, size_image, size_image_header, timedate, cert_addr, num_sections, sec_hdrs_addr, init_flags, ep_enc, tls_addr, pe_stack, pe_heap_res, pe_heap_comm, pe_base, pe_size, pe_csum, pe_timedate, debug_path_addr, debug_file_addr, debug_ufile_addr, kt_enc, nonkernel_import_dir, num_lib_versions, lib_versions_addr, kernel_lib_ver_addr, xapi_lib_ver_addr, logo_addr, logo_size = struct.unpack_from("<29I", data, 0x104)

    print("=== XBE HEADER ===")
    print(f"Base Address:       0x{base_addr:08X}")
    print(f"Size of Image:      0x{size_image:08X} ({size_image:,} bytes)")
    print(f"TimeDateStamp:      0x{timedate:08X}")
    print(f"PE Orig Base:       0x{pe_base:08X}, PE Size: 0x{pe_size:08X}, PE TimeDate: 0x{pe_timedate:08X}")

    def read_cstr(va):
        if va < base_addr or va >= base_addr + len(data):
            return ""
        off = va - base_addr
        end = data.find(b"\x00", off)
        return data[off:end].decode("ascii", errors="replace")

    print(f"Debug Pathname:     {read_cstr(debug_path_addr)}")
    print(f"Debug Filename:     {read_cstr(debug_file_addr)}")

    ep_retail = ep_enc ^ EP_XOR_RETAIL
    ep_debug = ep_enc ^ EP_XOR_DEBUG
    if base_addr <= ep_retail < base_addr + size_image:
        ep = ep_retail
        kt = kt_enc ^ KT_XOR_RETAIL
        mode = "RETAIL"
    else:
        ep = ep_debug
        kt = kt_enc ^ KT_XOR_DEBUG
        mode = "DEBUG"
    print(f"XBE Build Mode:     {mode}")
    print(f"Entry Point VA:     0x{ep:08X}")
    print(f"Kernel Thunk VA:    0x{kt:08X}")

    # Certificate
    cert_off = cert_addr - base_addr
    cert_size, cert_td, title_id = struct.unpack_from("<III", data, cert_off)
    title_name = data[cert_off+12:cert_off+12+80].decode("utf-16le", errors="replace").rstrip("\x00")
    print(f"Title ID:           0x{title_id:08X} ({chr((title_id>>24)&0xFF)}{chr((title_id>>16)&0xFF)}-{(title_id&0xFFFF):03d})")
    print(f"Title Name:         {title_name}")

    # Library versions (XDK version!)
    print(f"\n=== XDK LIBRARY VERSIONS ({num_lib_versions}) ===")
    lib_off = lib_versions_addr - base_addr
    for i in range(num_lib_versions):
        name_b, v_maj, v_min, v_build, v_flags = struct.unpack_from("<8sHHHH", data, lib_off + i * 16)
        name_s = name_b.rstrip(b"\x00").decode("ascii", errors="replace")
        qfe = v_flags & 0x1FFF
        approved = (v_flags >> 13) & 3
        debug_lib = (v_flags >> 15) & 1
        print(f"  {name_s:<10} v{v_maj}.{v_min}.{v_build}.{qfe} (debug={debug_lib}, approved={approved})")

    # Sections
    print(f"\n=== XBE SECTIONS ({num_sections}) ===")
    sec_off = sec_hdrs_addr - base_addr
    sections = []
    for i in range(num_sections):
        s_flags, v_addr, v_size, r_addr, r_size, s_name_addr, s_ref, head_ref, tail_ref = struct.unpack_from("<9I", data, sec_off + i * 56)
        s_name = read_cstr(s_name_addr)
        sections.append((s_name, v_addr, v_size, r_addr, r_size, s_flags))
        print(f"  [{i:2d}] {s_name:<12} VA=0x{v_addr:08X} VSize=0x{v_size:08X} RawOff=0x{r_addr:08X} RawSize=0x{r_size:08X} Flags=0x{s_flags:02X}")

    # Resolve Kernel Thunk Table
    for s_name, v_addr, v_size, r_addr, r_size, _ in sections:
        if v_addr <= kt < v_addr + v_size:
            kt_raw = r_addr + (kt - v_addr)
            print(f"\n=== KERNEL IMPORTS (at RawOff 0x{kt_raw:X}, Section {s_name}) ===")
            idx = 0
            ordinals = []
            while True:
                val = struct.unpack_from("<I", data, kt_raw + idx * 4)[0]
                if val == 0:
                    break
                ord_num = val & 0x7FFFFFFF
                ordinals.append((kt + idx * 4, ord_num, XBOX_KERNEL_ORDINALS.get(ord_num, f" Unknown_{ord_num}")))
                idx += 1
            print(f"  Total Kernel Imports: {len(ordinals)}")
            for thunk_va, ord_num, kname in ordinals:
                print(f"    [0x{thunk_va:08X}] Ord {ord_num:3d}: {kname}")
            break

def inspect_map():
    print("\n=== FINAL.MAP ANALYSIS ===")
    sym_re = re.compile(r"^\s*([0-9a-fA-F]{4}):([0-9a-fA-F]{8})\s+(\S+)\s+([0-9a-fA-F]{8})\s+(f\s+i|f\s+|\s+i|\s+)\s*(\S.*)$")
    lib_counts = defaultdict(int)
    obj_counts = defaultdict(int)
    func_counts = defaultdict(int)
    total_syms = 0
    total_funcs = 0
    in_statics = False

    with open(MAP_PATH, "r", encoding="latin1") as f:
        for line in f:
            if "Static symbols" in line:
                in_statics = True
                continue
            m = sym_re.match(line)
            if m:
                sec, off, name, rva, flags, lib_obj = m.groups()
                total_syms += 1
                is_func = "f" in flags
                if is_func:
                    total_funcs += 1
                lib_obj = lib_obj.strip()
                if ":" in lib_obj:
                    lib, obj = lib_obj.split(":", 1)
                else:
                    lib, obj = "GAME_ENGINE", lib_obj
                lib_counts[lib] += 1
                if is_func:
                    func_counts[lib] += 1
                if lib == "GAME_ENGINE":
                    obj_counts[obj] += 1

    print(f"Total Symbols Parsed:   {total_syms:,}")
    print(f"Total Function Symbols: {total_funcs:,}")
    print(f"Has Static Symbols:     {in_statics}")
    print("\nSymbols by Library:")
    for lib, count in sorted(lib_counts.items(), key=lambda x: -x[1]):
        print(f"  {lib:<20} {count:7,d} symbols ({func_counts[lib]:6,d} functions)")
    print(f"\nDistinct Game/Engine Object Files (.obj): {len(obj_counts)}")
    print("Top 35 Game/Engine Object Files by symbol count:")
    for obj, count in sorted(obj_counts.items(), key=lambda x: -x[1])[:35]:
        print(f"  {obj:<35} {count:5,d} symbols")

if __name__ == "__main__":
    inspect_xbe()
    inspect_map()
