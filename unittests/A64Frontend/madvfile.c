// SPDX-License-Identifier: MIT
//
// MADV_DONTNEED over a private file mapping is free: the private copy goes and
// the next access reads the file again. A guest that serves data out of its own
// image relies on that -- Bun's standalone executables carry their bundle in a
// PT_LOAD and MADV_DONTNEED the part they have finished with.
//
// On a host whose page is larger than the guest's, POWERarm cannot always hand
// such a mapping to the kernel and substitutes anonymous memory with the file
// read into it. Anonymous memory has no file underneath, so the same advice
// used to zero it for good: 40MB of the Claude Code bundle became NUL bytes
// and its own JavaScript failed to parse (POWERARM bunbytes).
//
// This covers both shapes: the loader's own PT_LOAD, and a file mapping the
// guest makes for itself at an offset the host cannot represent.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern unsigned char BigBlob[];
extern unsigned char BigBlobEnd[];

static unsigned char Expected(size_t Index) {
  return (unsigned char)((Index % 65536) % 251 + 1);
}

// Report the state of [From, To) as one line: intact, or where and how it went
// wrong. Offsets are relative to the blob so the line does not name an address.
static void Report(const char* What, size_t From, size_t To) {
  size_t Zeros = 0, Wrong = 0;
  long First = -1;
  for (size_t i = From; i < To; ++i) {
    if (i % 65536 < 8) {
      continue; // the per-block stamp
    }
    if (BigBlob[i] != Expected(i)) {
      if (First < 0) {
        First = (long)i;
      }
      ++Wrong;
      if (BigBlob[i] == 0) {
        ++Zeros;
      }
    }
  }
  printf("%s %-22s first_bad=%ld wrong=%zu zero=%zu\n", Wrong ? "FAIL" : "PASS", What, First, Wrong, Zeros);
}

int main(void) {
  const size_t Size = (size_t)(BigBlobEnd - BigBlob);
  printf("blob_size=%zu\n", Size);
  Report("loaded", 0, Size);

  // Advise away a large, page-aligned span in the middle and read it back.
  // 64K covers every page size this runs on, and the span is big enough that
  // the kernel really does drop it.
  const size_t Align = 65536;
  const uintptr_t Base = (uintptr_t)BigBlob;
  const uintptr_t Start = (Base + Size / 4 + Align - 1) & ~(uintptr_t)(Align - 1);
  const uintptr_t Stop = (Base + Size / 4 * 3) & ~(uintptr_t)(Align - 1);
  const size_t From = (size_t)(Start - Base), To = (size_t)(Stop - Base);
  if (madvise((void*)Start, (size_t)(Stop - Start), MADV_DONTNEED) != 0) {
    perror("madvise");
    return 2;
  }
  (void)To;
  Report("after_dontneed", 0, Size);

  // The same thing for a mapping the guest makes itself, at a file offset that
  // the guest page size reports, which is what a portable program uses.
  int FD = open("/proc/self/exe", O_RDONLY);
  if (FD < 0) {
    perror("open");
    return 2;
  }
  struct stat Stat;
  if (fstat(FD, &Stat)) {
    perror("fstat");
    return 2;
  }
  const size_t MapLength = 1 << 20;
  const size_t MapOffset = (size_t)sysconf(_SC_PAGESIZE);
  unsigned char* Map = mmap(NULL, MapLength, PROT_READ | PROT_WRITE, MAP_PRIVATE, FD, (off_t)MapOffset);
  if (Map == MAP_FAILED) {
    perror("mmap");
    return 2;
  }
  unsigned char Before[4096];
  memcpy(Before, Map, sizeof Before);
  Map[0] ^= 0xff; // a private modification the advice is entitled to drop
  if (madvise(Map, MapLength, MADV_DONTNEED) != 0) {
    perror("madvise map");
    return 2;
  }
  printf("%s mapped_after_dontneed\n", memcmp(Map, Before, sizeof Before) == 0 ? "PASS" : "FAIL");
  munmap(Map, MapLength);
  close(FD);
  return 0;
}
