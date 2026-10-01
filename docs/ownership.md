# Ownership rules

This is the reference for who owns what in AstraDB. It is not a summary of the
headers; it is the set of rules the headers apply, written once so that they
cannot drift apart.

The engine is written in C, which has no ownership model. That is the whole
reason this document exists: in C, "who frees this?" has no compiler-enforced
answer, so it has to be written down and reviewed.

## The rules

### 1. Every allocation is owned by exactly one thing

There are three owners, and only these three:

| Owner | Meaning | Freed with |
| --- | --- | --- |
| The **caller** | Returned by a function; the caller decides when it dies | `astra_dealloc` |
| The **library** | Allocated internally and kept; the caller has no pointer | automatically |
| The **caller and only the caller**, permanently | Static or string-literal storage | never |

If you cannot say which of the three applies to a pointer, the design is wrong.
Fix the design before writing the code.

### 2. Nothing is allocated behind the caller's back

No API in AstraDB allocates memory that the caller cannot see, reach or free.
A function that allocates always says so, in its own header, in the word
"Ownership".

If a function needs a large temporary, it uses its own stack frame or borrows
caller storage. It does not quietly call `astra_alloc` and leak the result.

### 3. Everything goes through the allocator

`<stdlib.h>`'s `malloc`, `calloc`, `realloc` and `free` appear exactly once in
the library, in `src/core/allocator.c`. Everywhere else uses `astra_alloc`,
`astra_alloc_zeroed`, `astra_realloc` and `astra_dealloc`.

This is what makes the allocator replaceable later. It is a rule about the
source tree, not about linking, so it is enforced by review and by the fact
that `allocator.h` is the header you include.

### 4. A pointer is released with the allocator that produced it

Memory from `astra_alloc` is released with `astra_dealloc`, never with `free`.
The two differ in debug builds, where each block carries a size header.

Installing a replacement allocator invalidates every pointer obtained from the
previous one. All of them must be released first.

### 5. Strings returned by the library are borrowed and immortal

`astra_status_name`, `astra_status_description`, `astra_error_category_name`,
`astra_log_level_name`, `astra_version` and `ASTRA_DB_NAME` all return static
storage owned by the library.

- Never free them.
- Never modify them.
- Never assume a new call returns a new pointer; the same pointer may come back
  every time, which is what makes them cheap.
- They are valid for the lifetime of the process.

The same is true of `astra_error::subsystem` and `astra_error::message`: they
are borrowed static strings, so creating, copying and clearing an `astra_error`
never allocates. That is deliberate. An error type that allocated would need
memory in order to report that there is no memory.

Strings the library hands *back* to it are copied, not retained:
`astra_config_set_data_dir` copies the path into the struct, so the caller's
buffer can be freed immediately.

### 6. Caller-owned structs stay caller-owned

`astra_config` and `astra_error` are values. The library never keeps a pointer
to one, never mutates one it was given, and never frees one. A stack temporary
and a heap block are equally correct.

Every setter validates before it mutates. A setter that rejects its input leaves
the struct byte for byte as it was, so a caller walking a list of overrides can
stop at the first failure without having left the object half applied.

### 7. Output buffers belong to the caller

`astra_error_format` and `astra_config_describe` write into a buffer the caller
supplies and allocate nothing.

Both follow `snprintf` semantics exactly: they return the length the full output
would have needed, excluding the NUL. A return value greater than or equal to
the buffer size means the output was truncated, and the buffer is still a valid
NUL terminated string. A negative return means the output could not be encoded.

Never pass a buffer without checking the return value against the size.

### 8. NULL is allowed only where the header says so

Each parameter's documentation says whether NULL is accepted, and it is not the
same everywhere:

- `astra_dealloc(NULL)` is a no-op, so cleanup paths need no guard.
- `astra_strdup(NULL)` and `astra_strndup(NULL, n)` return NULL.
- `astra_error_is_ok(NULL)` and `astra_error_reset(NULL)` are safe.
- `astra_error_log(NULL)` does nothing.
- Every `astra_config` setter rejects a NULL `cfg`.
- Every `astra_error` and `astra_log_level` function that takes an output
  parameter rejects NULL.

Where NULL is not allowed, the function reports `ASTRA_ERR_INVALID_ARGUMENT`
rather than crashing. That is the general rule: **a NULL required argument is a
reportable error, not undefined behaviour.**

### 9. The library never calls `exit`, `abort` or `assert` on a caller-visible path

`astra_shutdown` failing is returned as `ASTRA_ERR_NOT_INITIALIZED`, not
asserted. A library that terminates its host process is a library that cannot be
tested.

The single exception is a memory management error inside the allocator itself,
which cannot be reported through a destructor and must not be papered over.
There is currently no such path, because debug tracking deliberately does not
attempt to detect foreign pointers; see the comment in `allocator.c`.

### 10. Failures never leave a partially modified object

A function either completes or does nothing observable. `astra_config_set_*`
returns a status and leaves the struct alone on failure. `astra_realloc` returns
NULL with the original block still owned by the caller and still valid. An
`astra_error` is written only after its inputs validate.

### 11. Resources that persist are explicit about their lifetime

A thing that owns an operating system resource has exactly one owner, names it,
and releases it in one place. The Disk Manager is the first such object:

| Resource | Owned by | Acquired | Released |
| --- | --- | --- | --- |
| The open file handle on `main.db` | One `astra_disk_manager` | `astra_disk_manager_create` / `_open` | `astra_disk_manager_close` |
| A copy of the resolved path | One `astra_disk_manager` | with the handle | `astra_disk_manager_close` |
| The one-page header buffer | The library, briefly | `astra_file_create` / `_open` | before those return |

The rules that follow from that:

* **A manager is single owner of its file.** Two managers may not name the same
  database, and the library does not coordinate them if a caller opens one twice
  on purpose. One handle, one owner, no sharing, no reference counting.
* **A manager is not thread safe.** It holds one file position and one cached
  page count. The buffer manager that will sit on top of this is the thing that
  will serialise access; until it exists, a manager is used by one thread.
* **`astra_disk_manager_close` is the only release.** There is no
  `astra_disk_manager_free`. Closing syncs first and reports a failure to sync,
  so a manager that could not be persisted is not silently forgotten.
* **A manager that was not created does not exist.** Every entry point writes
  `*out_manager` before it can fail, so a failed `create` or `open` leaves NULL
  rather than a half-built object to clean up. There is no partially constructed
  manager to leak.
* **`astra_disk_manager_create` may create the data directory.** The directory is
  created one level deep and owned by the file system, not by the library: the
  library removes nothing on close, including the file it just made if the write
  of the header failed. A failed create removes its own half-built file, because a
  file that can never be opened is not a database, but it leaves the directory.
* **A Disk Manager under a Buffer Pool is used by one thread at a time.** The
  manager holds one file position and one cached page count and has no internal
  synchronisation. The Buffer Pool's own latch is what serialises access, and it is
  always released before a manager call, so the manager never sees two threads at
  once. A caller that uses a manager directly *and* hands it to a pool is using it
  concurrently with itself.

The Buffer Pool is the second object that owns a persistent resource, so the same
questions apply:

| Resource | Owned by | Acquired | Released |
| --- | --- | --- | --- |
| Every frame buffer | One `astra_buffer_pool` | `astra_buffer_pool_create` / `_open` | `astra_buffer_pool_destroy` |
| The page table and retired set | One `astra_buffer_pool` | with the frames | `astra_buffer_pool_destroy` |
| The registry slot | The library, once per manager | `astra_buffer_pool_open` | `astra_buffer_pool_destroy` |

* **`astra_buffer_pool_destroy` is the only release**, and it writes the dirty
  frames first. A pool destroyed without its writes flushed is not a pool that
  forgot something; that is why there is no `_free` and no `_close` that skips
  the flush.
* **A pool is thread safe; the manager it borrows is not.** That asymmetry is
  deliberate and is the reason a pool takes a borrowed `astra_disk_manager *`
  rather than creating one: the pool is the synchronisation layer that the manager
  lacks.
* **A pool borrows its manager and never frees it.** Closing a pool leaves the
  caller's manager exactly as it was.
* **One pool per manager.** The registry exists so that a second pool over the
  same file is refused rather than silently allowed to run two independent caches
  over one file. There is no reference counting and no shared cache.

### 12. A borrowed page pointer is valid exactly as long as its pin

`astra_buffer_pool_fetch_page` and `astra_buffer_pool_new_page` return an
`astra_page *` that the pool owns. The caller does not own it and must not release
it.

```c
astra_page *page = NULL;
if (astra_buffer_pool_fetch_page(pool, id, &page) == ASTRA_OK) {
    /* `page` is readable and writable here, and only here. */
    page_fill(page, id, 7u);
}
if (page != NULL) {
    /* Exactly one unpin per successful fetch, no matter which path got here. */
    (void)astra_buffer_pool_unpin_page(pool, id, true);
}
```

- **One pin per fetch, one unpin per pin.** A fetch that succeeds and is never
  unpinned leaks the frame for the pool's lifetime, and eventually every fetch
  fails with `ASTRA_ERR_INVALID_STATE`.
- **Unpinning is what may invalidate the pointer.** Any eviction can take the
  frame the instant the count reaches zero. After `astra_buffer_pool_unpin_page`
  returns, the pointer is dangling and reading it is undefined behaviour.
- **Unpinning does not require the latch, and does not block.** A caller that
  never unpins cannot make another caller's `flush_all` hang.
- **The `dirty` argument is the caller's promise about its own writes.** Passing
  `false` for a page that was modified discards the modification at the next
  eviction, by contract rather than by accident.
- **A fetch that returns an error returns no page.** `*out_page` is NULL and no
  unpin is owed.

### 13. Process-global state is enumerated and justified

There are exactly five pieces of mutable global state, all documented at their
definition:

| Global | File | Justification | Thread safety |
| --- | --- | --- | --- |
| `g_initialized` | `astra_core.c` | One process is one database session | C11 atomic |
| `g_min_level` | `log.c` | The log level changes while the server runs | C11 atomic |
| `g_ops` | `allocator.c` | A process cannot sensibly have two allocators | Not atomic; install at startup |
| `g_sink` / `g_sink_context` | `log.c` | Test-only redirection; private header | Not atomic; set before concurrency |
| `g_pool_owners` | `buffer_pool.c` | Fixed-size slot table enforcing one pool per Disk Manager | C11 atomics; claim is a CAS |

The last three are deliberate, documented exceptions to the project's "no hidden
global mutable state" principle. `g_pool_owners` is the only one that participates
in concurrency, and it is a fixed-size array of atomics rather than a list so that
claiming a slot is a single compare-and-swap with no lock and no allocation.


## Ownership in the storage module

`astra_page` is a value with a heap buffer inside it, and it is the one public
struct in AstraDB that owns memory.

```c
/* The page owns `data`; the Disk Manager never keeps a pointer to either. */
astra_page page = ASTRA_PAGE_INIT;

if (astra_page_init(&page, 16384) != ASTRA_OK) {
    return ASTRA_ERR_OUT_OF_MEMORY;
}
page.page_id = id;
memcpy(page.data, record, sizeof record);
page.is_dirty = true;

if (astra_disk_manager_write_page(db, &page) != ASTRA_OK) {
    /* `page.data` is still the caller's to release. */
}
astra_page_release(&page);          /* now the page owns nothing */
```

- **The caller owns a page, and only the caller releases it.** Neither
  `astra_disk_manager_read_page` nor `astra_disk_manager_write_page` retains the
  page, and neither allocates. A caller may reuse one page buffer across every
  read and write, which is the point: the buffer manager will hold many at once.
- **`astra_page_release` is idempotent and NULL tolerant.** It resets the struct
  to `ASTRA_PAGE_INIT`, so the same page can be released by a cleanup path and
  again by the code that created it without a leak or a double free.
- **Re-initialising a live page leaks.** `astra_page_init` overwrites the struct
  without looking at it. Release first.
- **`is_dirty` belongs to the caller.** The Disk Manager clears it after a
  successful write and never sets it. It is a hint, not a contract, and no
  subsystem consults it yet.
- **`ASTRA_PAGE_INIT` is the only way to declare a page.** It is a brace
  initialiser, so it works as a file-scope object, a local, or a member, and it
  cannot drift out of step with the struct.

The file's own header is the one buffer the caller never sees. It is allocated,
filled, verified and released inside `database_file.c`, and a manager exposes it
only as a readable, non-writable page 0. Nothing above this layer can corrupt it
by writing to it, and every open re-verifies it before any other page is touched.

Two ownership rules that come from identifiers rather than memory:

- **A page identifier is owned by the file, not by the caller.** The Disk Manager
  allocates it and the caller uses it; no caller frees it and no caller recycles
  one.
- **Truncation retires identifiers permanently.** Shrinking a database does not
  make the identifiers above the new end available again. The memory behind them
  may already be recorded somewhere a reader will find it, so reuse would be a
  correctness bug rather than an optimisation.

## Working memory ownership, by example

```c
/* The caller owns the copy. The literal is untouched. */
char *name = astra_strdup("wal");
if (name == NULL) {
    return ASTRA_ERR_OUT_OF_MEMORY;      /* allocation failure is reportable */
}
astra_log_info("wal", "opened %s", name);
astra_dealloc(name);
return ASTRA_OK;
```

```c
/* Borrowed: valid for the process, never freed. */
const char *code = astra_status_name(status);
```

```c
/* Caller owned value, copied on the way in. */
astra_config config;
if (astra_config_init(&config) != ASTRA_OK) {
    return ASTRA_ERR_INTERNAL;
}
if (astra_config_set_data_dir(&config, argv[1]) != ASTRA_OK) {
    return ASTRA_ERR_INVALID_ARGUMENT;
}
```

## Review checklist

Adding a public function? Confirm each of these before it merges:

- [ ] Does the doc comment state who owns any returned memory?
- [ ] Is NULL explicitly declared allowed or not allowed, per parameter?
- [ ] Are valid ranges stated, rather than left to be inferred?
- [ ] Is the error behaviour stated: which status, and what state is left behind?
- [ ] Is every return value checked by the caller, or explicitly discarded with `(void)`?
- [ ] Does it allocate through `astra_*` only?
- [ ] Does it leave the caller's objects unmodified on failure?
- [ ] If it owns an operating system resource, is there exactly one place that releases it?
- [ ] If it takes ownership of something, does the header say so in the word "Ownership"?

## See also

* `include/astra/core/allocator.h` - the heap, and the allocation rules
* `include/astra/core/error.h` - the error type, and why it does not allocate
* `include/astra/core/config.h` - the configuration value type
* `include/astra/core/log.h` - logging, and the borrowed-string contract
* `include/astra/storage/format.h` - the on-disk layout, and why it is checksummed
* `include/astra/storage/page.h` - the page value, and who releases its buffer
* `include/astra/storage/disk_manager.h` - the manager, and when it syncs
* `src/core/log_internal.h` - the private log sink seam used by the tests
* `README.md` - the development principles these rules implement