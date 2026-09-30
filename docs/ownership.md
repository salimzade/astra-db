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

Nothing in AstraDB owns a file handle, a lock or a thread at this phase. When
something does, its constructor takes an owner and its destructor names what it
releases. An object with no documented destructor is not finished.

### 12. Process-global state is enumerated and justified

There are exactly four pieces of mutable global state, all documented at their
definition:

| Global | File | Justification | Thread safety |
| --- | --- | --- | --- |
| `g_initialized` | `astra_core.c` | One process is one database session | C11 atomic |
| `g_min_level` | `log.c` | The log level changes while the server runs | C11 atomic |
| `g_ops` | `allocator.c` | A process cannot sensibly have two allocators | Not atomic; install at startup |
| `g_sink` / `g_sink_context` | `log.c` | Test-only redirection; private header | Not atomic; set before concurrency |

The last two are deliberate, documented exceptions to the project's "no hidden
global mutable state" principle. If you add a fifth, expect to justify it in the
same place.

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

## See also

* `include/astra/core/allocator.h` - the heap, and the allocation rules
* `include/astra/core/error.h` - the error type, and why it does not allocate
* `include/astra/core/config.h` - the configuration value type
* `include/astra/core/log.h` - logging, and the borrowed-string contract
* `src/core/log_internal.h` - the private log sink seam used by the tests
* `README.md` - the development principles these rules implement