"""Drift guard: regenerate icon/banner/chime into a temp dir and fail if any differs from the committed copy.

The comparison is on decoded content, not bytes: PNGs by header + inflated scanlines (another zlib / zlib-ng deflates the same
pixels to different bytes), WAVs by format + samples with a tolerance of 1 LSB per sample (another libm rounds a few samples
differently). The committed files themselves stay byte-for-byte what the generators wrote on the author's machine.

Structure is checked too (G2, RF3 review): a PNG must be exactly IHDR, one or more IDAT, IEND with nothing after; a WAV exactly RIFF/WAVE,
"fmt " and "data" with nothing after. An extra chunk (tEXt, iTXt, a LIST, a pasted bitmap in an ancillary chunk) or trailing bytes would
carry non-pixel, non-sample bytes into the public tree, so they fail as drift.

Usage: python3 tools/art/check_art.py [repo_root]   (default: this repo's root)
       python3 tools/art/check_art.py --self-test    (plants a one-byte fault; must be detected)
Exit status 0 = all identical, 1 = drift, 2 = self-test failed to detect the fault.
"""
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
WAV_TOLERANCE = 1      # LSB per 16-bit sample
SCRIPTS = {"icon.png": "gen_icon.py", "banner.png": "gen_banner.py", "banner.wav": "gen_chime.py"}


def regenerate(out_dir: Path) -> None:
    """Run every generator into out_dir; raises CalledProcessError if one fails."""
    for script in SCRIPTS.values():
        subprocess.run([sys.executable, str(HERE / script), str(out_dir)], check=True, cwd=HERE)


def decode_png(path: Path) -> tuple[bytes, bytes]:
    """(IHDR payload, inflated scanlines) of an 8-bit PNG; raises ValueError on a malformed file."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, ihdr, idat = 8, b"", b""
    tags: list[bytes] = []
    while pos + 8 <= len(data):
        length, tag = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if len(body) != length or zlib.crc32(tag + body) & 0xFFFFFFFF != struct.unpack(">I", data[pos + 8 + length:pos + 12 + length])[0]:
            raise ValueError(f"{path}: bad chunk {tag!r}")
        tags.append(tag)
        if tag == b"IHDR":
            ihdr = body
        elif tag == b"IDAT":
            idat += body
        pos += 12 + length
    if pos != len(data):
        raise ValueError(f"{path}: {len(data) - pos} trailing bytes after the last chunk")
    if not (len(tags) >= 3 and tags[0] == b"IHDR" and tags[-1] == b"IEND" and all(x == b"IDAT" for x in tags[1:-1])):
        raise ValueError(f"{path}: chunk list {[x.decode('latin-1') for x in tags]}, want IHDR, IDAT..., IEND")
    try:
        return ihdr, zlib.decompress(idat)
    except zlib.error as err:
        raise ValueError(f"{path}: {err}") from err


def same_png(a: Path, b: Path) -> bool:
    return decode_png(a) == decode_png(b)


def check_wav_structure(path: Path) -> None:
    """Raises ValueError unless the file is exactly RIFF/WAVE with the chunks "fmt " and "data" and no byte after them."""
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE" or struct.unpack("<I", data[4:8])[0] + 8 != len(data):
        raise ValueError(f"{path}: not a RIFF/WAVE file of the size its header says")
    pos, tags = 12, []
    while pos + 8 <= len(data):
        tag, length = struct.unpack("<4sI", data[pos:pos + 8])
        tags.append(tag)
        pos += 8 + length + (length & 1)
    if pos != len(data) or tags != [b"fmt ", b"data"]:
        raise ValueError(f"{path}: chunk list {[x.decode('latin-1') for x in tags]}, want fmt , data")


def same_wav(a: Path, b: Path) -> bool:
    """Same format and frame count, and every 16-bit sample within WAV_TOLERANCE of its counterpart."""
    check_wav_structure(a)
    with wave.open(str(a), "rb") as wa, wave.open(str(b), "rb") as wb:
        if wa.getparams()[:4] != wb.getparams()[:4] or wa.getsampwidth() != 2:
            return False
        sa, sb = wa.readframes(wa.getnframes()), wb.readframes(wb.getnframes())
    if len(sa) != len(sb):
        return False
    count = len(sa) // 2
    return all(abs(x - y) <= WAV_TOLERANCE for x, y in zip(struct.unpack(f"<{count}h", sa), struct.unpack(f"<{count}h", sb)))


def drifted(repo_root: Path) -> list[str]:
    """Names of committed files whose decoded content differs from a fresh regeneration (or that cannot be decoded)."""
    with tempfile.TemporaryDirectory() as tmp:
        fresh = Path(tmp)
        regenerate(fresh)
        out = []
        for name in SCRIPTS:
            same = same_wav if name.endswith(".wav") else same_png
            try:
                ok = same(repo_root / name, fresh / name)
            except (ValueError, wave.Error, EOFError, struct.error):
                ok = False
            if not ok:
                out.append(name)
        return out


def repack_png(path: Path, level: int) -> None:
    """Rewrite the PNG with the same pixels but a different deflate level (what another zlib would do)."""
    ihdr, raw = decode_png(path)
    def chunk(tag: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, level)) + chunk(b"IEND", b""))


def shift_wav_sample(path: Path, index: int, delta: int) -> None:
    with wave.open(str(path), "rb") as w:
        params, frames = w.getparams(), bytearray(w.readframes(w.getnframes()))
    value = struct.unpack_from("<h", frames, index * 2)[0] + delta
    struct.pack_into("<h", frames, index * 2, value)
    with wave.open(str(path), "wb") as w:
        w.setparams(params)
        w.writeframes(bytes(frames))


def png_chunk(tag: bytes, body: bytes) -> bytes:
    return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)


def riff_fix(data: bytes) -> bytes:
    """The RIFF size field made right for the bytes appended after the original data."""
    return data[:4] + struct.pack("<I", len(data) - 8) + data[8:]


def self_test() -> int:
    """Plant faults in copies of the committed files: a changed pixel and a 2-LSB sample must be flagged; a re-deflated PNG and a
    1-LSB sample must not."""
    root = HERE.parents[1]
    with tempfile.TemporaryDirectory() as tmp:
        copy = Path(tmp)
        for name in SCRIPTS:
            shutil.copy(root / name, copy / name)
        if drifted(copy):
            print("self-test: clean copy already drifts; fix the art first")
            return 2
        repack_png(copy / "icon.png", 1)
        repack_png(copy / "banner.png", 0)
        shift_wav_sample(copy / "banner.wav", 1000, 1)
        if (copy / "icon.png").read_bytes() == (root / "icon.png").read_bytes() or drifted(copy):
            print("self-test FAILED: a re-deflated PNG or a 1-LSB sample was reported as drift")
            return 2
        ihdr, raw = decode_png(copy / "icon.png")
        changed = bytearray(raw)
        changed[len(changed) // 2] ^= 0x01
        body = struct.pack(">I", len(ihdr)) + b"IHDR" + ihdr + struct.pack(">I", zlib.crc32(b"IHDR" + ihdr) & 0xFFFFFFFF)
        packed = zlib.compress(bytes(changed), 9)
        idat = struct.pack(">I", len(packed)) + b"IDAT" + packed + struct.pack(">I", zlib.crc32(b"IDAT" + packed) & 0xFFFFFFFF)
        iend = struct.pack(">I", 0) + b"IEND" + struct.pack(">I", zlib.crc32(b"IEND") & 0xFFFFFFFF)
        (copy / "icon.png").write_bytes(b"\x89PNG\r\n\x1a\n" + body + idat + iend)
        shift_wav_sample(copy / "banner.wav", 2000, 2)
        found = drifted(copy)
        if found != ["icon.png", "banner.wav"]:
            print(f"self-test FAILED: planted faults not detected as expected (got {found})")
            return 2
        for name in SCRIPTS:
            shutil.copy(root / name, copy / name)
        # structure faults: an ancillary PNG chunk, bytes after IEND, an extra RIFF chunk, bytes after the data chunk
        for label, name, edit in (
            ("a PNG tEXt chunk", "icon.png", lambda d: d[:-12] + png_chunk(b"tEXt", b"k\0v") + d[-12:]),
            ("bytes after IEND", "icon.png", lambda d: d + b"\0"),
            ("a RIFF LIST chunk", "banner.wav", lambda d: riff_fix(d + b"LIST" + struct.pack("<I", 4) + b"INFO")),
            ("bytes after the WAV data", "banner.wav", lambda d: d + b"\0\0"),
        ):
            shutil.copy(root / name, copy / name)
            (copy / name).write_bytes(edit((copy / name).read_bytes()))
            if drifted(copy) != [name]:
                print(f"self-test FAILED: {label} was not reported as drift")
                return 2
            shutil.copy(root / name, copy / name)
    print("self-test OK: a changed pixel and a 2-LSB sample were detected; a re-deflated PNG and a 1-LSB sample were not; an extra PNG/RIFF chunk and trailing bytes were")
    return 0


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] == "--self-test":
        return self_test()
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parents[1]
    bad = drifted(root)
    if bad:
        print("DRIFT: " + ", ".join(bad) + " differ from the generators' output")
        return 1
    print("art OK: icon.png banner.png banner.wav match the generators")
    return 0


if __name__ == "__main__":
    sys.exit(main())
