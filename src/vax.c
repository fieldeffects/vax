/* vax.c - manifest accessors, packed .vax reader and validator (SPEC.md). */

#include "vax.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- accessors --- */

static bool is_sym(const VaxNode *n, const char *s) {
    return n && n->kind == VAX_SYMBOL && strcmp(n->text, s) == 0;
}

static bool is_list_head(const VaxNode *n, const char *s) {
    return n && n->kind == VAX_LIST && n->count >= 1 && is_sym(n->items[0], s);
}

const VaxNode *vax_key(const VaxNode *list, const char *key) {
    if (!list || list->kind != VAX_LIST)
        return nullptr;
    for (size_t i = 0; i < list->count; i++) {
        const VaxNode *c = list->items[i];
        if (c->kind == VAX_LIST && c->count >= 1 && is_sym(c->items[0], key))
            return c;
    }
    return nullptr;
}

const VaxNode *vax_val(const VaxNode *list, const char *key) {
    const VaxNode *k = vax_key(list, key);
    return (k && k->count >= 2) ? k->items[1] : nullptr;
}

bool vax_get_int(const VaxNode *list, const char *key, int64_t *out) {
    const VaxNode *v = vax_val(list, key);
    if (v && v->kind == VAX_INT) {
        *out = v->i;
        return true;
    }
    return false;
}

const char *vax_get_str(const VaxNode *list, const char *key) {
    const VaxNode *v = vax_val(list, key);
    return (v && (v->kind == VAX_STRING || v->kind == VAX_SYMBOL)) ? v->text : nullptr;
}

/* --- layer collection, document order (SPEC 3.1) --- */

static void collect_walk(const VaxNode *n, const VaxNode ***arr, size_t *count, size_t *cap) {
    if (!n || n->kind != VAX_LIST)
        return;
    if (is_list_head(n, "layers")) {
        for (size_t i = 0; i < n->count; i++) {
            const VaxNode *c = n->items[i];
            if (is_list_head(c, "layer")) {
                if (*count == *cap) {
                    *cap = *cap ? *cap * 2 : 16;
                    *arr = realloc(*arr, *cap * sizeof **arr);
                }
                (*arr)[(*count)++] = c;
            }
        }
    }
    for (size_t i = 0; i < n->count; i++)
        collect_walk(n->items[i], arr, count, cap);
}

size_t vax_collect_layers(const VaxNode *root, const VaxNode ***out) {
    const VaxNode **arr = nullptr;
    size_t count = 0, cap = 0;
    collect_walk(root, &arr, &count, &cap);
    *out = arr;
    return count;
}

/* --- packed container (SPEC 3) --- */

static uint64_t le64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static VaxStatus read_whole_file(const char *path, unsigned char **data, size_t *size,
                                 char *err, size_t errlen) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "cannot open %s", path);
        return VAX_EIO;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        snprintf(err, errlen, "seek failed on %s", path);
        return VAX_EIO;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        snprintf(err, errlen, "ftell failed on %s", path);
        return VAX_EIO;
    }
    rewind(f);
    unsigned char *buf = malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf) {
        fclose(f);
        snprintf(err, errlen, "out of memory");
        return VAX_EIO;
    }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        snprintf(err, errlen, "read failed on %s", path);
        return VAX_EIO;
    }
    fclose(f);
    *data = buf;
    *size = (size_t)sz;
    return VAX_OK;
}

VaxStatus vax_open_packed(const char *path, VaxPacked *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    VaxStatus st = read_whole_file(path, &out->data, &out->size, err, errlen);
    if (st != VAX_OK)
        return st;
    if (out->size < 12 || memcmp(out->data, "VAX1", 4) != 0) {
        snprintf(err, errlen, "bad magic (expected \"VAX1\")");
        return VAX_EFORMAT;
    }
    uint64_t m = le64(out->data + 4);
    if (m > VAX_MAX_MANIFEST) {
        snprintf(err, errlen, "manifest exceeds 64 MiB limit");
        return VAX_ELIMIT;
    }
    if (12 + m > out->size) {
        snprintf(err, errlen, "manifest length out of range");
        return VAX_EFORMAT;
    }
    out->doc = vaxscm_parse((const char *)out->data + 12, (size_t)m);
    if (out->doc.error) {
        snprintf(err, errlen, "manifest: line %zu: %s", out->doc.err_line, out->doc.error);
        return VAX_EFORMAT;
    }
    out->nlayers = vax_collect_layers(out->doc.root, &out->layers);
    out->off = calloc(out->nlayers ? out->nlayers : 1, sizeof *out->off);
    out->len = calloc(out->nlayers ? out->nlayers : 1, sizeof *out->len);
    if (!out->off || !out->len) {
        snprintf(err, errlen, "out of memory");
        return VAX_EIO;
    }
    size_t pos = 12 + (size_t)m;
    for (size_t i = 0; i < out->nlayers; i++) {
        uint32_t w, h;
        size_t consumed;
        char lerr[256];
        st = vax_image_scan(out->data + pos, out->size - pos, &w, &h, &consumed, lerr, sizeof lerr);
        if (st != VAX_OK) {
            snprintf(err, errlen, "layer %zu: %s", i, lerr);
            return st;
        }
        out->off[i] = pos;
        out->len[i] = consumed;
        pos += consumed;
    }
    return VAX_OK;
}

void vax_packed_free(VaxPacked *p) {
    if (!p)
        return;
    vax_doc_free(p->doc);
    free(p->data);
    free(p->layers);
    free(p->off);
    free(p->len);
    memset(p, 0, sizeof *p);
}

/* --- validator (SPEC 10) --- */

typedef struct {
    VaxIssue *issues;
    size_t max;
    size_t found;
} Rep;

static void rep(Rep *r, size_t line, const char *fmt, ...) {
    r->found++;
    if (r->found > r->max || !r->issues)
        return;
    VaxIssue *i = &r->issues[r->found - 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(i->msg, sizeof i->msg, fmt, ap);
    va_end(ap);
    i->line = line;
}

static int axis_index(char a) {
    return a == 'x' ? 0 : a == 'y' ? 1 : 2;
}

static void rect_axes(char axis, char *u, char *v) {
    switch (axis) {
    case 'z': *u = 'x'; *v = 'y'; break;
    case 'y': *u = 'x'; *v = 'z'; break;
    default:  *u = 'y'; *v = 'z'; break;
    }
}

static int cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static void validate_layers(const VaxNode *layers, const int64_t dims[3], char axis,
                            char encoding, const unsigned char *palset, size_t n_pal,
                            const VaxSource *src, Rep *r) {
    if (!layers || !is_list_head(layers, "layers")) {
        rep(r, 0, "missing (layers ...) list");
        return;
    }
    size_t nl = 0;
    for (size_t i = 0; i < layers->count; i++)
        if (is_list_head(layers->items[i], "layer"))
            nl++;
    if (nl > VAX_MAX_LAYERS)
        rep(r, layers->line, "too many layers (%zu > %u)", nl, VAX_MAX_LAYERS);

    char ks[2] = {axis, 0};
    char u, v;
    rect_axes(axis, &u, &v);
    char ku[2] = {u, 0}, kv[2] = {v, 0};
    int ai = axis_index(axis), ui = axis_index(u), vi = axis_index(v);

    int64_t *slices = malloc((nl ? nl : 1) * sizeof *slices);
    size_t ns = 0;

    for (size_t i = 0; i < layers->count; i++) {
        const VaxNode *L = layers->items[i];
        if (!is_list_head(L, "layer"))
            continue;
        size_t line = L->line;
        const VaxNode *sv = vax_val(L, ks);
        if (!sv || sv->kind != VAX_INT) {
            rep(r, line, "layer missing (%c n) slice key", axis);
            continue;
        }
        int64_t s = sv->i;
        if (s < 0 || s >= dims[ai])
            rep(r, line, "slice index %lld out of range [0,%lld)", (long long)s, (long long)dims[ai]);
        else
            slices[ns++] = s;

        int64_t a = 0, b = 0;
        if (!vax_get_int(L, ku, &a))
            a = 0;
        if (!vax_get_int(L, kv, &b))
            b = 0;
        const VaxNode *wv = vax_val(L, "w");
        const VaxNode *hv = vax_val(L, "h");
        if (!wv || wv->kind != VAX_INT || !hv || hv->kind != VAX_INT || wv->i < 1 || hv->i < 1) {
            rep(r, line, "layer needs integer (w n) (h n) with n >= 1");
            continue;
        }
        int64_t w = wv->i, h = hv->i;
        if ((uint64_t)w * (uint64_t)h > VAX_MAX_LAYER_PX)
            rep(r, line, "layer %lldx%lld exceeds pixel limit", (long long)w, (long long)h);
        if (a < 0 || a + w > dims[ui])
            rep(r, line, "rect along %c out of bounds", u);
        if (b < 0 || b + h > dims[vi])
            rep(r, line, "rect along %c out of bounds", v);

        if (src && src->load_layer) {
            const unsigned char *data;
            size_t len;
            char lerr[256];
            VaxStatus st = src->load_layer(src->ctx, L, &data, &len, lerr, sizeof lerr);
            if (st != VAX_OK) {
                rep(r, line, "cannot load layer: %s", lerr);
                continue;
            }
            VaxImage img;
            size_t consumed;
            st = vax_image_decode(data, len, &img, &consumed, lerr, sizeof lerr);
            if (st != VAX_OK) {
                rep(r, line, "bad QOI stream: %s", lerr);
                continue;
            }
            if (img.width != (uint32_t)w || img.height != (uint32_t)h) {
                rep(r, line, "QOI size %ux%u does not match (w %lld) (h %lld)",
                    img.width, img.height, (long long)w, (long long)h);
            } else {
                uint64_t npx = (uint64_t)img.width * img.height;
                for (uint64_t k = 0; k < npx; k++) {
                    unsigned char alpha = img.pixels[k * 4 + 3];
                    if (alpha != 0 && alpha != 255) {
                        rep(r, line, "alpha value %u not in {0,255}", alpha);
                        break;
                    }
                    if (encoding == 'p' && alpha == 255) {
                        unsigned char red = img.pixels[k * 4];
                        if (red >= n_pal || !palset[red]) {
                            rep(r, line, "palette index %u undefined", red);
                            break;
                        }
                    }
                }
            }
            vax_image_free(&img);
        }
    }

    qsort(slices, ns, sizeof *slices, cmp_i64);
    for (size_t i = 1; i < ns; i++)
        if (slices[i] == slices[i - 1])
            rep(r, 0, "duplicate slice index %lld in one layers list", (long long)slices[i]);
    free(slices);
}

static void check_dup_keys(const VaxNode *n, Rep *r) {
    if (!n || n->kind != VAX_LIST)
        return;
    for (size_t i = 0; i < n->count; i++) {
        const VaxNode *c = n->items[i];
        if (c->kind == VAX_LIST && c->count >= 1 && c->items[0]->kind == VAX_SYMBOL) {
            for (size_t j = 0; j < i; j++) {
                const VaxNode *d = n->items[j];
                if (d->kind == VAX_LIST && d->count >= 1 && d->items[0]->kind == VAX_SYMBOL &&
                    strcmp(c->items[0]->text, d->items[0]->text) == 0) {
                    rep(r, c->line, "duplicate key '%s'", c->items[0]->text);
                    break;
                }
            }
        }
    }
    for (size_t i = 0; i < n->count; i++)
        check_dup_keys(n->items[i], r);
}

size_t vax_validate(const VaxNode *root, const VaxSource *src, VaxIssue *issues, size_t max_issues) {
    Rep r = {issues, max_issues, 0};
    if (!root || root->kind != VAX_LIST || root->count < 1 || !is_sym(root->items[0], "vax")) {
        rep(&r, 0, "top-level datum must be (vax ...)");
        return r.found;
    }

    if (root->count < 2 || !is_list_head(root->items[1], "version"))
        rep(&r, root->line, "first key must be (version n)");
    else {
        const VaxNode *v = vax_val(root, "version");
        if (!v || v->kind != VAX_INT)
            rep(&r, root->items[1]->line, "(version n) needs an integer");
        else if (v->i < 1)
            rep(&r, v->line, "invalid version %lld", (long long)v->i);
        else if (v->i > 1)
            rep(&r, v->line, "unsupported version %lld", (long long)v->i);
    }

    check_dup_keys(root, &r);

    int64_t dim[3] = {0, 0, 0};
    bool dims_ok = false;
    const VaxNode *dims = vax_key(root, "dims");
    if (!dims)
        rep(&r, root->line, "missing (dims x y z)");
    else {
        dims_ok = true;
        for (int k = 0; k < 3; k++) {
            const VaxNode *e = dims->items[k + 1];
            if (e->kind != VAX_INT || e->i < 1 || e->i > (int64_t)VAX_MAX_DIM) {
                rep(&r, e->line, "dimension out of range [1,2^24]");
                dims_ok = false;
            } else {
                dim[k] = e->i;
            }
        }
        if (dims_ok && (uint64_t)dim[0] * (uint64_t)dim[1] > VAX_MAX_DIM_PROD / (uint64_t)dim[2]) {
            rep(&r, dims->line, "dims product exceeds 2^40");
            dims_ok = false;
        }
    }

    char axis = 'z';
    const VaxNode *ax = vax_val(root, "axis");
    if (ax) {
        if (ax->kind == VAX_SYMBOL && ax->text[0] && !ax->text[1] && strchr("xyz", ax->text[0]))
            axis = ax->text[0];
        else
            rep(&r, ax->line, "invalid axis (want x, y or z)");
    }

    char encoding = 'r';
    const VaxNode *enc = vax_val(root, "encoding");
    if (enc) {
        if (is_sym(enc, "rgba"))
            encoding = 'r';
        else if (is_sym(enc, "palette"))
            encoding = 'p';
        else
            rep(&r, enc->line, "invalid encoding (want rgba or palette)");
    }

    const VaxNode *pal = vax_key(root, "palette");
    unsigned char *palset = nullptr;
    size_t n_pal = 0;
    if (encoding == 'p' && !pal)
        rep(&r, 0, "(encoding palette) requires (palette ...)");
    if (pal) {
        int64_t maxidx = -1;
        for (size_t i = 1; i < pal->count; i++) {
            const VaxNode *e = pal->items[i];
            if (e->kind == VAX_LIST && e->count >= 1 && e->items[0]->kind == VAX_INT && e->items[0]->i > maxidx)
                maxidx = e->items[0]->i;
        }
        if (maxidx >= (int64_t)VAX_MAX_LAYERS)
            rep(&r, pal->line, "palette index too large (limit %u)", VAX_MAX_LAYERS);
        else if (maxidx >= 0) {
            n_pal = (size_t)maxidx + 1;
            palset = calloc(n_pal, 1);
            for (size_t i = 1; i < pal->count; i++) {
                const VaxNode *e = pal->items[i];
                if (e->kind != VAX_LIST || e->count < 1 || e->items[0]->kind != VAX_INT) {
                    rep(&r, e->line, "bad palette entry");
                    continue;
                }
                int64_t idx = e->items[0]->i;
                if (idx < 0)
                    rep(&r, e->line, "negative palette index");
                else if (palset[idx])
                    rep(&r, e->line, "duplicate palette index %lld", (long long)idx);
                else
                    palset[idx] = 1;
                const VaxNode *col = vax_key(e, "color");
                if (!col || col->count < 4 || col->count > 5)
                    rep(&r, e->line, "palette entry needs (color r g b [a])");
                else
                    for (int k = 1; k < (int)col->count; k++) {
                        const VaxNode *c = col->items[k];
                        if (c->kind != VAX_INT || c->i < 0 || c->i > 255)
                            rep(&r, c->line, "color component out of range [0,255]");
                    }
            }
        }
    }

    const VaxNode *csn = vax_val(root, "chunk-size");
    if (csn) {
        if (csn->kind != VAX_INT || csn->i < 1 || csn->i > (int64_t)VAX_MAX_DIM)
            rep(&r, csn->line, "invalid chunk-size");
        else if (dims_ok) {
            int64_t cs = csn->i;
            int64_t nc[3], cdims[3];
            for (int k = 0; k < 3; k++) {
                nc[k] = (dim[k] + cs - 1) / cs;
                cdims[k] = 0;
            }
            const VaxNode *chunks = vax_key(root, "chunks");
            if (!chunks)
                rep(&r, root->line, "chunk-size requires (chunks ...)");
            else {
                size_t nch = 0;
                for (size_t i = 1; i < chunks->count; i++) {
                    const VaxNode *ch = chunks->items[i];
                    if (!is_list_head(ch, "chunk"))
                        continue;
                    nch++;
                    if (nch > VAX_MAX_LAYERS) {
                        rep(&r, ch->line, "too many chunks");
                        break;
                    }
                    int64_t c[3];
                    const char *names[3] = {"cx", "cy", "cz"};
                    bool ok = true;
                    for (int k = 0; k < 3; k++) {
                        if (!vax_get_int(ch, names[k], &c[k]) || c[k] < 0 || c[k] >= nc[k]) {
                            rep(&r, ch->line, "chunk coord %s out of range", names[k]);
                            ok = false;
                        }
                    }
                    if (!ok)
                        continue;
                    for (int k = 0; k < 3; k++) {
                        int64_t rem = dim[k] - c[k] * cs;
                        cdims[k] = rem < cs ? rem : cs;
                    }
                    const VaxNode *layers = vax_key(ch, "layers");
                    if (layers) {
                        validate_layers(layers, cdims, axis, encoding, palset, n_pal, src, &r);
                    } else if (src && src->load_scm) {
                        const char *f = vax_get_str(ch, "file");
                        if (!f) {
                            rep(&r, ch->line, "chunk needs (layers ...) or (file ...)");
                            continue;
                        }
                        VaxDoc d;
                        char lerr[256];
                        if (src->load_scm(src->ctx, f, &d, lerr, sizeof lerr) != VAX_OK) {
                            rep(&r, ch->line, "cannot load chunk file: %s", lerr);
                            continue;
                        }
                        if (!d.root || !is_list_head(d.root, "layers"))
                            rep(&r, ch->line, "chunk file must contain one (layers ...) datum");
                        else
                            validate_layers(d.root, cdims, axis, encoding, palset, n_pal, src, &r);
                        vax_doc_free(d);
                    } else
                        rep(&r, ch->line, "chunk needs (layers ...) or (file ...)");
                }
            }
        }
    } else if (dims_ok) {
        validate_layers(vax_key(root, "layers"), dim, axis, encoding, palset, n_pal, src, &r);
    }

    const VaxNode *anim = vax_key(root, "anim");
    if (anim) {
        const VaxNode *fps = vax_val(anim, "fps");
        bool fps_ok = fps && ((fps->kind == VAX_INT && fps->i > 0) || (fps->kind == VAX_REAL && fps->d > 0));
        if (!fps_ok)
            rep(&r, anim->line, "anim needs (fps n) with n > 0");
        for (size_t i = 1; i < anim->count; i++) {
            const VaxNode *fr = anim->items[i];
            if (is_list_head(fr, "frame") && dims_ok)
                validate_layers(vax_key(fr, "layers"), dim, axis, encoding, palset, n_pal, src, &r);
        }
    }

    free(palset);
    return r.found;
}
