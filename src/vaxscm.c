/* vaxscm.c - parser for the vaxscm manifest grammar (SPEC.md section 4). */

#include "vax.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    size_t line;
    size_t depth;
    size_t nodes;
    char *error;
    size_t err_line;
} P;

static int perr(P *p, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    free(p->error);
    p->error = malloc(256);
    if (p->error)
        vsnprintf(p->error, 256, fmt, ap);
    va_end(ap);
    p->err_line = p->line;
    return -1;
}

static VaxNode *node_new(P *p, VaxKind kind) {
    if (p->nodes >= VAX_MAX_NODES) {
        perr(p, "too many nodes (limit %u)", VAX_MAX_NODES);
        return nullptr;
    }
    VaxNode *n = calloc(1, sizeof *n);
    if (!n) {
        perr(p, "out of memory");
        return nullptr;
    }
    n->kind = kind;
    n->line = p->line;
    p->nodes++;
    return n;
}

static int list_push(P *p, VaxNode *parent, VaxNode *child) {
    if (parent->count == parent->cap) {
        size_t ncap = parent->cap ? parent->cap * 2 : 8;
        VaxNode **ni = realloc(parent->items, ncap * sizeof *ni);
        if (!ni)
            return perr(p, "out of memory");
        parent->items = ni;
        parent->cap = ncap;
    }
    parent->items[parent->count++] = child;
    return 0;
}

static bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool is_delim(char c) {
    return is_ws(c) || c == '(' || c == ')' || c == '"' || c == ';' || c == '\\';
}

static void skip_ws(P *p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == '\n')
            p->line++;
        if (is_ws(c)) {
            p->pos++;
        } else if (c == ';') {
            while (p->pos < p->len && p->src[p->pos] != '\n')
                p->pos++;
        } else {
            break;
        }
    }
}

static int parse_datum(P *p, VaxNode **out);

static int parse_list(P *p, VaxNode **out) {
    VaxNode *n = node_new(p, VAX_LIST);
    if (!n)
        return -1;
    p->pos++; /* consume '(' */
    if (++p->depth > VAX_MAX_DEPTH) {
        p->depth--;
        perr(p, "nesting deeper than %u", VAX_MAX_DEPTH);
        return -1;
    }
    for (;;) {
        skip_ws(p);
        if (p->pos >= p->len) {
            perr(p, "unterminated list");
            return -1;
        }
        if (p->src[p->pos] == ')') {
            p->pos++;
            break;
        }
        VaxNode *child = nullptr;
        if (parse_datum(p, &child) != 0)
            return -1;
        if (list_push(p, n, child) != 0)
            return -1;
    }
    p->depth--;
    *out = n;
    return 0;
}

static int parse_string(P *p, VaxNode **out) {
    VaxNode *n = node_new(p, VAX_STRING);
    if (!n)
        return -1;
    p->pos++; /* consume '"' */
    size_t cap = 16, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        perr(p, "out of memory");
        return -1;
    }
    while (p->pos < p->len && p->src[p->pos] != '"') {
        char c = p->src[p->pos++];
        if (c == '\n')
            p->line++;
        if (c == '\0') {
            free(buf);
            perr(p, "NUL byte in string");
            return -1;
        }
        if (c == '\\') {
            if (p->pos >= p->len) {
                free(buf);
                perr(p, "unterminated string escape");
                return -1;
            }
            char e = p->src[p->pos++];
            switch (e) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            default:
                free(buf);
                perr(p, "invalid string escape '\\%c'", e);
                return -1;
            }
        }
        if (len + 1 >= VAX_MAX_STRING) {
            free(buf);
            perr(p, "string longer than %u chars", VAX_MAX_STRING);
            return -1;
        }
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                perr(p, "out of memory");
                return -1;
            }
            buf = nb;
        }
        buf[len++] = c;
    }
    if (p->pos >= p->len) {
        free(buf);
        perr(p, "unterminated string");
        return -1;
    }
    p->pos++; /* consume '"' */
    buf[len] = '\0';
    n->text = buf;
    *out = n;
    return 0;
}

static bool token_is_number_start(const char *src, size_t len, size_t pos) {
    char c = src[pos];
    return (c >= '0' && c <= '9') ||
           (c == '-' && pos + 1 < len && src[pos + 1] >= '0' && src[pos + 1] <= '9');
}

static int parse_atom(P *p, VaxNode **out) {
    size_t start = p->pos;
    while (p->pos < p->len && !is_delim(p->src[p->pos]))
        p->pos++;
    size_t tlen = p->pos - start;
    const char *tok = p->src + start;

    if (token_is_number_start(p->src, p->len, start)) {
        /* whole token must be a valid number: [-]digits[.digits] */
        size_t i = (tok[0] == '-') ? 1 : 0;
        size_t digits = 0;
        while (i < tlen && tok[i] >= '0' && tok[i] <= '9') {
            i++;
            digits++;
        }
        bool is_real = false;
        if (i < tlen && tok[i] == '.') {
            is_real = true;
            i++;
            size_t frac = 0;
            while (i < tlen && tok[i] >= '0' && tok[i] <= '9') {
                i++;
                frac++;
            }
            if (frac == 0)
                return perr(p, "malformed number '%.*s'", (int)tlen, tok);
        }
        if (digits == 0 || i != tlen)
            return perr(p, "malformed number '%.*s'", (int)tlen, tok);

        VaxNode *n = node_new(p, is_real ? VAX_REAL : VAX_INT);
        if (!n)
            return -1;
        char *tmp = strndup(tok, tlen);
        if (!tmp) {
            perr(p, "out of memory");
            return -1;
        }
        if (is_real) {
            n->d = strtod(tmp, nullptr);
        } else {
            errno = 0;
            long long v = strtoll(tmp, nullptr, 10);
            if (errno == ERANGE) {
                free(tmp);
                return perr(p, "integer out of range '%.*s'", (int)tlen, tok);
            }
            n->i = (int64_t)v;
        }
        free(tmp);
        *out = n;
        return 0;
    }

    VaxNode *n = node_new(p, VAX_SYMBOL);
    if (!n)
        return -1;
    n->text = strndup(tok, tlen);
    if (!n->text) {
        perr(p, "out of memory");
        return -1;
    }
    *out = n;
    return 0;
}

static int parse_datum(P *p, VaxNode **out) {
    skip_ws(p);
    if (p->pos >= p->len) {
        perr(p, "unexpected end of input");
        return -1;
    }
    char c = p->src[p->pos];
    if (c == '(')
        return parse_list(p, out);
    if (c == ')') {
        perr(p, "unexpected ')'");
        return -1;
    }
    if (c == '"')
        return parse_string(p, out);
    return parse_atom(p, out);
}

VaxDoc vaxscm_parse(const char *src, size_t len) {
    VaxDoc doc = {nullptr, nullptr, 0};
    P p = {.src = src, .len = len, .pos = 0, .line = 1, .depth = 0, .nodes = 0,
           .error = nullptr, .err_line = 0};
    if (len > VAX_MAX_MANIFEST) {
        doc.error = strdup("manifest exceeds 64 MiB limit");
        doc.err_line = 1;
        return doc;
    }
    if (parse_datum(&p, &doc.root) != 0) {
        doc.error = p.error ? p.error : strdup("parse error");
        doc.err_line = p.err_line;
        return doc;
    }
    skip_ws(&p);
    if (p.pos != p.len) {
        doc.error = strdup("trailing data after top-level datum");
        doc.err_line = p.line;
        return doc;
    }
    return doc;
}

VaxDoc vaxscm_parse_file(const char *path) {
    VaxDoc doc = {nullptr, nullptr, 0};
    FILE *f = fopen(path, "rb");
    if (!f) {
        doc.error = strdup("cannot open file");
        return doc;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        doc.error = strdup("seek failed");
        return doc;
    }
    long sz = ftell(f);
    if (sz < 0 || (unsigned long)sz > VAX_MAX_MANIFEST) {
        fclose(f);
        doc.error = strdup("manifest exceeds 64 MiB limit");
        return doc;
    }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        doc.error = strdup("out of memory");
        return doc;
    }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        doc.error = strdup("read failed");
        return doc;
    }
    fclose(f);
    doc = vaxscm_parse(buf, (size_t)sz);
    free(buf);
    return doc;
}

static void node_free(VaxNode *n) {
    if (!n)
        return;
    for (size_t i = 0; i < n->count; i++)
        node_free(n->items[i]);
    free(n->items);
    free(n->text);
    free(n);
}

void vax_doc_free(VaxDoc doc) {
    node_free(doc.root);
    free(doc.error);
}
