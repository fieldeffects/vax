/* test_vax.c - unit tests for the vax reference parser. Run via `make test`. */

#define _DEFAULT_SOURCE /* mkdtemp */

#include "vax.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                      \
    } while (0)

/* The example manifest from SPEC.md section 14. */
static const char EXAMPLE[] =
    "; tree.vax/manifest.scm\n"
    "(vax\n"
    "  (version 1)\n"
    "  (name \"tree\")\n"
    "  (dims 64 64 96)\n"
    "  (axis z)\n"
    "  (encoding palette)\n"
    "  (palette\n"
    "    (1 (color 92 62 40)  (material (roughness 0.9)))\n"
    "    (2 (color 44 158 66) (material (roughness 0.6) (emissive 0.05))))\n"
    "  (layers\n"
    "    (layer (z 0)  (x 20) (y 20) (w 24) (h 24) (file \"layers/000.qoi\"))\n"
    "    (layer (z 1)  (x 21) (y 21) (w 22) (h 22) (file \"layers/001.qoi\"))\n"
    "    (layer (z 47) (x 30) (y 30) (w 4)  (h 4)  (file \"layers/047.qoi\"))))\n";

static void test_parse_example(void) {
    VaxDoc d = vaxscm_parse(EXAMPLE, sizeof EXAMPLE - 1);
    CHECK(d.error == nullptr);
    CHECK(d.root && d.root->kind == VAX_LIST);
    const VaxNode *root = d.root;
    CHECK(root->items[0]->kind == VAX_SYMBOL && strcmp(root->items[0]->text, "vax") == 0);

    int64_t v;
    CHECK(vax_get_int(root, "version", &v) && v == 1);
    CHECK(strcmp(vax_get_str(root, "name"), "tree") == 0);
    const VaxNode *dims = vax_key(root, "dims");
    CHECK(dims && dims->count == 4);
    CHECK(dims->items[1]->i == 64 && dims->items[2]->i == 64 && dims->items[3]->i == 96);
    CHECK(strcmp(vax_get_str(root, "axis"), "z") == 0);
    CHECK(strcmp(vax_get_str(root, "encoding"), "palette") == 0);

    const VaxNode *pal = vax_key(root, "palette");
    CHECK(pal && pal->count == 3); /* head + 2 entries */

    const VaxNode **layers;
    size_t nl = vax_collect_layers(root, &layers);
    CHECK(nl == 3);
    int64_t z, w, h;
    CHECK(vax_get_int(layers[2], "z", &z) && z == 47);
    CHECK(vax_get_int(layers[2], "w", &w) && w == 4);
    CHECK(vax_get_int(layers[2], "h", &h) && h == 4);
    CHECK(strcmp(vax_get_str(layers[2], "file"), "layers/047.qoi") == 0);
    free(layers);
    vax_doc_free(d);
}

static void test_parse_numbers_and_errors(void) {
    VaxDoc d = vaxscm_parse("42 -7 2.5 x", 10);
    CHECK(d.error != nullptr); /* trailing data: not a single datum */
    vax_doc_free(d);

    d = vaxscm_parse("(a 42 -7 2.5)", strlen("(a 42 -7 2.5)"));
    CHECK(d.error == nullptr);
    CHECK(d.root->items[1]->kind == VAX_INT && d.root->items[1]->i == 42);
    CHECK(d.root->items[2]->kind == VAX_INT && d.root->items[2]->i == -7);
    CHECK(d.root->items[3]->kind == VAX_REAL && d.root->items[3]->d == 2.5);
    vax_doc_free(d);

    d = vaxscm_parse("(vax (version 1)", 17);
    CHECK(d.error != nullptr); /* unterminated */
    vax_doc_free(d);

    d = vaxscm_parse("\"bad \\q\"", 8);
    CHECK(d.error != nullptr); /* bad escape */
    vax_doc_free(d);

    d = vaxscm_parse("1.", 2);
    CHECK(d.error != nullptr); /* malformed number */
    vax_doc_free(d);

    d = vaxscm_parse("1x", 2);
    CHECK(d.error != nullptr); /* malformed number */
    vax_doc_free(d);

    d = vaxscm_parse("(a) (b)", 7);
    CHECK(d.error != nullptr); /* two top-level datums */
    vax_doc_free(d);

    /* depth limit: 65 nested lists */
    char deep[200];
    size_t n = 0;
    for (int i = 0; i < 65; i++)
        deep[n++] = '(';
    for (int i = 0; i < 65; i++)
        deep[n++] = ')';
    d = vaxscm_parse(deep, n);
    CHECK(d.error != nullptr);
    vax_doc_free(d);

    /* comments and whitespace */
    d = vaxscm_parse("; hello\n(vax ; inline\n (version 1)) ; trailing\n",
                     strlen("; hello\n(vax ; inline\n (version 1)) ; trailing\n"));
    CHECK(d.error == nullptr);
    int64_t one;
    CHECK(vax_get_int(d.root, "version", &one) && one == 1);
    vax_doc_free(d);
}

/* Hand-crafted 5x1 QOI stream exercising RGBA, DIFF, LUMA, RGB and RUN ops. */
static const unsigned char QOI5x1[] = {
    'q', 'o', 'i', 'f', 0, 0, 0, 5, 0, 0, 0, 1, 4, 0,
    0xFF, 0x10, 0x20, 0x30, 0xFF, /* RGBA px0 = (10,20,30,255) */
    0x7F,                         /* DIFF +1,+1,+1  px1 = (11,21,31,255) */
    0xA2, 0x89,                   /* LUMA dg=2 dr=2 db=3 px2 = (13,23,34,255) */
    0xFE, 0x01, 0x02, 0x03,       /* RGB px3 = (1,2,3,255) */
    0xC0,                         /* RUN 1 px4 = px3 */
    0, 0, 0, 0, 0, 0, 0, 1
};

static void test_qoi(void) {
    VaxImage img;
    size_t consumed;
    char err[256];
    CHECK(vax_image_decode(QOI5x1, sizeof QOI5x1, &img, &consumed, err, sizeof err) == VAX_OK);
    CHECK(consumed == sizeof QOI5x1);
    CHECK(img.width == 5 && img.height == 1 && img.channels == 4);
    static const unsigned char want[5][4] = {
        {0x10, 0x20, 0x30, 0xFF},
        {0x11, 0x21, 0x31, 0xFF},
        {0x13, 0x23, 0x34, 0xFF},
        {0x01, 0x02, 0x03, 0xFF},
        {0x01, 0x02, 0x03, 0xFF},
    };
    CHECK(memcmp(img.pixels, want, sizeof want) == 0);
    vax_image_free(&img);

    /* truncated stream */
    CHECK(vax_image_decode(QOI5x1, sizeof QOI5x1 - 9, &img, &consumed, err, sizeof err) != VAX_OK);
    /* bad magic */
    unsigned char bad[sizeof QOI5x1];
    memcpy(bad, QOI5x1, sizeof bad);
    bad[0] = 'x';
    CHECK(vax_image_decode(bad, sizeof bad, &img, &consumed, err, sizeof err) != VAX_OK);
    /* scan mode: same length, no pixels */
    uint32_t w, h;
    CHECK(vax_image_scan(QOI5x1, sizeof QOI5x1, &w, &h, &consumed, err, sizeof err) == VAX_OK);
    CHECK(w == 5 && h == 1 && consumed == sizeof QOI5x1);
}

static const char PACKED_MANIFEST[] =
    "(vax (version 1) (dims 5 1 1) (axis z) (encoding rgba)"
    " (layers (layer (z 0) (w 5) (h 1))))";

typedef struct {
    VaxPacked *p;
    size_t next;
} PackedSrc;

static VaxStatus src_packed(void *ctx_, const VaxNode *layer, const unsigned char **data,
                            size_t *len, char *err, size_t errlen) {
    PackedSrc *c = ctx_;
    if (vax_get_str(layer, "file")) {
        snprintf(err, errlen, "packed form: layer must not have (file ...)");
        return VAX_EVALID;
    }
    if (c->next >= c->p->nlayers) {
        snprintf(err, errlen, "more layer entries than QOI streams");
        return VAX_EFORMAT;
    }
    *data = c->p->data + c->p->off[c->next];
    *len = c->p->len[c->next];
    c->next++;
    return VAX_OK;
}

static void write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror("fopen");
        exit(2);
    }
    fwrite(data, 1, len, f);
    fclose(f);
}

static void test_packed(void) {
    char tmpl[] = "/tmp/vaxtestXXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir != nullptr);
    char path[8192];
    snprintf(path, sizeof path, "%s/p.vax", dir);

    size_t m = sizeof PACKED_MANIFEST - 1;
    size_t total = 12 + m + sizeof QOI5x1;
    unsigned char *buf = malloc(total);
    memcpy(buf, "VAX1", 4);
    for (int i = 0; i < 8; i++)
        buf[4 + i] = (unsigned char)((m >> (8 * i)) & 0xFF);
    memcpy(buf + 12, PACKED_MANIFEST, m);
    memcpy(buf + 12 + m, QOI5x1, sizeof QOI5x1);
    write_file(path, buf, total);

    VaxPacked p;
    char err[256];
    CHECK(vax_open_packed(path, &p, err, sizeof err) == VAX_OK);
    CHECK(p.nlayers == 1);
    CHECK(p.len[0] == sizeof QOI5x1);

    PackedSrc ps = {&p, 0};
    VaxSource src = {.load_layer = src_packed, .load_scm = nullptr, .ctx = &ps};
    VaxIssue issues[16];
    size_t n = vax_validate(p.doc.root, &src, issues, 16);
    CHECK(n == 0);
    vax_packed_free(&p);

    /* bad alpha (128) must be flagged */
    buf[12 + m + 14 + 4] = 128;
    write_file(path, buf, total);
    CHECK(vax_open_packed(path, &p, err, sizeof err) == VAX_OK);
    ps.p = &p;
    ps.next = 0;
    n = vax_validate(p.doc.root, &src, issues, 16);
    CHECK(n >= 1);
    vax_packed_free(&p);

    /* bad magic must fail to open */
    buf[0] = 'X';
    write_file(path, buf, total);
    CHECK(vax_open_packed(path, &p, err, sizeof err) == VAX_EFORMAT);

    free(buf);
    unlink(path);
    rmdir(dir);
}

static VaxStatus src_folder(void *ctx, const VaxNode *layer, const unsigned char **data,
                            size_t *len, char *err, size_t errlen) {
    const char *base = ctx;
    const char *f = vax_get_str(layer, "file");
    if (!f) {
        snprintf(err, errlen, "missing (file ...)");
        return VAX_EVALID;
    }
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", base, f);
    static unsigned char *buf;
    free(buf);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        snprintf(err, errlen, "cannot open %s", path);
        return VAX_EIO;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    buf = malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        fclose(fp);
        snprintf(err, errlen, "read failed");
        return VAX_EIO;
    }
    fclose(fp);
    *data = buf;
    *len = (size_t)sz;
    return VAX_OK;
}

static void test_folder(void) {
    char tmpl[] = "/tmp/vaxtestXXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir != nullptr);
    char sub[8192], ldir[8192], mpath[8192], lpath[8192];
    snprintf(sub, sizeof sub, "%s/tree.vax", dir);
    snprintf(ldir, sizeof ldir, "%s/layers", sub);
    snprintf(mpath, sizeof mpath, "%s/manifest.scm", sub);
    snprintf(lpath, sizeof lpath, "%s/000.qoi", ldir);
    mkdir(sub, 0755);
    mkdir(ldir, 0755);

    char manifest[1024];
    snprintf(manifest, sizeof manifest,
             "(vax (version 1) (name \"t\") (dims 5 1 1) (axis z) (encoding rgba)"
             " (layers (layer (z 0) (w 5) (h 1) (file \"layers/000.qoi\"))))");
    write_file(mpath, manifest, strlen(manifest));
    write_file(lpath, QOI5x1, sizeof QOI5x1);

    VaxDoc d = vaxscm_parse_file(mpath);
    CHECK(d.error == nullptr);
    VaxSource src = {.load_layer = src_folder, .load_scm = nullptr, .ctx = sub};
    VaxIssue issues[16];
    size_t n = vax_validate(d.root, &src, issues, 16);
    CHECK(n == 0);
    vax_doc_free(d);

    /* out-of-bounds rect must be flagged */
    snprintf(manifest, sizeof manifest,
             "(vax (version 1) (dims 5 1 1) (layers (layer (z 0) (w 9) (h 1) (file \"layers/000.qoi\"))))");
    write_file(mpath, manifest, strlen(manifest));
    d = vaxscm_parse_file(mpath);
    CHECK(d.error == nullptr);
    n = vax_validate(d.root, &src, issues, 16);
    CHECK(n >= 1); /* w=9 exceeds dims x=5, and QOI w mismatch */
    vax_doc_free(d);

    unlink(lpath);
    unlink(mpath);
    rmdir(ldir);
    rmdir(sub);
    rmdir(dir);
}

int main(void) {
    test_parse_example();
    test_parse_numbers_and_errors();
    test_qoi();
    test_packed();
    test_folder();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
