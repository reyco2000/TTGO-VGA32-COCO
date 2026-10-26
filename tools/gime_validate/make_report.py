#!/usr/bin/env python3
"""GIME validation report: results.json + verdicts.md -> markdown.

usage: python3 -I make_report.py <results.json> <verdicts.md> <out.md>

Sections: header; per-area summary; FAIL and FIXED/STALE; KNOWN-FAIL with
baseline reasons; resolution-limited passes; the discrepancy table; full
verdict text; UNRESOLVED; shims and reference edits. random.* results are grouped by area and status
(the per-id detail stays in results.json); scripted and v86.* checks are
listed per id. Standard library only.
"""
import datetime
import json
import sys

# "RES" is not a status in results.json: it is a PASS whose class is
# RESOLUTION (ours differs from the references within its resolution), split
# out as the console does. "pass" counts the exact agreements only.
STATUSES = ["PASS", "RES", "FAIL", "KNOWN_FAIL", "FIXED", "DISC", "UNRESOLVED", "SKIP"]
COLS = ["pass", "res", "fail", "known", "fixed", "disc", "unres", "skip"]
EXAMPLES = 5            # ids shown per random group
MAX_IDS = 40            # ids shown per prefix verdict


def is_random(cid):
    return cid.startswith("random.")


def is_v86(cid):
    return cid.startswith("v86.")


def area_of(cid):
    p = cid.split(".")
    if p[0] in ("random", "v86") and len(p) > 1:
        return p[0] + "." + p[1]
    return p[0]


def parse_verdict_text(path):
    """id -> markdown text of the entry (heading line included)."""
    out, order = {}, []
    cur, lines, fence = None, [], False

    def flush():
        if cur is not None:
            out[cur] = "\n".join(lines).strip("\n")

    try:
        f = open(path, encoding="utf-8")
    except OSError:
        return out, order
    with f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("```"):
                fence = not fence
                if cur is not None:
                    lines.append(line)
                continue
            if not fence and line.startswith("## "):
                flush()
                cur = line[3:].strip()
                order.append(cur)
                lines = [line]
                continue
            if cur is not None:
                lines.append(line)
    flush()
    return out, order


def is_res(c):
    return c["status"] == "PASS" and c.get("class") == "RESOLUTION"


def shown_status(c):
    return "RES" if is_res(c) else c["status"]


def offsets(c):
    """ours - reference, per reference with a value."""
    v = c.get("values", {})
    o = v.get("ours")
    out = {}
    for r in ("xroar", "mame"):
        if o is not None and v.get(r) is not None:
            out[r] = o - v[r]
    return out


def fmt_offsets(off):
    return ", ".join("%s %+d" % (r, d) for r, d in sorted(off.items())) or "-"


def fmt(v):
    return "-" if v is None else str(v)


def values_of(c):
    v = c.get("values", {})
    return fmt(v.get("ours")), fmt(v.get("xroar")), fmt(v.get("mame"))


def esc(s):
    return str(s).replace("|", "\\|").replace("\n", " ")


def code_ids(ids, limit=None):
    shown = ids if limit is None else ids[:limit]
    s = ", ".join("`%s`" % i for i in shown)
    if limit is not None and len(ids) > limit:
        s += " ... and %d more (see results.json)" % (len(ids) - limit)
    return s


def table(head, rows):
    out = ["| " + " | ".join(head) + " |", "|" + "|".join("---" for _ in head) + "|"]
    out += ["| " + " | ".join(esc(c) for c in r) + " |" for r in rows]
    return out


def grouped_random(checks, status, sink, what):
    """One line per random area for the random checks with this status."""
    groups = {}
    for c in checks:
        if is_random(c["id"]) and c["status"] == status:
            groups.setdefault(area_of(c["id"]), []).append(c["id"])
    if not groups:
        return
    sink.append("")
    sink.append("%s (random, grouped by area; per-id detail in results.json):" % what)
    sink.append("")
    for a in sorted(groups):
        ids = groups[a]
        sink.append("- `%s`: %d (e.g. %s)" % (a, len(ids), code_ids(ids, EXAMPLES)))


def build(data, vtext, vorder):
    run = data.get("run", {})
    checks = data.get("checks", [])
    stale = data.get("stale", [])
    out = []
    seed = run.get("seed", 0)

    # 1. header
    out.append("# GIME validation report")
    out.append("")
    out.append("- date: %s" % datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC"))
    out.append("- ours: commit `%s`" % (run.get("ours_commit") or "unknown"))
    out.append("- XRoar: commit `%s`" % (run.get("xroar_commit") or "not built"))
    out.append("- MAME: commit `%s`, form `%s`" % (run.get("mame_commit") or "not built",
                                                  run.get("mame_form") or "none"))
    out.append("- random seed: 0x%X (%d)%s" % (seed, seed, "" if run.get("random_gated", True)
               else " - not the default run: random.* checks are classified but do not gate"))
    out.append("- checks: %d" % len(checks))
    out.append("")

    # 2. summary per area
    out.append("## Summary by area")
    out.append("")
    areas = {}
    for c in checks:
        row = areas.setdefault(area_of(c["id"]), dict.fromkeys(STATUSES, 0))
        row[shown_status(c)] = row.get(shown_status(c), 0) + 1
    rows, tot = [], dict.fromkeys(STATUSES, 0)

    def sort_key(a):
        return (a.startswith("v86."), a.startswith("random."), a)
    for a in sorted(areas, key=sort_key):
        rows.append([a] + [str(areas[a].get(s, 0)) for s in STATUSES])
        for s in STATUSES:
            tot[s] += areas[a].get(s, 0)
    rows.append(["**total**"] + ["**%d**" % tot[s] for s in STATUSES])
    out += table(["area"] + COLS, rows)
    out.append("")
    out.append("STALE baseline entries: %d" % len(stale))
    out.append("")

    # 3. FAIL and FIXED/STALE
    out.append("## FAIL, FIXED and STALE")
    out.append("")
    fails = [c for c in checks if c["status"] == "FAIL" and not is_random(c["id"])]
    fixed = [c for c in checks if c["status"] == "FIXED" and not is_random(c["id"])]
    sink = []
    if fails:
        out.append("### FAIL (%d)" % len(fails))
        out.append("")
        out += table(["id", "ours", "xroar", "mame", "class", "note"],
                     [[c["id"]] + list(values_of(c)) + [c.get("class", ""), c.get("note", "")]
                      for c in fails])
        out.append("")
    grouped_random(checks, "FAIL", sink, "FAIL")
    if sink:
        out.append("### FAIL, random")
        out += sink
        out.append("")
    if not fails and not sink:
        out.append("No FAIL.")
        out.append("")
    out.append("### FIXED (in the baseline but now passing; remove from known_failures.txt)")
    out.append("")
    sink = []
    grouped_random(checks, "FIXED", sink, "FIXED")
    if fixed or sink:
        for c in fixed:
            out.append("- `%s`  (baseline reason: %s)" % (c["id"], c.get("baseline_reason") or "none"))
        out += sink
    else:
        out.append("None.")
    out.append("")
    out.append("### STALE (in the baseline but no such check; remove from known_failures.txt)")
    out.append("")
    if stale:
        out += ["- `%s`" % s for s in stale]
    else:
        out.append("None.")
    out.append("")

    # 4. KNOWN-FAIL
    known = [c for c in checks if c["status"] == "KNOWN_FAIL"]
    out.append("## KNOWN-FAIL (%d)" % len(known))
    out.append("")
    scripted = [c for c in known if not is_random(c["id"])]
    if scripted:
        out += table(["id", "ours", "xroar", "mame", "reason"],
                     [[c["id"]] + list(values_of(c)) + [c.get("baseline_reason", "")]
                      for c in scripted])
    rk = [c for c in known if is_random(c["id"])]
    if rk:
        out.append("")
        out.append("random (grouped by area and reason):")
        out.append("")
        g = {}
        for c in rk:
            g.setdefault((area_of(c["id"]), c.get("baseline_reason", "")), []).append(c["id"])
        for (a, reason) in sorted(g):
            out.append("- `%s` x%d: %s (e.g. %s)" % (a, len(g[(a, reason)]), reason or "no reason",
                                                       code_ids(g[(a, reason)], EXAMPLES)))
    if not known:
        out.append("None.")
    out.append("")

    # 5. resolution-limited passes
    res = [c for c in checks if is_res(c)]
    out.append("## Resolution-limited (%d)" % len(res))
    out.append("")
    out.append("Passes where ours differs from the references by no more than its resolution for that "
               "capability (tolerance, in ticks; ours resolves events to a whole line, 912 ticks). "
               "Offsets are ours - reference. Counted as passes; listed so that a drift inside the "
               "tolerance stays visible.")
    out.append("")
    rs = [c for c in res if not is_random(c["id"])]
    if rs:
        out += table(["id", "ours", "xroar", "mame", "tol", "ours - ref"],
                     [[c["id"]] + list(values_of(c)) + [str(c.get("tol", 0)), fmt_offsets(offsets(c))]
                      for c in rs])
        out.append("")
    rr = [c for c in res if is_random(c["id"])]
    if rr:
        out.append("random (grouped by area; per-id detail in results.json):")
        out.append("")
        g = {}
        for c in rr:
            g.setdefault(area_of(c["id"]), []).append(c)
        for a in sorted(g):
            cs = g[a]
            rng = {}
            for c in cs:
                for r, d in offsets(c).items():
                    lo, hi = rng.get(r, (d, d))
                    rng[r] = (min(lo, d), max(hi, d))
            span = ", ".join("%s %+d..%+d" % (r, lo, hi) for r, (lo, hi) in sorted(rng.items())) or "-"
            out.append("- `%s`: %d, tol %s, ours - ref %s (e.g. %s)" % (
                a, len(cs), "/".join(sorted({str(c.get("tol", 0)) for c in cs})), span,
                code_ids([c["id"] for c in cs], EXAMPLES)))
        out.append("")
    if not res:
        out.append("None.")
        out.append("")

    # 6. discrepancy table
    disc = [c for c in checks if c.get("class") == "REF_SPLIT"]
    disc_s = [c for c in disc if not is_random(c["id"])]
    out.append("## Discrepancies (XRoar and MAME disagree, or one reference only and ours differs)")
    out.append("")
    rows = []
    for c in disc_s:
        vid = c.get("verdict_id")
        acc = conf = "-"
        if vid and vid in vtext:
            body = vtext[vid]
            for ln in body.splitlines():
                if ln.startswith("accurate:"):
                    acc = ln.split(":", 1)[1].strip()
                elif ln.startswith("confidence:"):
                    conf = ln.split(":", 1)[1].strip()
            if conf == "-":
                conf = "medium"
        if is_v86(c["id"]):
            acc = "n/a (info only)"
        rows.append([c["id"]] + list(values_of(c)) + [acc, conf,
                     "yes" if c.get("two_way") else "no", c["status"]])
    if rows:
        out += table(["id", "ours", "xroar", "mame", "verdict (accurate)", "confidence", "2-way", "status"], rows)
    else:
        out.append("None.")
    sink = []
    for st in ("DISC", "FAIL", "UNRESOLVED", "KNOWN_FAIL"):
        grouped_random([c for c in disc if c["status"] == st], st, sink, "random REF_SPLIT with status " + st)
    if sink:
        out += sink
    out.append("")

    # 7. verdict text
    out.append("## Verdicts")
    out.append("")
    used_exact, used_prefix = {}, {}
    for c in disc:
        vid = c.get("verdict_id")
        if not vid:
            continue
        (used_prefix if vid.endswith("*") else used_exact).setdefault(vid, []).append(c["id"])
    for vid in vorder:
        if vid in used_exact or vid in used_prefix:
            ids = used_exact.get(vid) or used_prefix.get(vid)
            body = vtext[vid]
            out.append("#" + body if body.startswith("## ") else body)   # one level under "## Verdicts"
            out.append("")
            if vid.endswith("*"):
                out.append("*Prefix verdict `%s`: covered %d check(s): %s*" % (vid, len(ids), code_ids(ids, MAX_IDS)))
                out.append("")
    unused = [v for v in vorder if v not in used_exact and v not in used_prefix]
    if unused:
        out.append("Verdict entries not applied to any discrepancy this run: %s" % code_ids(unused))
        out.append("")
    if not vorder:
        out.append("No verdict entries.")
        out.append("")

    # 8. UNRESOLVED
    unres = [c for c in checks if c["status"] == "UNRESOLVED"]
    out.append("## UNRESOLVED (%d)" % len(unres))
    out.append("")
    two = [c for c in unres if not is_random(c["id"]) and c.get("two_way")]
    three = [c for c in unres if not is_random(c["id"]) and not c.get("two_way")]
    if three:
        out.append("References disagree, no verdict (%d):" % len(three))
        out.append("")
        out += table(["id", "ours", "xroar", "mame"], [[c["id"]] + list(values_of(c)) for c in three])
        out.append("")
    if two:
        out.append("Only one reference observed and ours differs, no verdict (2-way, %d):" % len(two))
        out.append("")
        out += table(["id", "ours", "xroar", "mame"], [[c["id"]] + list(values_of(c)) for c in two])
        out.append("")
    sink = []
    grouped_random(checks, "UNRESOLVED", sink, "UNRESOLVED")
    out += sink
    if not unres:
        out.append("None.")
    out.append("")

    # 9. shims and reference edits
    out.append("## Shims and reference edits")
    out.append("")
    out.append("The reference GIME logic is compiled unmodified; shims replace only plumbing "
               "(events, delegates, part, logging, serialise, xalloc and the MAME device framework).")
    out.append("")
    out.append("- MAME form: `%s`" % (run.get("mame_form") or "none (not built)"))
    edits = run.get("ref_edits", [])
    if edits:
        out += ["- edit: %s" % e for e in edits]
    else:
        out.append("- no reference edits (no reference built)")
    out.append("")
    return "\n".join(out)


def main(argv):
    if len(argv) != 4:
        sys.stderr.write(__doc__)
        return 2
    with open(argv[1], encoding="utf-8") as f:
        data = json.load(f)
    vtext, vorder = parse_verdict_text(argv[2])
    text = build(data, vtext, vorder)
    with open(argv[3], "w", encoding="utf-8") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
