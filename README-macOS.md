# f3probe on macOS (Apple Silicon) — fork notes

This fork adds a **Darwin device backend** so `f3probe` builds and runs natively
on macOS / Apple Silicon. Upstream marks `f3probe`/`f3brew`/`f3fix` as
Linux-only because the device backend used Linux-kernel interfaces
(`O_DIRECT`, `BLK*` ioctls, `USBDEVFS_RESET`, `libudev`). Only **`f3probe`** is
ported here; `f3brew` and `f3fix` remain Linux-only (out of scope).

> ⚠️ **Validation status: UNVALIDATED as shipped.** The port was authored on a
> Linux container with no compiler and no hardware. It has **not** been
> compiled, and **not** been run against real cards. `f3probe` is a
> fraud-detection tool — a binary that confidently misreports a fake card is
> worse than no tool. **Do not trust its verdicts until you complete the
> validation below and fill in the results table.** See `PORT-LOG.md` for the
> full chain of reasoning.

## What changed (and what didn't)

- **Unchanged:** `src/libprobe.c` — the validated probing algorithm. Not touched.
- **Ported:** `src/libdevs.c` — only the block-device backend, behind
  `#ifdef __APPLE__` / `#ifdef __linux__`:
  | Purpose | Linux | macOS (this fork) |
  |---|---|---|
  | unbuffered open | `O_DIRECT` | open `/dev/rdiskN` + `fcntl(F_NOCACHE,1)` |
  | device size | `BLKGETSIZE64` | `DKIOCGETBLOCKCOUNT` × `DKIOCGETBLOCKSIZE` |
  | post-write flush | `fsync`+`FADV_DONTNEED` | `F_FULLFSYNC` + `DKIOCSYNCHRONIZECACHE` |
  | enumeration | `libudev` | drop it; pass the device path; auto `diskutil unmountDisk` |
- **Build:** `Makefile` drops `-ludev` on non-Linux and, on macOS, builds only
  `f3probe` among the extra tools.

**Note on the USB reset:** `f3probe` constructs its device with `RT_NONE` and
its algorithm never calls reset — it defeats caches with unbuffered raw I/O plus
its own `overwhelm_cache` step, not with a hardware reset. So the IOKit/USB
reset that the porting notes feared is **not needed for `f3probe`** (it is an
`f3brew` feature). This is why the port is small.

## Build (Apple Silicon)

```sh
brew install argp-standalone           # provides argp.h / libargp.a
make extra                             # builds build/f3probe (macOS: f3probe only)
make all                               # f3write / f3read (already supported upstream)
```
Homebrew on Apple Silicon lives under `/opt/homebrew`; the Makefile resolves it
via `brew --prefix argp-standalone`. If linking fails, confirm the static lib
name and path: `ls "$(brew --prefix argp-standalone)/lib"`.

## Usage & safety

```sh
sudo build/f3probe /dev/disk4          # pass the WHOLE disk, not a slice (diskNsM)
```
- Raw disk access needs `sudo`. Without it you get a clear "no access" message.
- The tool opens the **raw** node `/dev/rdisk4` and `diskutil unmountDisk`s the
  whole disk first. **Double-check the device** with `diskutil list` — writing
  to the wrong `/dev/diskN` destroys data.
- Probing writes only within `(1 MB, announced_end)` and (by default, non
  `--destructive`) restores the blocks it touched.

## You MUST validate before trusting it

`f3probe` builds cleanly ≠ `f3probe` is correct. Run, in order:

1. **Ground truth** — for each card, get an independent verdict from macOS
   `f3write`+`f3read` and from upstream `f3probe` on a Linux box. They must agree.
2. **Cache-defeat spike** — `cc -O2 -Wall -Wextra -o spike spike.c`, then
   `sudo ./spike --destroy diskN` on a known-genuine and a known-fake card, and
   do the printed eject/reseat + `dd` cross-check. The in-process verdict must
   match the physical `dd` read. If not, the reader's cache is lying → use a
   different reader or stop.
3. **Correctness battery** — for every (card × reader), compare `f3probe`'s full
   verdict (`fake_type`, real size, wraparound, block order) against both
   references. Add the double-blind A/B test on two same-announced-size cards.
4. **Robustness** — determinism, mounted-card handling, no-sudo error, Ctrl-C,
   card-usable-after, 512B vs 4K logical, 2 TB timing.

### Results table (fill this in — do not delete the "not validated" note above)

| Card | Announced | Reader | f3write/f3read | Linux f3probe | macOS f3probe | Match? |
|------|-----------|--------|----------------|---------------|---------------|--------|
| _e.g. genuine 32GB_ | | | | | | |
| _e.g. fake "2TB"_   | | | | | | |

### Honest limits statement (complete after Phase 4)

> Validated on cards {…} via readers {…}. Passing these does **not** prove
> general correctness for untested fake archetypes or untested readers.
> Known-unsupported / untrusted: {…}.
