#!/usr/bin/env python3
"""
Package com.oceanhorn2.port APK with:
- aapt2 base entries (AndroidManifest.xml preserved, resources.arsc ZIP_STORED 4-byte aligned)
- 16 KiB-aligned ZIP_STORED native .so libraries
- 16 KiB-aligned ZIP_STORED assets/oh2_macho.bin (Payload/OH2.app/OH2)
Uses Android's exact 0xD935 extra field layout: [0xD935 u16][len u16][align u16][padding zeros],
which both zipalign and apksigner understand and preserve across v1/v2/v3 signing.
"""
import argparse
import os
import struct
import sys
import zipfile
import zlib


def make_android_align_extra(lfh_offset: int, name_len: int, align: int) -> bytes:
    if align <= 1:
        return b""
    base = lfh_offset + 30 + name_len
    zeros_len = (align - ((base + 6) % align)) % align
    return struct.pack("<HHH", 0xD935, 2 + zeros_len, align) + (b"\x00" * zeros_len)


def write_stored_apk(out_path: str, base_apk: str, entries_16k: list[tuple[str, str]]) -> None:
    buf_items = []  # (name_bytes, compressed_bytes, uncomp_size, method, crc, mod_time, mod_date, align)

    with zipfile.ZipFile(base_apk, "r") as zbase:
        for zi in zbase.infolist():
            name_b = zi.filename.encode("utf-8")
            raw = zbase.read(zi.filename)
            crc = zlib.crc32(raw) & 0xFFFFFFFF
            dos_time = (zi.date_time[3] << 11) | (zi.date_time[4] << 5) | (zi.date_time[5] // 2)
            dos_date = ((zi.date_time[0] - 1980) << 9) | (zi.date_time[1] << 5) | zi.date_time[2]
            if zi.filename == "resources.arsc" or zi.filename.endswith(".png"):
                buf_items.append((name_b, raw, len(raw), 0, crc, dos_time, dos_date, 4))
            else:
                # Preserve deflate for AndroidManifest.xml (matches aapt2/M3 layout)
                comp = zlib.compressobj(zlib.Z_DEFAULT_COMPRESSION, zlib.DEFLATED, -15)
                cdata = comp.compress(raw) + comp.flush()
                buf_items.append((name_b, cdata, len(raw), 8, crc, dos_time, dos_date, 0))

    for arc_name, disk_path in entries_16k:
        if not os.path.isfile(disk_path):
            raise FileNotFoundError(f"Missing APK payload file: {disk_path}")
        with open(disk_path, "rb") as f:
            data = f.read()
        name_b = arc_name.encode("utf-8")
        crc = zlib.crc32(data) & 0xFFFFFFFF
        buf_items.append((name_b, data, len(data), 0, crc, 0, 0x5421, 16384))

    cd_records = []
    offset = 0
    with open(out_path, "wb") as out:
        for name_b, cdata, uncomp_sz, method, crc, dos_time, dos_date, align in buf_items:
            extra = make_android_align_extra(offset, len(name_b), align)
            data_off = offset + 30 + len(name_b) + len(extra)
            if align > 1 and (data_off % align) != 0:
                raise RuntimeError(f"Alignment bug on {name_b!r}: {data_off} % {align} != 0")

            comp_sz = len(cdata)
            lfh = struct.pack(
                "<IHHHHHIIIHH",
                0x04034B50,
                20,
                0,
                method,
                dos_time,
                dos_date,
                crc,
                comp_sz,
                uncomp_sz,
                len(name_b),
                len(extra),
            )
            out.write(lfh)
            out.write(name_b)
            out.write(extra)
            out.write(cdata)

            cdh = struct.pack(
                "<IHHHHHHIIIHHHHHII",
                0x02014B50,
                0x0314,
                20,
                0,
                method,
                dos_time,
                dos_date,
                crc,
                comp_sz,
                uncomp_sz,
                len(name_b),
                0,
                0,
                0,
                0,
                0,
                offset,
            )
            cd_records.append(cdh + name_b)
            offset = data_off + comp_sz

        cd_offset = offset
        cd_blob = b"".join(cd_records)
        out.write(cd_blob)
        eocd = struct.pack(
            "<IHHHHIIH",
            0x06054B50,
            0,
            0,
            len(cd_records),
            len(cd_records),
            len(cd_blob),
            cd_offset,
            0,
        )
        out.write(eocd)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-apk", required=True)
    ap.add_argument("--out-apk", required=True)
    ap.add_argument("--app-so", required=True)
    ap.add_argument("--val-so", required=True)
    ap.add_argument("--macho", required=True)
    args = ap.parse_args()

    write_stored_apk(
        args.out_apk,
        args.base_apk,
        [
            ("lib/arm64-v8a/liboh2_android.so", args.app_so),
            ("lib/arm64-v8a/libVkLayer_khronos_validation.so", args.val_so),
            ("assets/oh2_macho.bin", args.macho),
        ],
    )
    print(f"[pack_apk] Wrote 16KiB-aligned APK: {args.out_apk} ({os.path.getsize(args.out_apk)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
