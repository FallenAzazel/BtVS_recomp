import os
import struct
import sys

EP_XOR_RETAIL = 0xA8FC57AB
EP_XOR_DEBUG  = 0x94859D4B
KT_XOR_RETAIL = 0x5B6D40B6
KT_XOR_DEBUG  = 0xEFB1F152

IMAGE_SCN_CNT_CODE               = 0x00000020
IMAGE_SCN_CNT_INITIALIZED_DATA   = 0x00000040
IMAGE_SCN_CNT_UNINITIALIZED_DATA = 0x00000080
IMAGE_SCN_MEM_EXECUTE            = 0x20000000
IMAGE_SCN_MEM_READ               = 0x40000000
IMAGE_SCN_MEM_WRITE              = 0x80000000

def align_up(val, align):
    return (val + align - 1) & ~(align - 1)

def convert_xbe_to_pe(xbe_path, pe_path):
    with open(xbe_path, "rb") as f:
        data = f.read()

    if data[:4] != b"XBEH":
        raise ValueError("Not a valid XBE file (missing XBEH magic)")

    (
        base_addr, size_headers, size_image, size_image_header, timedate,
        cert_addr, num_sections, sec_hdrs_addr, init_flags, ep_enc,
        tls_addr, pe_stack, pe_heap_res, pe_heap_comm, pe_base,
        pe_size, pe_csum, pe_timedate, debug_path_addr, debug_file_addr,
        debug_ufile_addr, kt_enc, nonkernel_import_dir, num_lib_versions,
        lib_versions_addr, kernel_lib_ver_addr, xapi_lib_ver_addr,
        logo_addr, logo_size
    ) = struct.unpack_from("<29I", data, 0x104)

    ep_retail = ep_enc ^ EP_XOR_RETAIL
    if base_addr <= ep_retail < base_addr + size_image:
        ep_va = ep_retail
        kt_va = kt_enc ^ KT_XOR_RETAIL
    else:
        ep_va = ep_enc ^ EP_XOR_DEBUG
        kt_va = kt_enc ^ KT_XOR_DEBUG

    def read_cstr(va):
        if va < base_addr or va >= base_addr + len(data):
            return ""
        off = va - base_addr
        end = data.find(b"\x00", off)
        return data[off:end].decode("ascii", errors="replace")

    sec_off = sec_hdrs_addr - base_addr
    sections = []
    for i in range(num_sections):
        s_flags, v_addr, v_size, r_addr, r_size, s_name_addr, s_ref, head_ref, tail_ref = struct.unpack_from(
            "<9I", data, sec_off + i * 56
        )
        s_name = read_cstr(s_name_addr)
        sections.append({
            "name": s_name,
            "va": v_addr,
            "rva": v_addr - base_addr,
            "vsize": v_size,
            "raw_off": r_addr,
            "raw_size": r_size,
            "xbe_flags": s_flags,
        })

    # Build DOS Header + Stub (0x80 bytes)
    dos_header = bytearray(0x80)
    dos_header[0:2] = b"MZ"
    struct.pack_into("<I", dos_header, 0x3C, 0x80)  # e_lfanew = 0x80

    # PE Signature + IMAGE_FILE_HEADER (24 bytes)
    machine = 0x014C  # IMAGE_FILE_MACHINE_I386
    characteristics = 0x010F  # REL_STRIPPED | EXECUTABLE_IMAGE | LINE_NUMS_STRIPPED | LOCAL_SYMS_STRIPPED | 32BIT_MACHINE
    pe_header = struct.pack(
        "<4sHHIIIHH",
        b"PE\x00\x00",
        machine,
        num_sections,
        pe_timedate or timedate,
        0,
        0,
        224,  # SizeOfOptionalHeader (IMAGE_OPTIONAL_HEADER32)
        characteristics,
    )

    # Calculate code/data sizes
    size_of_code = sections[0]["vsize"] if sections else 0
    base_of_code = sections[0]["rva"] if sections else 0x1000
    base_of_data = sections[9]["rva"] if len(sections) > 9 else 0x1000
    max_end_rva = max(s["rva"] + s["vsize"] for s in sections)
    pe_size_of_image = align_up(max_end_rva, 0x1000)

    # IMAGE_OPTIONAL_HEADER32 (224 bytes)
    opt_header = bytearray(224)
    struct.pack_into(
        "<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII",
        opt_header,
        0,
        0x010B,                 # Magic (PE32)
        7, 0,                   # Linker version 7.0 (MSVC 2002 / XDK 4432)
        size_of_code,
        0,
        0,
        ep_va - base_addr,      # AddressOfEntryPoint (RVA)
        base_of_code,
        base_of_data,
        base_addr,              # ImageBase (0x00010000)
        0x20,                   # SectionAlignment (matches XBE 32-byte section alignment)
        0x20,                   # FileAlignment
        4, 0,                   # OS Version 4.0
        1, 0,                   # Image Version 1.0
        4, 0,                   # Subsystem Version 4.0
        0,                      # Win32VersionValue
        pe_size_of_image,       # SizeOfImage
        0x1000,                 # SizeOfHeaders
        pe_csum,                # CheckSum
        2,                      # Subsystem (IMAGE_SUBSYSTEM_WINDOWS_GUI)
        0,                      # DllCharacteristics
        pe_stack or 0x10000,    # SizeOfStackReserve
        0x10000,                # SizeOfStackCommit
        pe_heap_res or 0x100000,# SizeOfHeapReserve
        pe_heap_comm or 0x1000, # SizeOfHeapCommit
        0,                      # LoaderFlags
        16,                     # NumberOfRvaAndSizes
    )

    # Build Section Headers (40 bytes each) targeting original XBE raw offsets
    sec_headers = bytearray()
    for s in sections:
        name_bytes = s["name"].encode("ascii", errors="replace")[:8].ljust(8, b"\x00")
        xf = s["xbe_flags"]
        pe_flags = IMAGE_SCN_MEM_READ
        if xf & 0x01:  # Writable
            pe_flags |= IMAGE_SCN_MEM_WRITE
        if xf & 0x04:  # Executable
            pe_flags |= IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE
        else:
            pe_flags |= IMAGE_SCN_CNT_INITIALIZED_DATA

        sec_hdr = struct.pack(
            "<8sIIIIIIHHI",
            name_bytes,
            s["vsize"],
            s["rva"],
            s["raw_size"],
            s["raw_off"],
            0, 0, 0, 0,
            pe_flags,
        )
        sec_headers.extend(sec_hdr)

    full_headers = dos_header + pe_header + opt_header + sec_headers
    assert len(full_headers) <= 0x1000, f"Headers too large: {len(full_headers)}"
    full_headers = full_headers.ljust(0x1000, b"\x00")

    os.makedirs(os.path.dirname(pe_path), exist_ok=True)
    with open(pe_path, "wb") as out_f:
        out_f.write(full_headers)
        out_f.write(data[0x1000:])

    print(f"Created PE32 executable: {pe_path}")
    print(f"  ImageBase:          0x{base_addr:08X}")
    print(f"  EntryPoint VA:      0x{ep_va:08X} (RVA 0x{ep_va - base_addr:08X})")
    print(f"  KernelThunk VA:     0x{kt_va:08X} (RVA 0x{kt_va - base_addr:08X})")
    print(f"  Sections:           {len(sections)}")

if __name__ == "__main__":
    xbe_in = sys.argv[1] if len(sys.argv) > 1 else "extracted/Default.xbe"
    pe_out = sys.argv[2] if len(sys.argv) > 2 else "decompile/XGameBuffy_xbox.exe"
    convert_xbe_to_pe(xbe_in, pe_out)
