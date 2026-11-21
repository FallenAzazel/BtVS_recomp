import os
import struct
import sys

def create_lowbase_launcher_exe(out_path):
    image_base = 0x00010000
    sec_align  = 0x00001000
    file_align = 0x00000200

    # Section 0: .xbox -> RVA 0x00001000 .. 0x00921000 (VA 0x00011000 .. 0x00931000)
    xbox_rva   = 0x00001000
    xbox_vsize = 0x00920000

    # Section 1: .host -> RVA 0x00921000 .. 0x00922000 (VA 0x00931000 .. 0x00932000)
    host_rva      = 0x00921000
    host_vsize    = 0x00001000
    host_raw_off  = 0x00001000
    host_raw_size = 0x00000200

    size_of_image   = 0x00922000
    size_of_headers = 0x00001000
    pe_offset       = 0x00000C00

    # 4KB Header Page (0x0000..0x0FFF)
    # 0x0000..0x003F: DOS Header (MZ, e_lfanew = 0x0C00)
    # 0x0104..0x08FF: Reserved for XBE Header, Certificate, and Section Table at runtime
    # 0x0C00..0x0D60: PE Header + OptionalHeader32 + 2 Section Headers
    hdr_page = bytearray(size_of_headers)
    hdr_page[0:2] = b"MZ"
    struct.pack_into("<I", hdr_page, 0x3C, pe_offset)

    # PE File Header (24 bytes)
    pe_hdr = struct.pack(
        "<4sHHIIIHH",
        b"PE\x00\x00",
        0x014C,  # IMAGE_FILE_MACHINE_I386
        2,       # NumberOfSections
        0x3D35C92D,
        0, 0,
        224,     # SizeOfOptionalHeader
        0x010F,
    )

    # Optional Header32 (224 bytes)
    opt_hdr = bytearray(224)
    struct.pack_into(
        "<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII",
        opt_hdr,
        0,
        0x010B,         # PE32
        14, 0,          # Linker version
        host_vsize,     # SizeOfCode
        host_vsize,     # SizeOfInitializedData
        xbox_vsize,     # SizeOfUninitializedData
        host_rva,       # AddressOfEntryPoint (RVA 0x00921000 -> VA 0x00931000)
        host_rva,       # BaseOfCode
        host_rva,       # BaseOfData
        image_base,     # ImageBase = 0x00010000
        sec_align,      # SectionAlignment = 0x1000
        file_align,     # FileAlignment = 0x200
        4, 0,           # OS Version 4.0
        1, 0,           # Image Version
        4, 0,           # Subsystem Version 4.0
        0,              # Win32VersionValue
        size_of_image,  # SizeOfImage
        size_of_headers,# SizeOfHeaders = 0x1000
        0,              # CheckSum
        3,              # Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI (3)
        0,              # DllCharacteristics (0 = fixed base 0x00010000)
        0x00800000,     # SizeOfStackReserve (8 MB)
        0x00080000,     # SizeOfStackCommit (512 KB)
        0x04000000,     # SizeOfHeapReserve (64 MB)
        0x00100000,     # SizeOfHeapCommit (1 MB)
        0,              # LoaderFlags
        16,             # NumberOfRvaAndSizes
    )

    # Data Directory[1]: IMAGE_DIRECTORY_ENTRY_IMPORT
    import_dir_rva  = host_rva + 0x20
    import_dir_size = 40
    struct.pack_into("<II", opt_hdr, 104, import_dir_rva, import_dir_size)

    # Data Directory[12]: IMAGE_DIRECTORY_ENTRY_IAT
    iat_rva  = host_rva + 0x50
    iat_size = 8
    struct.pack_into("<II", opt_hdr, 192, iat_rva, iat_size)

    # Section Headers (2 * 40 = 80 bytes)
    sec_xbox = struct.pack(
        "<8sIIIIIIHHI",
        b".xbox\x00\x00\x00",
        xbox_vsize,
        xbox_rva,
        0,
        0,
        0, 0, 0, 0,
        0xE0000080,
    )

    sec_host = struct.pack(
        "<8sIIIIIIHHI",
        b".host\x00\x00\x00",
        host_vsize,
        host_rva,
        host_raw_size,
        host_raw_off,
        0, 0, 0, 0,
        0xE0000060,
    )

    pe_block = pe_hdr + opt_hdr + sec_xbox + sec_host
    hdr_page[pe_offset : pe_offset + len(pe_block)] = pe_block

    # Build .host section payload (0x200 bytes)
    host_data = bytearray(host_raw_size)

    # +0x00: EntryPoint machine code:
    #   FF 15 <VA of IAT>  -> call dword ptr [0x00931050] (BuffyRuntimeMain)
    #   C3                 -> ret
    iat_va = image_base + iat_rva
    host_data[0:2] = b"\xFF\x15"
    struct.pack_into("<I", host_data, 2, iat_va)
    host_data[6] = 0xC3

    # +0x20: IMAGE_IMPORT_DESCRIPTOR for BuffyRecompRuntime.dll
    ilt_rva       = host_rva + 0x48
    dll_name_rva  = host_rva + 0x60
    func_name_rva = host_rva + 0x78
    struct.pack_into("<IIIII", host_data, 0x20, ilt_rva, 0, 0, dll_name_rva, iat_rva)

    # +0x48: ILT (8 bytes) & +0x50: IAT (8 bytes)
    struct.pack_into("<II", host_data, 0x48, func_name_rva, 0)
    struct.pack_into("<II", host_data, 0x50, func_name_rva, 0)

    # +0x60: DLL name
    dll_name = b"BuffyRecompRuntime.dll\x00"
    host_data[0x60 : 0x60 + len(dll_name)] = dll_name

    # +0x78: IMAGE_IMPORT_BY_NAME (Hint=0, Name="BuffyRuntimeMain\0")
    fn_entry = b"\x00\x00BuffyRuntimeMain\x00"
    host_data[0x78 : 0x78 + len(fn_entry)] = fn_entry

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(hdr_page)
        f.write(host_data)

    print(f"Created low-base PE32 launcher: {out_path}")
    print(f"  ImageBase:       0x{image_base:08X}")
    print(f"  .xbox Reserved:  0x{image_base + xbox_rva:08X} .. 0x{image_base + xbox_rva + xbox_vsize:08X} ({xbox_vsize:,} bytes)")
    print(f"  .host Entry VA:  0x{image_base + host_rva:08X} -> imports BuffyRecompRuntime.dll!BuffyRuntimeMain")

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "recomp/build/BuffyWin32Recomp.exe"
    create_lowbase_launcher_exe(out)
