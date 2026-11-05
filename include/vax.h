#ifndef VAX_H
#define VAX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * vax.h - reference reader for the VAX format (see SPEC.md).
 *
 * Covers: the vaxscm manifest grammar (SPEC.md section 4), the packed
 * container layout (section 3), QOI layer decoding (section 6) and the
 * validation rules of section 10.
 */

/* Limits from SPEC.md section 11. */
#define VAX_MAX_MANIFEST (1u << 26)   /* 64 MiB manifest */
#define VAX_MAX_DIM      (1u << 24)   /* per-axis voxel count */
#define VAX_MAX_DIM_PROD (1ull << 40) /* total voxels */
#define VAX_MAX_LAYERS   (1u << 20)   /* per layer list / chunks / file refs */
#define VAX_MAX_STRING   4096u
#define VAX_MAX_DEPTH    64u
#define VAX_MAX_LAYER_PX (1u << 26)   /* w*h of one layer image */
#define VAX_MAX_NODES    (1u << 22)   /* manifest s-expression nodes */

typedef enum : uint8_t {
    VAX_LIST,
    VAX_SYMBOL,
    VAX_INT,
    VAX_REAL,
    VAX_STRING
} VaxKind;

typedef struct VaxNode {
    VaxKind kind;
    size_t line;              /* 1-based source line */
    struct VaxNode **items;   /* VAX_LIST children */
    size_t count, cap;
    char *text;               /* VAX_SYMBOL / VAX_STRING (unescaped, NUL-terminated) */
    int64_t i;                /* VAX_INT */
    double d;                 /* VAX_REAL */
} VaxNode;

typedef struct {
    VaxNode *root;            /* NULL on parse error */
    char *error;              /* malloc'd message, NULL on success */
    size_t err_line;
} VaxDoc;

[[nodiscard]] VaxDoc vaxscm_parse(const char *src, size_t len);
[[nodiscard]] VaxDoc vaxscm_parse_file(const char *path);
void vax_doc_free(VaxDoc doc);

/* List accessors. vax_key returns the first (key ...) child list;
 * vax_val returns its second element. Duplicate keys: first wins (SPEC 4). */
[[nodiscard]] const VaxNode *vax_key(const VaxNode *list, const char *key);
[[nodiscard]] const VaxNode *vax_val(const VaxNode *list, const char *key);
[[nodiscard]] bool vax_get_int(const VaxNode *list, const char *key, int64_t *out);
/* Text of a string or symbol value (e.g. "tree" or axis z). */
[[nodiscard]] const char *vax_get_str(const VaxNode *list, const char *key);

/* All (layer ...) entries in document order (SPEC 3.1). Returns count;
 * *out is a malloc'd array the caller frees. */
[[nodiscard]] size_t vax_collect_layers(const VaxNode *root, const VaxNode ***out);

typedef enum : uint8_t {
    VAX_OK = 0,
    VAX_EIO,      /* IO / parse failure */
    VAX_EFORMAT,  /* malformed bytes */
    VAX_ELIMIT,   /* a SPEC 11 limit was exceeded */
    VAX_EVALID    /* semantic validation failure */
} VaxStatus;

/* --- QOI layer images (SPEC 6) --- */

typedef struct {
    uint32_t width, height;
    uint8_t channels, colorspace;
    unsigned char *pixels;    /* RGBA8, width*height*4 bytes */
} VaxImage;

[[nodiscard]] VaxStatus vax_image_decode(const unsigned char *data, size_t len,
                                          VaxImage *img, size_t *consumed,
                                          char *err, size_t errlen);
/* Scan without allocating pixels: returns dimensions and exact stream length. */
[[nodiscard]] VaxStatus vax_image_scan(const unsigned char *data, size_t len,
                                        uint32_t *width, uint32_t *height,
                                        size_t *consumed, char *err, size_t errlen);
void vax_image_free(VaxImage *img);

/* --- Packed .vax container (SPEC 3) --- */

typedef struct {
    VaxDoc doc;
    unsigned char *data;      /* whole file, owned */
    size_t size;
    const VaxNode **layers;   /* document order */
    size_t nlayers;
    size_t *off, *len;        /* per-layer QOI stream inside data */
} VaxPacked;

[[nodiscard]] VaxStatus vax_open_packed(const char *path, VaxPacked *out,
                                         char *err, size_t errlen);
void vax_packed_free(VaxPacked *p);

/* --- Validation (SPEC 10) --- */

typedef struct {
    char msg[256];
    size_t line;              /* 0 = no source line */
} VaxIssue;

typedef struct VaxSource {
    /* Fetch the QOI bytes of one layer. Folder form: read the layer's
     * (file ...) path. Packed form: the next stream in document order. */
    VaxStatus (*load_layer)(void *ctx, const VaxNode *layer,
                            const unsigned char **data, size_t *len,
                            char *err, size_t errlen);
    /* Fetch and parse an external chunk .scm (may be NULL; then chunk
     * entries with (file ...) are reported as issues). */
    VaxStatus (*load_scm)(void *ctx, const char *path, VaxDoc *doc,
                          char *err, size_t errlen);
    void *ctx;
} VaxSource;

/* Returns the number of issues found (may exceed max_issues; only the
 * first max_issues are stored). */
[[nodiscard]] size_t vax_validate(const VaxNode *root, const VaxSource *src,
                                  VaxIssue *issues, size_t max_issues);

#endif
