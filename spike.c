/*
 * spike.c — Phase 1 cache-defeat spike for the f3probe macOS port.
 *
 * This is NOT part of f3. It is the throwaway de-risking program the porting
 * spec front-loads: before trusting a ported f3probe, prove that on THIS Mac +
 * THIS reader + THIS card, raw unbuffered I/O actually reflects what is
 * physically on the flash — so that on a fake card we can OBSERVE address
 * aliasing, and no cache silently returns matching-but-stale data.
 *
 * What it does (mirrors the spec's "killer test"):
 *   1. unmount the whole disk, open /dev/rdiskN with F_NOCACHE,
 *   2. read the device size via DKIOC* ioctls,
 *   3. write pattern A to block 0,                       flush, read block 0 (sanity),
 *   4. write pattern B to the LAST announced block,       flush, read block 0 again.
 *   - block 0 still == A  -> no aliasing at this offset (genuine, or real size
 *                            larger than the probed offset),
 *   - block 0 now  == B   -> the high write landed on block 0: ALIASING -> fake,
 *   - anything else        -> unexpected; investigate.
 *   It also reads the last block back and reports it.
 *
 * The decisive cache check is the PHYSICAL cross-check the program prints at the
 * end: after it finishes, physically eject + reseat the card and `dd` block 0;
 * the bytes dd reads MUST match what this program reported. If they differ, a
 * cache lied and the port cannot be trusted on this reader (escalate per spec).
 *
 * DESTRUCTIVE: it overwrites block 0 (partition-table area) and the last block
 * of the target disk. It WILL destroy data on that disk.
 *
 * Build:  cc -O2 -Wall -Wextra -o spike spike.c
 * Usage:  sudo ./spike disk4             # DRY RUN: print the plan, write nothing
 *         sudo ./spike --destroy disk4   # actually write (irreversible)
 *
 * Exit:   0 = completed (read the printed VERDICT); non-zero = setup/IO error.
 */

#define _DARWIN_C_SOURCE

#if !(defined(__APPLE__) && defined(__MACH__))
#error "spike.c is macOS-only; it uses F_NOCACHE/F_FULLFSYNC and the DKIOC* ioctls."
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/disk.h>

#define TAG_A 0xAAAAAAAAAAAAAAAAULL	/* written to block 0          */
#define TAG_B 0xBBBBBBBBBBBBBBBBULL	/* written to the high block    */

/* Fill @buf with repeating 16-byte records of {tag, block_idx} so a read-back
 * tells us unambiguously which write produced the bytes we see. */
static void fill_pattern(unsigned char *buf, size_t len,
	uint64_t tag, uint64_t block_idx)
{
	size_t off;
	memset(buf, 0, len);
	for (off = 0; off + 16 <= len; off += 16) {
		memcpy(buf + off, &tag, 8);
		memcpy(buf + off + 8, &block_idx, 8);
	}
}

static void read_record(const unsigned char *buf, uint64_t *tag, uint64_t *idx)
{
	memcpy(tag, buf, 8);
	memcpy(idx, buf + 8, 8);
}

/* Derive "/dev/diskN" (whole) and "/dev/rdiskN" (raw) from a user argument.
 * Accepts diskN, rdiskN, /dev/diskN, /dev/rdiskN. Rejects slices (diskNsM). */
static int disk_paths(const char *arg, char *whole, size_t wl, char *raw, size_t rl)
{
	const char *p = arg, *q;
	if (!strncmp(p, "/dev/", 5))
		p += 5;
	if (p[0] == 'r' && !strncmp(p + 1, "disk", 4))
		p++;
	if (strncmp(p, "disk", 4)) {
		fprintf(stderr, "`%s' is not a macOS disk (expected e.g. /dev/disk4)\n", arg);
		return -1;
	}
	q = p + 4;
	if (*q < '0' || *q > '9') {
		fprintf(stderr, "`%s' has no disk number\n", arg);
		return -1;
	}
	while (*q >= '0' && *q <= '9')
		q++;
	if (*q != '\0') {
		fprintf(stderr, "`%s' looks like a partition/slice; use the WHOLE disk "
			"(e.g. /dev/disk4, not /dev/disk4s1)\n", arg);
		return -1;
	}
	snprintf(whole, wl, "/dev/%s", p);
	snprintf(raw, rl, "/dev/r%s", p);
	return 0;
}

static int flush_dev(int fd)
{
	/* On a RAW disk node, F_FULLFSYNC is not applicable (it returns ENOTTY);
	 * the drive's write cache is flushed with DKIOCSYNCHRONIZECACHE. This is
	 * best-effort: if the reader doesn't implement SYNCHRONIZE CACHE we note it
	 * and continue — the raw write already left the OS, and the physical
	 * eject/reseat cross-check is the real arbiter of whether a cache lied. */
#ifdef DKIOCSYNCHRONIZECACHE
	if (ioctl(fd, DKIOCSYNCHRONIZECACHE) < 0)
		fprintf(stderr, "    note: DKIOCSYNCHRONIZECACHE failed (%s); continuing\n",
			strerror(errno));
#else
	(void)fd;
	fprintf(stderr, "    note: built without DKIOCSYNCHRONIZECACHE; no cache flush\n");
#endif
	return 0;
}

/* Seek + full write of exactly @len bytes at block @idx. */
static int write_block(int fd, uint64_t idx, const unsigned char *buf, uint32_t bsz)
{
	off_t want = (off_t)idx * bsz, got = lseek(fd, want, SEEK_SET);
	size_t done = 0;
	if (got != want) { perror("lseek(write)"); return -1; }
	while (done < bsz) {
		ssize_t rc = write(fd, buf + done, bsz - done);
		if (rc < 0) { perror("write"); return -1; }
		done += (size_t)rc;
	}
	return 0;
}

static int read_block(int fd, uint64_t idx, unsigned char *buf, uint32_t bsz)
{
	off_t want = (off_t)idx * bsz, got = lseek(fd, want, SEEK_SET);
	size_t done = 0;
	if (got != want) { perror("lseek(read)"); return -1; }
	while (done < bsz) {
		ssize_t rc = read(fd, buf + done, bsz - done);
		if (rc < 0) { perror("read"); return -1; }
		if (rc == 0) { fprintf(stderr, "unexpected EOF reading block %llu\n",
			(unsigned long long)idx); return -1; }
		done += (size_t)rc;
	}
	return 0;
}

static const char *tag_name(uint64_t tag)
{
	if (tag == TAG_A) return "A (block 0)";
	if (tag == TAG_B) return "B (high block)";
	return "??? (neither A nor B)";
}

int main(int argc, char **argv)
{
	const char *arg = NULL;
	bool destroy = false, have_real = false;
	char whole[64], raw[64], cmd[160];
	int fd, i;
	uint32_t bsz = 0;
	uint64_t bcount = 0, total, last_block, high_block, real_size = 0;
	const char *high_desc;
	unsigned char *bufA, *bufB, *rb;
	uint64_t tag, idx;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--destroy")) destroy = true;
		else if (!strncmp(argv[i], "--real-size=", 12)) {
			char *end;
			real_size = strtoull(argv[i] + 12, &end, 0);
			if (*end != '\0') {
				fprintf(stderr, "bad --real-size value `%s'\n", argv[i] + 12);
				return 2;
			}
			have_real = true;
		}
		else if (argv[i][0] == '-') {
			fprintf(stderr, "unknown option `%s'\n", argv[i]);
			return 2;
		} else arg = argv[i];
	}
	if (!arg) {
		fprintf(stderr,
			"usage: sudo %s [--destroy] [--real-size=BYTES] diskN\n"
			"  (without --destroy this is a DRY RUN: it prints the plan only)\n"
			"  --real-size=BYTES: announced offset at which to write pattern B.\n"
			"     For a known WRAPAROUND fake, set this to the card's real\n"
			"     capacity (from f3write/f3read) so the high write aliases onto\n"
			"     block 0. Default: the last announced block, which often does\n"
			"     NOT alias onto block 0. Shell tip: --real-size=$((64*1024**3)).\n",
			argv[0]);
		return 2;
	}
	if (disk_paths(arg, whole, sizeof whole, raw, sizeof raw))
		return 2;

	printf("Target whole disk : %s\n", whole);
	printf("Target raw node   : %s\n", raw);

	/* Read geometry first (non-destructive) so the dry run can show real sizes. */
	if (!destroy) {
		printf("\n*** DRY RUN — no writes performed. ***\n"
			"This program will, with --destroy:\n"
			"  1. diskutil unmountDisk %s\n"
			"  2. open %s with F_NOCACHE\n"
			"  3. write pattern A to block 0, flush, read block 0 back\n"
			"  4. write pattern B to the high block (default: last block; or the\n"
			"     wrap boundary if --real-size=BYTES is given), flush, re-read block 0\n"
			"It OVERWRITES block 0 and the high block (DESTRUCTIVE).\n"
			"Re-run as: sudo %s --destroy %s\n", whole, raw, argv[0], arg);
		/* Still try to open read-only to report the size, best-effort. */
		fd = open(raw, O_RDONLY);
		if (fd >= 0) {
			if (!ioctl(fd, DKIOCGETBLOCKCOUNT, &bcount) &&
					!ioctl(fd, DKIOCGETBLOCKSIZE, &bsz) && bsz)
				printf("\nGeometry: %llu blocks x %u bytes = %llu bytes (%.2f GB announced)\n",
					(unsigned long long)bcount, bsz,
					(unsigned long long)(bcount * (uint64_t)bsz),
					(bcount * (double)bsz) / 1e9);
			close(fd);
		} else {
			printf("\n(Could not open %s read-only to read its size: %s.\n"
				" You likely need sudo; that is expected.)\n", raw, strerror(errno));
		}
		return 0;
	}

	snprintf(cmd, sizeof cmd, "diskutil unmountDisk %s", whole);
	printf("\nRunning: %s\n", cmd);
	if (system(cmd) != 0)
		fprintf(stderr, "warning: `%s' did not return 0; open may fail (EBUSY)\n", cmd);

	fd = open(raw, O_RDWR);
	if (fd < 0) { fprintf(stderr, "open(%s): %s\n", raw, strerror(errno)); return 1; }
	if (fcntl(fd, F_NOCACHE, 1) < 0) { perror("fcntl(F_NOCACHE)"); close(fd); return 1; }

	if (ioctl(fd, DKIOCGETBLOCKCOUNT, &bcount) < 0) { perror("DKIOCGETBLOCKCOUNT"); close(fd); return 1; }
	if (ioctl(fd, DKIOCGETBLOCKSIZE, &bsz) < 0)     { perror("DKIOCGETBLOCKSIZE");  close(fd); return 1; }
	if (!bsz || bcount < 2) { fprintf(stderr, "device too small or bad block size\n"); close(fd); return 1; }
	total = bcount * (uint64_t)bsz;
	last_block = bcount - 1;
	printf("Geometry: %llu blocks x %u bytes = %llu bytes (%.2f GB announced)\n",
		(unsigned long long)bcount, bsz, (unsigned long long)total, total / 1e9);

	high_block = last_block;
	high_desc = "the LAST announced block";
	if (have_real) {
		if (real_size == 0 || real_size >= total) {
			fprintf(stderr, "--real-size=%llu must be > 0 and < announced size %llu\n",
				(unsigned long long)real_size, (unsigned long long)total);
			close(fd); return 2;
		}
		/* The first announced block at/after the real boundary. On a clean
		 * wraparound fake this physical-aliases onto block 0. */
		high_block = real_size / bsz;
		if (high_block == 0 || high_block >= bcount) {
			fprintf(stderr, "computed high block %llu is out of range\n",
				(unsigned long long)high_block);
			close(fd); return 2;
		}
		high_desc = "the wrap boundary (first block past real size)";
	}
	printf("Block 0 offset = 0; high block #%llu offset = %llu bytes (%s)\n",
		(unsigned long long)high_block,
		(unsigned long long)(high_block * (uint64_t)bsz), high_desc);

	/* Page-aligned buffers (spec belt-and-suspenders for raw I/O). */
	if (posix_memalign((void **)&bufA, (size_t)getpagesize(), bsz) ||
	    posix_memalign((void **)&bufB, (size_t)getpagesize(), bsz) ||
	    posix_memalign((void **)&rb,   (size_t)getpagesize(), bsz)) {
		fprintf(stderr, "posix_memalign failed\n"); close(fd); return 1;
	}
	fill_pattern(bufA, bsz, TAG_A, 0);
	fill_pattern(bufB, bsz, TAG_B, high_block);

	/* Step 3: A -> block 0, flush, read back (must see A). */
	printf("\n[3] Writing pattern A to block 0 ...\n");
	if (write_block(fd, 0, bufA, bsz) || flush_dev(fd) || read_block(fd, 0, rb, bsz)) goto io_err;
	read_record(rb, &tag, &idx);
	if (tag != TAG_A) {
		printf("VERDICT: SETUP FAILURE — wrote A to block 0 but read back %s.\n"
			"The basic write/read path is not working; do not trust further results.\n",
			tag_name(tag));
		goto done_fail;
	}
	printf("    OK: block 0 reads back as pattern A.\n");

	/* Step 4: B -> high block, flush, re-read block 0. */
	printf("[4] Writing pattern B to %s (#%llu) ...\n",
		high_desc, (unsigned long long)high_block);
	if (write_block(fd, high_block, bufB, bsz) || flush_dev(fd)) goto io_err;
	if (read_block(fd, 0, rb, bsz)) goto io_err;
	read_record(rb, &tag, &idx);
	printf("    Block 0 now holds: tag=%s idx=%llu\n", tag_name(tag), (unsigned long long)idx);

	printf("\n================= VERDICT =================\n");
	if (tag == TAG_A) {
		printf("NO ALIASING onto block 0: the high write did not corrupt block 0.\n"
			"This alone does NOT mean the card is genuine: limbo-type fakes don't\n"
			"corrupt low blocks, and the wrap boundary may map elsewhere than block 0.\n"
			"If this is a known WRAPAROUND fake, re-run with --real-size=<real\n"
			"capacity from f3write/f3read> so B lands on the wrap boundary.\n"
			"The cross-check below still confirms whether the read path is honest.\n");
	} else if (tag == TAG_B) {
		printf("ALIASING DETECTED: writing the high block overwrote block 0.\n"
			"This is the wraparound signature of a FAKE device, and the\n"
			"in-process unbuffered read SAW it — caches did not hide it.\n");
	} else {
		printf("UNEXPECTED: block 0 holds neither A nor B. Investigate (partial\n"
			"aliasing, a lying cache, or a flaky reader).\n");
	}
	printf("==========================================\n");

	/* Read the high block back too, for the record. */
	if (!read_block(fd, high_block, rb, bsz)) {
		read_record(rb, &tag, &idx);
		printf("(For reference, high block #%llu reads back as: tag=%s idx=%llu)\n",
			(unsigned long long)high_block, tag_name(tag), (unsigned long long)idx);
	}

	close(fd);
	printf("\n*** PHYSICAL CROSS-CHECK (do this to be sure no cache lied) ***\n"
		"1. Eject, then PHYSICALLY power-cycle the card (unplug the reader or pull\n"
		"   the card out and reinsert) so the reader and card caches are wiped:\n"
		"     diskutil eject %s     # then physically unplug & replug\n"
		"2. The card may come back as a DIFFERENT node — re-check with: diskutil list\n"
		"3. Read block 0 straight off the media and dump the first record\n"
		"   (use the raw rNODE so the OS cache is bypassed; sudo required):\n"
		"     sudo dd if=%s bs=%u count=1 2>/dev/null | xxd | head -1\n"
		"   First 8 bytes = the tag: aaaaaaaa.. = A (block 0), bbbbbbbb.. = B (high\n"
		"   block). It MUST match the VERDICT above. If it differs, a cache lied on\n"
		"   this reader and the port cannot be trusted here (try another reader or\n"
		"   escalate per the spec).\n", whole, raw, bsz);
	return 0;

io_err:
	fprintf(stderr, "I/O error during the spike; results are inconclusive.\n");
done_fail:
	close(fd);
	return 1;
}
