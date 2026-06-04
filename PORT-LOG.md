# PORT-LOG — f3probe → macOS (Apple Silicon)

> Running audit trail for the port described in `f3probe-macos-port-spec.md`.
> Append-only, table-first. **Read §0 first: it states what environment this
> work was actually done in, and therefore what is and is not proven.**

---

## 0. Environment reality check (READ THIS FIRST)

The spec assumes "a fresh Claude Code session running on the target Mac" with
physical test cards, card readers, and a Linux reference box. **None of that is
true of the environment this port was authored in.** Established facts:

| Fact | Evidence |
|---|---|
| Host OS is **Linux aarch64** (a linuxkit container), **not macOS** | `uname -a` → `Linux … 6.12.76-linuxkit … aarch64`; `sw_vers` absent |
| **No C compiler / build tooling** | `cc`, `gcc`, `clang`, `make`, `xcode-select` all absent from `PATH` |
| **No macOS tooling** | `diskutil`, `brew`, `sw_vers` absent |
| **No hardware** | No flash cards, no USB readers, no `/dev/rdiskN` to touch |
| No prior macOS commits | branch `macos-version` == `master` == `origin/master` (commit `67b8a5d`) |

**Consequence for "success":** the spec defines success *only* as the ported
binary's verdict matching ground truth across the Phase 4 validation battery on
real cards. **That cannot be performed here** — there is no compiler to build it,
no macOS to run it on, and no cards to probe. Per the spec's non-negotiable
principle, I will **not** claim validation I did not do.

What this environment *can* produce, correctly and reviewably, is the
**environment-independent engineering**: read the seam, design the mapping,
write the Darwin device backend + the Phase-1 spike + the build changes as
source, and hand over a precise runbook the user executes on the Mac. That is
what this log covers. Everything below is **UNVALIDATED CODE** until Phase 1/4/5
are run on the Mac by the user.

Phase status in this environment:

| Phase | What it needs | Status here |
|---|---|---|
| 0 Ground truth | Mac + cards + Linux box | ⛔ Cannot run (no hardware) — runbook provided |
| 1 Cache-defeat spike | compiler + Mac + fake card | 🟡 `spike.c` **written**, **not built/run** — runbook provided |
| 2 Map the seam | source only | ✅ Done (this log, §2) |
| 3 Implement Darwin backend | compiler to verify | 🟡 Code **written** (`libdevs.c`, `Makefile`), **not compiled** |
| 4 Correctness battery | Mac + cards + readers | ⛔ Cannot run — runbook provided |
| 5 Robustness | Mac + cards | ⛔ Cannot run — runbook provided |
| 6 Package fork | — | 🟡 README/runbook written |

---

## 1. The most important finding (corrects a central premise of the spec)

The spec says (§2.2, §3): *"a USB-level device reset (`USBDEVFS_RESET`) between
the write and verify phases … THAT USB reset is the crux of the whole port."*

**For `f3probe`, this is not accurate.** Reading the actual source:

- `f3probe.c:413` constructs the device with `create_block_device(filename, RT_NONE)`.
- `f3probe` exposes **no `--reset-type` option** at all (`f3probe.c:29-62`).
- `RT_NONE` → `bdev_none_reset()` → **does nothing** (`libdevs.c:818-822`).
- **`libprobe.c` contains zero references to reset** (`grep -ni reset libprobe.c`
  → nothing). The probe algorithm never resets the device.
- The USB reset path (`bdev_usb_reset`/`bdev_manual_usb_reset`, `USBDEVFS_RESET`)
  is used **only by `f3brew`** (`f3brew.c:562` default `RT_MANUAL_USB`,
  `f3brew.c:614` `dev_reset`). **`f3brew` is explicitly out of scope.**

How `f3probe` actually defeats caches (no hardware reset involved):

1. **OS page cache** — `bdev_open()` uses `O_DIRECT` on Linux (`libdevs.c:474`).
2. **Post-write flush** — `bdev_write_blocks()` does `fsync()` +
   `posix_fadvise(POSIX_FADV_DONTNEED)` (`libdevs.c:466-469`).
3. **Controller/card cache** — the algorithm *measures* the cache size
   (`find_cache_size`) and then **`overwhelm_cache()`** (`libprobe.c:135-143`)
   writes that many sequential blocks to flush stale data out, and issues only
   **random reads** to dodge sequential-read caches (`libprobe.c:29-41`).

**Why this matters:** the hardest, riskiest item the spec front-loads (IOKit USB
reset) is **not required for `f3probe`**. The port reduces to mapping three
primitives — unbuffered open, device-size query, and post-write flush — plus
dropping `libudev`. This is a much smaller and safer surface than the spec
feared. The cache-defeat correctness still must be proven empirically (Phase 1
spike + Phase 4), because `overwhelm_cache` depends on correctly *measuring* the
reader/card cache, and a lying cache could still defeat it — so the validation
battery remains essential. But the *mechanism* to port is simpler.

---

## 2. The seam — function-by-function mapping (Phase 2)

Only `src/libdevs.c` is ported. `libprobe.c` is **untouched** (validated algorithm).
The platform-independent layers (`file_device`, `perf_device`, `safe_device`) are
untouched except they already use `aligned_alloc` (C11, present on macOS 10.15+).

Backend contract used by `f3probe` (via `struct device` vtable, `libdevs.h`):

| Backend op | Linux primitive (today) | Darwin replacement | Where |
|---|---|---|---|
| open (unbuffered) | `open(O_RDWR \| O_DIRECT)` | open **`/dev/rdiskN`** `O_RDWR` + `fcntl(F_NOCACHE,1)` | `bdev_open` |
| device size | `ioctl(BLKGETSIZE64)` | `ioctl(DKIOCGETBLOCKCOUNT)` × `ioctl(DKIOCGETBLOCKSIZE)` | `create_block_device` |
| logical block size | `ioctl(BLKSSZGET)` | `ioctl(DKIOCGETBLOCKSIZE)` | `create_block_device` |
| read blocks | `lseek`+`read` | identical (raw fd) | `bdev_read_blocks` (unchanged) |
| write blocks + flush | `lseek`+`write`+`fsync`+`fadvise(DONTNEED)` | `lseek`+`write`+`fcntl(F_FULLFSYNC)`+`ioctl(DKIOCSYNCHRONIZECACHE)` | `bdev_write_blocks` |
| reset | `bdev_none_reset` (no-op for `f3probe`) | identical no-op | `bdev_none_reset` (unchanged) |
| free / filename | `close`/`free` | identical | unchanged |
| device enumeration | `libudev` (whole-disk check, USB-backed check) | **dropped**: require path arg; reject slices by name; auto-`diskutil unmountDisk` | `create_block_device` (Darwin) |

**Structure decision:** `#ifdef` branches **inside `libdevs.c`** (not a separate
`libdevs_darwin.c`). Reason: the `struct device` dispatch and the file/perf/safe
devices live in `libdevs.c` and are shared; only the includes, `bdev_open`, the
size query, the write-flush tail, and `create_block_device` differ. All Linux
`udev`/`USBDEVFS` code is wrapped in `#ifdef __linux__`; a self-contained Darwin
`create_block_device` is added under `#if defined(__APPLE__) && defined(__MACH__)`.
`libprobe.c` stays byte-for-byte identical.

**Alignment note (validation risk):** raw-device I/O on macOS requires
offset/length to be block-size multiples — satisfied (`pos << block_order`). The
spec recommends *page*-aligned buffers; `f3` aligns buffers to *block* size only
(`align_mem` in `libprobe.c`, `aligned_alloc` in `libflow.c`). On `/dev/rdiskN`
block alignment is normally sufficient, but **if raw reads/writes return EINVAL**
this is the first thing to check (see §5 of the spec). Fixing it would require
touching `libflow.c`'s `dbuf` allocation and `libprobe.c`'s stack buffers — flag
to the user rather than silently changing the forbidden file.

---
## 3. Implementation (Phase 3) — written, NOT compiled

All changes are isolated to the device backend and the build. `libprobe.c` is
**byte-for-byte unchanged** (`git diff --stat` shows it untouched).

| File | Change |
|---|---|
| `src/libdevs.c` | `_DARWIN_C_SOURCE`; guard `<linux/*>`/`<libudev.h>` behind `__linux__`, add `<sys/disk.h>` for Darwin; `bdev_open` uses raw node + `F_NOCACHE` instead of `O_DIRECT`; `bdev_write_blocks` uses `F_FULLFSYNC` + `DKIOCSYNCHRONIZECACHE`; all udev/USBDEVFS code wrapped in `#ifdef __linux__`; a self-contained Darwin `create_block_device()` (path parsing → reject slices, `diskutil unmountDisk`, `DKIOCGETBLOCKCOUNT`×`DKIOCGETBLOCKSIZE`, `RT_NONE` only). |
| `Makefile` | drop `-ludev` on non-Linux (`UDEV_LIBS`); on Darwin `EXTRA_TARGETS` = just `f3probe`; argp path prefers `brew --prefix argp-standalone`. |
| `spike.c` (new) | standalone Phase-1 cache-defeat spike (see §4). |

**Static review done (no compiler available):** preprocessor guards balanced;
no Linux-only symbol (`O_DIRECT`, `udev_*`, `USBDEVFS_RESET`, `BLKGETSIZE64`,
`BLKSSZGET`, `__progname`) appears outside an `#ifdef __linux__`; goto-cleanup
ladder in the Darwin `create_block_device` is sound; all referenced symbols
(`F_NOCACHE`, `F_FULLFSYNC`, `DKIOC*`, `getprogname`, `system`) are reachable
via the included headers under `_DARWIN_C_SOURCE`. **This is NOT a substitute
for compiling.** Checkpoint 3A (clean compile on arm64) is **NOT met** here —
it must be done on the Mac.

## 4. What the user must run on the Mac (Phases 0,1,3A,4,5 — the real success bar)

This is the runbook. **None of it has been executed here.** Until Phase 1 and
Phase 4 pass, the binary's verdicts must NOT be trusted (spec §0).

**Build (Checkpoint 3A):**
```
brew install argp-standalone
make extra            # on macOS this now builds ONLY build/f3probe
cc -O2 -Wall -Wextra -o spike spike.c
```
If `make` errors, paste the errors back — this is exactly the verification this
environment could not do. Watch for: missing `argp.h` (fix the brew prefix),
raw-I/O `EINVAL` (the buffer page-alignment caveat in §2), or a `DKIOC` symbol
not found (check `<sys/disk.h>` on the installed SDK).

**Phase 0 — ground truth (answer key BEFORE trusting the port):** for each card,
record announced size, and the verdict from BOTH (a) macOS `f3write`+`f3read`
and (b) upstream `f3probe` on the Linux box. They must agree.

**Phase 1 — cache-defeat spike (⚠️ most important):**
```
sudo ./spike disk4              # dry run: prints the plan + size, writes nothing
sudo ./spike --destroy disk4    # genuine card: VERDICT should say NO ALIASING
sudo ./spike --destroy disk9    # known-fake card: VERDICT should say ALIASING
```
Then do the printed physical cross-check (eject/reseat + `dd` block 0). The
in-process VERDICT must match the post-reseat `dd` read. If it doesn't, a cache
lied on this reader → STOP, try another reader, do not ship.

**Phase 4 — correctness battery:** for every (card × reader), run
`sudo build/f3probe /dev/disk4` and confirm `fake_type` + real size match BOTH
references from Phase 0. Include the reseat/`dd` boundary check and the
double-blind A/B test. Record every run.

**Phase 5 — robustness:** determinism (probe twice), mounted-card handling,
no-sudo error message, Ctrl-C cleanliness, card-still-usable-after, 512B vs 4K
logical, 2 TB timing sanity.

See `README-macOS.md` for the same steps in fork-README form, plus the results
table to fill in and the honest "validated / not validated" statement.

## 5. Honest status summary

- ✅ Done here: seam analysis, the RT_NONE/no-reset correction, the Darwin code,
  build changes, the spike, docs.
- ⛔ Not done here (impossible without a Mac/compiler/cards): compile, Phase 1
  spike run, Phase 4 correctness, Phase 5 robustness.
- **Therefore this port is UNVALIDATED.** It is a credible, reviewed starting
  point — not a trustworthy fraud-detection binary until the runbook passes.

## 6. Phase 1 field findings (from on-Mac runs by the user)

| Date | Finding | Action |
|---|---|---|
| 2026-05-31 | **`fcntl(fd, F_FULLFSYNC)` fails with `ENOTTY` on `/dev/rdiskN`.** F_FULLFSYNC is only valid on regular files; on a raw device node it is "inappropriate ioctl for device". The spike aborted at the first flush; `f3probe`'s `bdev_write_blocks` would have failed identically. | Drop F_FULLFSYNC on the raw node. Use `DKIOCSYNCHRONIZECACHE` only (the drive-cache flush, analogue of Linux fsync-on-block-device). Tolerate `ENOTTY`/`ENOTSUP` (reader without SYNCHRONIZE CACHE) with a one-time warning; surface other errno. Fixed in `spike.c` `flush_dev()` and `libdevs.c` `bdev_write_blocks()`. |
| 2026-05-31 | Raw **write to block 0 succeeded** before the flush error → no buffer-alignment/`EINVAL` problem on this reader (the page-alignment caveat in §2 did not bite). `diskutil unmountDisk` worked. | None; positive signal. |
| 2026-05-31 | **Phase 1 cache-defeat PASS on the limbo card.** After the flush fix: block 0 in-process read = `A`, post-reseat `dd` = `A` (match). Wrote `B` to the first limbo block (#249856); in-process read came back as **zeros (not `B`)** and post-reseat `dd` = zeros (match) — the discarded limbo write never returned from cache, in-process or physically. No cache lied on this reader. Ground truth (Linux upstream f3probe, same card): `limbo`, usable 122.00 MB / 249856 blocks, last good block 249855, module 2^41, ~512 MB cache. | Cache-defeat recipe **confirmed**: `/dev/rdiskN` + `F_NOCACHE` + `DKIOCSYNCHRONIZECACHE` (no USB reset). Caveat: single-point test; the definitive device-cache-defeat proof is f3probe (with `overwhelm_cache`) matching ground truth — next. Genuine-card half of Checkpoint 1 still pending. |
| 2026-05-31 | First `f3probe` build error on the diagnostic: `getpagesize()` undeclared under `_POSIX_C_SOURCE`. | Use `sysconf(_SC_PAGESIZE)` (core POSIX, always declared). Fixed. |
| 2026-05-31 | **Phase 4 verdict MATCH (limbo card, reader as used).** `f3probe --destructive --time-ops /dev/disk10` → `limbo`, usable **122.00 MB / 249856 blocks**, last good 249855, module 2^41, announced 1.95 TB — **identical to the Linux ground truth**. Probe time 1'31". Note: macOS reported cache size **0** (vs Linux 512 MB); verdict unaffected — consistent with the spike (no cache lie on this reader), so `overwhelm_cache` wasn't needed and the probe was ~15× faster. | Port correctly classifies this counterfeit on macOS. ✅ (one card, one reader) |
| 2026-05-31 | **Non-determinism observed (must characterize).** The run immediately before the match — *same I/O code* — returned `damaged / 0 blocks` because the first write (last announced block, offset ~2 TB) failed twice, then the probe bailed. The next run's writes succeeded and it completed correctly. Likely a flaky cheap-fake/bridge at the extreme announced LBA (perhaps aggravated by the card settling after the spike's reseats). Direction is the *safe* one (intermittent "damaged", not a false "good"), but unquantified. | **OPEN — Phase 5 determinism:** run `f3probe` ≥5×; require consistent `limbo`/122 MB. Tolerate occasional `damaged` (document rate); a single `good`/larger-size = hard stop, do not ship. |
| 2026-05-31 | **Phase 5 determinism PASS.** 5 consecutive `f3probe --destructive /dev/disk10` runs all returned identical `limbo` / 122.00 MB / 249856 blocks. The earlier one-off `damaged` did **not** recur. | Determinism confirmed for this card+reader. The transient `damaged` is documented as a known, safe-direction (never a false `good`) intermittency at the extreme announced LBA — not reproduced in 5 runs. |
| 2026-05-31 | **A different (write-protected) SD card failed with a bare `f3probe: Can't open device `/dev/rdisk10': Permission denied`** under `sudo`, even after `diskutil unmountDisk` succeeded. Cause: the card's physical lock switch was engaged, so `open(O_RDWR)` returned `EACCES`. The macOS error handling masked this twice: (a) the only `EACCES` hint was gated behind `getuid()` truthy, so a **root** user (sudo, uid 0) fell straight through to the generic `err()`; (b) there was no write-protect/read-only detection at all. root bypasses classic UNIX DAC, so `EACCES` as root is really write-protected media or a TCC/Full-Disk-Access denial — neither was mentioned. | Reworked `create_block_device()` open-failure handling in `libdevs.c`. Added `darwin_media_is_write_protected()` (read-only open + `DKIOCISWRITABLE`, `#ifdef`-guarded, returns true only when definitely locked) and `darwin_warn_write_protected()`. `EACCES` now branches: write-protected → lock-switch message; non-root → existing "run as root" hint; **root → new message naming write-protect + Full Disk Access**. Added a proactive post-open check for bridges that open RW yet reject writes. `err()` path restores the saved `open_errno`. Not yet built on-Mac. **Verify:** lock ON → clear message, not `Permission denied`; lock OFF → probe proceeds. |
