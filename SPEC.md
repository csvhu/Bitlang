# BitLang v3 Language Specification

## Overview and Philosophy

BitLang is a state-modifier script language, not a general-purpose program. Scripts are
throwaway; the tape is the persistent external state that matters. A BitLang script acts
upon the tape and is then discarded — the tape carries forward whatever was written to it.

File jumps (`%//`) are state handoffs between scripts, not subroutine calls. Selections
target tape regions precisely without disturbing others. `!^` (eval) allows the tape to
contain its own next transformation instructions — data and program are unified in the tape.

---

## Tape and Head Model

### Tape

The tape is a sparse, bit-addressed array organised into bytes. Bits are indexed from 0;
bit 0 is the most significant bit of byte 0. Bytes are 8 contiguous bits in big-endian
order (most significant bit first).

The interpreter stores the tape as a chunked, mutex-protected hash map. Only chunks that
have been written to are allocated. The tape is effectively infinite within the addressable
range of a `uint64_t` bit index.

### Head

The head is a bit pointer into the tape. All navigation commands move the head in units of
bits or bytes. Internally the head is always a bit index; byte-level commands multiply or
divide by 8.

When a command takes a byte index argument (e.g. `!@n`, selection indices), that argument
is a byte index. The conversion is `bit_index = byte_index x 8`.

### Concurrency

The tape mutex is per-tape and all reads and writes are thread-safe. Head and selection
state are per-thread under Forge. See the Forge section for concurrency semantics.

---

## Navigation

### Bit-level movement

| Command | Effect |
|---------|--------|
| `<` | Move head one bit left |
| `>` | Move head one bit right |

### Byte-level movement

| Command | Effect |
|---------|--------|
| `!<` | Move head one byte left (8 bits) |
| `!>` | Move head one byte right (8 bits) |

### Absolute and relative jumps

| Command | Effect |
|---------|--------|
| `!@n` | Absolute jump to byte `n` (head = `n x 8`) |
| `!+n` or `!+@n` | Relative forward by `n` bytes (head += `n x 8`) |
| `!-n` or `!-@n` | Relative backward by `n` bytes (head -= `n x 8`) |

`!@0` always jumps to the start of the tape. Moving the head before bit 0 is a runtime
error.

Numeric arguments to jump and selection commands are parsed as unsigned 64-bit integers.
Literals that would overflow the interpreter's safe limit (9 x 10^18) are rejected at
parse time with an error.

---

## Data Operations and Literals

### Bit operations

| Command | Effect |
|---------|--------|
| `+` | Set the current bit to 1 |
| `-` | Clear the current bit to 0 |
| `=` | Flip (toggle) the current bit |

The flip operation (`=`) is performed atomically under the tape mutex, making it safe
under Forge concurrency.

### Byte literals

| Command | Effect |
|---------|--------|
| `!#HH` | Write the byte `0xHH` at the head; advance head by 8 bits |
| `!'C'` | Write the byte for character `C` at the head; advance head by 8 bits |
| `!"string"` | Write the string bytes at the head; advance head past all written bytes |

String literals (`!"..."`) support the following escape sequences:

| Escape | Value |
|--------|-------|
| `\n` | newline (0x0A) |
| `\t` | horizontal tab (0x09) |
| `\r` | carriage return (0x0D) |
| `\0` | null byte (0x00) |
| `\\` | backslash |
| `\"` | double quote |
| `\xHH` | arbitrary byte with hex value `HH` |

Character literals (`!'C'`) support `\n`, `\t`, `\r`, `\0`, and `\\`.

Writing a byte or string literal writes bits in big-endian order (most significant bit
first). After a string literal, the head points to the bit immediately after the last
written byte.

---

## Selection System

### Selection state

A selection is a byte-addressed range consisting of:

- `active` — whether a selection is currently set
- `start` — start byte index (inclusive)
- `end` — end byte index (inclusive)
- `mark` / `mark_set` — a saved position set by `!M`

Selections are byte-indexed. `end` is inclusive. When a selection is active, I/O commands
operate on the entire selected range rather than the single byte at the head.

### Selection commands

| Command | Effect |
|---------|--------|
| `!M` | Set the mark to the current head byte position |
| `![n]` | Select single byte `n` |
| `![n:m]` | Select byte range `n..m` inclusive |
| `![n:+m]` | Select `m` bytes starting at byte `n` (i.e. `n..n+m-1`); `m` must be >= 1 |
| `![M:@]` | Select from the mark to the current head byte; requires a prior `!M` |

`![M:@]` automatically swaps start and end if the head is before the mark, so the
selection is always ordered. Using `![M:@]` without a prior `!M` is a runtime error.

`![n:+0]` (a zero-length relative selection) is a runtime error.

---

## I/O and Execution

### Output — `!O`

- If a selection is active, `!O` writes all bytes in the selection to the destination.
- If no selection is active and the destination is stdout, `!O` writes the single byte at
  the head and advances the head by one byte.
- If no selection is active and the destination is a file (`!O[path]`), the interpreter
  writes bytes from the head until a null byte is encountered (legacy v2 behaviour).

### Input — `!I`

- If a selection is active, `!I[path]` reads bytes from the source into the selection
  range, writing up to the selection length.
- If no selection is active, `!I` reads in stream mode: bytes are written from the head
  and the head advances with each byte.
- Omitting `[path]` reads from stdin.

### Stderr — `!E`

`!E` writes output to stderr using the same selection semantics as `!O`.

### Execute — `!X`

`!X` requires an active selection. The selection bytes are read as a null-terminated
string and passed directly to the shell (`/bin/sh -c`). No sanitisation is performed.
See SECURITY.md for the implications.

| Form | Behaviour |
|------|-----------|
| `!X` | Execute; discard stdout (streams to terminal) |
| `!X->@n` | Execute; capture stdout into tape starting at byte `n` |
| `!X->?` | Execute; write exit code as a single byte at the head |
| `!X->!` | Execute; pipe host stdin through the command |

---

## File Operations

### Load — `!L[path]`

Reads raw file bytes onto the tape at the head in stream mode (head advances with each
byte). File loads are capped at 256 MiB. Loads exceeding this limit are aborted with an
error.

### Write — `!W[path]`

Writes the current selection to the file at `path`. Requires an active selection.

### File jumps — `%//path`

Hands off execution to another `.bitlang` script. File jumps are state handoffs, not
subroutine calls — the target script shares the tape but gets its own token stream. The
interpreter enforces the `.bitlang` extension. The current head position is carried into
the target script and propagated back on return.

---

## Environment Variables

| Command | Effect |
|---------|--------|
| `!$VAR` | Read env var `VAR`; write its string value to the tape at the head |
| `!$VAR<-[n:m]` | Write the tape selection `[n:m]` into env var `VAR` |

Variable names may contain alphanumeric characters and underscores only. See SECURITY.md
for the implications of environment variable access.

---

## Self-Modification and Eval

### Self-load — `!~`

Loads the current script file onto the tape at the head (same semantics as `!L`). The
script path must be known (i.e. the interpreter was invoked with a file argument).

### Eval — `!^[n:m]`

Reads the tape region `[n:m]` as a string of BitLang source code and evaluates it. All
selection forms supported by `![...]` are valid here. The head and selection state are
shared with the calling context; head changes made inside the eval are propagated back.

Eval calls are limited to 64 levels of nesting. Exceeding this limit prints an error and
returns without executing the inner eval. This prevents stack exhaustion from recursive
`!^` chains.

Because `!^` combined with `!X` or `!$VAR` enables construction and execution of arbitrary
code at runtime, scripts using eval should be treated with the same caution as shell
scripts. See SECURITY.md.

---

## Control Flow and Blocks

### Comments

Lines beginning with `#` are comments and are ignored by the lexer.

### Line and program terminators

| Token | Meaning |
|-------|---------|
| `%` | Line end |
| `%%` | Block end |
| `!%!` | Program end; stops execution |

### Conditional — `!!{ condition }{ body }`

The `!!` token opens a conditional. The interpreter evaluates `condition` (a block of
commands), then reads the current bit. If the bit is 1, `body` is executed. If 0, the
body is skipped. The head is restored to its position before the condition after the
condition block completes.

### While loop — `[ body ]`

The interpreter executes `body` repeatedly while the current bit is 1. If the bit is 0
on entry, the body is never executed. The condition is re-evaluated (by reading the current
bit) after each iteration.

### Counted loop — `!n[ body ]`

Executes `body` exactly `n` times. `n` is a decimal integer literal. `!0[...]` executes
the body zero times. Nested counted loops are supported.

```
!3[ A B C ]
```

is exactly equivalent to:

```
A B C
A B C
A B C
```

### Forge blocks — `$ line1 | line2 | line3 $!`

Forge runs each `|`-separated segment concurrently in its own thread. All threads share
the tape (mutex-protected); each thread has its own head and selection state initialised
to a copy of the calling context's state at the point the Forge block is entered. The
calling thread waits for all Forge threads to complete before continuing.

Forge blocks support up to 64 segments. Segments beyond this limit are silently ignored.

---

## Error Conditions

The following conditions produce a diagnostic message on stderr and terminate the
interpreter (or abort the current operation):

| Condition | Where detected |
|-----------|----------------|
| Bare `!` at end of source | Lexer |
| Unknown `!x` sequence | Lexer |
| Unexpected character | Lexer |
| Expected number, none found | Lexer |
| Numeric literal overflow | Lexer |
| `![M:@]` used without prior `!M` | Executor |
| `![n:+0]` zero-length relative selection | Executor |
| Head moved before bit 0 | Executor |
| `!X` without active selection | Executor |
| `!W` without active selection | Executor |
| `!L` file exceeds 256 MiB | Executor |
| `!^` recursion depth exceeded (> 64) | Executor |
| `%//` with empty or non-`.bitlang` path | Executor |
| Allocation failure (OOM) | Any |

---

## Examples

### Write and output a string using a counted loop

```
!"Hello"
!@0
!O !4[ !> !O ]
```

Writes `Hello` to the tape starting at byte 0, jumps back to byte 0, outputs the first
byte, then repeats `!> !O` four times to step through and output the remaining four bytes.

### Write and output a string using a selection

```
!"Hello"
![0:4]
!O
```

Writes `Hello`, sets a selection over bytes 0 to 4 inclusive, then outputs the entire
selection in one `!O` call.

### Flip a bit conditionally

```
!#01
<<<<<<<<
[
    =
]
!@0
!O
```

Writes byte `0x01`, moves the head back to bit 0, then loops while the current bit is 1,
flipping it each iteration. After the loop the bit is 0. Outputs the result byte (`0x00`).

### Execute a shell command from the tape

```
!"echo hello"
![0:9]
!X
```

Writes the string `echo hello`, selects bytes 0 to 9, and executes it via the shell.

### Capture shell output into the tape

```
!"date"
![0:3]
!X->@10
```

Runs `date` and captures its stdout into the tape starting at byte 10.

### Parallel Forge execution

```
$
!"A" ![0:0] !O
|
!"B" ![0:0] !O
$!
```

Runs both segments concurrently. Each thread has its own head and selection. Output order
is non-deterministic.

---

## Implementation Limits (v3 reference interpreter)

| Limit | Value |
|-------|-------|
| Max `!^` recursion depth | 64 |
| Max `!L` file size | 256 MiB |
| Max Forge segments per block | 64 (excess silently ignored) |
| Max numeric literal | 9 x 10^18 |
| Tape chunk size | 4096 bits (512 bytes) |
| Tape chunk map buckets | 4096 |
