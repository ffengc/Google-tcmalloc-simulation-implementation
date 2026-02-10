# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

A simulation implementation of Google's tcmalloc (thread-caching malloc) — a three-tier high-concurrency memory pool in C++11. The architecture follows: **Thread Cache → Central Cache → Page Cache → OS**.

## Build Commands

```bash
# Production benchmark build (32-bit)
make out
./out

# Debug build with colored logging enabled
make debug
./debug

# Unit test build
make unit
./unit

# Clean binaries
make clean
```

All targets compile with `-std=c++11 -lpthread -m32`. Debug/unit builds add `-DPROJECT_DEBUG -g`.

**Prerequisite**: 32-bit libraries must be installed (`gcc-multilib g++-multilib` on Ubuntu/Debian).

## Architecture

Three-tier memory pool with increasing lock granularity:

- **Thread Cache** (`thread_cache.hpp/cc`): Thread-local (via `__thread` TLS), lock-free. 208 free-list buckets for allocations ≤256KB. Uses slow-start algorithm for batch fetching.
- **Central Cache** (`central_cache.hpp/cc`): Singleton with per-bucket mutexes (fine-grained locking). Manages spans cut into fixed-size objects. Returns full spans to page cache.
- **Page Cache** (`page_cache.hpp/cc`): Singleton with global mutex. 129 span lists indexed by page count (8KB pages). Coalesces adjacent free pages. Uses 3-level radix tree (`page_map.hpp`) for page-to-span mapping.

Public API is `tcmalloc(size)` and `tcfree(ptr)` in `tcmalloc.hpp`. Allocations >256KB go directly to Page Cache.

## Key Constants (common.hpp)

- `MAX_BYTES = 256KB` — threshold between thread cache and direct page cache allocation
- `BUCKETS_NUM = 208` — free-list buckets covering 8B-256KB with alignment tiers
- `PAGE_SHIFT = 13` — 8KB page size
- `PAGES_NUM = 129` — page cache span list count

## Size Alignment Strategy

208 buckets with tiered alignment to keep internal fragmentation ≤10%: 8B-aligned (1-128B), 16B-aligned (129-1024B), 128B-aligned (1025-8KB), 1KB-aligned (8KB-64KB), 8KB-aligned (64KB-256KB).

## Platform Support

Cross-platform via conditional compilation (`_WIN32`/`_WIN64` for VirtualAlloc, Linux for mmap). Platform detection in `common.hpp` sets `PAGE_ID` type and `SYS_BYTES`.

## Known Bugs

- ARM64 (Ubuntu): Segfault in multi-threaded scenarios
- ARM64: Only 3rd-level radix tree works (1st/2nd fail)
- Windows 32-bit: Occasional non-deterministic segfaults

## Documentation

- `README.md` / `README-cn.md` — Project overview (English/Chinese)
- `work.md` / `work-cn.md` — Detailed implementation walkthrough (English/Chinese)
