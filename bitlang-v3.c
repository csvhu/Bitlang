/*
 * bitlang-v3.c — Bitlang Interpreter (v3)
 *
 * PHILOSOPHY
 * ----------
 * Bitlang is not a program. It is a state-modifier script language.
 * Scripts are throwaway; the tape is the persistent external state that
 * matters. A Bitlang script acts upon the tape and is then discarded —
 * the tape carries forward whatever was written to it. File jumps (%//)
 * are handoffs between state-modification scripts, not subroutine calls.
 * Selections target tape regions precisely without disturbing others.
 * !^ (eval) allows the tape to contain its own next transformation
 * instructions — data and program are unified in the tape state.
 *
 * All v2 features preserved. New in v3:
 *
 *   Literals    : !"string\n"  !#FF  !'A'
 *   Selection   : !M  ![n:m]  ![n]  ![n:+m]  ![M:@]
 *   I/O (v3)    : !O  !I  now operate on selection when set
 *                 !E  output selection to stderr
 *   Execution   : !X            execute selection as shell command
 *                 !X->@n        execute, capture stdout to tape at byte n
 *                 !X->?         execute, capture exit code as byte at head
 *                 !X->!         execute, pipe stdin through command
 *   File ops    : !L[path]      load raw file bytes onto tape at head
 *                 !W[path]      write selection to file
 *   Environment : !$VAR         read env var, write string to tape at head
 *                 !$VAR<-[n:m]  write tape selection into env var
 *   Self-mod    : !~            load current script file onto tape at head
 *                 !^[n:m]       evaluate tape region as bitlang code
 *
 * Selection state: sel_active flag + sel_start/sel_end (byte indices).
 * When a selection is active, !O and !I operate on that range instead
 * of the single byte at head.
 *
 * Tape: sparse chunk-based hash map, mutex-protected. (same as v2)
 *
 * Usage: ./bitlang <file.bitlang>
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* =========================================================
 * SPARSE TAPE  (unchanged from v2)
 * ========================================================= */

#define CHUNK_BITS      4096
#define CHUNK_BYTES     (CHUNK_BITS / 8)
#define CHUNK_MAP_SIZE  4096

typedef struct Chunk {
    uint64_t      index;
    unsigned char data[CHUNK_BYTES];
    struct Chunk *next;
} Chunk;

typedef struct {
    Chunk          *map[CHUNK_MAP_SIZE];
    pthread_mutex_t lock;
} Tape;

static void tape_init(Tape *t) {
    memset(t->map, 0, sizeof(t->map));
    pthread_mutex_init(&t->lock, NULL);
}

static void tape_free(Tape *t) {
    for (int i = 0; i < CHUNK_MAP_SIZE; i++) {
        Chunk *c = t->map[i];
        while (c) { Chunk *n = c->next; free(c); c = n; }
    }
    pthread_mutex_destroy(&t->lock);
}

static Chunk *tape_chunk(Tape *t, uint64_t bit) {
    uint64_t cidx = bit / CHUNK_BITS;
    uint64_t slot = cidx % CHUNK_MAP_SIZE;
    Chunk   *c    = t->map[slot];
    while (c) { if (c->index == cidx) return c; c = c->next; }
    c = calloc(1, sizeof(Chunk));
    if (!c) { fputs("OOM\n", stderr); exit(1); }
    c->index     = cidx;
    c->next      = t->map[slot];
    t->map[slot] = c;
    return c;
}

static int tape_read_bit(Tape *t, uint64_t bit) {
    pthread_mutex_lock(&t->lock);
    Chunk   *c      = tape_chunk(t, bit);
    uint64_t lo     = bit % CHUNK_BITS;
    int      byte_i = (int)(lo / 8);
    int      bit_i  = 7 - (int)(lo % 8);
    int      val    = (c->data[byte_i] >> bit_i) & 1;
    pthread_mutex_unlock(&t->lock);
    return val;
}

static void tape_write_bit(Tape *t, uint64_t bit, int val) {
    pthread_mutex_lock(&t->lock);
    Chunk   *c      = tape_chunk(t, bit);
    uint64_t lo     = bit % CHUNK_BITS;
    int      byte_i = (int)(lo / 8);
    int      bit_i  = 7 - (int)(lo % 8);
    if (val) c->data[byte_i] |=  (unsigned char)(1 << bit_i);
    else     c->data[byte_i] &= ~(unsigned char)(1 << bit_i);
    pthread_mutex_unlock(&t->lock);
}

static void tape_flip_bit(Tape *t, uint64_t bit) {
    pthread_mutex_lock(&t->lock);
    Chunk   *c      = tape_chunk(t, bit);
    uint64_t lo     = bit % CHUNK_BITS;
    int      byte_i = (int)(lo / 8);
    int      bit_i  = 7 - (int)(lo % 8);
    c->data[byte_i] ^= (unsigned char)(1 << bit_i);
    pthread_mutex_unlock(&t->lock);
}

static unsigned char tape_read_byte(Tape *t, uint64_t bit) {
    unsigned char b = 0;
    for (int i = 0; i < 8; i++)
        b = (unsigned char)((b << 1) | tape_read_bit(t, bit + (uint64_t)i));
    return b;
}

static void tape_write_byte(Tape *t, uint64_t bit, unsigned char byte) {
    for (int i = 0; i < 8; i++)
        tape_write_bit(t, bit + (uint64_t)i, (byte >> (7 - i)) & 1);
}

/* Write a C string to tape starting at *head, advance head past it.
   Does NOT write a null terminator unless the string contains one. */
static void tape_write_string(Tape *t, uint64_t *head, const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        tape_write_byte(t, *head, (unsigned char)s[i]);
        *head += 8;
    }
}

/* =========================================================
 * SELECTION STATE
 *
 * Selections are byte-addressed (sel_start, sel_end are byte indices).
 * sel_end is inclusive.
 * sel_mark is set by !M and used by ![M:@].
 * ========================================================= */

typedef struct {
    int      active;      /* is a selection currently set? */
    uint64_t start;       /* start byte index              */
    uint64_t end;         /* end byte index (inclusive)    */
    uint64_t mark;        /* mark set by !M                */
    int      mark_set;
} Selection;



/* =========================================================
 * TOKEN TYPES
 * ========================================================= */

typedef enum {
    /* v2 tokens */
    TOK_BIT_LEFT,
    TOK_BIT_RIGHT,
    TOK_BYTE_LEFT,
    TOK_BYTE_RIGHT,
    TOK_ABS_JUMP,
    TOK_REL_FWD,
    TOK_REL_BWD,
    TOK_SET,
    TOK_CLEAR,
    TOK_FLIP,
    TOK_IF_OPEN,
    TOK_BLOCK_OPEN,
    TOK_BLOCK_CLOSE,
    TOK_WHILE_OPEN,
    TOK_WHILE_CLOSE,
    TOK_COUNT_LOOP,
    TOK_INPUT,
    TOK_OUTPUT,
    TOK_FORGE_OPEN,
    TOK_FORGE_SEP,
    TOK_FORGE_CLOSE,
    TOK_FILE_JUMP,
    TOK_LINE_END,
    TOK_BLOCK_END,
    TOK_PROG_END,
    TOK_EOF,
    /* v3 tokens */
    TOK_STR_LIT,      /* !"..."     str=unescaped bytes, num=length */
    TOK_HEX_LIT,      /* !#FF       num=byte value                  */
    TOK_CHAR_LIT,     /* !'A'       num=byte value                  */
    TOK_MARK,         /* !M                                         */
    TOK_SELECT,       /* ![n:m] ![n] ![n:+m] ![M:@]  see sel_* fields on token */
    TOK_STDERR,       /* !E                                         */
    TOK_EXEC,         /* !X  !X->@n  !X->?  !X->!                  */
    TOK_LOAD,         /* !L[path]                                   */
    TOK_WRITE,        /* !W[path]                                   */
    TOK_ENV_READ,     /* !$VAR                                      */
    TOK_ENV_WRITE,    /* !$VAR<-[n:m]                               */
    TOK_SELF_LOAD,    /* !~                                         */
    TOK_EVAL,         /* !^[n:m]                                    */
} TokenKind;

/* For TOK_SELECT, what kind of selection expression */
typedef enum {
    SEL_RANGE,      /* ![n:m]   */
    SEL_SINGLE,     /* ![n]     */
    SEL_REL,        /* ![n:+m]  */
    SEL_MARK_HEAD,  /* ![M:@]   */
} SelKind;

/* For TOK_EXEC, what capture mode */
typedef enum {
    EXEC_PLAIN,     /* !X           */
    EXEC_CAP_TAPE,  /* !X->@n       */
    EXEC_CAP_EXIT,  /* !X->?        */
    EXEC_PIPE_IN,   /* !X->!        */
} ExecMode;

typedef struct {
    TokenKind kind;
    long      num;        /* ABS_JUMP, REL_*, COUNT_LOOP, HEX_LIT, CHAR_LIT,
                             EXEC capture byte index for EXEC_CAP_TAPE        */
    long      num2;       /* SEL end for SEL_RANGE/SEL_REL                    */
    char     *str;        /* FILE_JUMP path, INPUT/OUTPUT path,
                             STR_LIT content, ENV var name, LOAD/WRITE path,
                             EVAL/SELECT range string                          */
    SelKind   sel_kind;
    ExecMode  exec_mode;
} Token;

/* =========================================================
 * LEXER
 * ========================================================= */

typedef struct { const char *src; size_t pos, len; } Lexer;

static void lexer_init(Lexer *l, const char *src) {
    l->src = src; l->pos = 0; l->len = strlen(src);
}

static void skip_ws(Lexer *l) {
    while (l->pos < l->len && isspace((unsigned char)l->src[l->pos])) l->pos++;
}

static void skip_comment(Lexer *l) {
    while (l->pos < l->len && l->src[l->pos] != '\n') l->pos++;
}

/* Parse a decimal number into uint64_t with overflow detection.
   Stored in tok.num as long; values exceeding LONG_MAX are clamped and
   flagged — callers that use the result as a tape address will reject them. */
#define READ_NUMBER_MAX ((uint64_t)9000000000000000000ULL)  /* sane tape limit */
static long read_number(Lexer *l) {
    if (l->pos >= l->len || !isdigit((unsigned char)l->src[l->pos])) {
        fprintf(stderr, "Error: expected number at pos %zu\n", l->pos); exit(1);
    }
    uint64_t n = 0;
    while (l->pos < l->len && isdigit((unsigned char)l->src[l->pos])) {
        unsigned digit = (unsigned)(l->src[l->pos++] - '0');
        if (n > (READ_NUMBER_MAX - digit) / 10) {
            fprintf(stderr, "Error: numeric literal overflow at pos %zu\n", l->pos);
            exit(1);
        }
        n = n * 10 + digit;
    }
    return (long)n;
}

static char *read_path_arg(Lexer *l) {
    skip_ws(l);
    if (l->pos >= l->len || l->src[l->pos] != '[') return NULL;
    l->pos++;
    size_t start = l->pos;
    while (l->pos < l->len && l->src[l->pos] != ']') l->pos++;
    size_t end = l->pos;
    if (l->pos < l->len) l->pos++;
    size_t slen = end - start;
    char  *path = malloc(slen + 1);
    if (!path) { fputs("OOM\n", stderr); exit(1); }
    memcpy(path, l->src + start, slen);
    path[slen] = '\0';
    return path;
}

static char *read_filejump_path(Lexer *l) {
    skip_ws(l);
    size_t start = l->pos;
    while (l->pos < l->len && l->src[l->pos] != '\n' && l->src[l->pos] != '%')
        l->pos++;
    size_t end = l->pos;
    while (end > start && isspace((unsigned char)l->src[end - 1])) end--;
    size_t slen = end - start;
    char  *path = malloc(slen + 1);
    if (!path) { fputs("OOM\n", stderr); exit(1); }
    memcpy(path, l->src + start, slen);
    path[slen] = '\0';
    return path;
}

/* Parse escape sequences in a string literal.
   Returns heap-allocated unescaped bytes; *out_len = byte count. */
static char *parse_string_literal(Lexer *l, size_t *out_len) {
    /* opening '"' already consumed */
    size_t cap = 64;
    char  *buf = malloc(cap);
    if (!buf) { fputs("OOM\n", stderr); exit(1); }
    size_t len = 0;

    while (l->pos < l->len && l->src[l->pos] != '"') {
        char c = l->src[l->pos++];
        if (c == '\\' && l->pos < l->len) {
            char e = l->src[l->pos++];
            switch (e) {
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                case 'r':  c = '\r'; break;
                case '0':  c = '\0'; break;
                case '\\': c = '\\'; break;
                case '"':  c = '"';  break;
                case 'x': {
                    /* \xHH */
                    char h[3] = {0};
                    if (l->pos + 1 < l->len) {
                        h[0] = l->src[l->pos++];
                        h[1] = l->src[l->pos++];
                    }
                    c = (char)strtol(h, NULL, 16);
                    break;
                }
                default: c = e; break;
            }
        }
        if (len >= cap) {
            size_t newcap = cap * 2;
            char  *tmp    = realloc(buf, newcap);
            if (!tmp) { free(buf); fputs("OOM\n", stderr); exit(1); }
            buf = tmp; cap = newcap;
        }
        buf[len++] = c;
    }
    if (l->pos < l->len) l->pos++; /* consume closing '"' */
    *out_len = len;
    return buf;
}

/* Parse hex byte literal after !# — two hex digits. */
static unsigned char parse_hex_byte(Lexer *l) {
    char h[3] = {0};
    if (l->pos < l->len) h[0] = l->src[l->pos++];
    if (l->pos < l->len) h[1] = l->src[l->pos++];
    return (unsigned char)strtol(h, NULL, 16);
}

/* Parse !$VARNAME — identifier chars only. Returns heap string. */
static char *read_env_name(Lexer *l) {
    size_t start = l->pos;
    while (l->pos < l->len &&
           (isalnum((unsigned char)l->src[l->pos]) || l->src[l->pos] == '_'))
        l->pos++;
    size_t slen = l->pos - start;
    char  *name = malloc(slen + 1);
    if (!name) { fputs("OOM\n", stderr); exit(1); }
    memcpy(name, l->src + start, slen);
    name[slen] = '\0';
    return name;
}

/*
 * Parse a selection argument: ![...]
 * Forms:
 *   [n:m]   SEL_RANGE,  num=n, num2=m
 *   [n]     SEL_SINGLE, num=n
 *   [n:+m]  SEL_REL,    num=n, num2=m (length)
 *   [M:@]   SEL_MARK_HEAD
 *
 * Expects '[' already consumed.
 */
static Token parse_select_arg(Lexer *l) {
    Token tok = { TOK_SELECT, 0, 0, NULL, SEL_SINGLE, EXEC_PLAIN };
    skip_ws(l);

    if (l->pos < l->len && l->src[l->pos] == 'M') {
        /* [M:@] */
        l->pos++;
        skip_ws(l);
        if (l->pos < l->len && l->src[l->pos] == ':') l->pos++;
        skip_ws(l);
        if (l->pos < l->len && l->src[l->pos] == '@') l->pos++;
        tok.sel_kind = SEL_MARK_HEAD;
    } else {
        tok.num = read_number(l);
        skip_ws(l);
        if (l->pos < l->len && l->src[l->pos] == ':') {
            l->pos++;
            skip_ws(l);
            if (l->pos < l->len && l->src[l->pos] == '+') {
                l->pos++;
                tok.num2     = read_number(l);
                tok.sel_kind = SEL_REL;
            } else {
                tok.num2     = read_number(l);
                tok.sel_kind = SEL_RANGE;
            }
        } else {
            tok.sel_kind = SEL_SINGLE;
        }
    }
    skip_ws(l);
    if (l->pos < l->len && l->src[l->pos] == ']') l->pos++;
    return tok;
}

static Token next_token(Lexer *l) {
    Token tok = { TOK_EOF, 0, 0, NULL, SEL_SINGLE, EXEC_PLAIN };

    for (;;) {
        skip_ws(l);
        if (l->pos >= l->len) return tok;
        char c = l->src[l->pos];

        if (c == '#') { skip_comment(l); continue; }

        if (c == '<') { l->pos++; tok.kind = TOK_BIT_LEFT;    return tok; }
        if (c == '>') { l->pos++; tok.kind = TOK_BIT_RIGHT;   return tok; }
        if (c == '+') { l->pos++; tok.kind = TOK_SET;         return tok; }
        if (c == '-') { l->pos++; tok.kind = TOK_CLEAR;       return tok; }
        if (c == '=') { l->pos++; tok.kind = TOK_FLIP;        return tok; }
        if (c == '{') { l->pos++; tok.kind = TOK_BLOCK_OPEN;  return tok; }
        if (c == '[') { l->pos++; tok.kind = TOK_WHILE_OPEN;  return tok; }
        if (c == ']') { l->pos++; tok.kind = TOK_WHILE_CLOSE; return tok; }
        if (c == '|') { l->pos++; tok.kind = TOK_FORGE_SEP;   return tok; }

        if (c == '}') {
            l->pos++;
            skip_ws(l);
            if (l->pos < l->len && l->src[l->pos] == '!') l->pos++;
            tok.kind = TOK_BLOCK_CLOSE;
            return tok;
        }

        if (c == '%') {
            if (l->pos + 1 < l->len && l->src[l->pos+1] == '%') {
                l->pos += 2; tok.kind = TOK_BLOCK_END;
                skip_ws(l);
                if (l->pos < l->len && l->src[l->pos] == '#') skip_comment(l);
                return tok;
            }
            if (l->pos + 2 < l->len &&
                l->src[l->pos+1] == '/' && l->src[l->pos+2] == '/') {
                l->pos += 3;
                tok.kind = TOK_FILE_JUMP;
                tok.str  = read_filejump_path(l);
                return tok;
            }
            l->pos++; tok.kind = TOK_LINE_END;
            skip_ws(l);
            if (l->pos < l->len && l->src[l->pos] == '#') skip_comment(l);
            return tok;
        }

        if (c == '!') {
            l->pos++;
            if (l->pos >= l->len) {
                fprintf(stderr, "Error: bare '!' at end of source\n"); exit(1);
            }
            char c2 = l->src[l->pos];

            /* !%! or !%!. — program end */
            if (c2 == '%') {
                l->pos++;
                if (l->pos < l->len && l->src[l->pos] == '!') {
                    l->pos++;
                    if (l->pos < l->len && l->src[l->pos] == '.') l->pos++;
                }
                tok.kind = TOK_PROG_END; return tok;
            }

            /* v2 navigation */
            if (c2 == '<') { l->pos++; tok.kind = TOK_BYTE_LEFT;  return tok; }
            if (c2 == '>') { l->pos++; tok.kind = TOK_BYTE_RIGHT; return tok; }
            if (c2 == '!') { l->pos++; tok.kind = TOK_IF_OPEN;    return tok; }

            /* !I[path] — input */
            if (c2 == 'I') {
                l->pos++; tok.kind = TOK_INPUT;
                tok.str = read_path_arg(l); return tok;
            }

            /* !O[path] or !O — output */
            if (c2 == 'O') {
                l->pos++; tok.kind = TOK_OUTPUT;
                tok.str = read_path_arg(l); return tok;
            }

            /* !E — stderr output */
            if (c2 == 'E') {
                l->pos++; tok.kind = TOK_STDERR; return tok;
            }

            /* !@n — absolute jump */
            if (c2 == '@') {
                l->pos++; tok.kind = TOK_ABS_JUMP;
                tok.num = read_number(l); return tok;
            }

            /* !+@n — relative forward */
            if (c2 == '+') {
                l->pos++;
                if (l->pos < l->len && l->src[l->pos] == '@') l->pos++;
                tok.kind = TOK_REL_FWD;
                tok.num  = read_number(l); return tok;
            }

            /* !-@n — relative backward */
            if (c2 == '-') {
                l->pos++;
                if (l->pos < l->len && l->src[l->pos] == '@') l->pos++;
                tok.kind = TOK_REL_BWD;
                tok.num  = read_number(l); return tok;
            }

            /* !n[ — count loop */
            if (isdigit((unsigned char)c2)) {
                tok.kind = TOK_COUNT_LOOP;
                tok.num  = read_number(l); return tok;
            }

            /* !"string" — string literal */
            if (c2 == '"') {
                l->pos++;
                size_t slen;
                char  *s    = parse_string_literal(l, &slen);
                tok.kind    = TOK_STR_LIT;
                tok.str     = s;
                tok.num     = (long)slen;
                return tok;
            }

            /* !#FF — hex byte literal */
            if (c2 == '#') {
                l->pos++;
                tok.kind = TOK_HEX_LIT;
                tok.num  = (long)parse_hex_byte(l);
                return tok;
            }

            /* !'A' — char literal */
            if (c2 == '\'') {
                l->pos++;
                unsigned char ch = 0;
                if (l->pos < l->len) {
                    ch = (unsigned char)l->src[l->pos++];
                    if (ch == '\\' && l->pos < l->len) {
                        char e = l->src[l->pos++];
                        switch (e) {
                            case 'n': ch = '\n'; break;
                            case 't': ch = '\t'; break;
                            case 'r': ch = '\r'; break;
                            case '0': ch = '\0'; break;
                            default:  ch = (unsigned char)e; break;
                        }
                    }
                }
                if (l->pos < l->len && l->src[l->pos] == '\'') l->pos++;
                tok.kind = TOK_CHAR_LIT;
                tok.num  = (long)ch;
                return tok;
            }

            /* !M — set mark */
            if (c2 == 'M') {
                l->pos++; tok.kind = TOK_MARK; return tok;
            }

            /* ![...] — selection */
            if (c2 == '[') {
                l->pos++;
                tok = parse_select_arg(l);
                return tok;
            }

            /* !X  !X->@n  !X->?  !X->! — execute */
            if (c2 == 'X') {
                l->pos++;
                tok.kind      = TOK_EXEC;
                tok.exec_mode = EXEC_PLAIN;
                skip_ws(l);
                /* check for -> */
                if (l->pos + 1 < l->len &&
                    l->src[l->pos] == '-' && l->src[l->pos+1] == '>') {
                    l->pos += 2;
                    skip_ws(l);
                    if (l->pos < l->len) {
                        char m = l->src[l->pos];
                        if (m == '@') {
                            l->pos++;
                            tok.exec_mode = EXEC_CAP_TAPE;
                            tok.num       = read_number(l);
                        } else if (m == '?') {
                            l->pos++;
                            tok.exec_mode = EXEC_CAP_EXIT;
                        } else if (m == '!') {
                            l->pos++;
                            tok.exec_mode = EXEC_PIPE_IN;
                        }
                    }
                }
                return tok;
            }

            /* !L[path] — load file onto tape */
            if (c2 == 'L') {
                l->pos++; tok.kind = TOK_LOAD;
                tok.str = read_path_arg(l); return tok;
            }

            /* !W[path] — write selection to file */
            if (c2 == 'W') {
                l->pos++; tok.kind = TOK_WRITE;
                tok.str = read_path_arg(l); return tok;
            }

            /* !$VARNAME  or  !$VARNAME<-[n:m] */
            if (c2 == '$') {
                l->pos++;
                char *name = read_env_name(l);
                skip_ws(l);
                /* check for <- */
                if (l->pos + 1 < l->len &&
                    l->src[l->pos] == '<' && l->src[l->pos+1] == '-') {
                    l->pos += 2;
                    skip_ws(l);
                    /* expect [n:m] selection */
                    if (l->pos < l->len && l->src[l->pos] == '[') {
                        l->pos++;
                        Token sel = parse_select_arg(l);
                        tok.kind     = TOK_ENV_WRITE;
                        tok.str      = name;
                        tok.num      = sel.num;
                        tok.num2     = sel.num2;
                        tok.sel_kind = sel.sel_kind;
                    } else {
                        tok.kind = TOK_ENV_WRITE;
                        tok.str  = name;
                    }
                } else {
                    tok.kind = TOK_ENV_READ;
                    tok.str  = name;
                }
                return tok;
            }

            /* !~ — load current script onto tape */
            if (c2 == '~') {
                l->pos++; tok.kind = TOK_SELF_LOAD; return tok;
            }

            /* !^[n:m] — evaluate tape region as bitlang */
            if (c2 == '^') {
                l->pos++;
                skip_ws(l);
                if (l->pos < l->len && l->src[l->pos] == '[') {
                    l->pos++;
                    Token sel    = parse_select_arg(l);
                    tok.kind     = TOK_EVAL;
                    tok.num      = sel.num;
                    tok.num2     = sel.num2;
                    tok.sel_kind = sel.sel_kind;
                } else {
                    tok.kind = TOK_EVAL;
                }
                return tok;
            }

            fprintf(stderr, "Error: unknown '!' sequence '!%c' at pos %zu\n",
                    c2, l->pos);
            exit(1);
        }

        if (c == '$') {
            l->pos++;
            if (l->pos < l->len && l->src[l->pos] == '!') {
                l->pos++; tok.kind = TOK_FORGE_CLOSE; return tok;
            }
            tok.kind = TOK_FORGE_OPEN; return tok;
        }

        fprintf(stderr, "Error: unexpected char '%c' at pos %zu\n", c, l->pos);
        exit(1);
    }
}

/* =========================================================
 * TOKEN STREAM  (same as v2)
 * ========================================================= */

typedef struct {
    Token  *tokens;
    size_t  count, capacity, pos;
} TokenStream;

static void ts_init(TokenStream *ts) {
    ts->capacity = 256;
    ts->tokens   = malloc(ts->capacity * sizeof(Token));
    if (!ts->tokens) { fputs("OOM\n", stderr); exit(1); }
    ts->count = ts->pos = 0;
}

static void ts_push(TokenStream *ts, Token t) {
    if (ts->count >= ts->capacity) {
        size_t newcap = ts->capacity * 2;
        Token *tmp    = realloc(ts->tokens, newcap * sizeof(Token));
        if (!tmp) { fputs("OOM\n", stderr); exit(1); }
        ts->tokens   = tmp;
        ts->capacity = newcap;
    }
    ts->tokens[ts->count++] = t;
}

static void ts_free(TokenStream *ts) {
    for (size_t i = 0; i < ts->count; i++)
        if (ts->tokens[i].str) free(ts->tokens[i].str);
    free(ts->tokens);
}

static Token ts_peek(TokenStream *ts) {
    if (ts->pos >= ts->count) return (Token){TOK_EOF, 0, 0, NULL, SEL_SINGLE, EXEC_PLAIN};
    return ts->tokens[ts->pos];
}

static Token ts_consume(TokenStream *ts) {
    if (ts->pos >= ts->count) return (Token){TOK_EOF, 0, 0, NULL, SEL_SINGLE, EXEC_PLAIN};
    return ts->tokens[ts->pos++];
}

static void tokenise(const char *src, TokenStream *ts) {
    Lexer l; lexer_init(&l, src);
    for (;;) {
        Token t = next_token(&l);
        ts_push(ts, t);
        if (t.kind == TOK_EOF || t.kind == TOK_PROG_END) break;
    }
}

/* =========================================================
 * FILE LOADING
 * ========================================================= */

static char *load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) { perror(path); exit(1); }
    rewind(f);
    char *buf = malloc((size_t)size + 1);
    if (!buf) { fputs("OOM\n", stderr); exit(1); }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "Error reading %s\n", path); exit(1);
    }
    buf[size] = '\0';
    fclose(f);
    return buf;
}

/* =========================================================
 * EXECUTOR STATE
 *
 * Bundled so it can be passed to sub-functions cleanly.
 * ========================================================= */

/* Maximum recursion depth for !^ (eval) to prevent stack exhaustion */
#define MAX_EVAL_DEPTH 64

/* Maximum bytes loadable via !L to prevent unbounded tape growth (256 MiB) */
#define MAX_LOAD_BYTES (256ULL * 1024ULL * 1024ULL)

typedef struct {
    Tape      *tape;
    uint64_t  *head;
    Selection *sel;
    const char *script_path;  /* for !~ */
    int        eval_depth;    /* recursion depth of !^ calls */
} ExecState;

static void exec_until_terminator(TokenStream *ts, ExecState *st);

/* =========================================================
 * SELECTION HELPERS
 * ========================================================= */

/* Resolve a token's selection fields into concrete (start, end) byte indices.
   Returns 0 on success, -1 if mark is needed but not set. */
static int sel_resolve(const Token *t, const Selection *sel,
                       uint64_t head_byte,
                       uint64_t *out_start, uint64_t *out_end) {
    switch (t->sel_kind) {
    case SEL_RANGE:
        *out_start = (uint64_t)t->num;
        *out_end   = (uint64_t)t->num2;
        break;
    case SEL_SINGLE:
        *out_start = (uint64_t)t->num;
        *out_end   = (uint64_t)t->num;
        break;
    case SEL_REL:
        if ((uint64_t)t->num2 == 0) {
            fputs("Error: ![n:+0] zero-length selection is not allowed\n", stderr);
            return -1;
        }
        *out_start = (uint64_t)t->num;
        *out_end   = (uint64_t)t->num + (uint64_t)t->num2 - 1;
        break;
    case SEL_MARK_HEAD:
        if (!sel->mark_set) {
            fputs("Error: ![M:@] used but no mark set (!M)\n", stderr);
            return -1;
        }
        *out_start = sel->mark;
        *out_end   = head_byte;
        if (*out_end < *out_start) {
            uint64_t tmp = *out_start; *out_start = *out_end; *out_end = tmp;
        }
        break;
    }
    return 0;
}

/* Build a heap buffer of bytes from tape selection [start..end] inclusive. */
static unsigned char *sel_read_bytes(Tape *tape,
                                     uint64_t start, uint64_t end,
                                     size_t *out_len) {
    size_t         len = (size_t)(end - start + 1);
    unsigned char *buf = malloc(len + 1);
    if (!buf) { fputs("OOM\n", stderr); exit(1); }
    for (size_t i = 0; i < len; i++)
        buf[i] = tape_read_byte(tape, (start + (uint64_t)i) * 8);
    buf[len] = '\0';
    *out_len  = len;
    return buf;
}

/* =========================================================
 * I/O HELPERS
 * ========================================================= */

static void do_input(ExecState *st, const char *path) {
    FILE *f = (!path || !path[0]) ? stdin : fopen(path, "rb");
    if (!f) { perror(path); return; }
    int ch;
    /* If selection active, write into that range */
    if (st->sel->active) {
        uint64_t byte = st->sel->start;
        while ((ch = fgetc(f)) != EOF && byte <= st->sel->end) {
            tape_write_byte(st->tape, byte * 8, (unsigned char)ch);
            byte++;
        }
    } else {
        /* Stream mode: write from head, advance */
        while ((ch = fgetc(f)) != EOF) {
            tape_write_byte(st->tape, *st->head, (unsigned char)ch);
            *st->head += 8;
        }
    }
    if (f != stdin) fclose(f);
}

static void do_output(ExecState *st, const char *path) {
    FILE *f = (!path || !path[0]) ? stdout : fopen(path, "ab");
    if (!f) { perror(path); return; }

    if (st->sel->active) {
        /* Output entire selection */
        for (uint64_t b = st->sel->start; b <= st->sel->end; b++)
            fputc(tape_read_byte(st->tape, b * 8), f);
    } else if (f == stdout) {
        /* Legacy stdout: single byte at head */
        fputc(tape_read_byte(st->tape, *st->head), f);
        *st->head += 8;
    } else {
        /* Legacy file: bytes until null terminator (v2 behaviour) */
        for (;;) {
            unsigned char byte = tape_read_byte(st->tape, *st->head);
            *st->head += 8;
            if (byte == 0) break;
            fputc(byte, f);
        }
    }

    if (f != stdout) fclose(f);
    else fflush(stdout);
}

static void do_stderr_out(ExecState *st) {
    if (st->sel->active) {
        for (uint64_t b = st->sel->start; b <= st->sel->end; b++)
            fputc(tape_read_byte(st->tape, b * 8), stderr);
    } else {
        fputc(tape_read_byte(st->tape, *st->head), stderr);
        *st->head += 8;
    }
}

/* =========================================================
 * EXECUTE SELECTION AS SHELL COMMAND
 * ========================================================= */

static void do_exec(ExecState *st, ExecMode mode, long cap_byte) {
    if (!st->sel->active) {
        fputs("Error: !X requires an active selection\n", stderr);
        return;
    }
    size_t         cmdlen;
    unsigned char *cmdbuf = sel_read_bytes(st->tape,
                                           st->sel->start, st->sel->end,
                                           &cmdlen);
    /* null-terminate for system/popen */
    char *cmd = (char *)cmdbuf;

    if (mode == EXEC_PLAIN) {
        int ret = system(cmd);
        (void)ret;

    } else if (mode == EXEC_CAP_TAPE) {
        /* Capture stdout into tape at cap_byte */
        FILE *p = popen(cmd, "r");
        if (!p) { perror("popen"); free(cmdbuf); return; }
        uint64_t pos = (uint64_t)cap_byte * 8;
        int ch;
        while ((ch = fgetc(p)) != EOF) {
            tape_write_byte(st->tape, pos, (unsigned char)ch);
            pos += 8;
        }
        pclose(p);

    } else if (mode == EXEC_CAP_EXIT) {
        /* Capture exit code as a byte at head */
        int ret = system(cmd);
        int code = WIFEXITED(ret) ? WEXITSTATUS(ret) : 255;
        tape_write_byte(st->tape, *st->head, (unsigned char)code);

    } else if (mode == EXEC_PIPE_IN) {
        /* Pipe stdin through the command */
        /* Read all of stdin first */
        size_t in_cap = 4096, in_len = 0;
        char  *in_buf = malloc(in_cap);
        if (!in_buf) { free(cmdbuf); fputs("OOM\n", stderr); exit(1); }
        int    ch;
        while ((ch = fgetc(stdin)) != EOF) {
            if (in_len >= in_cap) {
                size_t newcap = in_cap * 2;
                char  *tmp    = realloc(in_buf, newcap);
                if (!tmp) { free(in_buf); free(cmdbuf); fputs("OOM\n", stderr); exit(1); }
                in_buf = tmp; in_cap = newcap;
            }
            in_buf[in_len++] = (char)ch;
        }

        /* Write to a temp file, pipe through command */
        char tmp[] = "/tmp/bitlang_pipe_XXXXXX";
        int  fd    = mkstemp(tmp);
        if (fd >= 0) {
            size_t written = 0;
            while (written < in_len) {
                ssize_t w = write(fd, in_buf + written, in_len - written);
                if (w < 0) { perror("write"); break; }
                written += (size_t)w;
            }
            close(fd);
            /* run: cmd < tmpfile */
            size_t full_len = cmdlen + strlen(tmp) + 8;
            char  *full_cmd = malloc(full_len);
            if (!full_cmd) { free(in_buf); free(cmdbuf); unlink(tmp); fputs("OOM\n", stderr); exit(1); }
            snprintf(full_cmd, full_len, "%s < %s", cmd, tmp);
            int ret = system(full_cmd);
            (void)ret;
            free(full_cmd);
            unlink(tmp);
        }
        free(in_buf);
    }

    free(cmdbuf);
}

/* =========================================================
 * LOAD FILE ONTO TAPE
 * ========================================================= */

static void do_load(ExecState *st, const char *path) {
    if (!path || !path[0]) {
        fputs("Error: !L requires a path\n", stderr); return;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return; }
    uint64_t loaded = 0;
    int ch;
    while ((ch = fgetc(f)) != EOF) {
        if (loaded >= MAX_LOAD_BYTES) {
            fprintf(stderr, "Error: !L file exceeds size limit (%llu bytes): %s\n",
                    (unsigned long long)MAX_LOAD_BYTES, path);
            fclose(f);
            return;
        }
        tape_write_byte(st->tape, *st->head, (unsigned char)ch);
        *st->head += 8;
        loaded++;
    }
    fclose(f);
}

/* =========================================================
 * WRITE SELECTION TO FILE
 * ========================================================= */

static void do_write(ExecState *st, const char *path) {
    if (!st->sel->active) {
        fputs("Error: !W requires an active selection\n", stderr); return;
    }
    if (!path || !path[0]) {
        fputs("Error: !W requires a path\n", stderr); return;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    for (uint64_t b = st->sel->start; b <= st->sel->end; b++)
        fputc(tape_read_byte(st->tape, b * 8), f);
    fclose(f);
}

/* =========================================================
 * ENVIRONMENT VARIABLE OPS
 * ========================================================= */

static void do_env_read(ExecState *st, const char *name) {
    const char *val = getenv(name);
    if (!val) return;
    size_t len = strlen(val);
    tape_write_string(st->tape, st->head, val, len);
}

static void do_env_write(ExecState *st, const char *name,
                         uint64_t start, uint64_t end) {
    size_t         len;
    unsigned char *buf = sel_read_bytes(st->tape, start, end, &len);
    setenv(name, (char *)buf, 1);
    free(buf);
}

/* =========================================================
 * SELF-LOAD (!~)
 * ========================================================= */

static void do_self_load(ExecState *st) {
    if (!st->script_path) {
        fputs("Error: !~ used but script path is unknown\n", stderr); return;
    }
    do_load(st, st->script_path);
}

/* =========================================================
 * EVAL TAPE REGION AS BITLANG (!^[n:m])
 * ========================================================= */

static void do_eval(ExecState *st, uint64_t start, uint64_t end) {
    if (st->eval_depth >= MAX_EVAL_DEPTH) {
        fprintf(stderr, "Error: !^ recursion limit (%d) exceeded\n", MAX_EVAL_DEPTH);
        return;
    }
    size_t         len;
    unsigned char *buf = sel_read_bytes(st->tape, start, end, &len);

    /* Treat the bytes as source text */
    char *src = (char *)buf;

    TokenStream ts2; ts_init(&ts2);
    tokenise(src, &ts2);

    ExecState st2       = *st; /* share tape, head, sel, script_path */
    st2.eval_depth      = st->eval_depth + 1;
    exec_until_terminator(&ts2, &st2);
    /* propagate head changes back */
    *st->head = *st2.head;

    ts_free(&ts2);
    free(buf);
}

/* =========================================================
 * CONTROL FLOW HELPERS  (same as v2)
 * ========================================================= */

static void skip_to_while_close(TokenStream *ts) {
    int depth = 1;
    while (depth > 0) {
        Token sk = ts_consume(ts);
        if (sk.kind == TOK_WHILE_OPEN)  depth++;
        if (sk.kind == TOK_WHILE_CLOSE) depth--;
        if (sk.kind == TOK_EOF) break;
    }
}

static void skip_to_line_end(TokenStream *ts) {
    int depth = 0;
    for (;;) {
        Token sk = ts_peek(ts);
        if (sk.kind == TOK_EOF || sk.kind == TOK_PROG_END) return;
        if (sk.kind == TOK_WHILE_OPEN || sk.kind == TOK_IF_OPEN ||
            sk.kind == TOK_FORGE_OPEN)
            { depth++; ts_consume(ts); continue; }
        if (sk.kind == TOK_WHILE_CLOSE || sk.kind == TOK_BLOCK_CLOSE ||
            sk.kind == TOK_FORGE_CLOSE)
            { if (depth > 0) { depth--; ts_consume(ts); continue; } return; }
        if ((sk.kind == TOK_LINE_END || sk.kind == TOK_BLOCK_END) && depth == 0)
            return;
        ts_consume(ts);
    }
}

/* =========================================================
 * PARALLEL FORGE BLOCK  (same as v2, adapted for ExecState)
 * ========================================================= */

typedef struct {
    TokenStream  ts_view;
    ExecState    st;
    uint64_t     head_local;
    Selection    sel_local;
} ForgeArg;

static void *forge_thread(void *arg) {
    ForgeArg *fa = (ForgeArg *)arg;
    fa->st.head  = &fa->head_local;
    fa->st.sel   = &fa->sel_local;
    exec_until_terminator(&fa->ts_view, &fa->st);
    return NULL;
}

static void exec_forge(TokenStream *ts, ExecState *st) {
#define MAX_FORGE_LINES 64
    size_t starts[MAX_FORGE_LINES];
    int    nlines = 0;
    int    depth  = 1;
    size_t i      = ts->pos;

    if (nlines < MAX_FORGE_LINES) starts[nlines++] = i;

    while (i < ts->count && depth > 0) {
        TokenKind k = ts->tokens[i].kind;
        if (k == TOK_FORGE_OPEN)  { depth++; i++; continue; }
        if (k == TOK_FORGE_CLOSE) { depth--; if (depth == 0) break; i++; continue; }
        if (k == TOK_FORGE_SEP && depth == 1) {
            i++;
            if (nlines < MAX_FORGE_LINES) starts[nlines++] = i;
            continue;
        }
        i++;
    }
    ts->pos = i;
    if (ts->pos < ts->count && ts->tokens[ts->pos].kind == TOK_FORGE_CLOSE)
        ts->pos++;

    if (nlines == 0) return;

    pthread_t threads[MAX_FORGE_LINES];
    ForgeArg  args[MAX_FORGE_LINES];

    for (int j = 0; j < nlines; j++) {
        args[j].ts_view      = *ts;
        args[j].ts_view.pos  = starts[j];
        args[j].head_local   = *st->head;
        args[j].sel_local    = *st->sel;
        args[j].st           = *st;
        args[j].st.head      = &args[j].head_local;
        args[j].st.sel       = &args[j].sel_local;
        pthread_create(&threads[j], NULL, forge_thread, &args[j]);
    }
    for (int j = 0; j < nlines; j++)
        pthread_join(threads[j], NULL);
}

/* =========================================================
 * MAIN DISPATCH LOOP
 * ========================================================= */

static void exec_until_terminator(TokenStream *ts, ExecState *st) {
    for (;;) {
        Token t = ts_peek(ts);

        switch (t.kind) {
        case TOK_EOF:
        case TOK_LINE_END:
        case TOK_BLOCK_END:
        case TOK_BLOCK_CLOSE:
        case TOK_WHILE_CLOSE:
        case TOK_FORGE_SEP:
        case TOK_FORGE_CLOSE:
        case TOK_PROG_END:
            return;

        /* ---- v2 navigation ---- */
        case TOK_BIT_LEFT:
            ts_consume(ts);
            if (*st->head == 0) { fputs("Error: head before bit 0\n", stderr); exit(1); }
            (*st->head)--;
            break;

        case TOK_BIT_RIGHT:
            ts_consume(ts);
            (*st->head)++;
            break;

        case TOK_BYTE_LEFT:
            ts_consume(ts);
            if (*st->head < 8) { fputs("Error: head before bit 0\n", stderr); exit(1); }
            *st->head -= 8;
            break;

        case TOK_BYTE_RIGHT:
            ts_consume(ts);
            *st->head += 8;
            break;

        case TOK_ABS_JUMP:
            ts_consume(ts);
            *st->head = (uint64_t)t.num * 8;
            break;

        case TOK_REL_FWD:
            ts_consume(ts);
            *st->head += (uint64_t)t.num * 8;
            break;

        case TOK_REL_BWD:
            ts_consume(ts);
            if (*st->head < (uint64_t)t.num * 8) {
                fputs("Error: head before bit 0\n", stderr); exit(1);
            }
            *st->head -= (uint64_t)t.num * 8;
            break;

        /* ---- v2 bit ops ---- */
        case TOK_SET:
            ts_consume(ts);
            tape_write_bit(st->tape, *st->head, 1);
            break;

        case TOK_CLEAR:
            ts_consume(ts);
            tape_write_bit(st->tape, *st->head, 0);
            break;

        case TOK_FLIP:
            ts_consume(ts);
            tape_flip_bit(st->tape, *st->head);
            break;

        /* ---- v2 IF block ---- */
        case TOK_IF_OPEN: {
            ts_consume(ts);
            Token bopen = ts_consume(ts);
            if (bopen.kind != TOK_BLOCK_OPEN) {
                fputs("Error: expected '{' after '!!'\n", stderr); exit(1);
            }
            uint64_t saved = *st->head;
            exec_until_terminator(ts, st);
            Token bclose = ts_consume(ts);
            if (bclose.kind != TOK_BLOCK_CLOSE) {
                fputs("Error: expected '}!' closing IF condition\n", stderr); exit(1);
            }
            int truth = tape_read_bit(st->tape, *st->head);
            *st->head = saved;
            if (truth) exec_until_terminator(ts, st);
            else       skip_to_line_end(ts);
            break;
        }

        /* ---- v2 while loop ---- */
        case TOK_WHILE_OPEN: {
            ts_consume(ts);
            size_t loop_start = ts->pos;
            if (!tape_read_bit(st->tape, *st->head)) {
                skip_to_while_close(ts);
                break;
            }
            for (;;) {
                ts->pos = loop_start;
                exec_until_terminator(ts, st);
                Token wc = ts_consume(ts);
                if (wc.kind != TOK_WHILE_CLOSE) {
                    fputs("Error: expected ']' closing while\n", stderr); exit(1);
                }
                if (!tape_read_bit(st->tape, *st->head)) break;
            }
            break;
        }

        /* ---- v2 count loop ---- */
        case TOK_COUNT_LOOP: {
            ts_consume(ts);
            long count = t.num;
            Token wopen = ts_consume(ts);
            if (wopen.kind != TOK_WHILE_OPEN) {
                fprintf(stderr, "Error: expected '[' after '!%ld'\n", count); exit(1);
            }
            size_t loop_start = ts->pos;
            if (count == 0) { skip_to_while_close(ts); break; }
            for (long k = 0; k < count; k++) {
                ts->pos = loop_start;
                exec_until_terminator(ts, st);
                Token wc = ts_consume(ts);
                if (wc.kind != TOK_WHILE_CLOSE) {
                    fputs("Error: expected ']' closing count loop\n", stderr); exit(1);
                }
            }
            break;
        }

        /* ---- v2/v3 Input ---- */
        case TOK_INPUT:
            ts_consume(ts);
            do_input(st, t.str);
            break;

        /* ---- v2/v3 Output ---- */
        case TOK_OUTPUT:
            ts_consume(ts);
            do_output(st, t.str);
            break;

        /* ---- v3 Stderr output ---- */
        case TOK_STDERR:
            ts_consume(ts);
            do_stderr_out(st);
            break;

        /* ---- v2 Forge block ---- */
        case TOK_FORGE_OPEN:
            ts_consume(ts);
            exec_forge(ts, st);
            break;

        /* ---- v2 File jump ---- */
        case TOK_FILE_JUMP: {
            ts_consume(ts);
            if (!t.str || !t.str[0]) {
                fputs("Error: empty path in %//\n", stderr); exit(1);
            }
            size_t plen = strlen(t.str);
            if (plen < 9 || strcmp(t.str + plen - 8, ".bitlang") != 0) {
                fprintf(stderr,
                    "Error: %%// requires a .bitlang file (got '%s')\n", t.str);
                exit(1);
            }
            char *src2 = load_file(t.str);
            TokenStream ts2; ts_init(&ts2);
            tokenise(src2, &ts2);
            ExecState st2   = *st;
            st2.script_path = t.str;
            exec_until_terminator(&ts2, &st2);
            *st->head = *st2.head;
            ts_free(&ts2);
            free(src2);
            break;
        }

        /* ---- v3 String literal ---- */
        case TOK_STR_LIT:
            ts_consume(ts);
            tape_write_string(st->tape, st->head, t.str, (size_t)t.num);
            break;

        /* ---- v3 Hex byte literal ---- */
        case TOK_HEX_LIT:
            ts_consume(ts);
            tape_write_byte(st->tape, *st->head, (unsigned char)t.num);
            *st->head += 8;
            break;

        /* ---- v3 Char literal ---- */
        case TOK_CHAR_LIT:
            ts_consume(ts);
            tape_write_byte(st->tape, *st->head, (unsigned char)t.num);
            *st->head += 8;
            break;

        /* ---- v3 Mark ---- */
        case TOK_MARK:
            ts_consume(ts);
            st->sel->mark     = *st->head / 8;
            st->sel->mark_set = 1;
            break;

        /* ---- v3 Selection ---- */
        case TOK_SELECT: {
            ts_consume(ts);
            uint64_t s, e;
            if (sel_resolve(&t, st->sel, *st->head / 8, &s, &e) == 0) {
                st->sel->active = 1;
                st->sel->start  = s;
                st->sel->end    = e;
            }
            break;
        }

        /* ---- v3 Execute ---- */
        case TOK_EXEC:
            ts_consume(ts);
            do_exec(st, t.exec_mode, t.num);
            break;

        /* ---- v3 Load file ---- */
        case TOK_LOAD:
            ts_consume(ts);
            do_load(st, t.str);
            break;

        /* ---- v3 Write selection to file ---- */
        case TOK_WRITE:
            ts_consume(ts);
            do_write(st, t.str);
            break;

        /* ---- v3 Env read ---- */
        case TOK_ENV_READ:
            ts_consume(ts);
            do_env_read(st, t.str);
            break;

        /* ---- v3 Env write ---- */
        case TOK_ENV_WRITE: {
            ts_consume(ts);
            uint64_t s, e;
            if (sel_resolve(&t, st->sel, *st->head / 8, &s, &e) == 0)
                do_env_write(st, t.str, s, e);
            break;
        }

        /* ---- v3 Self-load ---- */
        case TOK_SELF_LOAD:
            ts_consume(ts);
            do_self_load(st);
            break;

        /* ---- v3 Eval tape region ---- */
        case TOK_EVAL: {
            ts_consume(ts);
            uint64_t s = (uint64_t)t.num;
            uint64_t e = (uint64_t)t.num2;
            do_eval(st, s, e);
            break;
        }

        default:
            ts_consume(ts);
            break;
        }
    }
}

static void exec_line(TokenStream *ts, ExecState *st) {
    exec_until_terminator(ts, st);
    Token term = ts_peek(ts);
    if (term.kind == TOK_LINE_END || term.kind == TOK_BLOCK_END)
        ts_consume(ts);
}

static void exec_program(TokenStream *ts, ExecState *st) {
    for (;;) {
        Token t = ts_peek(ts);
        if (t.kind == TOK_EOF || t.kind == TOK_PROG_END) break;
        exec_line(ts, st);
    }
}

/* =========================================================
 * MAIN
 * ========================================================= */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file.bitlang>\n", argv[0]);
        return 1;
    }

    char *src = load_file(argv[1]);
    TokenStream ts; ts_init(&ts);
    tokenise(src, &ts);

    Tape      tape; tape_init(&tape);
    uint64_t  head = 0;
    Selection sel  = { 0, 0, 0, 0, 0 };

    ExecState st;
    st.tape        = &tape;
    st.head        = &head;
    st.sel         = &sel;
    st.script_path = argv[1];
    st.eval_depth  = 0;

    exec_program(&ts, &st);

    tape_free(&tape);
    ts_free(&ts);
    free(src);
    return 0;
}
