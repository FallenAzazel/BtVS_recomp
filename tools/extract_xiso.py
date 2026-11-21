import os
import struct
import sys

SECTOR_SIZE = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"

def find_xdvdfs_offset(f):
    # Standard XISO offset is sector 32 (0x10000)
    # Redump images can have offsets like 0x18300000 + 0x10000 or 0xFD90000 + 0x10000
    candidates = [0, 0x18300000, 0xFD90000, 0x2080000]
    for base in candidates:
        f.seek(base + 32 * SECTOR_SIZE)
        data = f.read(SECTOR_SIZE)
        if len(data) == SECTOR_SIZE and data[:20] == MAGIC and data[2028:2048] == MAGIC:
            return base
    raise ValueError("Could not find MICROSOFT*XBOX*MEDIA volume descriptor")

def parse_dir_tree(f, base_offset, dir_sector, dir_size, out_dir, rel_path="", list_only=False):
    if dir_size == 0:
        return []
    f.seek(base_offset + dir_sector * SECTOR_SIZE)
    dir_data = f.read(dir_size)
    
    files_extracted = []
    # Traversal using a stack of byte offsets within dir_data
    stack = [0]
    visited = set()
    
    while stack:
        offset = stack.pop()
        if offset in visited or offset >= len(dir_data) or offset == 0xFFFF * 4:
            continue
        visited.add(offset)
        
        if offset + 14 > len(dir_data):
            continue
            
        left_subtree, right_subtree, start_sector, file_size, attributes, name_len = struct.unpack_from("<HHIIBB", dir_data, offset)
        if left_subtree == 0xFFFF and right_subtree == 0xFFFF and start_sector == 0xFFFFFFFF:
            continue
            
        name_bytes = dir_data[offset + 14 : offset + 14 + name_len]
        name = name_bytes.decode("ascii", errors="replace")
        
        # Push right and left children (offsets are in 32-bit words, so multiply by 4)
        if right_subtree != 0 and right_subtree != 0xFFFF:
            stack.append(right_subtree * 4)
        if left_subtree != 0 and left_subtree != 0xFFFF:
            stack.append(left_subtree * 4)
            
        current_rel = os.path.join(rel_path, name) if rel_path else name
        target_path = os.path.join(out_dir, current_rel)
        
        is_dir = (attributes & 0x10) != 0
        if is_dir:
            if not list_only:
                os.makedirs(target_path, exist_ok=True)
            files_extracted.extend(
                parse_dir_tree(f, base_offset, start_sector, file_size, out_dir, current_rel, list_only=list_only)
            )
        else:
            if not list_only:
                os.makedirs(os.path.dirname(target_path), exist_ok=True)
                f.seek(base_offset + start_sector * SECTOR_SIZE)
                remaining = file_size
                with open(target_path, "wb") as out_f:
                    while remaining > 0:
                        chunk = f.read(min(remaining, 1024 * 1024))
                        if not chunk:
                            break
                        out_f.write(chunk)
                        remaining -= len(chunk)
            files_extracted.append((current_rel, file_size, start_sector))
            
    return files_extracted

def main():
    iso_path = sys.argv[1]
    out_dir = sys.argv[2]
    list_only = "--list" in sys.argv
    
    with open(iso_path, "rb") as f:
        base_offset = find_xdvdfs_offset(f)
        f.seek(base_offset + 32 * SECTOR_SIZE)
        header = f.read(SECTOR_SIZE)
        root_sector, root_size = struct.unpack_from("<II", header, 20)
        print(f"Found XDVDFS at base offset 0x{base_offset:X}")
        print(f"Root directory: sector={root_sector}, size={root_size} bytes")
        
        if not list_only:
            os.makedirs(out_dir, exist_ok=True)
        files = parse_dir_tree(f, base_offset, root_sector, root_size, out_dir, list_only=list_only)
        total_bytes = sum(size for _, size, _ in files)
        action = "Listed" if list_only else "Successfully extracted"
        print(f"{action} {len(files)} files ({total_bytes:,} bytes) to '{out_dir}'.")
        for rel, size, sec in sorted(files)[:100]:
            print(f"  {rel} ({size:,} bytes, sector {sec})")
        if len(files) > 100:
            print(f"  ... and {len(files) - 100} more files.")

if __name__ == "__main__":
    main()
