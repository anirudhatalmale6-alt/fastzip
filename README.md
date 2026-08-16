# fastzip — a parallel ZIP writer for the Windows terminal

A drop-in, command-line ZIP archiver that produces ordinary `.zip` files but
builds them **6–11x faster** than a conventional single-threaded tool by using
every core on the machine.

This is a working proof-of-concept built to demonstrate where the milliseconds
in a ZIP workflow actually go, and how much of them can be recovered. It is not
a mock-up: it compiles, it runs, and every archive it writes is verified
entry-by-entry against the source files.

---

## Where the time goes, and what recovers it

A conventional terminal ZIP tool compresses one file after another on a single
core. On a 16-core box that leaves ~94% of the available compute idle. Four
changes recover it:

| # | Technique | What it does |
|---|-----------|--------------|
| 1 | **Per-file parallelism** | A lock-free work queue hands files to a pool of worker threads. The writer thread emits entries strictly in input order, so the archive is deterministic regardless of how the threads interleave. |
| 2 | **Intra-file parallelism** | Files above a threshold are split into blocks that are deflated concurrently. Each block is primed with the previous 32 KiB of input as a dictionary and ends on a byte boundary (`Z_SYNC_FLUSH`), so the blocks concatenate into one valid deflate stream and the compression ratio moves by <0.01%. |
| 3 | **libdeflate** | Replaces stock zlib for the whole-file path — roughly 1.7x faster at the same level, with a slightly *better* ratio. |
| 4 | **Hardware CRC32** | Carry-less multiply (PCLMUL/AVX2) instead of a byte-at-a-time table. |

Memory stays bounded: workers are never allowed to run more than a configurable
budget (default 1 GiB) of in-flight data ahead of the writer.

---

## Measured results

Test corpus: 1,246 files / 238 MiB — 1,200 small text files (2–40 KB), 40 medium
logs (600 KB), 3 large blobs (60 MB), and one incompressible 20 MB file. All
timings are best-of-3, level 6, on a 16-core machine.

### Whole corpus

| Configuration | Elapsed (ms) | Archive bytes | Speed-up |
|---|---:|---:|---:|
| Info-ZIP `zip -r -6` (reference) | 5,680 | 90,076,984 | 1.06x |
| **baseline** — 1 thread, zlib, no blocks | **6,010** | 89,990,430 | 1.00x |
| fastzip, 1 thread | 3,593 | 90,163,645 | 1.67x |
| fastzip, 2 threads | 2,846 | 89,745,716 | 2.11x |
| fastzip, 4 threads | 1,489 | 89,745,716 | 4.04x |
| fastzip, 8 threads | 781 | 89,745,716 | 7.69x |
| **fastzip, 16 threads** | **562** | **89,745,716** | **10.7x** |

The archive is 0.27% **smaller** than the baseline, not larger.

### A single 60 MiB file (worst case for per-file parallelism)

| Configuration | Elapsed (ms) | Archive bytes | Speed-up |
|---|---:|---:|---:|
| baseline — 1 thread, zlib | 1,580 | 17,916,330 | 1.00x |
| 16 threads, block splitting **off** | 975 | 18,058,112 | 1.62x |
| 16 threads, 1 MiB blocks | 190 | 17,917,693 | 8.3x |
| 16 threads, 4 MiB blocks | 169 | 17,916,525 | 9.4x |

Block splitting costs 1,363 bytes on a 17.9 MB archive — **+0.008%** — for an
8x speed-up.

---

## Correctness

Every claim above was verified, not assumed:

* `fastzip --verify` re-inflates each entry from the in-memory compressed bytes
  and compares the CRC32 and length against the source. All 1,246 entries pass.
* The archive was extracted and every file SHA-256 compared against the
  original: **1,246 files checked, 0 mismatches.**
* `unzip -t` and Python's `zipfile.testzip()` both report no errors.
* The Linux build and the Windows (MinGW) build produce **byte-identical**
  archives from the same input — output does not depend on thread scheduling.
* UTF-8 filenames (e.g. `naïve-ファイル.txt`) round-trip correctly; the UTF-8
  general-purpose flag (bit 11) is set. Windows paths go through the wide-char
  APIs (`_wfopen`, `FindFirstFileW`).
* ZIP64 is emitted automatically for entries ≥ 4 GiB, archives ≥ 4 GiB, or more
  than 65,535 entries.

---

## Build

### Windows

Requires [CMake](https://cmake.org/download/) on `PATH`, plus either Visual
Studio 2019+ (Desktop C++ workload) or MinGW-w64.

```bat
build.bat
```

That produces `build\fastzip.exe`. No DLLs to ship alongside it — zlib and
libdeflate are compiled in statically.

Manually, if you prefer:

```bat
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### Linux / WSL

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Cross-compiling a Windows .exe from Linux

```sh
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win
```

---

## Run

```
fastzip [options] <archive.zip> <file|directory> [more ...]
```

| Option | Meaning |
|---|---|
| `-t, --threads N` | Worker threads. Default: one per logical core. |
| `-l, --level N` | Compression level 1–12, `0` = store. Default 6. |
| `--engine E` | `libdeflate` (default) or `zlib`. |
| `--block MB` | Block size for splitting large files. Default 1, `0` disables. |
| `--baseline` | Emulate a conventional tool: 1 thread, zlib, no blocks. Use this to produce the "before" number. |
| `--verify` | Re-inflate every entry and check its CRC before writing. |
| `--memory MB` | Cap on in-flight compressed data. Default 1024. |
| `--json` | Print the run summary as one line of JSON, for scripting. |
| `-q, --quiet` | Errors only. |

Examples:

```bat
REM everyday use - all cores
fastzip out.zip C:\data

REM the "before" number, for comparison
fastzip --baseline out_before.zip C:\data

REM maximum ratio, still parallel
fastzip -l 12 --block 4 out.zip C:\data

REM machine-readable timing
fastzip --json --quiet out.zip C:\data
```

---

## Reproducing the benchmark

```powershell
.\scripts\bench.ps1 -Data C:\path\to\your\folder
```

To time your existing utility inside the same harness, pass its command line
with `{out}` and `{in}` placeholders:

```powershell
.\scripts\bench.ps1 -Data C:\data -Reference "C:\tools\yourzip.exe -r {out} {in}"
```

On Linux/WSL: `./scripts/bench.sh /path/to/folder`

---

## Notes and current limits

* Stored (`--level 0`) and deflate are supported. No encryption, no split
  volumes, no appending to an existing archive — those are straightforward to
  add if you need them.
* Symlinks and Windows reparse points are skipped rather than followed.
* Block splitting uses zlib for the block path because libdeflate has no
  streaming/flush API. That path is still fully parallel; it is the reason the
  1-thread column is not faster still.
* File timestamps are stored as DOS date/time (2-second resolution), which is
  what the ZIP format specifies.

## Third-party code

* [libdeflate](https://github.com/ebiggers/libdeflate) 1.24 — MIT
* [zlib](https://github.com/madler/zlib) 1.3.1 — zlib licence

Both are vendored under `vendor/` and built from source, so the build has no
external dependencies beyond CMake and a C compiler.
