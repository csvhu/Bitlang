# BitLang

## Note

csvhu is the primary architect of BitLang. The implementation was developed with AI assistance.

## Disclaimer

BitLang is early-stage and experimental. It may contain bugs or incomplete features. Use at your own risk.

## What is BitLang?

BitLang is an esoteric, state-modifier programming language built around an **infinite bit-addressed tape**. Scripts are throwaway tools that modify the tape — the tape is the persistent state that matters, not the script.

### Core Concepts

- **Tape**: Infinite, sparse, bit-addressed memory (indexed from bit 0) — the central data structure
- **State Modification**: Scripts modify the tape and are discarded; the tape carries forward
- **Bit & Byte Operations**: Direct manipulation of bits (`+`, `-`, `=`) and byte-level navigation
- **Selections**: Target tape regions precisely without disturbing the head
- **File Jumps**: Hand off execution between scripts via `%//` — not subroutine calls
- **Forge**: Run lines concurrently with shared tape and independent heads
- **Shell Integration**: Execute shell commands directly from tape selections
- **Eval/Self-Modification**: `!^` evaluates tape bytes as BitLang code — data and program are one
- **Environment & File I/O**: Read/write env vars and files directly to/from tape

### v3 Features

- String, hex, and char literals (`!"string"`, `!#FF`, `!'A'`)
- Flexible selection syntax (`![n:m]`, `![n]`, `![n:+m]`, `![M:@]`)
- Shell execution with output capture (`!X`, `!X->@n`, `!X->?`)
- File loading and writing (`!L[path]`, `!W[path]`)
- Environment variable operations (`!$VAR`, `!$VAR<-[n:m]`)
- Self-referential script loading (`!~`)
- Dynamic code evaluation (`!^[n:m]`)

## ⚠️ Security & Production Use

**BitLang is not intended for production use or untrusted input.**

It is a learning tool and reference implementation — not a secure runtime. The interpreter
gives scripts intentional, broad access to the host system by design: shell execution
(`!X`), arbitrary file I/O (`!L`, `!W`), environment variable access (`!$VAR`), and
dynamic code evaluation (`!^`) are all first-class features with no sandboxing.

Known implementation-level bugs (NULL dereferences on allocation failure, a TOCTOU race
in `tape_flip_bit`, unsigned underflow in zero-length selections, and integer overflow in
numeric literals) have been fixed in the current version. Resource limits are now enforced
for file loads (256 MiB) and eval recursion depth (64 levels).

See [SECURITY.md](SECURITY.md) for a full description of the attack surface, fixed
vulnerabilities, and remaining known limits.

Do not run untrusted `.bitlang` scripts. Do not expose the interpreter to untrusted input
in any automated or networked context.
