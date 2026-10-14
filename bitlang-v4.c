/* bitlang-v4.c -- Bitlang interpreter */

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

/* ===== LIMITS ===== */

#define MAX_HEAD          (UINT64_MAX - 7)
#define MAX_BYTE_INDEX    ((UINT64_MAX - 7) / 8)
#define MAX_LOAD_BYTES    (256ULL * 1024ULL * 1024ULL)
#define MAX_SEL_BYTES     MAX_LOAD_BYTES
#define MAX_SCRIPT_BYTES  MAX_LOAD_BYTES
#define MAX_EXEC_DEPTH    64
#define MAX_FORGE_LINES   64

/* ===== ERRORS ===== */

__attribute__((noreturn))
static void oom(void) {
    fputs("OOM\n", stderr);
    exit(1);
}

/* ===== SPARSE TAPE ===== */

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

static Chunk *tape_find(Tape *t, uint64_t cidx) {
    Chunk *c = t->map[cidx % CHUNK_MAP_SIZE];
    while (c) {
        if (c->index == cidx) return c;
        c = c->next;
    }
    return NULL;
}

static Chunk *tape_chunk(Tape *t, uint64_t cidx) {
    Chunk *c = tape_find(t, cidx);
    if (c) return c;
    c = calloc(1, sizeof(Chunk));
    if (!c) oom();
    uint64_t slot = cidx % CHUNK_MAP_SIZE;
    c->index     = cidx;
    c->next      = t->map[slot];
    t->map[slot] = c;
    return c;
}

static int bit_get_locked(Tape *t, uint64_t bit) {
    Chunk *c = tape_find(t, bit / CHUNK_BITS);
    if (!c) return 0;
    uint64_t lo = bit % CHUNK_BITS;
    return (int)(((unsigned)c->data[lo / 8] >> (7 - lo % 8)) & 1u);
}

static void bit_put_locked(Tape *t, uint64_t bit, int val) {
    Chunk   *c    = tape_chunk(t, bit / CHUNK_BITS);
    uint64_t lo   = bit % CHUNK_BITS;
    unsigned mask = 1u << (7 - lo % 8);
    if (val) c->data[lo / 8] |= (unsigned char)mask;
    else     c->data[lo / 8] &= (unsigned char)~mask;
}

static int tape_read_bit(Tape *t, uint64_t bit) {
    pthread_mutex_lock(&t->lock);
    int val = bit_get_locked(t, bit);
    pthread_mutex_unlock(&t->lock);
    return val;
}

static void tape_write_bit(Tape *t, uint64_t bit, int val) {
    pthread_mutex_lock(&t->lock);
    bit_put_locked(t, bit, val);
    pthread_mutex_unlock(&t->lock);
}

static void tape_flip_bit(Tape *t, uint64_t bit) {
    pthread_mutex_lock(&t->lock);
    bit_put_locked(t, bit, !bit_get_locked(t, bit));
    pthread_mutex_unlock(&t->lock);
}

static unsigned char tape_read_byte(Tape *t, uint64_t bit) {
    unsigned char b = 0;
    pthread_mutex_lock(&t->lock);
    for (int i = 0; i < 8; i++)
        b = (unsigned char)((b << 1) | bit_get_locked(t, bit + (uint64_t)i));
    pthread_mutex_unlock(&t->lock);
    return b;
}

static void tape_write_byte(Tape *t, uint64_t bit, unsigned char byte) {
    pthread_mutex_lock(&t->lock);
    for (int i = 0; i < 8; i++)
        bit_put_locked(t, bit + (uint64_t)i, (byte >> (7 - i)) & 1);
    pthread_mutex_unlock(&t->lock);
}

static void head_add(uint64_t *head, uint64_t n) {
    if (n > MAX_HEAD - *head) {
        fputs("Error: head past end of tape\n", stderr);
        exit(1);
    }
    *head += n;
}

static void tape_write_string(Tape *t, uint64_t *head, const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        tape_write_byte(t, *head, (unsigned char)s[i]);
        head_add(head, 8);
    }
}

/* ===== SELECTION STATE ===== */

typedef struct {
    int      active;
    uint64_t start;
    uint64_t end;
    uint64_t mark;
    int      mark_set;
} Selection;

/* ===== TOKEN TYPES ===== */

typedef enum {
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
    TOK_STR_LIT,
    TOK_HEX_LIT,
    TOK_CHAR_LIT,
    TOK_MARK,
    TOK_SELECT,
    TOK_STDERR,
    TOK_EXEC,
    TOK_LOAD,
    TOK_WRITE,
    TOK_ENV_READ,
    TOK_ENV_WRITE,
    TOK_SELF_LOAD,
    TOK_EVAL,
} TokenKind;

typedef enum {
    SEL_RANGE,
    SEL_SINGLE,
    SEL_REL,
    SEL_MARK_HEAD,
} SelKind;

typedef enum {
    EXEC_PLAIN,
    EXEC_CAP_TAPE,
    EXEC_CAP_EXIT,
    EXEC_PIPE_IN,
} ExecMode;

typedef struct {
    TokenKind kind;
    uint64_t  num;
    uint64_t  num2;
    char     *str;
    SelKind   sel_kind;
    ExecMode  exec_mode;
    int       has_sel;
} Token;

static Token new_token(void) {
    Token t;
    memset(&t, 0, sizeof t);
    t.kind      = TOK_EOF;
    t.sel_kind  = SEL_SINGLE;
    t.exec_mode = EXEC_PLAIN;
    return t;
}

/* ===== LEXER ===== */

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

static char *dup_range(const char *s, size_t n) {
    char *out = malloc(n + 1);
    if (!out) oom();
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static uint64_t read_number(Lexer *l) {
    if (l->pos >= l->len || !isdigit((unsigned char)l->src[l->pos])) {
        fprintf(stderr, "Error: expected number at pos %zu\n", l->pos);
        exit(1);
    }
    uint64_t n = 0;
    while (l->pos < l->len && isdigit((unsigned char)l->src[l->pos])) {
        unsigned digit = (unsigned)(l->src[l->pos++] - '0');
        if (n > (MAX_BYTE_INDEX - digit) / 10) {
            fprintf(stderr, "Error: numeric literal too large at pos %zu\n", l->pos);
            exit(1);
        }
        n = n * 10 + digit;
    }
    return n;
}

static char *read_path_arg(Lexer *l) {
    skip_ws(l);
    if (l->pos >= l->len || l->src[l->pos] != '[') return NULL;
    l->pos++;
    size_t start = l->pos;
    while (l->pos < l->len && l->src[l->pos] != ']') l->pos++;
    if (l->pos >= l->len) {
        fprintf(stderr, "Error: unterminated '[' in path argument at pos %zu\n", start);
        exit(1);
    }
    size_t end = l->pos;
    l->pos++;
    return dup_range(l->src + start, end - start);
}

static char *read_filejump_path(Lexer *l) {
    skip_ws(l);
    size_t start = l->pos;
    while (l->pos < l->len && l->src[l->pos] != '\n' && l->src[l->pos] != '%')
        l->pos++;
    size_t end = l->pos;
    while (end > start && isspace((unsigned char)l->src[end - 1])) end--;
    return dup_range(l->src + start, end - start);
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int read_hex(Lexer *l, unsigned *out) {
    int      n = 0;
    unsigned v = 0;
    while (n < 2 && l->pos < l->len &&
           hexval((unsigned char)l->src[l->pos]) >= 0) {
        v = v * 16u + (unsigned)hexval((unsigned char)l->src[l->pos++]);
        n++;
    }
    *out = v;
    return n;
}

static char *parse_string_literal(Lexer *l, size_t *out_len) {
    size_t cap = 64;
    char  *buf = malloc(cap);
    if (!buf) oom();
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
                    unsigned v;
                    if (read_hex(l, &v) == 0) {
                        fprintf(stderr, "Error: invalid \\x escape at pos %zu\n", l->pos);
                        exit(1);
                    }
                    c = (char)v;
                    break;
                }
                default: c = e; break;
            }
        }
        if (len >= cap) {
            size_t newcap = cap * 2;
            char  *grown  = realloc(buf, newcap);
            if (!grown) { free(buf); oom(); }
            buf = grown; cap = newcap;
        }
        buf[len++] = c;
    }
    if (l->pos >= l->len) {
        free(buf);
        fputs("Error: unterminated string literal\n", stderr);
        exit(1);
    }
    l->pos++;
    *out_len = len;
    return buf;
}

static char *read_env_name(Lexer *l) {
    size_t start = l->pos;
    while (l->pos < l->len &&
           (isalnum((unsigned char)l->src[l->pos]) || l->src[l->pos] == '_'))
        l->pos++;
    return dup_range(l->src + start, l->pos - start);
}

static Token parse_select_arg(Lexer *l) {
    Token tok   = new_token();
    tok.kind    = TOK_SELECT;
    tok.has_sel = 1;
    skip_ws(l);

    if (l->pos < l->len && l->src[l->pos] == 'M') {
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
    Token tok = new_token();

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
                fprintf(stderr, "Error: bare '!' at end of source\n");
                exit(1);
            }
            char c2 = l->src[l->pos];

            if (c2 == '%') {
                l->pos++;
                if (l->pos < l->len && l->src[l->pos] == '!') {
                    l->pos++;
                    if (l->pos < l->len && l->src[l->pos] == '.') l->pos++;
                }
                tok.kind = TOK_PROG_END; return tok;
            }

            if (c2 == '<') { l->pos++; tok.kind = TOK_BYTE_LEFT;  return tok; }
            if (c2 == '>') { l->pos++; tok.kind = TOK_BYTE_RIGHT; return tok; }
            if (c2 == '!') { l->pos++; tok.kind = TOK_IF_OPEN;    return tok; }

            if (c2 == 'I') {
                l->pos++; tok.kind = TOK_INPUT;
                tok.str = read_path_arg(l); return tok;
            }

            if (c2 == 'O') {
                l->pos++; tok.kind = TOK_OUTPUT;
                tok.str = read_path_arg(l); return tok;
            }

            if (c2 == 'E') {
                l->pos++; tok.kind = TOK_STDERR; return tok;
            }

            if (c2 == '@') {
                l->pos++; tok.kind = TOK_ABS_JUMP;
                tok.num = read_number(l); return tok;
            }

            if (c2 == '+') {
                l->pos++;
                if (l->pos < l->len && l->src[l->pos] == '@') l->pos++;
                tok.kind = TOK_REL_FWD;
                tok.num  = read_number(l); return tok;
            }

            if (c2 == '-') {
                l->pos++;
                if (l->pos < l->len && l->src[l->pos] == '@') l->pos++;
                tok.kind = TOK_REL_BWD;
                tok.num  = read_number(l); return tok;
            }

            if (isdigit((unsigned char)c2)) {
                tok.kind = TOK_COUNT_LOOP;
                tok.num  = read_number(l); return tok;
            }

            if (c2 == '"') {
                l->pos++;
                size_t slen;
                char  *s    = parse_string_literal(l, &slen);
                tok.kind    = TOK_STR_LIT;
                tok.str     = s;
                tok.num     = (uint64_t)slen;
                return tok;
            }

            if (c2 == '#') {
                l->pos++;
                unsigned v;
                if (read_hex(l, &v) == 0) {
                    fprintf(stderr, "Error: expected hex digits after '!#' at pos %zu\n", l->pos);
                    exit(1);
                }
                tok.kind = TOK_HEX_LIT;
                tok.num  = v;
                return tok;
            }

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
                tok.num  = ch;
                return tok;
            }

            if (c2 == 'M') {
                l->pos++; tok.kind = TOK_MARK; return tok;
            }

            if (c2 == '[') {
                l->pos++;
                tok = parse_select_arg(l);
                return tok;
            }

            if (c2 == 'X') {
                l->pos++;
                tok.kind      = TOK_EXEC;
                tok.exec_mode = EXEC_PLAIN;
                skip_ws(l);
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

            if (c2 == 'L') {
                l->pos++; tok.kind = TOK_LOAD;
                tok.str = read_path_arg(l); return tok;
            }

            if (c2 == 'W') {
                l->pos++; tok.kind = TOK_WRITE;
                tok.str = read_path_arg(l); return tok;
            }

            if (c2 == '$') {
                l->pos++;
                char *name = read_env_name(l);
                skip_ws(l);
                if (l->pos + 1 < l->len &&
                    l->src[l->pos] == '<' && l->src[l->pos+1] == '-') {
                    l->pos += 2;
                    skip_ws(l);
                    tok.kind = TOK_ENV_WRITE;
                    tok.str  = name;
                    if (l->pos < l->len && l->src[l->pos] == '[') {
                        l->pos++;
                        Token sel    = parse_select_arg(l);
                        tok.num      = sel.num;
                        tok.num2     = sel.num2;
                        tok.sel_kind = sel.sel_kind;
                        tok.has_sel  = 1;
                    }
                } else {
                    tok.kind = TOK_ENV_READ;
                    tok.str  = name;
                }
                return tok;
            }

            if (c2 == '~') {
                l->pos++; tok.kind = TOK_SELF_LOAD; return tok;
            }

            if (c2 == '^') {
                l->pos++;
                skip_ws(l);
                tok.kind = TOK_EVAL;
                if (l->pos < l->len && l->src[l->pos] == '[') {
                    l->pos++;
                    Token sel    = parse_select_arg(l);
                    tok.num      = sel.num;
                    tok.num2     = sel.num2;
                    tok.sel_kind = sel.sel_kind;
                    tok.has_sel  = 1;
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

/* ===== TOKEN STREAM ===== */

typedef struct {
    Token  *tokens;
    size_t  count, capacity, pos;
} TokenStream;

static void ts_init(TokenStream *ts) {
    ts->capacity = 256;
    ts->tokens   = malloc(ts->capacity * sizeof(Token));
    if (!ts->tokens) oom();
    ts->count = ts->pos = 0;
}

static void ts_push(TokenStream *ts, Token t) {
    if (ts->count >= ts->capacity) {
        size_t newcap = ts->capacity * 2;
        Token *grown  = realloc(ts->tokens, newcap * sizeof(Token));
        if (!grown) oom();
        ts->tokens   = grown;
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
    if (ts->pos >= ts->count) return new_token();
    return ts->tokens[ts->pos];
}

static Token ts_consume(TokenStream *ts) {
    if (ts->pos >= ts->count) return new_token();
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

/* ===== FILE LOADING ===== */

static char *load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }

    size_t cap = 4096, len = 0;
    char  *buf = malloc(cap);
    if (!buf) oom();

    for (;;) {
        if (len + 1 >= cap) {
            size_t newcap = cap * 2;
            char  *grown  = realloc(buf, newcap);
            if (!grown) oom();
            buf = grown; cap = newcap;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, f);
        len += n;
        if (len > MAX_SCRIPT_BYTES) {
            fprintf(stderr, "Error: %s exceeds the script size limit (%llu bytes)\n",
                    path, (unsigned long long)MAX_SCRIPT_BYTES);
            fclose(f);
            exit(1);
        }
        if (n == 0) break;
    }
    if (ferror(f)) {
        fprintf(stderr, "Error reading %s\n", path);
        fclose(f);
        exit(1);
    }
    fclose(f);
    buf[len] = '\0';
    return buf;
}

/* ===== EXECUTOR STATE ===== */

typedef struct {
    Tape       *tape;
    uint64_t   *head;
    Selection  *sel;
    const char *script_path;
    int         depth;
} ExecState;

static void exec_until_terminator(TokenStream *ts, ExecState *st);
static void exec_program(TokenStream *ts, ExecState *st);

/* ===== SELECTION HELPERS ===== */

static int sel_resolve(const Token *t, const Selection *sel,
                       uint64_t head_byte,
                       uint64_t *out_start, uint64_t *out_end) {
    uint64_t s = 0, e = 0;
    switch (t->sel_kind) {
    case SEL_RANGE:
        s = t->num;
        e = t->num2;
        break;
    case SEL_SINGLE:
        s = e = t->num;
        break;
    case SEL_REL:
        if (t->num2 == 0) {
            fputs("Error: ![n:+0] zero-length selection is not allowed\n", stderr);
            return -1;
        }
        s = t->num;
        e = t->num + t->num2 - 1;
        break;
    case SEL_MARK_HEAD:
        if (!sel->mark_set) {
            fputs("Error: ![M:@] used but no mark set (!M)\n", stderr);
            return -1;
        }
        s = sel->mark;
        e = head_byte;
        break;
    }
    if (e < s) { uint64_t tmp = s; s = e; e = tmp; }
    if (e > MAX_BYTE_INDEX) {
        fputs("Error: selection extends past end of tape\n", stderr);
        return -1;
    }
    *out_start = s;
    *out_end   = e;
    return 0;
}

static int token_region(const Token *t, ExecState *st,
                        uint64_t *out_start, uint64_t *out_end) {
    if (t->has_sel)
        return sel_resolve(t, st->sel, *st->head / 8, out_start, out_end);
    if (st->sel->active) {
        *out_start = st->sel->start;
        *out_end   = st->sel->end;
        return 0;
    }
    fputs("Error: no selection given and none is active\n", stderr);
    return -1;
}

static unsigned char *sel_read_bytes(Tape *tape,
                                     uint64_t start, uint64_t end,
                                     size_t *out_len) {
    uint64_t span = end - start + 1;
    if (span > MAX_SEL_BYTES) {
        fprintf(stderr, "Error: selection too large (limit %llu bytes)\n",
                (unsigned long long)MAX_SEL_BYTES);
        return NULL;
    }
    size_t         len = (size_t)span;
    unsigned char *buf = malloc(len + 1);
    if (!buf) oom();
    for (size_t i = 0; i < len; i++)
        buf[i] = tape_read_byte(tape, (start + (uint64_t)i) * 8);
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

/* ===== I/O ===== */

static void do_input(ExecState *st, const char *path) {
    FILE *f = (!path || !path[0]) ? stdin : fopen(path, "rb");
    if (!f) { perror(path); return; }
    int ch;
    if (st->sel->active) {
        uint64_t byte = st->sel->start;
        while (byte <= st->sel->end && (ch = fgetc(f)) != EOF) {
            tape_write_byte(st->tape, byte * 8, (unsigned char)ch);
            byte++;
        }
    } else {
        while ((ch = fgetc(f)) != EOF) {
            tape_write_byte(st->tape, *st->head, (unsigned char)ch);
            head_add(st->head, 8);
        }
    }
    if (f != stdin) fclose(f);
}

static void do_output(ExecState *st, const char *path) {
    FILE *f = (!path || !path[0]) ? stdout : fopen(path, "ab");
    if (!f) { perror(path); return; }

    if (st->sel->active) {
        for (uint64_t b = st->sel->start; b <= st->sel->end; b++)
            fputc(tape_read_byte(st->tape, b * 8), f);
    } else if (f == stdout) {
        fputc(tape_read_byte(st->tape, *st->head), f);
        head_add(st->head, 8);
    } else {
        for (;;) {
            unsigned char byte = tape_read_byte(st->tape, *st->head);
            head_add(st->head, 8);
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
        head_add(st->head, 8);
    }
}

/* ===== SHELL EXECUTION ===== */

static void do_exec(ExecState *st, ExecMode mode, uint64_t cap_byte) {
    if (!st->sel->active) {
        fputs("Error: !X requires an active selection\n", stderr);
        return;
    }
    size_t         cmdlen;
    unsigned char *cmdbuf = sel_read_bytes(st->tape,
                                           st->sel->start, st->sel->end,
                                           &cmdlen);
    if (!cmdbuf) return;
    char *cmd = (char *)cmdbuf;

    fflush(stdout);
    fflush(stderr);

    if (mode == EXEC_PLAIN) {
        int ret = system(cmd);
        (void)ret;

    } else if (mode == EXEC_CAP_TAPE) {
        FILE *p = popen(cmd, "r");
        if (!p) { perror("popen"); free(cmdbuf); return; }
        uint64_t pos = cap_byte * 8;
        int ch;
        while ((ch = fgetc(p)) != EOF) {
            tape_write_byte(st->tape, pos, (unsigned char)ch);
            if (pos > MAX_HEAD - 8) {
                fputs("Error: !X capture reached end of tape\n", stderr);
                break;
            }
            pos += 8;
        }
        pclose(p);

    } else if (mode == EXEC_CAP_EXIT) {
        int ret  = system(cmd);
        int code = WIFEXITED(ret) ? WEXITSTATUS(ret) : 255;
        tape_write_byte(st->tape, *st->head, (unsigned char)code);

    } else if (mode == EXEC_PIPE_IN) {
        size_t in_cap = 4096, in_len = 0;
        char  *in_buf = malloc(in_cap);
        if (!in_buf) { free(cmdbuf); oom(); }
        int ch;
        while ((ch = fgetc(stdin)) != EOF) {
            if (in_len >= in_cap) {
                size_t newcap = in_cap * 2;
                char  *grown  = realloc(in_buf, newcap);
                if (!grown) { free(in_buf); free(cmdbuf); oom(); }
                in_buf = grown; in_cap = newcap;
            }
            in_buf[in_len++] = (char)ch;
        }

        char tmp_path[] = "/tmp/bitlang_pipe_XXXXXX";
        int  fd         = mkstemp(tmp_path);
        if (fd < 0) {
            perror("mkstemp");
        } else {
            size_t written = 0;
            while (written < in_len) {
                ssize_t w = write(fd, in_buf + written, in_len - written);
                if (w < 0) { perror("write"); break; }
                written += (size_t)w;
            }
            close(fd);
            size_t full_len = strlen(cmd) + strlen(tmp_path) + 8;
            char  *full_cmd = malloc(full_len);
            if (!full_cmd) { free(in_buf); free(cmdbuf); unlink(tmp_path); oom(); }
            snprintf(full_cmd, full_len, "%s < %s", cmd, tmp_path);
            int ret = system(full_cmd);
            (void)ret;
            free(full_cmd);
            unlink(tmp_path);
        }
        free(in_buf);
    }

    (void)cmdlen;
    free(cmdbuf);
}

/* ===== FILE AND ENVIRONMENT OPERATIONS ===== */

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
        head_add(st->head, 8);
        loaded++;
    }
    fclose(f);
}

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

static void do_env_read(ExecState *st, const char *name) {
    const char *val = getenv(name);
    if (!val) return;
    tape_write_string(st->tape, st->head, val, strlen(val));
}

static void do_env_write(ExecState *st, const char *name,
                         uint64_t start, uint64_t end) {
    size_t         len;
    unsigned char *buf = sel_read_bytes(st->tape, start, end, &len);
    if (!buf) return;
    (void)len;
    if (setenv(name, (char *)buf, 1) != 0) perror("setenv");
    free(buf);
}

static void do_self_load(ExecState *st) {
    if (!st->script_path) {
        fputs("Error: !~ used but script path is unknown\n", stderr); return;
    }
    do_load(st, st->script_path);
}

/* ===== EVAL ===== */

static void do_eval(ExecState *st, uint64_t start, uint64_t end) {
    if (st->depth >= MAX_EXEC_DEPTH) {
        fprintf(stderr, "Error: nesting limit (%d) exceeded in !^\n", MAX_EXEC_DEPTH);
        return;
    }
    size_t         len;
    unsigned char *buf = sel_read_bytes(st->tape, start, end, &len);
    if (!buf) return;
    (void)len;

    TokenStream ts2; ts_init(&ts2);
    tokenise((char *)buf, &ts2);

    ExecState st2 = *st;
    st2.depth     = st->depth + 1;
    exec_program(&ts2, &st2);

    ts_free(&ts2);
    free(buf);
}

/* ===== CONTROL FLOW HELPERS ===== */

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

/* ===== FORGE ===== */

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
    size_t starts[MAX_FORGE_LINES];
    int    nlines = 1;
    int    depth  = 1;
    size_t i      = ts->pos;

    starts[0] = i;

    while (i < ts->count && depth > 0) {
        TokenKind k = ts->tokens[i].kind;
        if (k == TOK_FORGE_OPEN)  { depth++; i++; continue; }
        if (k == TOK_FORGE_CLOSE) { depth--; if (depth == 0) break; i++; continue; }
        if (k == TOK_FORGE_SEP && depth == 1) {
            i++;
            if (nlines >= MAX_FORGE_LINES) {
                fprintf(stderr, "Error: too many forge lines (max %d)\n", MAX_FORGE_LINES);
                exit(1);
            }
            starts[nlines++] = i;
            continue;
        }
        i++;
    }
    ts->pos = i;
    if (ts->pos < ts->count && ts->tokens[ts->pos].kind == TOK_FORGE_CLOSE)
        ts->pos++;

    pthread_t threads[MAX_FORGE_LINES];
    ForgeArg  args[MAX_FORGE_LINES];
    int       started[MAX_FORGE_LINES];

    for (int j = 0; j < nlines; j++) {
        args[j].ts_view      = *ts;
        args[j].ts_view.pos  = starts[j];
        args[j].head_local   = *st->head;
        args[j].sel_local    = *st->sel;
        args[j].st           = *st;
        args[j].st.head      = &args[j].head_local;
        args[j].st.sel       = &args[j].sel_local;
        started[j] = (pthread_create(&threads[j], NULL, forge_thread, &args[j]) == 0);
        if (!started[j]) {
            fprintf(stderr, "Warning: could not start forge thread %d; running it inline\n", j);
            forge_thread(&args[j]);
        }
    }
    for (int j = 0; j < nlines; j++)
        if (started[j]) pthread_join(threads[j], NULL);
}

/* ===== DISPATCH LOOP ===== */

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

        case TOK_BIT_LEFT:
            ts_consume(ts);
            if (*st->head == 0) { fputs("Error: head before bit 0\n", stderr); exit(1); }
            (*st->head)--;
            break;

        case TOK_BIT_RIGHT:
            ts_consume(ts);
            head_add(st->head, 1);
            break;

        case TOK_BYTE_LEFT:
            ts_consume(ts);
            if (*st->head < 8) { fputs("Error: head before bit 0\n", stderr); exit(1); }
            *st->head -= 8;
            break;

        case TOK_BYTE_RIGHT:
            ts_consume(ts);
            head_add(st->head, 8);
            break;

        case TOK_ABS_JUMP:
            ts_consume(ts);
            *st->head = t.num * 8;
            break;

        case TOK_REL_FWD:
            ts_consume(ts);
            head_add(st->head, t.num * 8);
            break;

        case TOK_REL_BWD:
            ts_consume(ts);
            if (*st->head < t.num * 8) {
                fputs("Error: head before bit 0\n", stderr); exit(1);
            }
            *st->head -= t.num * 8;
            break;

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

        case TOK_COUNT_LOOP: {
            ts_consume(ts);
            uint64_t count = t.num;
            Token wopen = ts_consume(ts);
            if (wopen.kind != TOK_WHILE_OPEN) {
                fprintf(stderr, "Error: expected '[' after '!%llu'\n",
                        (unsigned long long)count);
                exit(1);
            }
            size_t loop_start = ts->pos;
            if (count == 0) { skip_to_while_close(ts); break; }
            for (uint64_t k = 0; k < count; k++) {
                ts->pos = loop_start;
                exec_until_terminator(ts, st);
                Token wc = ts_consume(ts);
                if (wc.kind != TOK_WHILE_CLOSE) {
                    fputs("Error: expected ']' closing count loop\n", stderr); exit(1);
                }
            }
            break;
        }

        case TOK_INPUT:
            ts_consume(ts);
            do_input(st, t.str);
            break;

        case TOK_OUTPUT:
            ts_consume(ts);
            do_output(st, t.str);
            break;

        case TOK_STDERR:
            ts_consume(ts);
            do_stderr_out(st);
            break;

        case TOK_FORGE_OPEN:
            ts_consume(ts);
            exec_forge(ts, st);
            break;

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
            if (st->depth >= MAX_EXEC_DEPTH) {
                fprintf(stderr, "Error: nesting limit (%d) exceeded in %%// %s\n",
                        MAX_EXEC_DEPTH, t.str);
                break;
            }
            char *src2 = load_file(t.str);
            TokenStream ts2; ts_init(&ts2);
            tokenise(src2, &ts2);
            ExecState st2   = *st;
            st2.script_path = t.str;
            st2.depth       = st->depth + 1;
            exec_program(&ts2, &st2);
            ts_free(&ts2);
            free(src2);
            break;
        }

        case TOK_STR_LIT:
            ts_consume(ts);
            tape_write_string(st->tape, st->head, t.str, (size_t)t.num);
            break;

        case TOK_HEX_LIT:
        case TOK_CHAR_LIT:
            ts_consume(ts);
            tape_write_byte(st->tape, *st->head, (unsigned char)t.num);
            head_add(st->head, 8);
            break;

        case TOK_MARK:
            ts_consume(ts);
            st->sel->mark     = *st->head / 8;
            st->sel->mark_set = 1;
            break;

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

        case TOK_EXEC:
            ts_consume(ts);
            do_exec(st, t.exec_mode, t.num);
            break;

        case TOK_LOAD:
            ts_consume(ts);
            do_load(st, t.str);
            break;

        case TOK_WRITE:
            ts_consume(ts);
            do_write(st, t.str);
            break;

        case TOK_ENV_READ:
            ts_consume(ts);
            do_env_read(st, t.str);
            break;

        case TOK_ENV_WRITE: {
            ts_consume(ts);
            uint64_t s, e;
            if (token_region(&t, st, &s, &e) == 0)
                do_env_write(st, t.str, s, e);
            break;
        }

        case TOK_SELF_LOAD:
            ts_consume(ts);
            do_self_load(st);
            break;

        case TOK_EVAL: {
            ts_consume(ts);
            uint64_t s, e;
            if (token_region(&t, st, &s, &e) == 0)
                do_eval(st, s, e);
            break;
        }

        default:
            ts_consume(ts);
            break;
        }
    }
}

/* ===== PROGRAM ===== */

static const char *closer_name(TokenKind k) {
    switch (k) {
    case TOK_WHILE_CLOSE: return "]";
    case TOK_BLOCK_CLOSE: return "}";
    case TOK_FORGE_SEP:   return "|";
    case TOK_FORGE_CLOSE: return "$!";
    default:              return "?";
    }
}

static void exec_line(TokenStream *ts, ExecState *st) {
    exec_until_terminator(ts, st);
    Token term = ts_peek(ts);
    switch (term.kind) {
    case TOK_LINE_END:
    case TOK_BLOCK_END:
        ts_consume(ts);
        break;
    case TOK_EOF:
    case TOK_PROG_END:
        break;
    default:
        fprintf(stderr, "Error: unmatched '%s'\n", closer_name(term.kind));
        exit(1);
    }
}

static void exec_program(TokenStream *ts, ExecState *st) {
    for (;;) {
        Token t = ts_peek(ts);
        if (t.kind == TOK_EOF || t.kind == TOK_PROG_END) break;
        exec_line(ts, st);
    }
}

/* ===== MAIN ===== */

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
    st.depth       = 0;

    exec_program(&ts, &st);

    tape_free(&tape);
    ts_free(&ts);
    free(src);
    return 0;
}
