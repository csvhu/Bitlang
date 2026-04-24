# Security Policy

## Status

BitLang is an **experimental esolang interpreter**. It has not been security-hardened or
robustness-tested. It is a learning tool and reference implementation — not a secure runtime.

**Do not run untrusted `.bitlang` scripts.** Do not expose the interpreter to untrusted input
in any automated or networked context.

---

## Known Attack Surface

BitLang's design gives scripts broad, intentional access to the host system. The following
features are inherently dangerous and exist by design:

### Shell Execution (`!X`)
The `!X` instruction reads the active tape selection as a string and passes it directly to
`system()` or `popen()`, which invokes `/bin/sh -c`. There is no sanitization, escaping, or
sandboxing. Any `.bitlang` script can execute arbitrary shell commands with the privileges of
the interpreter process.

### Environment Variable Access (`!$VAR`, `!$VAR<-[n:m]`)
Scripts can read any environment variable into the tape (`!$VAR`) or write tape content back
into the process environment (`!$VAR<-[n:m]`). This includes sensitive variables such as
`PATH`, `HOME`, `LD_PRELOAD`, API keys, or session tokens present in the environment at
launch time.

### File I/O (`!L`, `!W`)
Scripts can read any file accessible to the interpreter process (`!L[path]`) and write tape
content to any writable path (`!W[path]`). Paths are not validated or restricted.

### Dynamic Evaluation (`!^`)
The `!^[n:m]` instruction reads bytes from the tape and evaluates them as BitLang source code
at runtime. Combined with `!X` or `!$VAR`, this enables scripts to construct and execute
arbitrary code dynamically.

### File Jumps (`%//`)
Scripts can hand off execution to another `.bitlang` file on disk. The interpreter enforces
the `.bitlang` extension but does not restrict the path in any other way.

---

## Fixed Implementation Vulnerabilities

The following bugs identified in earlier versions of the interpreter have been corrected.

**Unsigned underflow in `sel_resolve` (SEL_REL)** *(fixed)*
`![n:+0]` now produces a hard error instead of wrapping `end` to `UINT64_MAX`.

**Unchecked `malloc`/`calloc`/`realloc` return values** *(fixed)*
All heap allocations — `tape_chunk`, `sel_read_bytes`, `parse_string_literal`, `ts_init`,
`ts_push`, `read_path_arg`, `read_filejump_path`, `read_env_name`, and the `EXEC_PIPE_IN`
stdin buffer — now check for NULL and exit with a diagnostic on allocation failure.

**TOCTOU race in `tape_flip_bit` under Forge** *(fixed)*
`tape_flip_bit` now performs the read-modify-write as a single atomic operation under one
mutex acquisition (XOR on the byte), eliminating the lost-update window.

**Integer overflow in `read_number`** *(fixed)*
Numeric literals are now parsed into `uint64_t` with explicit overflow detection. Literals
exceeding the interpreter's safe limit (`9 × 10¹⁸`) are rejected with an error.

**Unchecked `write()` in `EXEC_PIPE_IN`** *(fixed)*
The full stdin buffer is now written in a loop with proper `ssize_t` tracking.

---

## Remaining Known Limits and Behaviour

**`!L` file size limit**
File loads via `!L` are now capped at 256 MiB (`MAX_LOAD_BYTES`). Loads exceeding this
limit are aborted with an error message. The cap is a compile-time constant and can be
adjusted, but should not be removed without adding an explicit resource policy.

**`!^` recursion depth limit**
`!^` (eval) is now limited to `MAX_EVAL_DEPTH` (64) levels of nesting. Exceeding this
prints an error and returns without executing the inner eval. Stack exhaustion via deeply
recursive `!^` chains is no longer possible within the default limit.

**`MAX_FORGE_LINES` silent truncation**
Forge blocks with more than 64 `|`-separated lines silently truncate. This is a known
design-level cap that has not been changed; excess lines are ignored without warning.

---

## Researcher Notes

Security researchers are welcome to audit this codebase for educational or curiosity-driven
purposes. The interpreter is intentionally permissive — the interesting surface is less about
the design and more about finding behavioural edge cases, parser confusion, or ways to reach
undefined behaviour through otherwise valid-looking scripts.

Some areas still worth exploring:

- Parser edge cases: malformed `!X->@`, `![M:@]` without prior `!M`, deeply nested Forge
  blocks hitting the `MAX_FORGE_LINES` (64) silent truncation cap
- Forge concurrency: the tape mutex is per-tape, but head and selection state are per-thread;
  races beyond `tape_flip_bit` may still exist in other multi-byte operations under Forge

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

Fixes that are likely to be accepted: NULL checks, the `SEL_REL` underflow guard, the
`tape_flip_bit` atomic rewrite, overflow detection in `read_number`, Forge truncation warning.

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
