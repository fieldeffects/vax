/* vax-check - validate a .vax asset (folder or packed) per SPEC.md section 10.
 *
 * usage: vax-check <name.vax | name.vax/>
 * exit:  0 ok, 1 validation issues, 2 parse/IO error
 */

#include "vax.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    const char *base;      /* folder form: dir containing manifest.scm */
    VaxPacked *packed;     /* packed form */
    size_t next;           /* packed form: next layer stream index */
    unsigned char *buf;    /* folder form: scratch buffer for file reads */
    size_t bufcap;
} Ctx;

/* Join base + rel, rejecting anything that escapes the .vax directory
 * (SPEC 13). Returns 0 on success. */
static int safe_join(const char *base, const char *rel, char *out, size_t outsz) {
    if (!rel || rel[0] == '/' || strchr(rel, '\\'))
        return -1;
    if (((rel[0] >= 'a' && rel[0] <= 'z') || (rel[0] >= 'A' && rel[0] <= 'Z')) && rel[1] == ':')
        return -1;
    const char *p = rel;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t clen = slash ? (size_t)(slash - p) : strlen(p);
        if (clen == 2 && p[0] == '.' && p[1] == '.')
            return -1;
        if (!slash)
            break;
        p = slash + 1;
    }
    if (snprintf(out, outsz, "%s/%s", base, rel) >= (int)outsz)
        return -1;
    return 0;
}

static VaxStatus read_file(const char *path, unsigned char **data, size_t *len,
                           char *err, size_t errlen) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "cannot open %s", path);
        return VAX_EIO;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        snprintf(err, errlen, "seek failed");
        return VAX_EIO;
    }
    long sz = ftell(f);
    rewind(f);
    if (sz < 0) {
        fclose(f);
        snprintf(err, errlen, "ftell failed");
        return VAX_EIO;
    }
    unsigned char *buf = malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf) {
        fclose(f);
        snprintf(err, errlen, "out of memory");
        return VAX_EIO;
    }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        snprintf(err, errlen, "read failed");
        return VAX_EIO;
    }
    fclose(f);
    *data = buf;
    *len = (size_t)sz;
    return VAX_OK;
}

static VaxStatus load_layer_folder(void *ctx_, const VaxNode *layer,
                                   const unsigned char **data, size_t *len,
                                   char *err, size_t errlen) {
    Ctx *c = ctx_;
    const char *f = vax_get_str(layer, "file");
    if (!f) {
        snprintf(err, errlen, "folder form: layer needs (file \"...\")");
        return VAX_EVALID;
    }
    char path[4096];
    if (safe_join(c->base, f, path, sizeof path) != 0) {
        snprintf(err, errlen, "unsafe path '%s'", f);
        return VAX_EVALID;
    }
    free(c->buf);
    c->buf = nullptr;
    size_t n;
    VaxStatus st = read_file(path, &c->buf, &n, err, errlen);
    if (st != VAX_OK)
        return st;
    *data = c->buf;
    *len = n;
    return VAX_OK;
}

static VaxStatus load_layer_packed(void *ctx_, const VaxNode *layer,
                                   const unsigned char **data, size_t *len,
                                   char *err, size_t errlen) {
    Ctx *c = ctx_;
    if (vax_get_str(layer, "file")) {
        snprintf(err, errlen, "packed form: layer must not have (file ...)");
        return VAX_EVALID;
    }
    if (c->next >= c->packed->nlayers) {
        snprintf(err, errlen, "more layer entries than QOI streams");
        return VAX_EFORMAT;
    }
    *data = c->packed->data + c->packed->off[c->next];
    *len = c->packed->len[c->next];
    c->next++;
    return VAX_OK;
}

static VaxStatus load_scm_folder(void *ctx_, const char *path, VaxDoc *doc,
                                 char *err, size_t errlen) {
    Ctx *c = ctx_;
    char full[4096];
    if (safe_join(c->base, path, full, sizeof full) != 0) {
        snprintf(err, errlen, "unsafe path '%s'", path);
        return VAX_EVALID;
    }
    *doc = vaxscm_parse_file(full);
    if (doc->error)
        snprintf(err, errlen, "%s: line %zu: %s", full, doc->err_line, doc->error);
    return doc->error ? VAX_EFORMAT : VAX_OK;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <name.vax | name.vax/>\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "vax-check: cannot stat %s\n", path);
        return 2;
    }

    Ctx ctx = {0};
    VaxDoc doc = {0};
    const VaxNode *root;
    VaxSource src = {0};

    if (S_ISDIR(st.st_mode)) {
        char manifest[4096];
        if (snprintf(manifest, sizeof manifest, "%s/manifest.scm", path) >= (int)sizeof manifest) {
            fprintf(stderr, "vax-check: path too long\n");
            return 2;
        }
        doc = vaxscm_parse_file(manifest);
        if (doc.error) {
            fprintf(stderr, "vax-check: %s: line %zu: %s\n", manifest, doc.err_line, doc.error);
            return 2;
        }
        ctx.base = path;
        src.load_layer = load_layer_folder;
        src.load_scm = load_scm_folder;
        src.ctx = &ctx;
        root = doc.root;
    } else {
        VaxPacked *p = calloc(1, sizeof *p);
        char err[256];
        VaxStatus s = vax_open_packed(path, p, err, sizeof err);
        if (s != VAX_OK) {
            fprintf(stderr, "vax-check: %s: %s\n", path, err);
            return 2;
        }
        ctx.packed = p;
        src.load_layer = load_layer_packed;
        src.load_scm = nullptr;
        src.ctx = &ctx;
        root = p->doc.root;
    }

    VaxIssue issues[64];
    size_t n = vax_validate(root, &src, issues, 64);
    for (size_t i = 0; i < n && i < 64; i++)
        fprintf(stderr, "%s:%zu: %s\n", path, issues[i].line, issues[i].msg);

    if (n > 0) {
        fprintf(stderr, "vax-check: %zu issue(s)\n", n);
        free(ctx.buf);
        if (ctx.packed) {
            ctx.packed->doc.root = nullptr; /* already reported; avoid double free */
            vax_packed_free(ctx.packed);
            free(ctx.packed);
        } else {
            vax_doc_free(doc);
        }
        return 1;
    }

    const VaxNode **layers;
    size_t nl = vax_collect_layers(root, &layers);
    int64_t dx = 0, dy = 0, dz = 0;
    const VaxNode *dims = vax_key(root, "dims");
    if (dims && dims->count >= 4) {
        dx = dims->items[1]->kind == VAX_INT ? dims->items[1]->i : 0;
        dy = dims->items[2]->kind == VAX_INT ? dims->items[2]->i : 0;
        dz = dims->items[3]->kind == VAX_INT ? dims->items[3]->i : 0;
    }
    const char *name = vax_get_str(root, "name");
    printf("OK %s%s%s dims %lldx%lldx%lld layers %zu\n", path, name ? " name=" : "",
           name ? name : "", (long long)dx, (long long)dy, (long long)dz, nl);
    free(layers);
    free(ctx.buf);
    if (ctx.packed) {
        ctx.packed->doc.root = nullptr;
        vax_packed_free(ctx.packed);
        free(ctx.packed);
    } else {
        vax_doc_free(doc);
    }
    return 0;
}
