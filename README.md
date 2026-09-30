# AstraDB

AstraDB is a new, high-performance enterprise relational database management
system written entirely in C. The long-term goal is a system with capabilities
comparable to Oracle and PostgreSQL, exposed through a much simpler and cleaner
SQL-like language.

**This repository is at the very beginning of the project.** It currently
contains the build system, the public API skeleton and the low-level core
infrastructure that every later subsystem will sit on: a structured error type,
an allocation abstraction, logging, basic types and process configuration. No
storage, SQL, transactions, WAL, indexing or networking exists yet.

## Current status

Milestone 1 - core infrastructure.

| Area | State |
| --- | --- |
| Build system (CMake, C17) | done |
| Warning policy (GCC/Clang, MSVC), `-Werror` | done |
| Debug / Release / RelWithDebInfo configurations | done |
| AddressSanitizer + UndefinedBehaviorSanitizer builds | done |
| Structured errors (`astra_status`, `astra_error`) | done |
| Allocation abstraction with debug tracking | done |
| Logging (6 levels, timestamps, subsystems, configurable threshold) | done |
| Basic types (`uint8`-`int64`, `page_id_t`, `txn_id_t`, `lsn_t`) | done |
| Process configuration (`astra_config`) | done |
| Unit tests + CTest integration | 25 groups, 672 checks |
| Ownership documentation | [`docs/ownership.md`](docs/ownership.md) |
| Storage engine, SQL, transactions, WAL, indexes | not started |

Layout:

```
astra-db/
├── CMakeLists.txt          top level build
├── cmake/                  build system modules (compiler warnings, sanitizers)
├── include/astra/          public API headers
│   ├── astra.h               umbrella header
│   ├── version.h             single source of truth for the version
│   └── core/                 public core module headers
│       ├── allocator.h         every heap allocation goes through this
│       ├── config.h            process configuration value type
│       ├── error.h             status codes, categories, astra_error
│       ├── log.h               logging
│       └── types.h             fixed width aliases, page_id_t, txn_id_t, lsn_t
├── src/
│   ├── main.c              CLI entry point
│   └── core/               library internals
│       ├── astra_core.c       library lifecycle
│       ├── allocator.c        the heap
│       ├── config.c           configuration validation
│       ├── error.c            error tables and astra_error
│       ├── log.c              logging
│       ├── types.c            identifier predicates
│       ├── astra_internal.h   private: shared internal helpers
│       └── log_internal.h     private: log sink seam, used by tests
├── tests/                  unit tests, one file per module
├── docs/                   ownership rules
├── benchmarks/             reserved, empty
└── tools/                  reserved, empty
```

## The core modules

### Errors

No exceptions anywhere. Every fallible function returns an `astra_status`, and
functions that want to describe *why* also fill in a caller-owned `astra_error`
carrying a code, a subsystem name and a human-readable message.

An `astra_error` holds **borrowed** pointers to static strings, so creating or
clearing one never allocates. That matters: an error type that needed memory
would fail exactly when memory is exhausted.

```c
astra_error err;

if (load_page(&page, id) != ASTRA_OK) {
    astra_error_log(&err);        /* one line, at ERROR level */
    return astra_error_make(&err, ASTRA_ERR_CORRUPTION, "page", "checksum mismatch");
}
```

Ten categories are defined: success, invalid argument, out of memory, I/O,
invalid state, not found, already exists, corruption, unsupported and internal.
Categories let a caller treat related codes alike without enumerating them.

### Memory

`malloc`, `calloc`, `realloc` and `free` appear exactly once in the library, in
`allocator.c`. Everything else uses `astra_alloc`, `astra_alloc_zeroed`,
`astra_realloc` and `astra_dealloc`, which is what makes the heap replaceable
later.

In debug builds each block carries a size header and the library tracks live
blocks and live bytes, so a leak is a test failure rather than a slow memory
growth noticed months later. In release builds that code does not exist at all:
no header, no counter, no branch.

This is **not** a pool. There is no free list and no bump allocator; those belong
with the buffer manager, which will know what block sizes it needs.

### Logging

Six levels from `TRACE` to `FATAL`, plus `NONE` to silence output. Each line
carries a UTC timestamp, the level, the subsystem and the source location:

```
2026-09-30T19:35:47.674Z INFO  [main] main.c:48: AstraDB 0.1.0 ready
```

The threshold is configurable at runtime through one atomic. Records below it
are discarded before any formatting happens, so raising the level makes logging
genuinely cheap rather than merely quieter. Output is synchronous; there is no
queue and no background thread yet.

### Types

`uint8` through `int64` are width-checked aliases of `<stdint.h>`, with
`_Static_assert`s pinning the sizes so a non-conforming platform fails at
compile time. `page_id_t`, `txn_id_t` and `lsn_t` are all 64 bits and
documented, with `ASTRA_*_INVALID` sentinels.

They deliberately carry no database semantics yet. No allocation policy, no
ordering guarantee, no persistence format.

### Configuration

A plain value type holding the data directory, the log level and the page size,
with documented defaults. Every setter validates before it mutates, so a
rejected setting leaves the configuration untouched. There is no configuration
file format yet; adding one later means adding a function that fills this
struct.

## Build instructions

Requirements: CMake 3.20 or newer and a C17 compiler (GCC, Clang, or MSVC).
Linux is the primary production target; Windows is supported for development.

```sh
cmake -S . -B build
cmake --build build
```

The default configuration is `Release`. To build `Debug` instead:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug
```

On Windows with Visual Studio, append `--config Debug` to the build command as
well, because the generator is multi-configuration.

Run the CLI:

```sh
./build/astra          # Linux
build\Debug\astra.exe  # Windows, Debug
```

Expected output:

```
AstraDB 0.1.0
configuration: data_dir=data log_level=INFO page_size=4096
Skeleton build: no storage engine, query engine or server yet.
2026-09-30T19:35:47.674Z INFO  [main] main.c:48: AstraDB 0.1.0 ready
```

### Options

| Option | Default | Effect |
| --- | --- | --- |
| `ASTRA_BUILD_TESTS` | `ON` | Build the test executable and register it with CTest |
| `ASTRA_WERROR` | `ON` | Treat compiler warnings as errors |
| `ASTRA_SANITIZE` | `none` | One of `none`, `address`, `undefined`, `address+undefined` |

The version lives in exactly one place, `include/astra/version.h`. CMake parses
that header, so the project version and the public headers can never disagree.

### Sanitizers

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DASTRA_SANITIZE=address+undefined
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

`ASTRA_SANITIZE=address+undefined` is supported on GCC and Clang.
MSVC offers AddressSanitizer only, so on Windows use `-DASTRA_SANITIZE=address`.

Note that LeakSanitizer is part of AddressSanitizer on Linux and macOS but not
on Windows, where MSVC has no leak detector. The test suite compensates on all
platforms by asserting that the allocator's live block count returns to its
starting value.

## Test instructions

```sh
cmake -S . -B build -DASTRA_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The suite can also be run directly:

```sh
./build/astra_tests
```

`astra_tests` prints one line per group and exits non-zero if any check failed.
The harness flushes after every group, so a crash part way through a run still
shows how far it got.

There is one test binary, registered once. Whether it is sanitized is decided by
`ASTRA_SANITIZE` at configure time, so CI can filter with
`ctest -L sanitizer`.

## Development principles

These are the rules the codebase is built under. They are not aspirations; new
code is expected to follow them.

* **C only.** The engine is written in C. No C++.
* **C17 baseline.** No compiler extensions (`C_EXTENSIONS OFF`).
* **Portability.** Linux is the primary production target, Windows is a
  supported development target. No platform assumptions outside clearly
  isolated code.
* **No unnecessary dependencies.** The core engine has none. Third-party code
  is allowed only for clearly non-core functionality and must be justified.
* **Own the fundamentals.** The storage engine, transaction system, WAL, MVCC,
  indexing and query engine are implemented by this project, not taken from a
  library.
* **No premature features.** Advanced functionality is added when the
  subsystem it depends on actually exists. No placeholder classes, no fake
  implementations, no speculative abstractions.
* **No premature optimisation.** Write the straightforward version first.
  Measure before optimising.
* **Clear ownership and lifetimes.** Every subsystem documents what it owns and
  for how long. See [`docs/ownership.md`](docs/ownership.md).
* **No hidden global mutable state.** The exceptions are enumerated and
  justified in `docs/ownership.md`; there are four, and each one names its
  thread-safety contract.
* **No hidden allocations.** Allocation is visible at the call site; functions
  that allocate say so and state who frees the result.
* **Explicit error handling.** No silent failures, no ignored return values.
  Errors are returned as values, not thrown or printed from the library.
* **Obvious memory ownership.** Every public API documents who owns returned
  memory: the caller frees it, or the library retains it.
* **Zero warnings.** `ASTRA_WERROR` is on by default. The warning set includes
  `-Wconversion`, `-Wsign-conversion`, `-Wcast-qual`, `-Wwrite-strings`,
  `-Wswitch-enum` and `-Wmissing-prototypes`.

## Roadmap

Deliberately coarse. Milestones are sequenced so that each one is testable on
its own.

1. **Bootstrap** - build system, layout, version handling, API skeleton.
   *(done)*
2. **Memory and error foundation** - errors, allocator, logging, basic types,
   configuration. *(done)*
3. **Page and file layer** - page abstraction, buffered file I/O, checksums.
   *(next)*
4. **WAL** - write-ahead log, log records, flush and recovery protocol.
5. **Storage engine** - heap tables, tuples, free space management, crash
   recovery driven by the WAL.
6. **Transactions and MVCC** - transaction identifiers, visibility rules,
   snapshot reads.
7. **B+Tree indexes** - node layout, page splits and merges, cursors.
8. **SQL front end** - parser and a clean SQL-like language.
9. **Query planner and optimizer** - rule based, then cost based.
10. **Advanced SQL** - views, stored procedures, functions, triggers.
11. **JSON and full-text search.**
12. **Partitioning, replication, sharding.**
13. **Security** - authentication, authorization, auditing, encryption.
14. **Operations** - backup and restore.

## License

MIT. See [LICENSE](LICENSE).