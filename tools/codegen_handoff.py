"""Hands the localized registers across calls in the generated code.

With non_argument_as_local, RexGlue 0.10.0 turns r0, r2, r11, r12, f0 and v32-v63 into locals of
every function, which assumes no value in them ever crosses a call. The game breaks that in a few
places: the save/restore helpers and __chkstk take r12 from their caller, a few functions are
entered with r11 set or return in it. The generator writes nothing for those, so the callee reads a
zeroed local. non_volatile_as_local does the same to r14-r31, f14-f31, v14-v31 and v64-v127, which
the Xbox compiler's whole-program allocation also passes across calls; the vector ones go back to
ctx (see vectors_back_to_ctx), the others get the hand-offs below.

This pass works out, from the generated code itself, which of those registers each function reads
before writing (live in) and which a caller reads after a call before writing (live out). It is a
control-flow liveness per function, iterated over the call graph to a fixpoint. Then it writes
`ctx.X = X;` before a call and `X = ctx.X;` after it, `X = ctx.X;` at the entry of a function that
reads X live in, and `ctx.X = X;` before every return of a function whose X is live out. ctx keeps
the original register file semantics for exactly those values.

    python tools/codegen_handoff.py generated/android [--report]
"""
import re
import sys
from pathlib import Path

REGS = ["r0", "r2", "r11", "r12", "f0"] + ["v%d" % i for i in range(32, 64)]
# non_volatile_as_local: r14-r31, f14-f31, v14-v31 and v64-v127.
NONVOL_REGS = (["r%d" % i for i in range(14, 32)] + ["f%d" % i for i in range(14, 32)] +
               ["v%d" % i for i in range(14, 32)] + ["v%d" % i for i in range(64, 128)])
REGS += NONVOL_REGS
BIT = {name: 1 << i for i, name in enumerate(REGS)}
ALL = (1 << len(REGS)) - 1
NONVOL = 0
for _name in NONVOL_REGS:
    NONVOL |= BIT[_name]
NONVOL_VECTORS = 0
for _name in NONVOL_REGS:
    if _name[0] == "v":
        NONVOL_VECTORS |= BIT[_name]
# General registers that may pass through a caller to its callee unchanged (r11 into a few
# functions, r12 into the save/restore helpers). f0 and v32-v63 are only read on entry as garbage (a
# stvx of a register never set), so they are taken from ctx in place, not chased. Nor are the
# non-volatiles: by the ABI a function never needs its caller's, it only saves them; what reads one
# on entry is a body the generator split off its owner, which gets it from the caller directly.
PASS_THROUGH = ((1 << 4) - 1) | (NONVOL & ~NONVOL_VECTORS)
# What a caller writes into ctx before a call. Not the non-volatile vector registers: their entry
# reads are vrlimi128-style lane merges into a register about to be overwritten, thousands of them.
HANDOFF = ALL
REG_ALT = ("r0|r2|r11|r12|f0|v3[2-9]|v[45][0-9]|v6[0-3]|"
           "r1[4-9]|r2[0-9]|r3[01]|f1[4-9]|f2[0-9]|f3[01]|v1[4-9]|v2[0-9]|v3[01]|v6[4-9]|v[7-9][0-9]|v1[01][0-9]|v12[0-7]")
# A local is used with a field (r11.u32, v40.u8) or whole (v127 = vTemp; vTemp = v127;).
LOCAL_AHEAD = r"(?=\.|\s*=(?!=)|\s*[;,)])"
USE_RE = re.compile(r"(?<![\w.])(%s)" % REG_ALT + LOCAL_AHEAD)
DEF_RE = re.compile(r"^(%s)(?:\.\w+)?\s*=(?!=)" % REG_ALT)
PARTIAL_DEF_RE = re.compile(r"^(%s)\.\w+\[[^\]]*\]\s*=(?!=)" % REG_ALT)
STORE_DEF_RE = re.compile(r"^simde_mm_store_\w+\(\(?[\w:* ]*\)?(%s)\.\w+," % REG_ALT)
DECL_RE = re.compile(r"^\t(PPC\w+|uint32_t) (\w+)\{\};$")
FUNC_RE = re.compile(r"^DEFINE_REX_FUNC\((\w+)\) \{$")
CALL_RE = re.compile(r"^((?:sub|__)\w+)\(ctx, base\);$")
LABEL_RE = re.compile(r"^(loc_[0-9A-F]+):$")
GOTO_RE = re.compile(r"^goto (loc_[0-9A-F]+);$")
IF_GOTO_RE = re.compile(r"^if \(.*\) goto (loc_[0-9A-F]+);$")
IF_RETURN_RE = re.compile(r"^if \(.*\) return;$")
# `mflr r12; stw r12,-8(r1)` (and the same store inside __savegprlr_N).
LR_SAVE = "REX_STORE_U32(ctx.r1.u32 + -8, r12.u32);"
# A prologue saving a non-volatile on the stack (std r31,-8(r1); stfd f31,-16(r1)): it stores the
# caller's value only to put it back in the epilogue, which a local makes pointless, so it is no read.
# stvx128 v127,r1,r12 with r12 = -224, before the stack pointer moves: a vector save.
VECTOR_SAVE_RE = re.compile(r"^simde_mm_store_si128\(\(simde__m128i\*\)REX_RAW_ADDR\(ea\), "
                            r"simde_mm_shuffle_epi8\(simde_mm_load_si128\(\(simde__m128i\*\)(%s)\.u8\), "
                            r"simde_mm_load_si128\(\(simde__m128i\*\)VectorMaskL\)\)\);$" % REG_ALT)
NONVOL_SAVE_RE = re.compile(r"^REX_STORE_U(?:32|64)\(ctx\.r1\.u32 \+ -?\d+, (%s)\.u(?:32|64)\);$" % REG_ALT)
GENERATOR_SYNC_RE = re.compile(r"^ctx\.(\w+) = \1;$")
NOT_POINTER_TARGETS = {"appMainBoot"}
for _key in ("setjmp_address", "longjmp_address"):
    _m = re.search(r"(?m)^%s\s*=\s*0x([0-9A-Fa-f]{8})" % _key,
                   (Path(__file__).resolve().parent.parent / "config" / "renut_funcs.toml").read_text(encoding="utf-8"))
    if _m:
        NOT_POINTER_TARGETS.add("sub_%s" % _m.group(1).upper())
SETJMP_SAVE = "env = ctx;"
SETJMP_RESTORE = "if (temp.s64 != 0) ctx = env;"


def mask_of(names):
    m = 0
    for n in names:
        m |= BIT[n]
    return m


def names_of(mask):
    return [n for n in REGS if mask & BIT[n]]


class Function:
    __slots__ = ("name", "path", "start", "end", "stmts", "succ", "kinds", "use", "defs", "callee", "saved",
                 "declared", "body_defs", "live_in", "live_out", "after_call", "decl_end", "direct_callers")

    def __init__(self, name, path, start):
        self.name, self.path, self.start = name, path, start
        self.live_in = 0
        self.live_out = 0
        self.direct_callers = 0


def parse(lines, path, funcs):
    i = 0
    n = len(lines)
    while i < n:
        m = FUNC_RE.match(lines[i])
        if not m:
            i += 1
            continue
        fn = Function(m.group(1), path, i)
        j = i + 1
        while j < n and lines[j] != "}":
            j += 1
        fn.end = j
        build(fn, lines)
        funcs[fn.name] = fn
        i = j + 1


# Guest address of each save helper -> the non-volatiles it saves (filled by main).
SAVES = {}


def save_masks(init_text):
    for a, name in HELPER_ENTRY_RE.findall(init_text):
        m = re.match(r"__save(gprlr|fpr|vmx)_(\d+)$", name)
        if not m:
            continue
        kind, n = m.group(1), int(m.group(2))
        if kind == "gprlr":
            regs = ["r%d" % i for i in range(n, 32)]
        elif kind == "fpr":
            regs = ["f%d" % i for i in range(n, 32)]
        else:
            regs = ["v%d" % i for i in range(n, 32 if n < 32 else 128)]
        SAVES[int(a, 16)] = mask_of(regs)


def build(fn, lines):
    # Statements: (line index, stripped text) without comments and blank lines.
    stmts = []
    declared = 0
    decl_end = fn.start + 1
    # The non-volatiles the function saves for its caller: through a save helper (its call is
    # elided but its comment stays) or a store into the frame before the stack pointer moves.
    saved = 0
    framed = False
    prologue_saves = set()
    for k in range(fn.start + 1, fn.end):
        raw = lines[k]
        s = raw.strip()
        e = ELIDED_RE.match(raw)
        if e:
            saved |= SAVES.get(int(e.group(1), 16), 0)
        if not s or s.startswith("//"):
            continue
        if s == "ctx.r1.u32 = ea;":
            framed = True
        if not framed:
            m = NONVOL_SAVE_RE.match(s) or VECTOR_SAVE_RE.match(s)
            if m:
                saved |= BIT[m.group(1)]
                prologue_saves.add(len(stmts))
        d = DECL_RE.match(raw)
        if d:
            if d.group(2) in BIT:
                declared |= BIT[d.group(2)]
            decl_end = k + 1
            continue
        if s == "REX_FUNC_PROLOGUE();":
            decl_end = max(decl_end, k + 1)
            continue
        stmts.append((k, s))
    fn.declared = declared
    fn.decl_end = decl_end
    count = len(stmts)
    labels = {}
    for idx, (_, s) in enumerate(stmts):
        m = LABEL_RE.match(s)
        if m:
            labels[m.group(1)] = idx
    # Brace matching for if/else/switch blocks.
    match = {}
    stack = []
    for idx, (_, s) in enumerate(stmts):
        if s.endswith("{"):
            stack.append(idx)
        elif s == "}":
            if stack:
                match[stack.pop()] = idx
    close_of_open = match
    open_of_close = {v: k for k, v in match.items()}
    EXIT = -1
    succ = [None] * count
    kinds = [0] * count  # 0 plain, 1 call, 2 indirect call, 3 return, 4 conditional (no kill)
    use = [0] * count
    defs = [0] * count
    callee = [None] * count
    switch_cases = {}
    for open_idx, close_idx in close_of_open.items():
        if stmts[open_idx][1].startswith("switch ("):
            switch_cases[open_idx] = [x for x in range(open_idx + 1, close_idx)
                                      if stmts[x][1].startswith("case ") or stmts[x][1] == "default:"]
    after_for = set()
    for idx, (_, s) in enumerate(stmts):
        nxt = idx + 1 if idx + 1 < count else EXIT
        uses = USE_RE.findall(s)
        u = mask_of(uses)
        d = 0
        m = DEF_RE.match(s)
        if m and not PARTIAL_DEF_RE.match(s):
            d = BIT[m.group(1)]
            # The defined register itself is not a use unless it appears again on the right.
            if uses.count(m.group(1)) == 1:
                u &= ~d
        else:
            m = STORE_DEF_RE.match(s)
            if m:
                d = BIT[m.group(1)]
                if uses.count(m.group(1)) == 1:
                    u &= ~d
        if s == LR_SAVE or GENERATOR_SYNC_RE.match(s):
            # skip_lr: mflr emits nothing, so this stores a dead link register. And the copies the
            # generator writes into ctx for an SEH funclet hand over every local the owner has,
            # set or not: the funclet reads the ones that matter, which the owner set.
            u = 0
        m = NONVOL_SAVE_RE.match(s) or (VECTOR_SAVE_RE.match(s) if idx in prologue_saves else None)
        if m and BIT[m.group(1)] & NONVOL:
            u &= ~BIT[m.group(1)]
        if s.startswith("if (") or idx in after_for:
            d = 0  # conditional: may not happen
        if s.startswith("for ("):
            after_for.add(idx + 1)
        use[idx], defs[idx] = u, d
        if GOTO_RE.match(s):
            succ[idx] = [labels[GOTO_RE.match(s).group(1)]]
        elif IF_GOTO_RE.match(s):
            succ[idx] = [labels[IF_GOTO_RE.match(s).group(1)], nxt]
        elif s == "return;":
            succ[idx] = [EXIT]
            kinds[idx] = 3
        elif IF_RETURN_RE.match(s):
            succ[idx] = [EXIT, nxt]
            kinds[idx] = 3
        elif idx in switch_cases:
            succ[idx] = switch_cases[idx]
        elif s.endswith("{") and s.startswith("if ("):
            close_idx = close_of_open[idx]
            succ[idx] = [nxt, close_idx + 1 if close_idx + 1 < count else EXIT]
        elif s == "}":
            o = open_of_close.get(idx)
            after = idx + 1
            if after < count and stmts[after][1] == "else {":
                after = close_of_open[after] + 1
            succ[idx] = [after if after < count else EXIT]
        else:
            succ[idx] = [nxt]
            c = CALL_RE.match(s)
            if c:
                kinds[idx] = 1
                callee[idx] = c.group(1)
            elif s.startswith("REX_CALL_INDIRECT_FUNC("):
                kinds[idx] = 2
    fn.stmts, fn.succ, fn.kinds, fn.use, fn.defs, fn.callee = stmts, succ, kinds, use, defs, callee
    body_defs = 0
    for x in defs:
        body_defs |= x
    for idx, s in enumerate(stmts):
        m = DEF_RE.match(s[1]) or PARTIAL_DEF_RE.match(s[1]) or STORE_DEF_RE.match(s[1])
        if m:
            body_defs |= BIT[m.group(1)]
    fn.saved = saved & NONVOL
    # What the caller sees changed: a saved register comes back as it was.
    fn.body_defs = body_defs & ~fn.saved


def liveness(fn, funcs, mode, maydef):
    """Backward liveness over statements.

    mode "in": what the function reads before writing, from its entry. A call ends a value's life
    (what is read after it is the callee's output), except that the general registers a callee
    reads on entry are read at the call too, so they pass through callers that do not set them.
    mode "out": a return reads the function's live-out set; reports what is live right after each
    call.
    """
    count = len(fn.stmts)
    live_in = [0] * count
    preds = [[] for _ in range(count)]
    for idx, ss in enumerate(fn.succ):
        for t in ss:
            if t >= 0:
                preds[t].append(idx)
    work = list(range(count - 1, -1, -1))
    queued = [True] * count
    exit_live = fn.live_out if mode == "out" else 0
    after_call = {}
    while work:
        idx = work.pop()
        queued[idx] = False
        out = 0
        for t in fn.succ[idx]:
            out |= exit_live if t < 0 else live_in[t]
        kind = fn.kinds[idx]
        if kind == 1 or kind == 2:
            after_call[idx] = out
            # A call ends the life of what the callee can write (the Xbox compiler's whole-program
            # register allocation keeps f0 or r11 live across a callee it knows leaves them alone).
            g = funcs.get(fn.callee[idx]) if kind == 1 else None
            # A non-volatile the callee saves comes back unchanged; one it changes without saving
            # (the Xbox compiler's whole-program register allocation does that) is its output.
            kill = maydef.get(g.name, ALL) if g is not None else ALL & ~NONVOL
            new = (out & ~kill) | fn.use[idx]
            if g is not None and mode == "in":
                new |= g.live_in & PASS_THROUGH
        else:
            new = (out & ~fn.defs[idx]) | fn.use[idx]
        if new != live_in[idx]:
            live_in[idx] = new
            for p in preds[idx]:
                if not queued[p]:
                    queued[p] = True
                    work.append(p)
    entry = live_in[0] if count else exit_live
    return entry, after_call


def analyze(funcs):
    # What a call to f can write, through its callees too: a register a caller reads after the call
    # is the callee's output only if the callee can write it.
    maydef = {name: f.body_defs for name, f in funcs.items()}
    for f in funcs.values():
        for c in f.callee:
            if c in funcs:
                funcs[c].direct_callers += 1
    changed = True
    while changed:
        changed = False
        for name, f in funcs.items():
            m = maydef[name]
            for c in f.callee:
                if c in maydef:
                    m |= maydef[c]
            if m != maydef[name]:
                maydef[name] = m
                changed = True
    rounds = 0
    while True:
        rounds += 1
        changed = False
        for f in funcs.values():
            entry, _ = liveness(f, funcs, "in", maydef)
            if entry != f.live_in:
                f.live_in = entry
                changed = True
        if not changed:
            break
    # Functions nobody calls directly are reached through pointers (SEH handlers take the frame in
    # r12): an indirect call hands over what the caller holds of their inputs.
    # Not pointer targets although nothing calls them: the save/restore helpers (their calls are
    # elided under non_volatile_as_local), the guest's setjmp/longjmp (replaced at their call sites)
    # and the entry point the host starts.
    indirect_in = 0
    for f in funcs.values():
        if f.direct_callers == 0 and not f.name.startswith("__") and f.name not in NOT_POINTER_TARGETS:
            indirect_in |= f.live_in & PASS_THROUGH
    indirect_after = 0
    while True:
        rounds += 1
        changed = False
        for f in funcs.values():
            _, after = liveness(f, funcs, "out", maydef)
            f.after_call = after
            for idx, live in after.items():
                if f.kinds[idx] == 1:
                    g = funcs.get(f.callee[idx])
                    if g is None:
                        continue
                    want = g.live_out | (live & maydef[g.name])
                    if want != g.live_out:
                        g.live_out = want
                        changed = True
                else:
                    indirect_after |= live
        if not changed:
            break
    return rounds, indirect_in, indirect_after, maydef


def patch(lines, fns, funcs, indirect_in, indirect_out):
    """Returns the new lines of one file."""
    inserts_before = {}
    inserts_after = {}
    replace = {}
    for f in fns:
        decl = f.declared
        if not decl:
            continue
        ent = f.live_in & decl
        if ent:
            inserts_before.setdefault(f.decl_end, []).extend("\t%s = ctx.%s;" % (r, r) for r in names_of(ent))
        wb = f.live_out & decl
        held = decl & NONVOL
        for idx, (k, s) in enumerate(f.stmts):
            kind = f.kinds[idx]
            indent = lines[k][:len(lines[k]) - len(lines[k].lstrip("\t"))]
            # setjmp snapshots ctx and a longjmp puts the snapshot back: the non-volatiles held in
            # locals go into it first and come back out of it.
            if s == SETJMP_SAVE and held:
                inserts_before.setdefault(k, []).extend("%sctx.%s = %s;" % (indent, r, r) for r in names_of(held))
            elif s == SETJMP_RESTORE and held:
                reloads = " ".join("%s = ctx.%s;" % (r, r) for r in names_of(held))
                replace[k] = "%sif (temp.s64 != 0) { ctx = env; %s }" % (indent, reloads)
            elif kind == 3 and wb:
                writes = " ".join("ctx.%s = %s;" % (r, r) for r in names_of(wb))
                if s == "return;":
                    replace[k] = "%s{ %s return; }" % (indent, writes)
                else:
                    replace[k] = lines[k].replace("return;", "{ %s return; }" % writes)
            elif kind in (1, 2):
                if kind == 1:
                    g = funcs.get(f.callee[idx])
                    gin = g.live_in if g else 0
                    gout = g.live_out if g else 0
                else:
                    gin, gout = indirect_in, indirect_out
                before = gin & decl & HANDOFF
                after = gout & decl & f.after_call.get(idx, 0)
                if before:
                    inserts_before.setdefault(k, []).extend("%sctx.%s = %s;" % (indent, r, r) for r in names_of(before))
                if after:
                    inserts_after.setdefault(k, []).extend("%s%s = ctx.%s;" % (indent, r, r) for r in names_of(after))
    if not (inserts_before or inserts_after or replace):
        return None, 0
    out = []
    edits = 0
    for k, line in enumerate(lines):
        if k in inserts_before:
            out.extend(inserts_before[k])
            edits += len(inserts_before[k])
        if k in replace:
            out.append(replace[k])
            edits += 1
        else:
            out.append(line)
        if k in inserts_after:
            out.extend(inserts_after[k])
            edits += len(inserts_after[k])
    return out, edits


HELPER_ENTRY_RE = re.compile(r"\{ 0x([0-9A-F]{8}), (__(?:save|rest)\w+) \}")
ELIDED_RE = re.compile(r"^\t// bl? 0x([0-9a-f]{8})$")
NONVOL_ALT = ("r1[4-9]|r2[0-9]|r3[01]|f1[4-9]|f2[0-9]|f3[01]|v1[4-9]|v2[0-9]|v3[01]|v6[4-9]|v[7-9][0-9]|"
              "v1[01][0-9]|v12[0-7]")
NONVOL_LOCAL_RE = re.compile(r"(?<![\w.])(%s)" % NONVOL_ALT + LOCAL_AHEAD)
NONVOL_CTX_RE = re.compile(r"ctx\.(%s)\b" % NONVOL_ALT)
NONVOL_DECL_RE = re.compile(r"^\tPPC\w*Register (%s)\{\};$" % NONVOL_ALT)
SYNC_RE = re.compile(r"^\t+(ctx\.\w+ = \w+|\w+ = ctx\.\w+);$")


NONVOL_VECTOR_ALT = "v1[4-9]|v2[0-9]|v3[01]|v6[4-9]|v[7-9][0-9]|v1[01][0-9]|v12[0-7]"
NONVOL_VECTOR_LOCAL_RE = re.compile(r"(?<![\w.])(%s)" % NONVOL_VECTOR_ALT + LOCAL_AHEAD)
NONVOL_VECTOR_DECL_RE = re.compile(r"^\tPPCVRegister (%s)\{\};$" % NONVOL_VECTOR_ALT)


def vectors_back_to_ctx(folder, texts):
    """Keeps v14-v31 and v64-v127 in ctx, as without non_volatile_as_local.

    The Xbox compiler's whole-program allocation passes values in them across thousands of calls
    (a function's first use of v77 is often a copy of it), so as locals they would need a hand-off
    at most calls. Back in ctx they cost what they cost before; r14-r31 and f14-f31, which carry the
    integer and scalar work, stay local. The vector save/restore helpers the generator dropped
    with the flag are called again.
    """
    init = Path(folder) / "renut_android_init.cpp"
    vmx_at = {int(a, 16): n for a, n in HELPER_ENTRY_RE.findall(init.read_text(encoding="utf-8", errors="replace"))
              if "vmx" in n}
    calls = 0
    for p, lines in texts.items():
        out = []
        changed = False
        used = set()
        for k, line in enumerate(lines):
            if NONVOL_VECTOR_DECL_RE.match(line):
                changed = True
                continue
            new = NONVOL_VECTOR_LOCAL_RE.sub(lambda m: "ctx." + m.group(1), line)
            if new is not line and new != line:
                changed = True
            out.append(new)
            e = ELIDED_RE.match(line)
            if e and int(e.group(1), 16) in vmx_at:
                helper = vmx_at[int(e.group(1), 16)]
                nxt = lines[k + 1].strip() if k + 1 < len(lines) else ""
                if not nxt.startswith(helper + "("):
                    out.append("\t%s(ctx, base);" % helper)
                    used.add(helper)
                    calls += 1
                    changed = True
        if used:
            # The file's header declares only what the generator called.
            first = next(i for i, l in enumerate(out) if l.startswith("DEFINE_REX_FUNC("))
            out[first:first] = ["REX_EXTERN(%s);" % h for h in sorted(used)] + [""]
        if changed:
            texts[p] = out
    return calls


def expand_shared_helpers(folder, texts):
    """Puts the save/restore helpers back, through ctx, in the functions that keep r14-r31 in ctx.

    With non_volatile_as_local the generator prints nothing for a call to __savegprlr_N and the
    like, everywhere. A function it leaves on ctx (an SEH funclet, which shares its owner's
    registers) then saves nothing and restores nothing, and its caller copies the clobbered values
    back after the call. The helpers' own statements are taken from their bodies in the generated
    code, with the localized registers turned back into ctx.
    """
    init = Path(folder) / "renut_android_init.cpp"
    helper_at = {int(a, 16): n for a, n in HELPER_ENTRY_RE.findall(init.read_text(encoding="utf-8", errors="replace"))}
    names = set(helper_at.values())
    bodies = {}
    for lines in texts.values():
        i = 0
        while i < len(lines):
            m = FUNC_RE.match(lines[i])
            if m and m.group(1) in names:
                body = []
                j = i + 1
                while lines[j] != "}":
                    s = lines[j].strip()
                    if s and not s.startswith("//") and not DECL_RE.match(lines[j]) and s not in (
                            "REX_FUNC_PROLOGUE();", "return;"):
                        # The link register is never read back (skip_lr), and r12 holds it here.
                        if not ("gprlr" in m.group(1) and re.search(r"(?<![\w.])r12\.", s)):
                            body.append(NONVOL_LOCAL_RE.sub(lambda x: "ctx." + x.group(1), s))
                    j += 1
                bodies[m.group(1)] = body
                i = j
            i += 1
    expanded = 0
    for p, lines in texts.items():
        out = []
        changed = False
        i = 0
        while i < len(lines):
            m = FUNC_RE.match(lines[i])
            if not m:
                out.append(lines[i])
                i += 1
                continue
            j = i + 1
            while lines[j] != "}":
                j += 1
            block = lines[i:j + 1]
            localized = any(NONVOL_DECL_RE.match(l) for l in block)
            shared = not localized and any(NONVOL_CTX_RE.search(l) and not SYNC_RE.match(l) for l in block
                                           if not l.lstrip().startswith("//"))
            if shared:
                has_r12 = "\tPPCRegister r12{};" in block
                for k, line in enumerate(block):
                    out.append(line)
                    e = ELIDED_RE.match(line)
                    if not e or int(e.group(1), 16) not in helper_at:
                        continue
                    helper = helper_at[int(e.group(1), 16)]
                    nxt = block[k + 1].strip() if k + 1 < len(block) else ""
                    if nxt.startswith(helper + "(") or helper not in bodies:
                        continue
                    text = " ".join(bodies[helper])
                    if not has_r12:
                        text = re.sub(r"(?<![\w.])r12\.", "ctx.r12.", text)
                    decls = [d for d in ("r0", "r11") if re.search(r"(?<![\w.])%s\." % d, text)]
                    head = "".join("PPCRegister %s{}; " % d for d in decls)
                    if re.search(r"(?<![\w.])ea\b", text):
                        head += "uint32_t ea{}; "
                    if re.search(r"(?<![\w.])temp\.", text):
                        head += "PPCRegister temp{}; "
                    out.append("\t{ %s// %s, through ctx (shared registers)" % (head, helper))
                    out.extend("\t\t" + s for s in bodies[helper])
                    out.append("\t}")
                    expanded += 1
                    changed = True
            else:
                out.extend(block)
            i = j + 1
        if changed:
            texts[p] = out
    return expanded


def main(folder, report_only=False):
    files = sorted(Path(folder).glob("*recomp*.cpp"))
    texts = {}
    funcs = {}
    by_file = {}
    save_masks((Path(folder) / "renut_android_init.cpp").read_text(encoding="utf-8", errors="replace"))
    for p in files:
        texts[p] = p.read_text(encoding="utf-8", errors="replace").split("\n")
    original = dict(texts)
    if any("\tPPCRegister r31{};" in lines for lines in texts.values()):
        print("vector save/restore helper calls put back: %d" % vectors_back_to_ctx(folder, texts))
    expanded = expand_shared_helpers(folder, texts)
    print("save/restore helpers put back in shared functions: %d" % expanded)
    for p in files:
        lines = texts[p]
        before = set(funcs)
        parse(lines, p, funcs)
        by_file[p] = [funcs[n] for n in funcs if n not in before]
    rounds, indirect_in, indirect_after, maydef = analyze(funcs)
    ins = [f for f in funcs.values() if f.live_in]
    outs = [f for f in funcs.values() if f.live_out]
    print("functions %d, rounds %d" % (len(funcs), rounds))
    print("live in : %d functions" % len(ins))
    for r in REGS:
        names = [f.name for f in ins if f.live_in & BIT[r]]
        if names:
            print("   %-4s %5d  %s" % (r, len(names), " ".join(sorted(names)[:8])))
    print("live out: %d functions" % len(outs))
    for r in REGS:
        names = [f.name for f in outs if f.live_out & BIT[r]]
        if names:
            print("   %-4s %5d  %s" % (r, len(names), " ".join(sorted(names)[:8])))
    print("indirect calls: hand in %s, live after %s" % (names_of(indirect_in), names_of(indirect_after)))
    if report_only:
        return
    # After a pointer call the caller keeps its own copy: the Xbox compiler never leaves a volatile
    # register live across an unknown callee, so the few reads found there are of the caller's value.
    indirect_out = 0
    total = 0
    for p, fns in by_file.items():
        new, edits = patch(texts[p], fns, funcs, indirect_in, indirect_out)
        if new is None and texts[p] is not original[p]:
            new = texts[p]
        if new is not None:
            p.write_text("\n".join(new), encoding="utf-8", newline="")
            total += edits
    print("lines written: %d" % total)


if __name__ == "__main__":
    main(sys.argv[1], "--report" in sys.argv[2:])
