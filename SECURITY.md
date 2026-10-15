# Security Policy

## Status

BitLang is an **experimental esolang interpreter**. It is a learning tool and reference
implementation — not a secure runtime.

The v4 interpreter has been compiled with AddressSanitizer and UBSan and run against a set
of regression scripts covering every fixed bug below. It has not been fuzzed, formally
audited, or hardened against hostile input.

**Do not run untrusted `.bitlang` scripts.** Do not expose the interpreter to untrusted input
in any automated or networked context.

---

## Known Attack Surface

BitLang's design gives scripts broad, intentional access to the host system. The following
features are inherently dangerous and exist by design:

### Shell Execution (`!X`)
`!X` reads the active tape selection (up to the first null byte) as a string and passes it
directly to `system()` or `popen()`, which invokes `/bin/sh -c`. There is no sanitization,
escaping, or sandboxing. Any `.bitlang` script can execute arbitrary shell commands with the
privileges of the interpreter process.

With `!X->!`, the interpreter reads all of its own stdin, writes it to a temporary file in
`/tmp` (created with `mkstemp`, mode 0600), and runs the command with that file as input.
The file is deleted afterwards, but is left behind if the interpreter is killed mid-run.

### Environment Variable Access (`!$VAR`, `!$VAR<-[n:m]`)
Scripts can read any environment variable into the tape or write tape content into the
process environment. This includes sensitive variables such as `PATH`, `HOME`,
`LD_PRELOAD`, API keys, or session tokens present at launch, and it affects every command
started afterwards by `!X`.

### File I/O (`!L`, `!W`, `!O[path]`, `!I[path]`)
Scripts can read any file accessible to the interpreter process and write to any writable
path. Paths are not validated or restricted. `!W[path]` **truncates** the target file;
`!O[path]` **appends** to it.

### Dynamic Evaluation (`!^`)
`!^` reads bytes from the tape and runs them as BitLang source. Combined with `!X` or
`!$VAR`, scripts can construct and execute arbitrary code at runtime. A lexer error in
evaluated text terminates the whole interpreter.

### File Jumps (`%//`)
Scripts can hand off execution to another `.bitlang` file. The interpreter enforces the
`.bitlang` extension but does not restrict the path in any other way. Relative paths are
resolved against the current working directory, not the calling script's directory.

### Resource Exhaustion
There is no overall memory, CPU, or time limit. A script can loop forever, grow the tape
until memory runs out, or start processes with `!X`. The per-operation limits listed below
only stop single runaway operations.

---

## Fixed Implementation Vulnerabilities

### Fixed in v3

- **Unsigned underflow in `sel_resolve` (`SEL_REL`)** — `![n:+0]` is a hard error.
- **Unchecked `malloc`/`calloc`/`realloc`** — all allocations check for NULL and exit with a
  diagnostic.
- **Race in `tape_flip_bit` under Forge** — the read-modify-write happens under one lock.
- **Integer overflow in `read_number`** — literals are parsed into `uint64_t` with overflow
  detection (see the v4 note below: the limit chosen in v3 was too high).
- **Unchecked `write()` in `EXEC_PIPE_IN`** — the full buffer is written in a loop.

### Fixed in v4

- **Endless loop on stray closers** — a top-level `]`, `}`, `|` or `$!` made the interpreter
  spin at 100% CPU. It is now an `unmatched` error.
- **Address wraparound** — v3 accepted numbers up to 9 × 10^18, but byte indices are
  multiplied by 8, so anything above 2^61 wrapped around the 64-bit bit address space
  (`!@2305843009213693952` jumped to bit 0). The limit is now 2^61 − 1, and every head move
  is checked, so the head can no longer wrap past either end of the tape.
- **Huge allocation abort in `!^`** — `!^[5]` and reversed ranges like `[5:2]` underflowed
  the length calculation and tried to allocate about 2^64 bytes. Ranges are now ordered
  and validated, and any selection read into memory (`!X`, `!$VAR<-`, `!^`) is capped at
  256 MiB with a clean error instead of a crash.
- **Unbounded recursion through `%//`** — a script that jumped to itself overflowed the stack
  and segfaulted. File jumps now share the 64-level nesting limit with `!^`.
- **Input loss in `!I`** — a full selection consumed one extra input byte. Fixed.
- **Hex parsing** — `!#F` swallowed the next character and `"\x4"` swallowed the closing
  quote. Hex escapes now take one or two hex digits only, and invalid hex is an error.
- **Silent truncation and parse leniency** — more than 64 Forge segments, unterminated
  strings, and unterminated `[path` arguments are now errors instead of being ignored.
- **Unchecked `pthread_create`** — a failed thread start led to joining a thread that never
  existed. The segment now runs inline with a warning.
- **Script loading** — `load_file` now works on pipes, checks read errors, closes the file on
  every path, and is capped at 256 MiB.
- **Read allocation** — reading never-written tape no longer allocates memory.

---

## Remaining Known Limits and Behaviour

**Per-operation size limits**
`!L` loads are capped at 256 MiB per call, selections read into memory are capped at
256 MiB, and script files are capped at 256 MiB. These are compile-time constants. There is
no cap on the total size of the tape, on stream-mode `!I`, or on `!X->@n` output, which are
bounded only by available memory and tape address space.

**Nesting limit**
`!^` and `%//` share a nesting limit of 64 levels. Exceeding it prints an error and skips
that eval or jump. Breadth is not limited: a script can still run unbounded work at each
level.

**Forge concurrency**
The tape lock is per tape, and each single-bit or single-byte read or write is atomic.
Operations that touch many bytes (string literals, selection I/O, `!L`, `!X->@n`) are not
atomic as a whole, so Forge segments writing overlapping regions can interleave. Head and
selection state are per segment. `setenv` and `getenv` are not synchronised, so environment
operations from several Forge segments can race.

**Error handling**
Fatal errors call `exit(1)` immediately, including from inside evaluated text or a Forge
thread. Non-fatal errors (for example `!X` without a selection) print a message and skip that
one operation. SPEC.md lists which is which.

**Selections cannot be cleared**
Once a selection is set it stays active until replaced by another one.

---

## Researcher Notes

Security researchers are welcome to audit this codebase for educational or curiosity-driven
purposes. The interpreter is intentionally permissive — the interesting surface is less about
the design and more about finding behavioural edge cases, parser confusion, or ways to reach
undefined behaviour through otherwise valid-looking scripts.

Some areas still worth exploring:

- Parser edge cases: `}!` consumes a following `!`, so `}!>` lexes as bit-right; `!X->` is
  always read as a capture suffix; malformed `!X->@`; `![M:@]` without a prior `!M`
- Forge concurrency: multi-byte operations and environment access under Forge
- Evaluated text: lexer errors and deeply nested `!^` and `%//` chains
- Very large captures and loads near the size limits

---

## Contributing Fixes

If you've found one of the known issues (or a new one) and want to contribute a fix, that's
welcome — but please **ask before opening a pull request.**

This is a personal project with intentional design decisions behind it. A fix that looks
obviously correct might conflict with how a feature is supposed to behave, or with changes
already planned. Asking first saves both of us time.

**How to propose a fix:**

1. Open a regular GitHub issue describing what you want to fix and how you're thinking of
   approaching it.
2. Wait for a response before writing code. A short "sounds good, go ahead" is all you need.
3. Keep the fix minimal and scoped — one issue per PR. Don't refactor surrounding code unless
   it's directly necessary for the fix.

Fixes that are likely to be accepted: missing error checks, overflow and bounds guards, and
other fixes that do not change what a valid script does.

Fixes that need more discussion first: anything touching the language semantics, the tape
architecture, the Forge threading model, or `!^` eval behaviour.

---

## Reporting a Vulnerability

If you discover a vulnerability in the interpreter implementation (as opposed to exercising
intentional language features), please report it privately rather than opening a public issue.

**Use GitHub's private vulnerability reporting:**
Security tab → "Report a vulnerability"

Please include:

- A minimal `.bitlang` script or input that triggers the issue
- The observed behaviour (crash, output, etc.) and the expected behaviour
- The platform and compiler you used to build the interpreter
- Any relevant notes on how you found it

There is no SLA, bounty, or CVE process — this is a personal project. Reports will be
acknowledged and credited in any subsequent fix.
