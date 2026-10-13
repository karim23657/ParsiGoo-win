# Make the ONNX package loadable by Windows-7-compatible ONNX Runtime builds.
#
# The published graphs are stamped ONNX IR version 10. IR 10 support only
# arrived in ONNX Runtime 1.18, but Windows 7 support was dropped *after*
# ONNX Runtime 1.14 — so an IR-10 model cannot load on the last ORT that runs
# on Windows 7. The graphs use no IR-10-specific features, so lowering the
# stamp to IR 8 is safe; the opsets stay at 18 (ORT 1.14 supports opset 18).
#
#   python3 scripts/prepare_win7_onnx.py model/onnx
#
# The stamp is patched IN PLACE rather than through onnx.save(): re-serialising
# re-embeds or duplicates the external *.data blobs (the package doubles from
# ~470 MB to ~890 MB). Only the single ir_version varint byte changes.
import sys
from pathlib import Path

GRAPHS = ["g2p_encoder.onnx", "g2p_decoder.onnx", "flow_lm_step.onnx",
          "mimi_encoder.onnx", "mimi_decoder_step_kv.onnx"]

TARGET_IR = 8


def read_varint(buf, i):
    """Returns (value, next_index)."""
    result = shift = 0
    while True:
        b = buf[i]
        result |= (b & 0x7F) << shift
        i += 1
        if not (b & 0x80):
            return result, i
        shift += 7


def write_varint(buf, i, value):
    while True:
        if value < 0x80:
            buf[i] = value
            return i + 1
        buf[i] = (value & 0x7F) | 0x80
        value >>= 7
        i += 1


def patch(path: Path) -> str:
    """Lower the IR stamp of one .onnx file, in place.

    An .onnx file is a raw protobuf whose first field is ModelProto.ir_version
    (field 1, varint wire type), so the stamp is the varint that starts right
    after the single-byte tag 0x08 at offset 0.
    """
    buf = bytearray(path.read_bytes())
    if len(buf) < 3:
        raise SystemExit(f"{path}: too short to be an .onnx file")
    if buf[0] != 0x08:
        raise SystemExit(
            f"{path}: unexpected first byte 0x{buf[0]:02x}, expected 0x08 "
            "(ModelProto.ir_version tag) — not an ONNX graph")
    cur, end = read_varint(buf, 1)
    if cur == TARGET_IR:
        return f"already IR {TARGET_IR} (no change)"
    if cur > TARGET_IR:
        write_varint(buf, 1, TARGET_IR)
        path.write_bytes(bytes(buf))
        return f"IR {cur} -> {TARGET_IR}"
    raise SystemExit(
        f"{path}: IR {cur} is older than the target IR {TARGET_IR}; refusing "
        "to raise the stamp (that would need a real re-serialisation)")


def main():
    pkg = Path(sys.argv[1] if len(sys.argv) > 1 else "model/onnx")
    if not pkg.is_dir():
        raise SystemExit(f"not a directory: {pkg}")
    for name in GRAPHS:
        f = pkg / name
        if not f.exists():
            raise SystemExit(f"missing {f}")
        print(f"{name}: {patch(f)}")
    print("done — graphs are now ORT 1.14 (Windows 7) compatible, "
          "external weights untouched")


if __name__ == "__main__":
    main()