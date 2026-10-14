# BitLang

## Note

csvhu is the primary architect of BitLang. The implementation was developed with AI assistance.

## Disclaimer

BitLang is early-stage and experimental. It may contain bugs or incomplete features. Use at your own risk.

## What is BitLang?

BitLang is an esoteric, state-modifier programming language built around an **infinite bit-addressed tape**. Scripts are throwaway tools that modify the tape — the tape is the persistent state that matters, not the script.

### Core Concepts

- **Tape**: Sparse, bit-addressed memory (indexed from bit 0) — the central data structure
- **State Modification**: Scripts modify the tape and are discarded; the tape carries forward
- **Bit & Byte Operations**: Direct manipulation of bits (`+`, `-`, `=`) and byte-level navigation
- **Selections**: Target tape regions precisely without disturbing the head
- **File Jumps**: Hand off execution between scripts via `%//` — not subroutine calls
- **Forge**: Run lines concurrently with shared tape and independent heads
- **Shell Integration**: Execute shell commands directly from tape selections
- **Eval/Self-Modification**: `!^` evaluates tape bytes as BitLang code — data and program are one
- **Environment & File I/O**: Read/write env vars and files directly to/from tape

## Quick Start

The reference interpreter is a single C file (POSIX systems, needs pthreads):

```sh
gcc -O2 -o bitlang bitlang-v4.c -lpthread
./bitlang hello.bitlang
```

`hello.bitlang`:

```
!"Hello, World!\n"
![0:13]
!O
```

This writes the string onto the tape, selects bytes 0 to 13, and outputs the selection.
Newlines are ordinary whitespace in BitLang; `%` is what ends a line. See [SPEC.md](SPEC.md)
for the full language.

## Language Features

- Bit operations (`+`, `-`, `=`), bit and byte navigation, absolute and relative jumps
- Conditionals (`!!{ ... }!`), while loops (`[ ... ]`), counted loops (`!n[ ... ]`)
- String, hex, and char literals (`!"string"`, `!#FF`, `!'A'`)
- Selection syntax (`![n:m]`, `![n]`, `![n:+m]`, `![M:@]`)
- Shell execution with output capture (`!X`, `!X->@n`, `!X->?`, `!X->!`)
- File loading and writing (`!L[path]`, `!W[path]`)
- Environment variable operations (`!$VAR`, `!$VAR<-[n:m]`)
- Self-referential script loading (`!~`)
- Dynamic code evaluation (`!^[n:m]`)
- Concurrent lines with Forge (`$ ... | ... $!`)
- File jumps between scripts (`%//path.bitlang`)

## What's New in v4

v4 keeps the v3 syntax and fixes the interpreter bugs found in a code review. The visible
changes:

- `%//` and `!^` now run **every** line of the target script or tape region, not just the first.
- `!^` with no brackets, and `!$VAR<-` with no brackets, use the active selection.
- A stray `]`, `}`, `|` or `$!` is now an error instead of an endless loop.
- `!I` into a selection no longer loses a byte of input.
- `!#F` (one hex digit) no longer swallows the next character. Invalid hex (`!#ZZ`, `\xZ`) is an error.
- Numbers and byte indices are limited to 2^61 − 1 so that bit addresses cannot wrap around.
  The head can no longer silently wrap past the end of the tape.
- Reversed ranges like `![5:2]` are treated as `![2:5]`.
- `%//` has a nesting limit (shared with `!^`, 64 levels) instead of crashing on recursion.
- Unterminated strings and `[path` arguments are errors. More than 64 Forge segments is an error.
- Byte reads and writes take the tape lock once per byte, so they are atomic and much faster.

The complete list is in the "Changes from v3" section of [SPEC.md](SPEC.md).

## ⚠️ Security & Production Use

**BitLang is not intended for production use or untrusted input.**

It is a learning tool and reference implementation — not a secure runtime. The interpreter
gives scripts intentional, broad access to the host system by design: shell execution
(`!X`), arbitrary file I/O (`!L`, `!W`, `!O[path]`), environment variable access (`!$VAR`),
and dynamic code evaluation (`!^`) are all first-class features with no sandboxing.

The v4 interpreter has been built with AddressSanitizer and UBSan and run against a set of
regression scripts. It has not been fuzzed or independently audited.

See [SECURITY.md](SECURITY.md) for the attack surface, fixed vulnerabilities, and remaining
known limits.

Do not run untrusted `.bitlang` scripts. Do not expose the interpreter to untrusted input
in any automated or networked context.
