#!/usr/bin/env python3
"""Census of evaluation-order hazards (GCC vs mwcc), bet 2.6c (review 2.6 N7). Inside ONE full expression, a call F(..., E, ...) that writes field f of *E (directly or through
callees) and, unsequenced with it, either a read of E->f / E.f outside F's own argument list, or another call that reads or writes f through the same E. Plus the global variant:
two unsequenced calls that write the same file-scope variable, or a call that writes G and a read of G. The C standard leaves the order unspecified; mwcc (the DS build) and GCC
(this port) can differ, which is how ScrCmd_GoTo / ScrCmd_Call landed 4 bytes off. gcc's -Wsequence-point cannot see these (the write is inside a call).

  evalorder.py --check [--allow tools/evalorder-allow.tsv]   scan the 1263 C files `make port` compiles (tools/port_files.py: game glib ns sdk snd7 extra) plus every function-like
                                                              macro of the port; exit 1 on any hit that is not on the allow-list (a new hazard, or a line that moved: re-triage)
  evalorder.py --write-allow FILE                             write the current hits as an allow-list (only after triaging every one as harmless; see the private design notes)
  evalorder.py ROOT FILELIST OUT.tsv                          the raw scan over "<kind> <path>" lines (the review's form)
Read-only on the sources. Allow-list line: path:line TAB pattern TAB callee TAB field TAB function."""
import os, re, subprocess, sys, collections

TOK = re.compile(r"""
 (?P<ws>\s+)|
 (?P<id>[A-Za-z_]\w*)|
 (?P<num>0[xX][0-9a-fA-F]+[uUlL]*|\d+\.?\d*(?:[eE][+-]?\d+)?[uUlLfF]*)|
 (?P<str>"(?:\\.|[^"\\])*")|
 (?P<chr>'(?:\\.|[^'\\])*')|
 (?P<op><<=|>>=|\.\.\.|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||\+=|-=|\*=|/=|%=|&=|\^=|\|=|[-+*/%&|^~!<>=?:;,.(){}\[\]#])
""", re.X)
ASSIGN = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "^=", "|=", "<<=", ">>="}
KEYW = {"if", "while", "for", "switch", "return", "sizeof", "do", "else", "case", "default", "goto", "break", "continue", "struct", "union", "enum",
        "typedef", "static", "const", "volatile", "extern", "inline", "register", "unsigned", "signed", "int", "char", "short", "long", "void", "float",
        "double", "_Bool", "__attribute__", "asm", "__asm__"}


def strip(src):
    src = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    out = []
    cont = False
    for ln in src.split("\n"):
        s = ln.lstrip()
        if cont or s.startswith("#"):
            cont = ln.rstrip().endswith("\\")
            out.append("")
        else:
            out.append(ln)
    return "\n".join(out)


def tokenize(src):
    toks = []
    line = 1
    pos = 0
    while pos < len(src):
        m = TOK.match(src, pos)
        if not m:
            pos += 1
            continue
        k = m.lastgroup
        t = m.group(0)
        if k != "ws":
            toks.append((t, line, k))
        line += t.count("\n")
        pos = m.end()
    return toks


def match_fwd(toks, i):
    o, c = toks[i][0], {"(": ")", "[": "]", "{": "}"}[toks[i][0]]
    d = 0
    for j in range(i, len(toks)):
        if toks[j][0] == o:
            d += 1
        elif toks[j][0] == c:
            d -= 1
            if d == 0:
                return j
    return len(toks) - 1


def match_back(toks, j):
    c, o = toks[j][0], {")": "(", "]": "["}[toks[j][0]]
    d = 0
    for i in range(j, -1, -1):
        if toks[i][0] == c:
            d += 1
        elif toks[i][0] == o:
            d -= 1
            if d == 0:
                return i
    return 0


class Func:
    def __init__(s, name, params, body, path):
        s.name, s.params, s.body, s.path = name, params, body, path
        s.w = collections.defaultdict(set)   # param index -> fields written through it ('*' = whole object)
        s.r = collections.defaultdict(set)   # param index -> fields read through it
        s.gw = set()                         # globals written
        s.gr = set()


def parse_params(ptoks):
    out, cur, d = [], [], 0
    for t in ptoks:
        if t[0] in "([":
            d += 1
        elif t[0] in ")]":
            d -= 1
        if t[0] == "," and d == 0:
            out.append(cur)
            cur = []
        else:
            cur.append(t)
    if cur:
        out.append(cur)
    res = []
    for p in out:
        ids = [t[0] for t in p if t[2] == "id" and t[0] not in KEYW]
        name = ids[-1] if len(ids) >= 2 else None
        ptr = any(t[0] == "*" for t in p) or any(t[0] == "[" for t in p)
        res.append((name, ptr))
    return res


def find_funcs(toks, path):
    funcs, globs = [], set()
    i, d = 0, 0
    stmt_start = 0
    while i < len(toks):
        t = toks[i][0]
        if t == "{" and d == 0:
            j = i - 1
            if toks[j][0] == ")":
                k = match_back(toks, j)
                if k > 0 and toks[k - 1][2] == "id" and toks[k - 1][0] not in KEYW:
                    e = match_fwd(toks, i)
                    funcs.append(Func(toks[k - 1][0], parse_params(toks[k + 1:j]), (i + 1, e), path))
                    i = e + 1
                    stmt_start = i
                    continue
            e = match_fwd(toks, i)   # struct / initializer at file scope
            i = e + 1
            continue
        if t == ";" and d == 0:
            seg = toks[stmt_start:i]
            # a file-scope variable: identifier before '=' / '[' / ';' and not a prototype
            if seg and not any(x[0] == "typedef" for x in seg) and not any(x[0] == "extern" for x in seg[:1]):
                for q in range(len(seg)):
                    if seg[q][2] == "id" and seg[q][0] not in KEYW and q + 1 <= len(seg) and (q + 1 == len(seg) or seg[q + 1][0] in ("=", "[", ",", ";")):
                        if not (q + 1 < len(seg) and seg[q + 1][0] == "("):
                            globs.add(seg[q][0])
                            break
            stmt_start = i + 1
        if t in "([":
            d += 1
        elif t in ")]":
            d -= 1
        i += 1
    return funcs, globs


def chain_end(toks, i, end):
    """i points at the token after the base (-> or . or [); return index past the postfix chain and the first field name"""
    field = None
    while i < end:
        t = toks[i][0]
        if t in ("->", ".") and i + 1 < end and toks[i + 1][2] == "id":
            if field is None:
                field = toks[i + 1][0]
            i += 2
        elif t == "[":
            i = match_fwd(toks, i) + 1
            if field is None:
                field = "[]"
        else:
            break
    return i, field


def calls_in(toks, a, b):
    """(callee name, open-paren index, close index, [arg token ranges]) for direct calls NAME( ... ) in [a, b)"""
    out = []
    for i in range(a, b - 1):
        if toks[i][2] == "id" and toks[i][0] not in KEYW and toks[i + 1][0] == "(" and (i == 0 or toks[i - 1][0] not in ("->", ".")):
            c = match_fwd(toks, i + 1)
            args, s, d = [], i + 2, 0
            for q in range(i + 2, c):
                x = toks[q][0]
                if x in "([{":
                    d += 1
                elif x in ")]}":
                    d -= 1
                elif x == "," and d == 0:
                    args.append((s, q))
                    s = q + 1
            if s < c:
                args.append((s, c))
            out.append((toks[i][0], i + 1, c, args))
    return out


def arg_text(toks, r):
    return " ".join(t[0] for t in toks[r[0]:r[1]])


def base_of(toks, r):
    """an argument that is a plain identifier `p` -> 'p'; '&x' -> '&x'; else None"""
    s = [t[0] for t in toks[r[0]:r[1]]]
    if len(s) == 1 and re.match(r"[A-Za-z_]\w*$", s[0]):
        return s[0]
    if len(s) == 2 and s[0] == "&" and re.match(r"[A-Za-z_]\w*$", s[1]):
        return s[1]   # &obj: the callee's p->f is obj.f
    return None


def local_names(f, toks):
    a, b = f.body
    names = {p[0] for p in f.params if p[0]}
    for i in range(a + 1, b - 1):
        if toks[i][2] == "id" and (toks[i - 1][2] == "id" and toks[i - 1][0] not in ("return", "case", "goto", "sizeof", "else", "do") or toks[i - 1][0] == "*" and i >= a + 2 and toks[i - 2][2] == "id" and toks[i - 2][0] not in ("return", "case")) \
                and toks[i + 1][0] in ("=", ";", ",", "[", ")"):
            names.add(toks[i][0])
    return names


def local_effects(f, toks, globs):
    a, b = f.body
    pidx = {p[0]: n for n, p in enumerate(f.params) if p[0] and p[1]}
    if getattr(f, "is_macro", False):
        globs = set()
    else:
        globs = globs - local_names(f, toks)
    for i in range(a, b):
        t, _, k = toks[i]
        if k != "id":
            continue
        prev = toks[i - 1][0] if i > a else ""
        if prev in ("->", "."):
            continue
        if t in pidx and i + 1 < b and toks[i + 1][0] in ("->", "["):
            e, field = chain_end(toks, i + 1, b)
            nxt = toks[e][0] if e < b else ""
            if nxt in ASSIGN or nxt in ("++", "--") or prev in ("++", "--"):
                f.w[pidx[t]].add(field)
                if nxt in ASSIGN and nxt != "=":
                    f.r[pidx[t]].add(field)
            else:
                f.r[pidx[t]].add(field)
        elif t in pidx and prev == "*" and i + 1 < b:
            nxt = toks[i + 1][0]
            if nxt in ASSIGN or (i - 2 >= a and toks[i - 2][0] in ("++", "--")):
                f.w[pidx[t]].add("*")
            else:
                f.r[pidx[t]].add("*")
        elif t in globs and t not in pidx:
            e, field = chain_end(toks, i + 1, b)
            nxt = toks[e][0] if e < b else ""
            if nxt in ASSIGN or nxt in ("++", "--") or prev in ("++", "--"):
                f.gw.add(t)
            else:
                f.gr.add(t)


def propagate(funcs_by_name, toks_of):
    changed = True
    rounds = 0
    while changed and rounds < 30:
        changed = False
        rounds += 1
        for fl in funcs_by_name.values():
            for f in fl:
                toks = toks_of[f.path]
                a, b = f.body
                pidx = {p[0]: n for n, p in enumerate(f.params) if p[0] and p[1]}
                for name, o, c, args in calls_in(toks, a, b):
                    for g in funcs_by_name.get(name, []):
                        before = (sum(len(v) for v in f.w.values()), sum(len(v) for v in f.r.values()), len(f.gw), len(f.gr))
                        for j, r in enumerate(args):
                            bse = base_of(toks, r)
                            if bse in pidx:
                                f.w[pidx[bse]] |= g.w.get(j, set())
                                f.r[pidx[bse]] |= g.r.get(j, set())
                        f.gw |= g.gw
                        f.gr |= g.gr
                        after = (sum(len(v) for v in f.w.values()), sum(len(v) for v in f.r.values()), len(f.gw), len(f.gr))
                        if after != before:
                            changed = True
    return rounds


def expressions(toks, a, b):
    """yield (start, end) token ranges of full expressions in a function body"""
    i = a
    s = a
    while i < b:
        t = toks[i][0]
        if t in ("if", "while", "switch") and i + 1 < b and toks[i + 1][0] == "(":
            c = match_fwd(toks, i + 1)
            yield (i + 2, c)
            i = c + 1
            s = i
            continue
        if t == "for" and i + 1 < b and toks[i + 1][0] == "(":
            c = match_fwd(toks, i + 1)
            q0 = i + 2
            d = 0
            for q in range(i + 2, c):
                x = toks[q][0]
                if x in "([":
                    d += 1
                elif x in ")]":
                    d -= 1
                elif x == ";" and d == 0:
                    yield (q0, q)
                    q0 = q + 1
            yield (q0, c)
            i = c + 1
            s = i
            continue
        if t == "{":
            if i > a and toks[i - 1][0] in ("=", ","):   # initializer list: part of the expression
                i = match_fwd(toks, i) + 1
                continue
            s = i + 1
        elif t == "}":
            s = i + 1
        elif t == ";":
            yield (s, i)
            s = i + 1
        elif t in ("else", "do") or (t == ":" and i > s and toks[s][0] in ("case", "default")):
            s = i + 1
        elif t == "(":
            i = match_fwd(toks, i) + 1
            continue
        i += 1


def path_to(toks, a, b, i):
    """the chain of enclosing groups of token i inside [a,b): list of (open index, kind) where kind is 'call' or 'paren' or 'brace' or 'br'"""
    stack = []
    for q in range(a, i):
        x = toks[q][0]
        if x in "([{":
            kind = "paren"
            if x == "(" and q > a and (toks[q - 1][2] == "id" and toks[q - 1][0] not in KEYW or toks[q - 1][0] in (")", "]")):
                kind = "call"
            elif x == "{":
                kind = "brace"
            elif x == "[":
                kind = "br"
            stack.append((q, kind))
        elif x in ")]}":
            if stack:
                stack.pop()
    return stack


def sequenced(toks, a, b, i, j):
    """True when token positions i < j are separated by a sequence point (&&, ||, ?, :, comma operator) in their innermost common group"""
    pi, pj = path_to(toks, a, b, i), path_to(toks, a, b, j)
    n = 0
    while n < len(pi) and n < len(pj) and pi[n] == pj[n]:
        n += 1
    common = pi[:n]
    depth = len(common)
    kind = common[-1][1] if common else "top"
    d = 0
    lo = common[-1][0] + 1 if common else a
    for q in range(lo, b):
        x = toks[q][0]
        if q >= j:
            break
        if x in "([{":
            d += 1
            continue
        if x in ")]}":
            d -= 1
            continue
        if d == 0 and q > i - 0 and q < j:
            if x in ("&&", "||", "?", ":"):
                return True
            if x == "," and kind in ("paren", "top"):
                return True
    return False


def collect_macros(root, paths):
    import glob, os
    out = []
    for pat in paths:
        for fn in glob.glob(os.path.join(root, pat), recursive=True):
            try:
                src = open(fn, errors="replace").read()
            except OSError:
                continue
            src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
            src = re.sub(r"\\\n", " ", src)
            for m in re.finditer(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)\(([^)]*)\)(.*)$", src, re.M):
                params = [x.strip() for x in m.group(2).split(",") if x.strip()]
                body = re.sub(r"//.*", "", m.group(3))
                out.append((m.group(1), params, body, fn))
    return out


def scan(root, flist, out):
    toks_of, funcs_by_name, globs = {}, collections.defaultdict(list), set()
    files = [l.split()[1] for l in open(flist) if l.strip()]
    allf = []
    for p in files:
        try:
            src = open(root + "/" + p, errors="replace").read()
        except OSError:
            continue
        toks = tokenize(strip(src))
        toks_of[p] = toks
        fs, gs = find_funcs(toks, p)
        globs |= gs
        allf += fs
    for (mname, mparams, mbody, mfn) in collect_macros(root, ["port/include/**/*.h", "port/subprojects/*/include/**/*.h", "port/lib/**/*.h", "port/src/**/*.c", "port/src/**/*.h", "port/subprojects/*/libraries/**/*.[ch]"]):
        key = "macro:%d:" % len(toks_of) + mname + ":" + mfn
        mt = tokenize(mbody)
        toks_of[key] = mt
        f = Func(mname, [(x, True) for x in mparams], (0, len(mt)), key)
        f.is_macro = True
        allf.append(f)
    for f in allf:
        funcs_by_name[f.name].append(f)
    for f in allf:
        local_effects(f, toks_of[f.path], globs)
    for f in allf:
        f.gw1 = set(f.gw)
    rounds = propagate(funcs_by_name, toks_of)
    for f in allf:   # global writes for hits: the function's own plus its direct callees' own (depth 2), not the transitive closure
        toks = toks_of[f.path]
        g2 = set(f.gw1)
        for name, o, c, args in calls_in(toks, f.body[0], f.body[1]):
            for g in funcs_by_name.get(name, []):
                g2 |= g.gw1
        f.gwh = g2 | {x for x in f.gw if re.search(r"RNG|Rand|rand|Seed|seed|Random|random", x)}
    class M:
        pass
    merged = {}
    for name, fl in funcs_by_name.items():
        m = M()
        m.w, m.r, m.gwh = collections.defaultdict(set), collections.defaultdict(set), set()
        for g in fl:
            for k, v in g.w.items():
                m.w[k] |= v
            for k, v in g.r.items():
                m.r[k] |= v
            m.gwh |= g.gwh
        merged[name] = [m]
    funcs_by_name = merged
    hits = []
    for f in allf:
        if getattr(f, "is_macro", False):
            continue
        toks = toks_of[f.path]
        a, b = f.body
        lnames = local_names(f, toks)
        for (s, e) in expressions(toks, a, b):
            cs = calls_in(toks, s, e)
            if not cs:
                continue
            # events: (pos, kind, base, fieldset, is_write, span)
            for (name, o, c, args) in cs:
                for g in funcs_by_name.get(name, []):
                    for jn, r in enumerate(args):
                        bse = base_of(toks, r)
                        if not bse or not g.w.get(jn):
                            continue
                        wf = g.w[jn]
                        # (1) reads of bse->field outside this call
                        for q in range(s, e - 1):
                            if o <= q <= c:
                                continue
                            if toks[q][0] == bse and toks[q + 1][0] in ("->", ".") and (q == s or toks[q - 1][0] not in ("->", ".")):
                                en, field = chain_end(toks, q + 1, e)
                                nxt = toks[en][0] if en < e else ""
                                if nxt == "=" and not (q > s and toks[q - 1][0] in ("++", "--")):
                                    # plain store target: lvalue, no read of the field (but a store of a field the call writes is still unsequenced)
                                    if field in wf or "*" in wf:
                                        if not sequenced(toks, s, e, min(q, o), max(q, o)):
                                            hits.append((f.path, toks[q][1], f.name, "store-vs-call", name, bse, field, " ".join(t[0] for t in toks[s:e])))
                                    continue
                                if (field in wf or "*" in wf) and not sequenced(toks, s, e, min(q, o), max(q, o)):
                                    hits.append((f.path, toks[q][1], f.name, "read-vs-call", name, bse, field, " ".join(t[0] for t in toks[s:e])))
                        # (2) another call touching the same fields through the same base
                        for (n2, o2, c2, a2) in cs:
                            if o2 == o or (o2 > o and c2 <= c) or (o > o2 and c <= c2):
                                continue
                            if o2 < o:
                                continue   # each unordered pair once (the later call reports)
                            for h in funcs_by_name.get(n2, []):
                                for j2, r2 in enumerate(a2):
                                    if base_of(toks, r2) != bse:
                                        continue
                                    touched = h.w.get(j2, set()) | h.r.get(j2, set())
                                    if (touched & wf or "*" in wf or "*" in touched) and not sequenced(toks, s, e, o, o2):
                                        hits.append((f.path, toks[o][1], f.name, "call-vs-call", name + "/" + n2, bse, ",".join(sorted(touched & wf) or ["*"]), " ".join(t[0] for t in toks[s:e])))
                                break
                    break
            # (3) globals: two calls writing the same global, or a call writing G and a read of G
            for (name, o, c, args) in cs:
                g = funcs_by_name.get(name)
                if not g or not g[0].gwh:
                    continue
                for (n2, o2, c2, a2) in cs:
                    if o2 <= o or (o2 > o and c2 <= c):
                        continue
                    h = funcs_by_name.get(n2)
                    if not h:
                        continue
                    both = g[0].gwh & h[0].gwh
                    if both and not sequenced(toks, s, e, o, o2):
                        hits.append((f.path, toks[o][1], f.name, "global-call-vs-call", name + "/" + n2, "", ",".join(sorted(both)[:4]), " ".join(t[0] for t in toks[s:e])))
                for q in range(s, e):
                    if o <= q <= c:
                        continue
                    if toks[q][0] in g[0].gwh and toks[q][0] not in lnames and (q == s or toks[q - 1][0] not in ("->", ".")) and not (q + 1 < e and toks[q + 1][0] == "("):
                        nxt = toks[q + 1][0] if q + 1 < e else ""
                        if nxt == "=":
                            continue
                        if not sequenced(toks, s, e, min(q, o), max(q, o)):
                            hits.append((f.path, toks[q][1], f.name, "global-read-vs-call", name, "", toks[q][0], " ".join(t[0] for t in toks[s:e])))
    seen = set()
    with open(out, "w") as fo:
        for h in hits:
            key = (h[0], h[1], h[3], h[4], h[6])
            if key in seen:
                continue
            seen.add(key)
            fo.write("\t".join(str(x) for x in h) + "\n")
    print("files %d, functions %d, propagation rounds %d, hits %d" % (len(toks_of), len(allf), rounds, len(seen)))


HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
KINDS = ("game", "glib", "ns", "sdk", "snd7", "extra")


def file_list(path):
    with open(path, "w") as f:
        for k in KINDS:
            r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "port_files.py"), k], capture_output=True, text=True, cwd=PROJ, check=True)
            for ln in r.stdout.split():
                f.write("%s %s\n" % (k, ln))


def hit_key(line):
    c = line.rstrip("\n").split("\t")
    # raw hit: path, line, function, pattern, callee, base, field, expression
    return ("%s:%s" % (c[0], c[1]), c[3], c[4], c[6])


def scan_all(tmpdir):
    os.makedirs(tmpdir, exist_ok=True)
    fl, out = os.path.join(tmpdir, "evalorder-files.txt"), os.path.join(tmpdir, "evalorder-hits.tsv")
    file_list(fl)
    scan(PROJ, fl, out)
    return [l for l in open(out) if l.strip()]


def main(argv):
    if len(argv) == 4 and not argv[1].startswith("--"):
        scan(argv[1], argv[2], argv[3])
        return 0
    tmp = os.path.join(PROJ, "build", "evalorder")
    allow_path = argv[argv.index("--allow") + 1] if "--allow" in argv else os.path.join(HERE, "evalorder-allow.tsv")
    if not os.path.isdir(os.path.join(PROJ, "port", "src")):
        print("evalorder: SKIPPED (no port/src: run tools/sync_port.sh)")
        return 0
    hits = scan_all(tmp)
    if "--write-allow" in argv:
        with open(argv[argv.index("--write-allow") + 1], "w") as f:
            f.write("# evalorder allow-list (bet 2.6c): every hit of tools/evalorder.py that was triaged as HARMLESS. key = file:line, pattern, callee, field; then the function.\n")
            f.write("# store-vs-call: `E->f = F(E, ..)`: the store is sequenced after the right operand (C11 6.5.16). call-vs-call on Pokemon_GetValue / GameRecords_GetRecordValue: the\n")
            f.write("# decrypt/re-encrypt round trip is undone before each call returns, the results do not depend on the order. global-read-vs-call: `rtcWork.commonResult = RTC_*Async()`.\n")
            for l in sorted(hits):
                c = l.rstrip("\n").split("\t")
                f.write("%s:%s\t%s\t%s\t%s\t%s\n" % (c[0], c[1], c[3], c[4], c[6], c[2]))
        print("evalorder: wrote %d allow-list entries" % len(hits))
        return 0
    allow = set()
    for l in open(allow_path):
        if l.strip() and not l.startswith("#"):
            c = l.rstrip("\n").split("\t")
            allow.add((c[0], c[1], c[2], c[3]))
    seen = {hit_key(l) for l in hits}
    new = [l for l in hits if hit_key(l) not in allow]
    stale = sorted(allow - seen)
    print("evalorder: %d hits, %d on the allow-list (%s), %d NOT allowed, %d stale allow-list entries" % (len(hits), len(hits) - len(new), os.path.relpath(allow_path, PROJ), len(new), len(stale)))
    for l in new:
        c = l.rstrip("\n").split("\t")
        print("evalorder: NEW HAZARD %s:%s in %s [%s] %s (%s): %s" % (c[0], c[1], c[2], c[3], c[4], c[6], c[7][:200]))
    for s in stale[:10]:
        print("evalorder: stale allow-list entry (the hit is gone or its line moved): %s %s %s" % (s[0], s[1], s[2]))
    return 1 if new else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
