#!/usr/bin/env python3
"""gxtrace_test.py -- bet 2.2a: tools/gxtrace.py on hand-built version 2 files: a well-formed one, every structural failure, and the matrix-reload rule both ways.
  python3 -I tools/host-tests/gxtrace_test.py     (prints 'gxtrace_test: N checks, F failures')"""
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import gxtrace  # noqa: E402

checks = fails = 0


def check(ok, what):
    global checks, fails
    checks += 1
    if not ok:
        fails += 1
        print("FAIL: " + what)


def chunk(tag, payload):
    return tag + struct.pack("<I", len(payload)) + payload


def header(frames, count=None, flags=1, version=2):
    return b"GXTR" + struct.pack("<10I", version, 10, count if count is not None else frames, flags, 8, 0x60, 0x3A0, 0xA4000, 1024, 5) + bytes(20)


def ent(word, addr, path):
    return struct.pack("<IHBB", word, addr, path, 0)


def fifo(*words):
    return b"".join(ent(w, 0, 0) for w in words)


def frame(n, stream=b"", pages3d=b"", pages2d=b"", vb=None, idx=0, first=None, fend=None, l2=b""):
    cnt = [0, 0, 0, 0]
    nrd = 0
    for o in range(0, len(stream), 8):
        if stream[o + 6] == 4:
            nrd += 1
        else:
            cnt[stream[o + 6]] += 1
    p3 = struct.unpack_from("<I", pages3d, 12)[0] if pages3d else 0
    p2 = struct.unpack_from("<I", pages2d, 12)[0] if pages2d else 0
    fe = fend if fend is not None else struct.pack("<8I", sum(cnt), *cnt, p3 + p2, 0, nrd)
    b = chunk(b"FRAM", struct.pack("<4I", 10 + n, vb if vb is not None else 100 + n, idx, (1 if idx == 0 else 0) if first is None else first) + bytes(16))
    if stream:
        b += chunk(b"STRM", stream)
    b += chunk(b"R3D ", bytes(0x3A0)) + chunk(b"VCNT", bytes(16)) + pages3d
    b += chunk(b"R2A ", bytes(0x70)) + chunk(b"R2B ", bytes(0x70)) + l2 + chunk(b"PLTT", bytes(2048)) + chunk(b"OAM ", bytes(2048)) + pages2d
    return b + chunk(b"FEND", fe)


def page(region, idxs):
    return chunk(b"PAGE", struct.pack("<2I", region, len(idxs)) + b"".join(struct.pack("<I", i) + bytes(1024) for i in idxs))


def run(data, **kw):
    with tempfile.NamedTemporaryFile(delete=False) as f:
        f.write(data)
    try:
        return gxtrace.check(f.name, **kw)
    finally:
        os.unlink(f.name)


# packed commands, one per FIFO word for clarity: MTX_MODE 0x10 (1 arg), MTX_IDENTITY 0x15, BEGIN_VTXS 0x40 (1 arg), MTX_MULT_4x3 0x19 (12 args)
# a full reload: mode 0 identity; mode 2 identity; begin
s_ok = fifo(0x10, 0) + fifo(0x15) + fifo(0x10, 2) + fifo(0x15) + fifo(0x40, 0)
ok, lines = run(header(1) + frame(0, stream=s_ok, pages3d=page(0, [3, 5]), pages2d=page(16, [7])))
check(ok, "well-formed file with a full reload passes: " + " | ".join(lines))
check(any("hwlog" not in l and "matrix-reload rule" in l and "projection loaded=True position loaded=True vector loaded=True" in l for l in lines), "rule line reports all three loaded")

# no reload of the position/vector matrix
s_bad = fifo(0x10, 0) + fifo(0x15) + fifo(0x40, 0)
ok, lines = run(header(1) + frame(0, stream=s_bad))
check(not ok and any("vector loaded=False" in l for l in lines), "no position/vector reload before BEGIN_VTXS fails")

# use before load: MTX_MULT_4x3 (0x19, 12 args) in mode 2 before any load
s_use = fifo(0x10, 2) + fifo(0x19) + fifo(*range(12)) + fifo(0x15) + fifo(0x10, 0) + fifo(0x15) + fifo(0x40, 0)
ok, lines = run(header(1) + frame(0, stream=s_use))
check(not ok and any("violation" in l for l in lines), "a multiply before the matrix was loaded is a violation")

# stacks: a leading MTX_POP 0 is nothing; a leading MTX_POP 1 is a depth demand of 1 (balanced by a PUSH of the previous frame), not a use-before-load; PUSH adds depth
s_pop0 = fifo(0x10, 2) + fifo(0x12, 0) + fifo(0x15) + fifo(0x10, 0) + fifo(0x15) + fifo(0x40, 0)
ok, lines = run(header(1) + frame(0, stream=s_pop0))
check(ok and any("position/vector depth >= 0" in l for l in lines), "a leading MTX_POP 0 is neither a violation nor a depth demand: " + " | ".join(lines))
s_pop1 = fifo(0x10, 2) + fifo(0x12, 1) + fifo(0x15) + fifo(0x10, 0) + fifo(0x15) + fifo(0x40, 0)
ok, lines = run(header(1) + frame(0, stream=s_pop1))
check(ok and any("position/vector depth >= 1" in l for l in lines), "a leading MTX_POP 1 demands position/vector depth 1 and is not a violation: " + " | ".join(lines))
s_push = fifo(0x10, 2) + fifo(0x15) + fifo(0x11) + fifo(0x12, 1) + fifo(0x10, 0) + fifo(0x15) + fifo(0x40, 0)
ok, lines = run(header(1) + frame(0, stream=s_push))
check(ok and any("position/vector depth >= 0" in l and "position/vector +0" in l for l in lines), "PUSH then POP 1 balances: " + " | ".join(lines))
s_pop63 = fifo(0x10, 2) + fifo(0x12, 0x3f) + fifo(0x15) + fifo(0x10, 0) + fifo(0x15) + fifo(0x40, 0)   # 6-bit signed: 0x3f = -1 pops (a push)
ok, lines = run(header(1) + frame(0, stream=s_pop63))
check(ok and any("position/vector +1" in l for l in lines), "MTX_POP count is a signed 6-bit number")

# no geometry at all
ok, lines = run(header(1) + frame(0, stream=fifo(0x10, 0)))
check(not ok, "a window without BEGIN_VTXS fails")
ok, lines = run(header(1) + frame(0, stream=fifo(0x10, 0)), no_geometry_ok=True)
check(ok, "... unless --no-geometry-ok")

# structure failures
base = header(1) + frame(0, stream=s_ok)
for what, d in [
    ("bad magic", b"XXXX" + base[4:]),
    ("version 1", header(1, version=1) + base[64:]),
    ("truncated", base[:-4]),
    ("FEND entry count wrong", header(1) + frame(0, stream=s_ok, fend=struct.pack("<8I", 99, 0, 0, 0, 0, 0, 0, 0))),
    ("page index out of range", header(1) + frame(0, stream=s_ok, pages3d=page(0, [200]))),
    ("pages not ascending", header(1) + frame(0, stream=s_ok, pages3d=page(0, [5, 3]))),
    ("2D region in the 3D slot", header(1) + frame(0, stream=s_ok, pages3d=page(16, [1]))),
    ("regions not ascending", header(1) + frame(0, stream=s_ok, pages3d=page(1, [1]) + page(0, [1]))),
    ("frames not consecutive", header(2) + frame(0, stream=s_ok) + frame(2, stream=s_ok, idx=1)),
    ("vblanks backwards", header(2) + frame(0, stream=s_ok, vb=50) + frame(1, stream=s_ok, idx=1, vb=40)),
    ("first flag missing", header(1) + frame(0, stream=s_ok, first=0)),
    ("more frames than requested", header(1, count=1) + frame(0, stream=s_ok) + frame(1, stream=s_ok, idx=1)),
]:
    ok, lines = run(d)
    check(not ok, "rejected: " + what)

# 2 frames pass and the table has two rows
ok, lines = run(header(2) + frame(0, stream=s_ok, pages3d=page(0, [1])) + frame(1, stream=fifo(0x40, 0), idx=1), no_geometry_ok=False)
check(ok, "two frames, the second without reload: the rule judges the stream from the window start: " + " | ".join(lines))

# bet 2.2c B3: header flag bit 1 + FEND word 6 (the seal kind)
def seal_fend(kind, stream):
    cnt = [0, 0, 0, 0]
    for o in range(0, len(stream), 8):
        cnt[stream[o + 6]] += 1
    return struct.pack("<8I", sum(cnt), *cnt, 0, kind, 0)

ok, lines = run(header(1, flags=3) + frame(0, stream=s_ok, fend=seal_fend(1, s_ok)))
check(ok and "seal=snapshot" in lines[0], "flags 3: snapshot seal read from the header: " + lines[0])
ok, lines = run(header(1, flags=1) + frame(0, stream=s_ok))
check(ok and "seal=frame-end" in lines[0], "flags 1: frame-end seal (a trace from before 2.2c)")
ok, lines = run(header(1, flags=3) + frame(0, stream=s_ok, fend=seal_fend(2, s_ok)))
check(ok and any("sealed by the next frame end" in l for l in lines), "seal kind 2 is accepted and reported")
ok, lines = run(header(1, flags=3) + frame(0, stream=s_ok))
check(not ok, "flags 3 with seal kind 0 is rejected")
ok, lines = run(header(1, flags=1) + frame(0, stream=s_ok, fend=seal_fend(1, s_ok)))
check(not ok, "flags 1 with seal kind 1 is rejected")

# bet 2.2c F5: the sbc.c BB sequence counter
seq = [(0x15 << 16) | (0x11 << 8) | 0x10 | (0x12 << 24), 0, 1, (0x1B << 24) | (0x1B << 16) | (0x17 << 8) | 0x10, 2] + [4096, 0, 0, 0, 4096, 0, 0, 0, 4096, 5, 6, 7] + [4096, 4096, 4096] + [4096, 4096, 4096]
import gxtrace
parsed = gxtrace.parse(header(1) + frame(0, stream=fifo(*seq)))[1]
found = gxtrace.bb_sequences(parsed)
check(len(found) == 1 and found[0]["trans"] == (5, 6, 7), "BB sequence found with its translation: " + str(found))
check(len(gxtrace.bb_sequences(gxtrace.parse(header(1) + frame(0, stream=fifo(*(seq[:14] + [0, 0, 0] + seq[17:]))))[1])) == 1, "BB sequence with a zero translation (at the eye) still counted")
check(gxtrace.bb_sequences(gxtrace.parse(header(1) + frame(0, stream=fifo(*seq[:5])))[1]) == [], "a truncated sequence is not counted")

# hwlog cross-check
hw = "gxtrace: state 3 (3 = done); 1 frames, %d entries; appended %d/0/0/0; counters %d/0/0/0; %d bytes"
d = header(1) + frame(0, stream=s_ok)
n = len(s_ok) // 8
ok, lines = run(d, hwlog=hw % (n, n, n, len(d)))
check(ok, "hwlog counters agree")
ok, lines = run(d, hwlog=hw % (n, n, n + 1, len(d)))
check(not ok, "hwlog counters disagree")

# bet 2.3a: header flag bit 2 and the L2A / L2B per-line 2D chunks
def l2(form_a, img_a, form_b, img_b):
    return chunk(b"L2A ", struct.pack("<I", form_a) + img_a) + chunk(b"L2B ", struct.pack("<I", form_b) + img_b)

Z = bytes(0x70)
snap_fend = lambda stream, kind=1: seal_fend(kind, stream)
ok, lines = run(header(1, flags=7) + frame(0, stream=s_ok, fend=snap_fend(s_ok), l2=l2(0, Z, 0, Z)))
check(ok and any("per-line 2D" in l and "2 uniform" in l for l in lines), "flags 7, uniform L2A/L2B accepted and counted: " + " | ".join(lines))
ok, lines = run(header(1, flags=7) + frame(0, stream=s_ok, fend=snap_fend(s_ok), l2=l2(1, Z * 192, 1, Z * 192)))
check(ok and any("2 full" in l and "2 of them identical" in l for l in lines), "192-image form accepted, 'identical on every line' reported: " + " | ".join(lines))
varied = b"".join(bytes([n]) + bytes(0x6F) for n in range(192))
ok, lines = run(header(1, flags=7) + frame(0, stream=s_ok, fend=snap_fend(s_ok), l2=l2(1, varied, 1, Z * 192)))
check(not ok and any("last-line image differs" in l for l in lines), "last line must equal R2A: a varying last line is rejected")
ok, lines = run(header(1, flags=7) + frame(0, stream=s_ok, fend=snap_fend(s_ok, 2), l2=l2(2, b"", 2, b"")))
check(ok and any("2 none" in l for l in lines), "form NONE on a next-vblank-sealed frame accepted")
ok, lines = run(header(1, flags=7) + frame(0, stream=s_ok, fend=snap_fend(s_ok, 1), l2=l2(2, b"", 2, b"")))
check(not ok, "form NONE on a snapshot-sealed frame rejected")
ok, lines = run(header(1, flags=7) + frame(0, stream=s_ok, fend=snap_fend(s_ok, 2), l2=l2(0, Z, 0, Z)))
check(not ok, "an image on a next-vblank-sealed frame rejected")
ok, lines = run(header(1, flags=7) + frame(0, stream=s_ok, fend=snap_fend(s_ok)))
check(not ok, "flag 4 set but the frame has no L2A/L2B: rejected")
ok, lines = run(header(1, flags=3) + frame(0, stream=s_ok, fend=snap_fend(s_ok), l2=l2(0, Z, 0, Z)))
check(not ok, "L2A/L2B without header flag 4: rejected")
ok, lines = run(header(1, flags=7) + frame(0, stream=s_ok, fend=snap_fend(s_ok), l2=l2(0, Z[:-4], 0, Z)))
check(not ok, "short uniform image rejected")
ok, lines = run(header(1, flags=7) + frame(0, stream=s_ok, fend=snap_fend(s_ok), l2=l2(7, Z, 0, Z)))
check(not ok, "unknown form rejected")
bad_a = bytes(4) + bytes([1]) + bytes(0x6B)   # DISPSTAT/VCOUNT bytes 4..7 non-zero
ok, lines = run(header(1, flags=7) + frame(0, stream=s_ok, fend=snap_fend(s_ok), l2=l2(0, bad_a, 0, Z)))
check(not ok, "a record whose DISPSTAT byte is not zeroed (and differs from the masked R2A) rejected")

# bet 2.4a: RDBK entries (path 4, header flag bit 3)
def rd_head(n, addr, pad=1):
    return struct.pack("<IHBB", n, addr, 4, pad)


def rd_val(v, addr, pad=2):
    return struct.pack("<IHBB", v, addr, 4, pad)


def rdbk(off, *vals):
    return rd_head(len(vals), off) + b"".join(rd_val(v, off + 4 * i) for i, v in enumerate(vals))


RD = 9   # CLOCK_FRAME | RDBK
s_rd = fifo(0x10, 0) + fifo(0x15) + rdbk(0x600, 0x06000000) + fifo(0x10, 2) + fifo(0x15) + rdbk(0x640, *range(16)) + rdbk(0x630, 0x7ff) + fifo(0x40, 0)
ok, lines = run(header(1, flags=RD) + frame(0, stream=s_rd))
check(ok and any("RDBK readbacks over the window: 3 groups, 21 entries" in l for l in lines), "RDBK groups accepted, counted, ignored by the matrix rule: " + " | ".join(lines))
hdr_, frs = gxtrace.parse(header(1, flags=RD) + frame(0, stream=s_rd))
grp, errs = gxtrace.rdbk_groups(frs[0])
check(not errs and [(g[0], g[1], len(g[2])) for g in grp] == [(3, 0x600, 1), (6, 0x640, 16), (6, 0x630, 1)], "rdbk_groups: stream position = store entries before it: %s %s" % (grp[:1], errs))
check(grp[1][2] == list(range(16)), "rdbk_groups: values kept in order")
check(len(list(gxtrace.entries(frs[0]))) == 8 and len(list(gxtrace.entries(frs[0], rdbk=True))) == 8 + 21, "entries() skips RDBK unless asked")
# slice 2.4c: the GXSTAT / DISP3DCNT WRITE entry (path 4, pad 3, standing alone, counted in FEND word 7)
s_wr = rd_val(0x80000000, 0x600, pad=3) + fifo(0x10, 0) + fifo(0x15) + fifo(0x10, 2) + fifo(0x15) + rdbk(0x600, 0x86000000) + rd_val(0x3001, 0x060, pad=3) + fifo(0x40, 0)
ok, lines = run(header(1, flags=RD) + frame(0, stream=s_wr))
check(ok and any("RDBK readbacks over the window: 1 groups, 4 entries" in l for l in lines), "write entries accepted and counted in the RDBK entry total: " + " | ".join(lines))
hdr_, frs = gxtrace.parse(header(1, flags=RD) + frame(0, stream=s_wr))
grp, errs, wrs = gxtrace.rdbk_scan(frs[0])
check(not errs and wrs == [(0, 0x600, 0x80000000), (6, 0x060, 0x3001)] and [(g[0], g[1]) for g in grp] == [(6, 0x600)], "rdbk_scan: writes with their stream position: %s %s %s" % (wrs, grp, errs))
ok, lines = run(header(1, flags=RD) + frame(0, stream=s_ok + rd_val(0, 0x640, pad=3)))
check(not ok and any("bad RDBK write" in l for l in lines), "a write entry naming a register other than GXSTAT / DISP3DCNT rejected")
ok, lines = run(header(1, flags=1) + frame(0, stream=s_rd))
check(not ok and any("RDBK entries but header flag" in l for l in lines), "RDBK entries without header flag bit 3 rejected")
ok, lines = run(header(1, flags=RD) + frame(0, stream=s_rd, fend=struct.pack("<8I", 7, 0, 0, 0, 7, 0, 0, 20)))
check(not ok and any("FEND word 7" in l for l in lines), "FEND word 7 != RDBK entry count rejected")
ok, lines = run(header(1, flags=RD) + frame(0, stream=s_ok + rd_head(1, 0x600) + rd_val(0, 0x600, pad=0)))
check(not ok and any("bad RDBK value" in l for l in lines), "RDBK value with pad 0 rejected")
ok, lines = run(header(1, flags=RD) + frame(0, stream=s_ok + rd_head(1, 0x600, pad=0) + rd_val(0, 0x600)))
check(not ok and any("bad RDBK head" in l for l in lines), "RDBK head with pad 0 rejected")
ok, lines = run(header(1, flags=RD) + frame(0, stream=s_ok + rd_head(0, 0x600)))
check(not ok and any("bad RDBK head" in l for l in lines), "RDBK head with zero values rejected")
ok, lines = run(header(1, flags=RD) + frame(0, stream=s_ok + rd_head(17, 0x640)))
check(not ok and any("bad RDBK head" in l for l in lines), "RDBK head with 17 values rejected")
ok, lines = run(header(1, flags=RD) + frame(0, stream=s_ok + rd_head(1, 0x6B0) + rd_val(0, 0x6B0)))
check(not ok and any("bad RDBK head" in l for l in lines), "RDBK head outside 0x600..0x6A7 rejected")
ok, lines = run(header(1, flags=RD) + frame(0, stream=s_ok + rd_head(2, 0x620) + rd_val(1, 0x620) + rd_val(2, 0x628)))
check(not ok and any("bad RDBK value" in l for l in lines), "RDBK value at the wrong address rejected")
ok, lines = run(header(1, flags=RD) + frame(0, stream=s_ok + rd_head(2, 0x620) + rd_val(1, 0x620) + fifo(0x15) + rd_val(2, 0x624)))
check(not ok and any("is cut after 1 of 2" in l for l in lines), "RDBK group cut by a store rejected")
ok, lines = run(header(1, flags=RD) + frame(0, stream=s_ok + rd_head(2, 0x620) + rd_val(1, 0x620)))
check(not ok and any("cut by the end of the frame" in l for l in lines), "RDBK group cut by the frame end rejected")

print("gxtrace_test: %d checks, %d failures" % (checks, fails))
sys.exit(1 if fails else 0)
