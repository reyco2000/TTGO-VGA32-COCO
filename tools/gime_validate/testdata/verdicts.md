# Fixture verdicts for make_report.py (format examples, not real findings)

## timer.tins1_period_596
accurate: xroar
confidence: high
XRoar measures it on hardware.

### Evidence
tcc1014.c:123

## video.fs_*
accurate: mame
Prefix entry.

## video.fs_special
accurate: ours
confidence: low
Exact beats the prefix.

## random.timer.*
accurate: xroar
confidence: medium
Random timer family.

## unused.entry
accurate: mame
Never applied.
