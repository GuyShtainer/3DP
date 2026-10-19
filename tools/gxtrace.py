#!/usr/bin/env python3
"""gxtrace.py -- parser, validator and one-screen summary for gxtrace.bin version 2 (bet 2.2a; format: source/hw/hw_gx.h and the private design notes).

  python3 -I tools/gxtrace.py FILE [--hwlog hwlog.txt] [--no-geometry-ok]

stdlib only. Exit 0 = the file is well formed AND the matrix-reload rule holds; 1 otherwise.

What is checked:
  * header (magic, version 2, field values), then a sequence of frames, each exactly
        FRAM  STRM*  R3D  VCNT  PAGE*(3D regions 0..9)  R2A  R2B  [L2A  L2B]  PLTT  OAM  PAGE*(2D regions 16..29)  FEND
    (L2A / L2B only when header flags bit 2 is set, bet 2.3a: `u32 form` + one 0x70-byte image per engine for all 192 lines (form 0), 192 images (form 1) or nothing (form 2,
    only for a frame sealed by the next vblank hook); the last line's image must equal R2A / R2B, with R2A's DISPSTAT / VCOUNT bytes 4..7 zeroed);
    with the exact payload lengths, ascending unique region ids, page indexes in range and ascending, nothing after the last FEND;
  * frame numbers consecutive, the DS vblank count never going backwards, the first frame flagged, FEND's entry counts == the STRM entries of that frame;
  * PAGE chunks: region ids ascending and unique per half, page indexes ascending and in range (so no page twice in a frame);
  * the MATRIX-RELOAD RULE (the trace carries neither the current matrices nor the stacks before bet 2.4): decoding the stream in program order, before the first BEGIN_VTXS the game must have
    loaded the PROJECTION matrix (MTX_MODE 0, then MTX_IDENTITY / MTX_LOAD_4x4 / MTX_LOAD_4x3) and the POSITION and VECTOR matrices (MTX_MODE 2 then a load/identity),
    and must not have used (multiplied, scaled, translated, pushed, stored) a matrix of mode 0/1/2 before that matrix was loaded in the window (POP and RESTORE take the matrix
    from the stack, so they count as stack-depth demands, reported as such, not as uses);
  * RDBK entries (bet 2.4a, header flags bit 3, STRM entries with path 4): head { word = n in 1..16, addr = register offset 0x600..0x6A7 (even), pad 1 } followed by exactly n
    values { addr = off + 4 * i, pad 2 }, never cut by another entry or a frame end; FEND word 7 == the frame's RDBK entry count; all other entries have pad 0;
  * optionally (--hwlog) the stream entry counts against the hwlog's `gxtrace:` counters.
"""
import re
import struct
import sys

TAG_FRAM, TAG_STRM, TAG_R3D, TAG_VCNT, TAG_PAGE = b"FRAM", b"STRM", b"R3D ", b"VCNT", b"PAGE"
TAG_R2A, TAG_R2B, TAG_PLTT, TAG_OAM, TAG_FEND = b"R2A ", b"R2B ", b"PLTT", b"OAM ", b"FEND"
TAG_L2A, TAG_L2B = b"L2A ", b"L2B "
L2_UNIFORM, L2_LINES, L2_NONE = 0, 1, 2
L2_LINES_N = 192
HDR = 64
PAGE_BYTES = 1024
REGS3D_LEN = 0x3A0
REGS2D_LEN = 0x70
# page regions: id -> (name, pages)
REGIONS = {}
for _i in range(4):
    REGIONS[_i] = ("TEX%d" % _i, 128)
for _i in range(6):
    REGIONS[4 + _i] = ("TEXPLTT%d" % _i, 16)
REGIONS.update({16: ("mainBG", 512), 17: ("mainOBJ", 256), 18: ("subBG", 128), 19: ("subOBJ", 128)})
for _i in range(4):
    REGIONS[20 + _i] = ("mainBGext%d" % _i, 8)
    REGIONS[24 + _i] = ("subBGext%d" % _i, 8)
REGIONS.update({28: ("mainOBJext", 8), 29: ("subOBJext", 8)})
REGIONS_3D = [r for r in REGIONS if r < 16]
REGIONS_2D = [r for r in REGIONS if r >= 16]
SIZE_CEILING = 16 * 1024 * 1024   # a ten-frame field window is ~2.8 MB; 16 MiB is the "something is wrong" line

ARGC = {0x10: 1, 0x11: 0, 0x12: 1, 0x13: 1, 0x14: 1, 0x15: 0, 0x16: 16, 0x17: 12, 0x18: 16, 0x19: 12, 0x1a: 9, 0x1b: 3, 0x1c: 3,
        0x20: 1, 0x21: 1, 0x22: 1, 0x23: 2, 0x24: 1, 0x25: 1, 0x26: 1, 0x27: 1, 0x28: 1, 0x29: 1, 0x2a: 1, 0x2b: 1,
        0x30: 1, 0x31: 1, 0x32: 1, 0x33: 1, 0x34: 32, 0x40: 1, 0x41: 0, 0x50: 1, 0x60: 1, 0x70: 3, 0x71: 2, 0x72: 1}
NAMES = {0x10: "MTX_MODE", 0x11: "MTX_PUSH", 0x12: "MTX_POP", 0x13: "MTX_STORE", 0x14: "MTX_RESTORE", 0x15: "MTX_IDENTITY", 0x16: "MTX_LOAD_4x4",
         0x17: "MTX_LOAD_4x3", 0x18: "MTX_MULT_4x4", 0x19: "MTX_MULT_4x3", 0x1a: "MTX_MULT_3x3", 0x1b: "MTX_SCALE", 0x1c: "MTX_TRANS", 0x40: "BEGIN_VTXS",
         0x41: "END_VTXS", 0x50: "SWAP_BUFFERS"}
LOADS = (0x15, 0x16, 0x17)
USES = (0x11, 0x12, 0x13, 0x14, 0x18, 0x19, 0x1a, 0x1b, 0x1c)


class FormatError(Exception):
    pass


def parse(data):
    """-> (header dict, [frame dict]). Raises FormatError with the byte offset on any structural problem."""
    if len(data) < HDR or data[:4] != b"GXTR":
        raise FormatError("bad magic / too short (%d bytes)" % len(data))
    (ver, frm, cnt, flags, esz, roff, rlen, vlen, pg, vb0) = struct.unpack_from("<10I", data, 4)
    if ver != 2:
        raise FormatError("version %d, this reader is version 2 (v1 was removed in bet 2.2a)" % ver)
    if (esz, roff, rlen, pg) != (8, 0x60, REGS3D_LEN, PAGE_BYTES):
        raise FormatError("header constants: entry %d regs_off %#x regs_len %#x page %d" % (esz, roff, rlen, pg))
    hdr = dict(from_frame=frm, count=cnt, flags=flags, vram_bytes=vlen, vblanks_at_open=vb0, size=len(data), clock="frame" if flags & 1 else "wall",
               seal="snapshot" if flags & 2 else "frame-end", lines2d=bool(flags & 4), rdbk=bool(flags & 8))   # bit 1 (bet 2.2c, B3): the state is sealed at the game's snapshot point (after the vblank callback), FEND word 6 = seal kind per frame
    pos, frames, cur = HDR, [], None
    stage = 0   # 1 after FRAM (STRM* allowed), 2 after R3D, 3 after VCNT (3D PAGE* allowed), 4 after R2A, 5 after R2B, 6 after PLTT, 7 after OAM (2D PAGE* allowed, then FEND)
    # with header flag bit 2 (bet 2.3a) R2B is followed by L2A (stage 5 -> 52) and L2B (52 -> 53) and PLTT needs stage 53
    flat = {TAG_R2A: (3, REGS2D_LEN, "r2a", 4), TAG_R2B: (4, REGS2D_LEN, "r2b", 5), TAG_PLTT: (53 if hdr["lines2d"] else 5, 2048, "pltt", 6), TAG_OAM: (6, 2048, "oam", 7)}
    while pos < len(data):
        if pos + 8 > len(data):
            raise FormatError("offset %d: truncated chunk header" % pos)
        tag, ln = struct.unpack_from("<4sI", data, pos)
        body = data[pos + 8:pos + 8 + ln]
        if len(body) != ln or ln % 4:
            raise FormatError("offset %d: chunk %r length %d does not fit / is not a multiple of 4" % (pos, tag, ln))
        off = pos
        pos += 8 + ln
        if tag == TAG_FRAM:
            if cur is not None:
                raise FormatError("offset %d: FRAM inside an unfinished frame (missing FEND)" % off)
            if ln != 32:
                raise FormatError("offset %d: FRAM length %d" % (off, ln))
            f, vb, wi, fl = struct.unpack_from("<4I", body)
            cur = dict(frame=f, vblanks=vb, win_index=wi, first=fl & 1, strm=[], r3d=None, vcnt=None, pages3d={}, pages2d={}, r2a=None, r2b=None, pltt=None,
                       oam=None, fend=None, off=off, nbytes=0, l2a=None, l2b=None)
            stage = 1
            continue
        if cur is None:
            raise FormatError("offset %d: chunk %r outside a frame" % (off, tag))
        if tag == TAG_STRM:
            if stage != 1 or ln % 8:
                raise FormatError("offset %d: STRM out of place / length %d" % (off, ln))
            cur["strm"].append(body)
        elif tag == TAG_R3D:
            if stage != 1 or ln != REGS3D_LEN:
                raise FormatError("offset %d: R3D out of place / length %d" % (off, ln))
            cur["r3d"], stage = body, 2
        elif tag == TAG_VCNT:
            if stage != 2 or ln != 16:
                raise FormatError("offset %d: VCNT out of place / length %d" % (off, ln))
            cur["vcnt"], stage = body, 3
        elif tag == TAG_PAGE:
            if ln < 8:
                raise FormatError("offset %d: PAGE too short" % off)
            rid, n = struct.unpack_from("<2I", body)
            if rid not in REGIONS or ln != 8 + n * (4 + PAGE_BYTES) or n == 0:
                raise FormatError("offset %d: PAGE region %d npages %d length %d" % (off, rid, n, ln))
            if stage == 3 and rid in REGIONS_3D:
                bag = cur["pages3d"]
            elif stage == 7 and rid in REGIONS_2D:
                bag = cur["pages2d"]
            else:
                raise FormatError("offset %d: PAGE region %d in stage %d" % (off, rid, stage))
            if bag and rid <= max(bag):
                raise FormatError("offset %d: PAGE regions not ascending / duplicated (%d)" % (off, rid))
            pages, p, last = {}, 8, -1
            for _ in range(n):
                pi = struct.unpack_from("<I", body, p)[0]
                if pi <= last or pi >= REGIONS[rid][1]:
                    raise FormatError("offset %d: page index %d out of range / not ascending in region %d" % (off, pi, rid))
                last = pi
                pages[pi] = body[p + 4:p + 4 + PAGE_BYTES]
                p += 4 + PAGE_BYTES
            bag[rid] = pages
        elif tag in (TAG_L2A, TAG_L2B):
            need, key, nxt = (5, "l2a", 52) if tag == TAG_L2A else (52, "l2b", 53)
            if not hdr["lines2d"] or stage != need or ln < 4:
                raise FormatError("offset %d: %r out of place (stage %d, header per-line flag %s) / length %d" % (off, tag, stage, hdr["lines2d"], ln))
            form = struct.unpack_from("<I", body)[0]
            want = {L2_UNIFORM: 4 + REGS2D_LEN, L2_LINES: 4 + L2_LINES_N * REGS2D_LEN, L2_NONE: 4}.get(form)
            if want is None or ln != want:
                raise FormatError("offset %d: %r form %d length %d" % (off, tag, form, ln))
            cur[key], stage = (form, body[4:]), nxt
        elif tag in flat:
            need, want_len, key, nxt = flat[tag]
            if stage != need or ln != want_len:
                raise FormatError("offset %d: %r out of place (stage %d) / length %d" % (off, tag, stage, ln))
            cur[key], stage = body, nxt
        elif tag == TAG_FEND:
            if stage != 7 or ln != 32:
                raise FormatError("offset %d: FEND out of place (stage %d) / length %d" % (off, stage, ln))
            cur["fend"] = struct.unpack_from("<8I", body)
            cur["nbytes"] = pos - cur["off"]
            frames.append(cur)
            cur, stage = None, 0
        else:
            raise FormatError("offset %d: unknown chunk tag %r" % (off, tag))
    if cur is not None:
        raise FormatError("file ends inside a frame (frame %d has no FEND): the run was cut short" % cur["frame"])
    return hdr, frames


RDBK_PATH = 4           # bet 2.4a: a trace-only entry kind, the game READ a geometry-engine register (header flags bit 3)
RDBK_MAX = 16
RDBK_REGS = (0x600, 0x6A8)   # GXSTAT .. VECMTX end: the register offsets a readback may name
WRITE_PAD = 3                # slice 2.4c: a standing-alone entry, the game WROTE GXSTAT (0x600) or DISP3DCNT (0x060); word = the value the port's engine applied
WRITE_REGS = (0x600, 0x060)


def entries(frame, rdbk=False):
    """Iterate (word, addr, path, pad) over a frame's STRM chunks in program order. RDBK entries (path 4) are left out unless `rdbk`: every consumer that replays the
    stores (Decoder, bb_sequences, matrix_rule, the census) wants the four store paths only."""
    for body in frame["strm"]:
        for o in range(0, len(body), 8):
            w, a, p, z = struct.unpack_from("<IHBB", body, o)
            if p == RDBK_PATH and not rdbk:
                continue
            yield w, a, p, z


def rdbk_groups(frame):
    """-> (groups, errors); see rdbk_scan."""
    g, e, _ = rdbk_scan(frame)
    return g, e


def rdbk_scan(frame):
    """-> (groups, errors, writes). writes = [(index of the next store entry, register offset, value)]: the game's GXSTAT / DISP3DCNT writes (pad 3, slice 2.4c).
    groups = [(index of the next store entry, register offset, [values])]: the stream position of each game readback (the number of path 0..3
    entries before it) and the values the port returned. errors = format violations (head/value pads, counts, addresses, a group cut by another entry)."""
    groups, errs, left, cur, nstore, writes = [], [], 0, None, 0, []
    for w, a, p, z in entries(frame, rdbk=True):
        if p != RDBK_PATH:
            if left:
                errs.append("a readback group at %#x is cut after %d of %d values" % (cur[1], len(cur[2]), len(cur[2]) + left))
                left = 0
            if z != 0 or p > 3:
                errs.append("bad entry (path %d pad %d)" % (p, z))
            nstore += 1
        elif left == 0 and z == WRITE_PAD:
            if w > 0xffffffff or a not in WRITE_REGS:
                errs.append("bad RDBK write (word %#x addr %#x)" % (w, a))
            writes.append((nstore, a, w))
        elif left == 0:
            if z != 1 or not 1 <= w <= RDBK_MAX or a & 1 or not RDBK_REGS[0] <= a < RDBK_REGS[1]:
                errs.append("bad RDBK head (word %d addr %#x pad %d)" % (w, a, z))
                continue
            cur, left = (nstore, a, []), w
            groups.append(cur)
        else:
            want = cur[1] + 4 * len(cur[2])
            if z != 2 or a != want:
                errs.append("bad RDBK value (addr %#x want %#x pad %d)" % (a, want, z))
            cur[2].append(w)
            left -= 1
    if left:
        errs.append("a readback group at %#x is cut by the end of the frame" % cur[1])
    return groups, errs, writes


class Decoder:
    """The GX command-packing state machine shared by the four input paths (census: hw_census.c fifo_word / port_store), reporting each command as it starts."""

    def __init__(self, on_cmd):
        self.on_cmd, self.q, self.left, self.cur, self.pidx, self.unknown = on_cmd, [], 0, 0, {}, 0

    def _advance(self):
        while self.q and self.left == 0:
            c = self.q.pop(0)
            if c not in ARGC:
                self.unknown += 1
                continue
            self.on_cmd(c, None)
            self.cur, self.argn, self.left = c, 0, ARGC[c]

    def fifo(self, w):
        if self.left == 0:
            self.q = [b for b in ((w >> (8 * i)) & 0xff for i in range(4)) if b]
            self._advance()
            return
        self.on_cmd(self.cur, (self.argn, w))
        self.argn += 1
        self.left -= 1
        if self.left == 0:
            self._advance()

    def port(self, addr, w):
        c = (addr - 0x400) // 4
        n = ARGC.get(c)
        if n is None:
            self.unknown += 1
            return
        if n == 0:
            self.on_cmd(c, None)
            return
        i = self.pidx.get(c, 0)
        if i == 0:
            self.on_cmd(c, None)
        self.on_cmd(c, (i, w))
        self.pidx[c] = (i + 1) % n


def bb_sequences(frames):
    """Bet 2.2c F5. The `else` branch of NNSi_G3dFuncSbc_BB (sbc.c:722-790) reads G3X_GetClipMtx and emits MTX_MODE 0, MTX_PUSH, MTX_IDENTITY, MTX_POP 1, MTX_MODE 2,
    MTX_LOAD_4x3, MTX_SCALE, MTX_SCALE. -> list of dicts (frame, trans) one per such sequence in the window; trans = the 4x3's translation words (a billboard placed at the
    camera-space origin has (0, 0, 0): the identity CLIPMTX readback of the port, review 2.2 O1)."""
    cmds = []   # [(frame, cmd, [args])]
    cur_frame = [None]

    def on_cmd(c, arg):
        if arg is None:
            cmds.append((cur_frame[0], c, []))
        elif cmds:
            cmds[-1][2].append(arg[1])

    dec = Decoder(on_cmd)
    for fr in frames:
        cur_frame[0] = fr["frame"]
        for w, a, p, z in entries(fr):
            if p == 3 and a >= 0x440:
                dec.port(a, w)
            elif a == 0 or (p == 3 and 0x400 <= a < 0x440):
                dec.fifo(w)
            elif 0x440 <= a < 0x600:
                dec.port(a, w)
            else:
                dec.fifo(w)
    out = []
    for i in range(len(cmds) - 7):
        seq = cmds[i:i + 8]
        if [c[1] for c in seq] == [0x10, 0x11, 0x15, 0x12, 0x10, 0x17, 0x1B, 0x1B] and seq[0][2][:1] == [0] and seq[4][2][:1] == [2] and len(seq[5][2]) == 12:
            out.append(dict(frame=seq[0][0], trans=tuple(w if w < 0x80000000 else w - (1 << 32) for w in seq[5][2][9:12])))
    return out


def matrix_rule(frames):
    """-> (ok, lines, info). See the module docstring. The matrix STACKS are state outside the trace too, so besides the rule this reports the stack depth the window needs
    at its start (a leading MTX_POP is legal: it is balanced by a PUSH of the previous frame, before the window) and the net depth change over the window."""
    st = dict(mode=None, loaded={0: False, 1: False, 2: False}, viol=[], first_vtx=None, tex_ops=0, cmds={}, depth=[0, 0], min_depth=[0, 0], pops_pending=False,
              first_vtx_frame=None)
    cur_frame = [None]

    def grp(m):
        return 0 if m == 0 else 1   # projection stack / position-vector stack (modes 1 and 2 share it)

    def on_cmd(c, arg):
        m = st["mode"]
        if arg is not None:
            if c == 0x10 and arg[0] == 0:
                st["mode"] = arg[1] & 3
            elif c == 0x12 and arg[0] == 0 and m in (0, 1, 2):   # MTX_POP: a signed 6-bit count of pops
                n = arg[1] & 0x3f
                n = n - 64 if n & 0x20 else n
                g = grp(m)
                st["depth"][g] -= n
                st["min_depth"][g] = min(st["min_depth"][g], st["depth"][g])
            return
        st["cmds"][c] = st["cmds"].get(c, 0) + 1
        if c == 0x11 and m in (0, 1, 2):
            st["depth"][grp(m)] += 1
        if c == 0x40 and st["first_vtx"] is None:
            st["first_vtx"] = dict(st["loaded"])
            st["first_vtx_frame"] = cur_frame[0]
            return
        if st["first_vtx"] is not None:
            return   # the rule is about the state at the FIRST vertex; later commands are the replayed stream's own business
        if c in LOADS:
            if m == 0:
                st["loaded"][0] = True
            elif m == 1:
                st["loaded"][1] = True
            elif m == 2:
                st["loaded"][1] = st["loaded"][2] = True
        elif c in USES and c not in (0x12, 0x14):   # POP and RESTORE take the current matrix FROM the stack: they depend on stack state, not on a matrix value
            if m == 3:
                st["tex_ops"] += 1
            elif m in (0, 1, 2) and not st["loaded"][m]:
                st["viol"].append("%s in MTX_MODE %s before that matrix was loaded in the window" % (NAMES.get(c, hex(c)), m))
            elif m is None:
                st["viol"].append("%s before any MTX_MODE in the window" % NAMES.get(c, hex(c)))

    dec = Decoder(on_cmd)
    for fr in frames:
        cur_frame[0] = fr["frame"]
        for w, a, p, z in entries(fr):
            if p == 3 and a >= 0x440:
                dec.port(a, w)
            elif a == 0 or (p == 3 and 0x400 <= a < 0x440):
                dec.fifo(w)
            elif 0x440 <= a < 0x600:
                dec.port(a, w)   # CPUSEND / SENDFIFO word aimed at a command port
            else:
                dec.fifo(w)
    info = dict(first_vtx=st["first_vtx"], violations=st["viol"], tex_ops=st["tex_ops"], unknown=dec.unknown, cmds=st["cmds"], need_depth=[-d for d in st["min_depth"]],
                net_depth=list(st["depth"]), first_vtx_frame=st["first_vtx_frame"])
    lines = []
    if st["first_vtx"] is None:
        return False, ["matrix-reload rule: NO BEGIN_VTXS in the window -- there is no geometry to check; open the window on a frame that draws"], info
    L = st["first_vtx"]
    ok = L[0] and L[1] and L[2] and not st["viol"]
    lines.append("matrix-reload rule: before the first BEGIN_VTXS (frame %d) projection loaded=%s position loaded=%s vector loaded=%s; %d use-before-load violation(s); "
                 "texture-matrix ops before it: %d (mode 3 is not covered by the rule)" % (st["first_vtx_frame"], L[0], L[1], L[2], len(st["viol"]), st["tex_ops"]))
    for v in st["viol"][:4]:
        lines.append("  violation: " + v)
    lines.append("matrix stacks (outside the trace): the window needs projection depth >= %d and position/vector depth >= %d at its start; net change over the window: "
                 "projection %+d, position/vector %+d" % (info["need_depth"][0], info["need_depth"][1], info["net_depth"][0], info["net_depth"][1]))
    return ok, lines, info


def check(path, hwlog="", no_geometry_ok=False):
    """-> (ok, summary lines). Used by the private emulator harness as the gxtrace judge."""
    try:
        data = open(path, "rb").read()
        hdr, frames = parse(data)
    except (OSError, FormatError) as e:
        return False, ["gxtrace.bin: " + str(e)]
    out = ["gxtrace.bin v2: %d bytes (%.2f MiB), clock=%s, seal=%s, window from frame %d (requested %d), %d frame(s) recorded" %
           (hdr["size"], hdr["size"] / 1048576.0, hdr["clock"], hdr["seal"], hdr["from_frame"], hdr["count"], len(frames))]
    ok = True
    if not frames or len(frames) > hdr["count"]:
        out.append("FAIL: %d frames for a window of %d" % (len(frames), hdr["count"]))
        ok = False
    if hdr["size"] > SIZE_CEILING:
        out.append("FAIL: file larger than the %d MiB ceiling" % (SIZE_CEILING >> 20))
        ok = False
    paths_total = [0, 0, 0, 0]
    rdbk_total = [0, 0]
    out.append("  frame vblank  words dma/cpusend/sendfifo/port     pages3d pages2d   bytes")
    prev, prev_vb = None, None
    late_seals = 0
    l2_forms, l2_could_be_uniform = [0, 0, 0], 0
    for i, fr in enumerate(frames):
        cnt = [0, 0, 0, 0]
        for w, a, p, z in entries(fr):
            if p > 3 or z != 0:
                out.append("FAIL: frame %d: bad entry (path %d pad %d)" % (fr["frame"], p, z))
                ok = False
                break
            cnt[p] += 1
        groups, gerrs, wrs = rdbk_scan(fr)
        nrd = sum(len(g[2]) + 1 for g in groups) + len(wrs)
        for e in gerrs:
            out.append("FAIL: frame %d: %s" % (fr["frame"], e))
            ok = False
        if (nrd and not hdr["rdbk"]) or fr["fend"][7] != nrd:
            out.append("FAIL: frame %d: %d RDBK entries but header flag %s / FEND word 7 = %d" % (fr["frame"], nrd, hdr["rdbk"], fr["fend"][7]))
            ok = False
        rdbk_total[0] += len(groups)
        rdbk_total[1] += nrd
        fe = fr["fend"]
        if list(fe[1:5]) != cnt or fe[0] != sum(cnt):
            out.append("FAIL: frame %d: FEND counts %s != the STRM entries %s" % (fr["frame"], list(fe[:5]), [sum(cnt)] + cnt))
            ok = False
        want_kinds = (1, 2) if hdr["flags"] & 2 else (0,)   # 1 = sealed at the snapshot point, 2 = by the next frame end (the hook was missed)
        if fe[6] not in want_kinds:
            out.append("FAIL: frame %d: seal kind %d with header seal=%s" % (fr["frame"], fe[6], hdr["seal"]))
            ok = False
        if fe[6] == 2:
            late_seals += 1
        if hdr["lines2d"]:
            for key, eng_regs, mask in (("l2a", fr["r2a"], True), ("l2b", fr["r2b"], False)):
                form, img = fr[key]
                l2_forms[form] += 1
                if (form == L2_NONE) != (fe[6] == 2):
                    out.append("FAIL: frame %d: %s form %d with seal kind %d (NONE belongs to the next-vblank seal only)" % (fr["frame"], key.upper(), form, fe[6]))
                    ok = False
                elif form != L2_NONE:
                    last = img[(L2_LINES_N - 1) * REGS2D_LEN:] if form == L2_LINES else img
                    ref = (eng_regs[:4] + bytes(4) + eng_regs[8:]) if mask else eng_regs
                    if last != ref:
                        out.append("FAIL: frame %d: %s last-line image differs from the sealed registers (DISPSTAT/VCOUNT masked)" % (fr["frame"], key.upper()))
                        ok = False
                    if mask and last[4:8] != bytes(4):
                        out.append("FAIL: frame %d: L2A DISPSTAT/VCOUNT bytes are not zero" % fr["frame"])
                        ok = False
                    if form == L2_LINES and img == img[:REGS2D_LEN] * L2_LINES_N:
                        l2_could_be_uniform += 1
        p3 = sum(len(v) for v in fr["pages3d"].values())
        p2 = sum(len(v) for v in fr["pages2d"].values())
        if fe[5] != p3 + p2:
            out.append("FAIL: frame %d: FEND pages %d != %d" % (fr["frame"], fe[5], p3 + p2))
            ok = False
        if prev is not None and fr["frame"] != prev + 1:
            out.append("FAIL: frame numbers not consecutive (%d after %d)" % (fr["frame"], prev))
            ok = False
        if prev_vb is not None and fr["vblanks"] < prev_vb:
            out.append("FAIL: DS vblank count went backwards at frame %d" % fr["frame"])
            ok = False
        if (i == 0) != bool(fr["first"]) or fr["win_index"] != i:
            out.append("FAIL: frame %d: window index / first flag wrong" % fr["frame"])
            ok = False
        prev, prev_vb = fr["frame"], fr["vblanks"]
        for k in range(4):
            paths_total[k] += cnt[k]
        out.append("  %5d %6d  %6d %6d %6d %6d   %7d %7d  %8d" % (fr["frame"], fr["vblanks"], cnt[0], cnt[1], cnt[2], cnt[3], p3, p2, fr["nbytes"]))
    if frames:
        f0 = frames[0]
        out.append("first frame state: %d 3D pages (%s), %d 2D pages (%s); VRAMCNT A..I,WRAMCNT = %s" % (
            sum(len(v) for v in f0["pages3d"].values()), ",".join("%s:%d" % (REGIONS[r][0], len(v)) for r, v in sorted(f0["pages3d"].items())),
            sum(len(v) for v in f0["pages2d"].values()), ",".join("%s:%d" % (REGIONS[r][0], len(v)) for r, v in sorted(f0["pages2d"].items())),
            f0["vcnt"][:10].hex()))
        later = frames[1:]
        if later:
            out.append("later frames: avg %.1f 3D + %.1f 2D dirty pages per frame (content changed since the previous frame)" % (
                sum(sum(len(v) for v in f["pages3d"].values()) for f in later) / len(later), sum(sum(len(v) for v in f["pages2d"].values()) for f in later) / len(later)))
    if hdr["lines2d"]:
        out.append("per-line 2D (L2A+L2B chunks): %d uniform (one image for all 192 lines), %d full (192 images; %d of them identical on every line), %d none (next-vblank seal)" % (
            l2_forms[L2_UNIFORM], l2_forms[L2_LINES], l2_could_be_uniform, l2_forms[L2_NONE]))
    if late_seals:
        out.append("note: %d frame(s) sealed by the next frame end (the game's snapshot hook was not reached in time)" % late_seals)
    if hdr["rdbk"]:
        out.append("RDBK readbacks over the window: %d groups, %d entries" % tuple(rdbk_total))
    out.append("stream entries over the window: %d (dma/cpusend/sendfifo/port = %s)" % (sum(paths_total), "/".join(map(str, paths_total))))
    if frames:
        rok, rlines, info = matrix_rule(frames)
        if not rok and no_geometry_ok and info["first_vtx"] is None:
            rok = True
        out += rlines
        ok = ok and rok
        if info["unknown"]:
            out.append("note: %d unknown command byte(s) in the stream" % info["unknown"])
    bbs = bb_sequences(frames) if frames else []
    at_eye = sum(1 for b in bbs if b["trans"] == (0, 0, 0))
    out.append("billboard sequences (sbc.c BB, the CLIPMTX-dependent branch): %d over the window, %d with translation (0,0,0) = at the eye%s" % (
        len(bbs), at_eye, "" if bbs else "; none: this stream does not depend on the CLIPMTX / VECMTX readbacks through BB"))
    if hwlog:
        m = re.search(r"gxtrace: state (\d+) \([^)]*\); (\d+) frames, (\d+) entries; appended [\d/]+; counters (\d+)/(\d+)/(\d+)/(\d+); (\d+) bytes", hwlog)
        if not m:
            out.append("FAIL: no 'gxtrace:' line in hwlog")
            ok = False
        else:
            counters = [int(m.group(i)) for i in range(4, 8)]
            if counters != paths_total or int(m.group(8)) != hdr["size"] or int(m.group(2)) != len(frames):
                out.append("FAIL: hwlog says %s words / %s frames / %s bytes, the file has %s / %d / %d" % ("/".join(map(str, counters)), m.group(2), m.group(8),
                                                                                                          "/".join(map(str, paths_total)), len(frames), hdr["size"]))
                ok = False
            else:
                out.append("hwlog agrees: counters %s, %d frames, %d bytes" % ("/".join(map(str, counters)), len(frames), hdr["size"]))
    return ok, out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    hw = open(argv[argv.index("--hwlog") + 1], errors="replace").read() if "--hwlog" in argv else ""
    ok, lines = check(argv[1], hw, "--no-geometry-ok" in argv)
    print("\n".join(lines))
    print("gxtrace.py: " + ("OK" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
