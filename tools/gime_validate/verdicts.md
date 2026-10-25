# GIME validation verdicts

When XRoar and MAME disagree on a check, an entry here says which one is
accurate (hardware-true). The runner then reports DISC if our value matches
that model and FAIL if it does not. A split with no entry is UNRES.
`v86.` checks never need an entry.

Entry format: a level-2 heading with the check id, then the machine-readable
lines, then free markdown (what each model does with file:line, evidence, and
whether ours is a bug or a simplification).

```
## timer.tins1_period_596
accurate: xroar
confidence: high
<free markdown>
```

- `accurate:` (required) is one of `ours`, `xroar`, `mame`.
- `confidence:` (optional, default `medium`) is `high`, `medium` or `low`.
- A `## ` line always starts a new entry, so sub-headings inside an entry
  must use `###` or deeper.
- Lines inside a fenced block (like the example above) are ignored.
- A malformed entry (unknown model, bad confidence, missing `accurate:`, a
  duplicate id, a bad id) is a hard error naming the line.

### Prefix verdicts

An id may end in `*` (and only there) to cover every check whose id starts
with the text before it, e.g. `## random.irq.*`. Use it for a family of checks
with one cause. An exact id beats any prefix; among prefixes the longest wins.
`results.json` records the entry applied to each check (`verdict_id`) and the
report lists every prefix verdict with the checks it covered.

```
## random.timer.*
accurate: xroar
confidence: medium
<free markdown>
```
