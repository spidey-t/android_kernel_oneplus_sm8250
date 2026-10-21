#!/usr/bin/env python3
import argparse
import struct
import sys

DT_MAGIC = 0xD7B7AB1E
FDT_MAGIC = b"\xd0\x0d\xfe\xed"
HEADER_SIZE = 32
ENTRY_SIZE = 32
PAGE_SIZE = 4096
VERSION = 0


def pack(paths, pad_to):
    payloads = []
    for path in paths:
        with open(path, "rb") as handle:
            data = handle.read()
        if data[:4] != FDT_MAGIC:
            sys.exit("pack_dtbo: %s is not a devicetree blob" % path)
        payloads.append(data)
    if not payloads:
        sys.exit("pack_dtbo: no .dtbo inputs given")

    entries_offset = HEADER_SIZE
    payloads_offset = HEADER_SIZE + ENTRY_SIZE * len(payloads)
    total_size = payloads_offset + sum(len(p) for p in payloads)

    out = bytearray(struct.pack(
        ">8I", DT_MAGIC, total_size, HEADER_SIZE, ENTRY_SIZE,
        len(payloads), entries_offset, PAGE_SIZE, VERSION))
    offset = payloads_offset
    for payload in payloads:
        out += struct.pack(">8I", len(payload), offset, 0, 0, 0, 0, 0, 0)
        offset += len(payload)
    for payload in payloads:
        out += payload

    if pad_to:
        if pad_to < len(out):
            sys.exit("pack_dtbo: image (%d bytes) exceeds pad size (%d bytes)"
                     % (len(out), pad_to))
        out += b"\x00" * (pad_to - len(out))
    return bytes(out)


def main():
    parser = argparse.ArgumentParser(
        description="pack .dtbo blobs into a flashable dtbo.img (dt_table)")
    parser.add_argument("dtbos", nargs="+", metavar="FILE.dtbo")
    parser.add_argument("-o", "--output", required=True)
    parser.add_argument("--pad-to", type=int, default=0,
                        metavar="BYTES", help="zero-pad image to this size "
                        "(dtbo partition size)")
    args = parser.parse_args()

    data = pack(args.dtbos, args.pad_to)
    with open(args.output, "wb") as handle:
        handle.write(data)
    print("pack_dtbo: wrote %s (%d bytes, %d entries)"
          % (args.output, len(data), len(args.dtbos)))


if __name__ == "__main__":
    main()
