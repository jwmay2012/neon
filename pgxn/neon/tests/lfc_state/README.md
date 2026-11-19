# LFC manifest capture regressions

The manifest is a best-effort list of pages to fetch during prewarming. Capture
does not need a point-in-time view, but must keep memory access and serialization
valid while allowing ordinary cache operations to run.

`lfc_get_state()` copies at most 128 entries under each shared lock hold. It
resumes by looking up copied keys, never by following a pointer retained across
unlock. A short lookahead tolerates pinned or evicted resume entries. Allocation,
per-page validation, bitmap construction and final compaction run unlocked.

Concurrent LRU changes may omit or repeat hints. If every resume key disappears,
or the cache is disabled/recreated, capture returns the valid prefix already
collected. Work is bounded by the initial entry count. The existing manifest
format, maximum-entry argument and tag validation remain unchanged. A quiescent
cache retains its complete, most-recent-first ordering.

## Run locally

Requires Zsh, a C compiler, and PostgreSQL 16 server binaries and development
headers. Run as a non-root user:

```zsh
zsh pgxn/neon/tests/lfc_state/run.zsh /path/to/pg_config
```

The runner builds a test module and starts a disposable PostgreSQL instance on
a private Unix socket, with TCP disabled. It stops the server on exit and prints
the directory containing logs. Nothing is installed into the supplied PostgreSQL
installation. No pageserver, database dataset, credentials or cloud resources
are needed.

The module compiles the snapshot function, entry declarations, wire-format
macros and tag validator extracted directly from the production source. The
fixture supplies synthetic cache metadata using real PostgreSQL shared hash
tables, intrusive lists and LWLocks; it does not emulate page reads or prove a
complete Neon restart/prewarm cycle.

Regression cases cover every supported chunk width, partial-byte bitmaps,
entry limits, empty/shrinking caches, pinned/evicted/reused resume entries,
LRU movement, generation changes, disablement, cancellation and invalid tags.
Deterministic interleavings run after unlock. The churn case requires at least
90% unique coverage; this is a fixture assertion, not a completeness guarantee
under arbitrary concurrent activity.

## Large-cache comparison

The benchmark seeds 6,291,456 metadata entries and runs a competing backend
acquiring the same lock exclusively. It reports capture time, largest lock hold
and the competing backend's largest wait. It does not allocate six TiB of page
contents. Allow roughly a GiB of shared-memory/data allocation.

```zsh
zsh pgxn/neon/tests/lfc_state/run.zsh /path/to/pg_config 6291456 benchmark
```

Set `LFC_SNAPSHOT_SOURCE` to an unchanged `file_cache.c` to measure the same
fixture against the previous implementation. The normal regression suite must
reject that implementation because it allocates and scans under one lock.
Benchmark timings depend on the machine; correctness tests do not assert a
wall-clock performance threshold.

An isolated PostgreSQL 16 / optimized-build comparison with 6,291,456 entries
produced the following example. Both captures returned all entries; the separate
deterministic churn case retained 63,599 of 65,536 unique hints (97.0%).

| Measurement | Previous capture | Batched capture |
|---|---:|---:|
| Total capture time | 1,204 ms | 1,323 ms |
| Largest snapshot lock hold | 1,204 ms | 0.398 ms |
| Competing backend's largest lock wait | 1,204 ms | 0.292 ms |

The improvement is reader progress during capture, not a reduction in total
snapshot CPU work. Linux and macOS regression runs pass; the unchanged source
fails the allocation-outside-lock assertion.
