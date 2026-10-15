# BitLang v4 Language Specification

## Overview and Philosophy

BitLang is a state-modifier script language, not a general-purpose program. Scripts are
throwaway; the tape is the persistent external state that matters. A BitLang script acts
upon the tape and is then discarded — the tape carries forward whatever was written to it.

File jumps (`%//`) are state handoffs between scripts, not subroutine calls. Selections
target tape regions precisely without disturbing others. `!^` (eval) allows the tape to
contain its own next transformation instructions — data and program are unified in the tape.

This document describes the v4 language and its reference interpreter (`bitlang-v4.c`). The
syntax is the same as v3; the differences are listed under "Changes from v3".

---

## Source Format

- Whitespace (including newlines) separates tokens and is otherwise ignored.
- A **line** ends at `%` (or `%%`), not at a newline. A script with no `%` is a single line.
- `#` at the start of a token begins a comment that runs to the end of the physical line.
  `#` inside a string or literal is not a comment, and `!#` is the hex literal prefix.
- `!%!` (optionally `!%!.`) ends the program. Nothing after it is read or lexed.

| Token | Meaning |
|-------|---------|
| `%` | Line end |
| `%%` | Block end (behaves like `%`) |
| `!%!` | Program end; stops execution |

---

## Tape and Head Model

### Tape

The tape is a sparse, bit-addressed array organised into bytes. Bits are indexed from 0;
bit 0 is the most significant bit of byte 0. Bytes are 8 contiguous bits in big-endian
order (most significant bit first).

The interpreter stores the tape as a chunked, mutex-protected hash map. Only chunks that
have been written to are allocated; reading never-written tape returns zeros without
allocating. The largest addressable byte index is 2^61 − 1 (2305843009213693951).

### Head

The head is a bit pointer into the tape. All navigation commands move the head in units of
bits or bytes. Internally the head is always a bit index; byte-level commands multiply or
divide by 8.

When a command takes a byte index argument (e.g. `!@n`, selection indices), that argument
is a byte index. The conversion is `bit_index = byte_index x 8`.

Moving the head before bit 0, or past the end of the addressable tape, is a fatal error.

### Numbers

Numeric arguments are unsigned decimal integers. Values above 2^61 − 1 are rejected at parse
time with an error.

### Concurrency

The tape mutex is per-tape. Each single-bit operation and each single-byte read or write is
atomic. Head and selection state are per-thread under Forge. See the Forge section.

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

`!@0` always jumps to the start of the tape.

---

## Data Operations and Literals

### Bit operations

| Command | Effect |
|---------|--------|
| `+` | Set the current bit to 1 |
| `-` | Clear the current bit to 0 |
| `=` | Flip (toggle) the current bit |

The flip is performed atomically under the tape mutex.

### Byte literals

| Command | Effect |
|---------|--------|
| `!#H` or `!#HH` | Write the byte with the given hex value at the head; advance head by 8 bits |
| `!'C'` | Write the byte for character `C` at the head; advance head by 8 bits |
| `!"string"` | Write the string bytes at the head; advance head past all written bytes |

Hex digits may be upper or lower case. `!#` needs at least one hex digit, and takes at most
two, so `!#F+` is the byte `0x0F` followed by `+`.

String literals (`!"..."`) support these escape sequences:

| Escape | Value |
|--------|-------|
| `\n` | newline (0x0A) |
| `\t` | horizontal tab (0x09) |
| `\r` | carriage return (0x0D) |
| `\0` | null byte (0x00) |
| `\\` | backslash |
| `\"` | double quote |
| `\xH` or `\xHH` | arbitrary byte with the given hex value (one or two digits required) |

Any other escaped character stands for itself. An unterminated string is a fatal error.

Character literals (`!'C'`) support `\n`, `\t`, `\r` and `\0`. Any other escaped character
stands for itself, so `\\` and `\'` work.

Literals write bits in big-endian order (most significant bit first). After a literal, the
head points to the bit immediately after the last written byte.

---

## Selection System

### Selection state

A selection is a byte-addressed range consisting of:

- `active` — whether a selection is currently set
- `start` — start byte index (inclusive)
- `end` — end byte index (inclusive)
- `mark` / `mark_set` — a saved position set by `!M`

`end` is inclusive. When a selection is active, I/O commands operate on the entire selected
range rather than the single byte at the head. A selection stays active until another
selection replaces it; there is no command to clear it.

### Selection commands

| Command | Effect |
|---------|--------|
| `!M` | Set the mark to the current head byte position |
| `![n]` | Select single byte `n` |
| `![n:m]` | Select byte range `n..m` inclusive |
| `![n:+m]` | Select `m` bytes starting at byte `n` (i.e. `n..n+m-1`); `m` must be >= 1 |
| `![M:@]` | Select from the mark to the current head byte; requires a prior `!M` |

If the start is after the end (`![5:2]`, or `![M:@]` with the head before the mark), the two
are swapped so the selection is always ordered. A selection that would extend past byte
2^61 − 1 is an error and leaves the current selection unchanged. `![M:@]` without a prior
`!M`, and `![n:+0]`, are errors that leave the current selection unchanged.

Commands that read a selection into memory (`!X`, `!$VAR<-`, `!^`) accept at most 256 MiB.

---

## I/O and Execution

### Output — `!O`

- With an active selection, `!O` writes every byte of the selection.
- With no selection and no path, `!O` writes the single byte at the head to stdout and
  advances the head by one byte.
- With no selection and a path (`!O[path]`), the interpreter writes bytes from the head
  until a null byte is reached, advancing the head.
- `!O[path]` **appends** to the file, creating it if needed.

### Input — `!I`

- With an active selection, `!I` reads bytes into the selection range, up to the selection
  length or end of input. Unread input stays in the source, so a later `!I` continues from
  where this one stopped.
- With no selection, `!I` reads to end of input, writing from the head and advancing the
  head with each byte.
- Omitting `[path]` reads from stdin.

### Stderr — `!E`

`!E` writes to stderr: the whole selection if one is active, otherwise the single byte at
the head, advancing the head by one byte.

### Execute — `!X`

`!X` requires an active selection. The selection bytes, up to the first null byte, are
passed to the shell (`/bin/sh -c`). No sanitisation is performed. See SECURITY.md. Output
buffers are flushed before the command runs.

| Form | Behaviour |
|------|-----------|
| `!X` | Execute; output goes to the terminal |
| `!X->@n` | Execute; capture stdout into the tape starting at byte `n` |
| `!X->?` | Execute; write the exit code as a single byte at the head (255 if the command did not exit normally). The head does not move |
| `!X->!` | Execute with the host's stdin as the command's input (the interpreter reads all of stdin first) |

`->` directly after `!X` is always read as part of the form. Capture stops, with an error,
if it reaches the end of the addressable tape.

---

## File Operations

### Load — `!L[path]`

Reads raw file bytes onto the tape at the head in stream mode (head advances with each
byte). Loads are capped at 256 MiB per call; a larger file stops with an error after the
first 256 MiB have been written.

### Write — `!W[path]`

Writes the current selection to the file at `path`, replacing its contents. Requires an
active selection.

### File jumps — `%//path`

Runs another `.bitlang` script to completion, then continues after the jump.

- The path is everything after `%//` up to the next `%` or newline, with surrounding
  whitespace trimmed. It must end in `.bitlang`. Relative paths use the working directory.
- The target shares the tape, head, selection and mark with the caller. Head changes made
  by the target are visible after the jump.
- Inside the target, `!~` refers to the target's own file.
- File jumps and `!^` share a nesting limit of 64 levels. Exceeding it prints an error and
  skips that jump.

---

## Environment Variables

| Command | Effect |
|---------|--------|
| `!$VAR` | Read env var `VAR`; write its string value to the tape at the head (no null terminator is written). Nothing happens if the variable is unset |
| `!$VAR<-[n:m]` | Set env var `VAR` to the bytes of selection `[n:m]`, up to the first null byte |
| `!$VAR<-` | The same, using the active selection |

Variable names may contain alphanumeric characters and underscores only.

---

## Self-Modification and Eval

### Self-load — `!~`

Loads the current script file onto the tape at the head (same semantics as `!L`). The
script path must be known.

### Eval — `!^[n:m]` and `!^`

Reads a tape region as BitLang source (up to the first null byte) and runs it as a complete
program: every `%`-separated line runs, and `!%!` ends it. All selection forms valid in
`![...]` are accepted. With no brackets, `!^` uses the active selection, and it is an error
if there is none.

The eval shares the tape, head and selection with the caller. A lexer error in the
evaluated text is fatal for the whole interpreter. Eval and file jumps share the 64-level
nesting limit.

---

## Control Flow

### Conditional — `!!{ condition }! body`

`!!` is followed by a block in braces. The interpreter runs the block, then reads the bit
at the head (wherever the block left it), then restores the head to where it was before the
block. If the bit was 1, `body` runs. If it was 0, `body` is skipped.

`body` extends to the end of the line (`%`, `%%`, or the closer of the enclosing loop or
Forge block).

The `}` of the block consumes one following `!` if present, so write a space before any
command that starts with `!`: `}! !>` is a byte move, while `}!>` is a bit move.

### While loop — `[ body ]`

Runs `body` while the bit at the head is 1. If the bit is 0 on entry, the body is skipped.
The bit is re-read after each iteration.

### Counted loop — `!n[ body ]`

Runs `body` exactly `n` times. `!0[ ... ]` runs it zero times. Loops can be nested.

```
!3[ A B C ]
```

is equivalent to:

```
A B C
A B C
A B C
```

A loop must open and close on the same line. A `%` inside a loop body is an error.

### Forge — `$ line1 | line2 | line3 $!`

Each `|`-separated segment runs concurrently in its own thread. All threads share the tape.
Each thread gets its own head and selection, copied from the calling context when the block
starts. The caller waits for all threads, then continues with its own head and selection
unchanged.

A Forge block may have up to 64 segments; more is a fatal error. If the system cannot
start a thread, that segment runs in the calling thread with a warning.

Single-bit and single-byte tape operations are atomic. Multi-byte operations (strings,
selection I/O, `!L`) are not atomic as a whole, so segments writing to overlapping regions
can interleave. Output order between segments is not defined.

---

## Error Conditions

Fatal errors print a message on stderr and end the interpreter with exit status 1.

| Fatal condition | Detected in |
|-----------------|-------------|
| Bare `!` at end of source | Lexer |
| Unknown `!x` sequence, unexpected character | Lexer |
| Expected number, none found; numeric literal above 2^61 − 1 | Lexer |
| Unterminated string literal; `\x` or `!#` with no hex digit | Lexer |
| Unterminated `[` in a path argument | Lexer |
| Unreadable script file, or script above 256 MiB | Loader |
| Head moved before bit 0 or past the end of the tape | Executor |
| `!!` not followed by `{`, or missing `}!` | Executor |
| Loop not closed with `]` on the same line; `!n` not followed by `[` | Executor |
| Unmatched `]`, `}`, `\|` or `$!` | Executor |
| `%//` with an empty or non-`.bitlang` path, or an unreadable target | Executor |
| More than 64 Forge segments | Executor |
| Allocation failure | Any |

Non-fatal errors print a message and skip that one operation; execution continues.

| Non-fatal condition |
|---------------------|
| `![M:@]` without a prior `!M`; `![n:+0]`; selection past the end of the tape |
| `!X`, `!W`, or `!^` / `!$VAR<-` without a selection |
| Selection over 256 MiB passed to `!X`, `!$VAR<-` or `!^` |
| `!L` or `!W` with no path; any file that cannot be opened by `!L`, `!W`, `!I`, `!O` |
| `!L` file exceeds 256 MiB (the first 256 MiB are kept) |
| `!^` or `%//` nesting limit (64) reached |
| `!X` capture reaches the end of the tape |
| `popen`, `mkstemp` or `setenv` failure |
| `!~` when the script path is unknown |

---

## Examples

### Write and output a string using a counted loop

```
!"Hello"
!@0
!5[ !O ]
```

Writes `Hello` at byte 0, jumps back to byte 0, then runs `!O` five times. Each `!O` with no
selection outputs the byte at the head and moves the head one byte right.

### Write and output a string using a selection

```
!"Hello"
![0:4]
!O
```

Writes `Hello`, selects bytes 0 to 4 inclusive, and outputs the whole selection.

### Flip a bit in a loop

```
!#01
<
[ = ]
!@0
!O
```

After `!#01` the head is at bit 8. `<` moves it to bit 7, the lowest bit of byte 0, which is
1. The loop flips it to 0 and stops. The output is the byte `0x00`.

### Conditional

```
+ !!{ }! !#41
![0:0] !O
```

The condition block is empty, so the bit at the head (just set by `+`) decides. It is 1, so
`!#41` runs and the output is `A`.

### Execute a shell command from the tape

```
!"echo hello"
![0:9]
!X
```

Writes `echo hello`, selects bytes 0 to 9, and runs it. Prints `hello`.

### Capture shell output into the tape

```
!"echo hi"
![0:6]
!X->@10
![10:12]
!O
```

Runs `echo hi`, captures its output (`hi` and a newline) at byte 10, then outputs it.

### Parallel Forge execution

```
$
!"A" ![0:0] !O
|
!@8 !"B" ![8:8] !O
$!
```

Runs both segments concurrently, each with its own head. The segments write to different
bytes, so the tape ends up the same every time; the order of the two output characters is
not defined.

### Eval

```
!"!'Z'" ![0:3]
!@30
!^
![30:30] !O
```

Writes the source `!'Z'`, selects it, moves to byte 30, and evaluates the selection. The
evaluated code writes `Z` at the head, and the script outputs it.

### File jump

`helper.bitlang`:

```
!"X" ![0:0] %
```

`main.bitlang`:

```
%// helper.bitlang
!O %
```

The helper writes `X` and selects byte 0. Back in `main`, the selection is still active and
`!O` outputs `X`.

---

## Implementation Limits (v4 reference interpreter)

| Limit | Value |
|-------|-------|
| Max numeric literal and byte index | 2^61 − 1 (2305843009213693951) |
| Max head position | bit 2^64 − 8 |
| Max `!^` and `%//` nesting depth (shared) | 64 |
| Max `!L` size per call | 256 MiB |
| Max selection read into memory (`!X`, `!$VAR<-`, `!^`) | 256 MiB |
| Max script file size | 256 MiB |
| Max Forge segments per block | 64 (more is an error) |
| Tape chunk size | 4096 bits (512 bytes) |
| Tape chunk map buckets | 4096 |

---

## Changes from v3

**Behaviour changes**

- `%//` and `!^` run every line of the target, not only the first.
- `!^` and `!$VAR<-` with no brackets use the active selection (v3 used byte 0).
- `!^[n]`, `!^[n:+m]` and `![M:@]` are honoured by `!^` (v3 read the numbers as a plain
  range).
- Reversed ranges are swapped. In v3 they gave inconsistent results across commands.
- The numeric limit dropped from 9 x 10^18 to 2^61 − 1, because larger byte indices wrapped
  around when converted to bit addresses.
- `!#` accepts one or two hex digits and rejects none; `\x` follows the same rule.
- Unterminated strings, unterminated `[path`, and more than 64 Forge segments are errors.
- A stray `]`, `}`, `|` or `$!` at the top level is an error.

**Bug fixes**

- Endless loop on stray closers.
- `!I` into a full selection lost one input byte.
- `!#F` swallowed the next character; `\x4"` swallowed the closing quote.
- Recursive `%//` crashed with a stack overflow.
- A failed thread start led to joining a thread that never ran.
- Reads of unwritten tape allocated memory.

**Internal changes**

- One lock acquisition per byte instead of one per bit, so byte writes are atomic and
  loads are much faster.
- Script files are read in a loop, so pipes work, and are size-limited.
- Output is flushed before shell commands run.
