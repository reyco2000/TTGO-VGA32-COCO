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

`accurate:` is one of `ours`, `xroar`, `mame`. `confidence:` is `high`,
`medium` or `low`. A malformed entry is a hard error naming the line.
