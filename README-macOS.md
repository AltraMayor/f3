# f3probe on macOS (Apple Silicon) — fork notes

This fork adds a **Darwin device backend** so `f3probe` builds and runs natively
on macOS / Apple Silicon. Upstream marks `f3probe`/`f3brew`/`f3fix` as
Linux-only because the device backend used Linux-kernel interfaces
(`O_DIRECT`, `BLK*` ioctls, `USBDEVFS_RESET`, `libudev`). Only **`f3probe`** is
ported here; `f3brew` and `f3fix` remain Linux-only (out of scope).

> ⚠️ **Validation status: validated on ONE card + ONE reader; not yet proven
> beyond that.** The port compiles cleanly on Apple Silicon and, on a known
> **limbo** counterfeit ("1.95 TB" announced / 122 MB real) through one USB
> reader, produces a verdict **identical to Linux upstream f3probe** and stable
> across 5 consecutive runs; the spike's eject/reseat `dd` cross-check confirmed
> it reads physical media, not cache. It has **NOT** been tested on a genuine
> device, on other fake archetypes (wraparound/chain/bad), on other readers, or
> on 4K-logical media. `f3probe` is a fraud-detection tool — treat untested
> configurations as unproven. See the results table and limits below, and
> `PORT-LOG.md` for the full chain of evidence.

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
- **`Permission denied` even with `sudo`?** Two macOS-specific causes (the tool
  now names both in the error): (1) the SD card's physical **lock switch** is
  engaged — `f3probe` needs read-write access, so slide it away from `LOCK`;
  confirm with `diskutil info /dev/diskN | grep -i read-only`. (2) Your terminal
  lacks **Full Disk Access** — add it under System Settings › Privacy & Security ›
  Full Disk Access, then fully quit and reopen the terminal.

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

### Results table

| Card | Announced | Reader | Linux f3probe (ground truth) | macOS f3probe | Match? |
|------|-----------|--------|------------------------------|---------------|--------|
| limbo fake | 1.95 TB (4194304000 × 512) | USB reader (model TBD — fill in) | `limbo`, usable 122.00 MB / 249856 blk, last 249855, module 2^41 | `limbo`, usable 122.00 MB / 249856 blk, last 249855, module 2^41 — **5/5 runs identical** | ✅ |
| _genuine card_ | | | | _not yet tested_ | — |
| _other fake archetypes_ | | | | _not yet tested_ | — |

Notes: on this card macOS reported cache size 0 (probe ~1.5 min) vs Linux 512 MB
(~23 min); the verdict was unaffected. The spike's eject/reseat `dd` cross-check
confirmed the read path reflects physical media. One earlier run returned a
transient `damaged` (a write error at the extreme announced LBA) that did not
recur in 5 subsequent runs — a safe-direction intermittency (never a false
`good`), documented in `PORT-LOG.md`.

### Honest limits statement

> **Validated:** one **limbo** counterfeit ("1.95 TB" announced, 122 MB real)
> via one USB reader, against Linux upstream f3probe — deterministic over 5 runs,
> and confirmed reading physical media (spike cross-check).
> **NOT proven:** genuine devices; other fake archetypes (wraparound, chain,
> bad); other USB readers or the built-in SD slot; 4K-logical-sector devices;
> behaviour under interrupt/again-after-reformat (Phase 5 leftovers).
> Treat all of the above as unsupported until tested.
