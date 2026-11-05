# VAX — Voxel Asset Exchange Format

**Version 1.0 (draft)** · File extension `.vax` · Manifest grammar `vaxscm`

## 1. Overview

VAX stores a voxel asset as a **manifest** written in a restricted subset of Scheme (s-expressions) plus **layer images** in [QOI](https://qoiformat.org) format. Each layer is one slice of the volume along a chosen axis, cropped to its own bounding box. Empty slices are simply absent from the manifest.

Two file forms:

- **Folder form (canonical, for authoring and version control):** a directory `name.vax/` containing `manifest.scm` and referenced `.qoi` files.
- **Packed form (built artifact, for distribution):** a single `name.vax` file = magic + manifest + concatenated QOI streams.

Conformance keywords follow RFC 2119 (MUST / SHOULD / MAY).

## 2. Folder form

```
name.vax/
  manifest.scm        UTF-8, one top-level datum (see §4)
  layers/
    000.qoi           QOI streams; any relative path may be referenced
    001.qoi
```

- `(file "...")` paths in the manifest are relative to the directory containing `manifest.scm`, use `/` separators, MUST NOT be absolute, MUST NOT contain `..` or drive letters.
- `layers/NNN.qoi` naming is the convention produced by `vax-unpack`; any relative path is legal.

## 3. Packed form (byte layout)

```
offset  size  field
0       4     magic "VAX1" (ASCII)
4       8     u64 LE  manifest_len = M
12      M     manifest bytes (canonical serialization, §3.2)
12+M    ...   QOI streams concatenated, in layer document order (§3.1)
```

### 3.1 Layer document order

All `(layer ...)` entries in the whole manifest — top-level `(layers ...)`, chunk layer lists, and animation frames — are assigned QOI streams **in the order the entries appear in the serialized manifest text (depth-first, left-to-right)**. In packed form a layer entry MUST NOT have a `(file ...)` key; its image is the next QOI stream in sequence. QOI streams are self-delimiting (8-byte end marker), so the reader decodes them sequentially without lengths.

### 3.2 Canonical serialization (for packed manifests)

Deterministic, so pack → unpack → pack is byte-stable:

- atom → its token (integers without decimal point, strings with escapes, symbols verbatim)
- list → `(` item (` ` item)* `)`; empty list → `()`
- top level → datum followed by one `\n`
- comments and extra whitespace are not preserved

Folder-form manifests MAY use arbitrary whitespace and `;` comments.

## 4. Manifest grammar (vaxscm)

A manifest is one datum. Grammar:

```
file    = ws datum ws
datum   = list | number | string | symbol
list    = "(" ws datum* ws ")"
number  = ["-"] digit+ ["." digit+]
string  = '"' (anychar except '"' '\' | "\"" | "\\" | "\n" | "\t" | "\r")* '"'
symbol  = symchar+                          ; token that is not a number
comment = ";" ... end-of-line
ws      = (space | tab | CR | LF | comment)*
```

- The top-level datum MUST be a list whose first element is the symbol `vax`.
- No exponents, no booleans, no quote/quasiquote, no dotted pairs, no vectors, no `#` syntax. Readers MUST reject them.
- Duplicate keys in the same list: readers MUST use the first occurrence; `vax-check` MUST report it as an error.
- Unknown keys anywhere MUST be ignored by readers (forward compatibility, §12).

### 4.1 Key reference

| Key | Required | Meaning |
|---|---|---|
| `(version n)` | yes, first key | format version; currently `1` |
| `(name "s")` | no | display name |
| `(dims x y z)` | yes | volume max extents, ints ≥ 1 |
| `(axis z\|x\|y)` | no, default `z` | layer axis |
| `(encoding rgba\|palette)` | no, default `rgba` | pixel interpretation (§6) |
| `(palette ...)` | if `palette` | palette entries (§4.3) |
| `(layers ...)` | yes* | layer list of the whole volume (*omit if `chunk-size` present) |
| `(chunk-size n)` | no | enables chunking (§7) |
| `(chunks ...)` | if `chunk-size` | chunk list (§7) |
| `(meta ...)` | no | free-form `(key value)` pairs, ignored by readers |
| `(anim ...)` | no | animation frames (§8) |

### 4.2 Layer entry

```
(layer (<axis> s) (u a) (v b) (w w) (h h) (file "path"))
```

- `<axis>` is exactly the symbol from `(axis ...)`; `s` = slice index, 0-based.
- `u`,`v` are the two remaining axis symbols; `a`,`b` = rect origin, default `0`. `w`,`h` required, ≥ 1. `w` spans axis `u`, `h` spans axis `v`.
- Folder form: `(file "...")` required. Packed form: MUST be absent.

### 4.3 Palette entry

```
(index (color r g b [a]) (material (key value) ...))
```

`index` unique non-negative int; color components 0–255, `a` default 255. `material` is free-form; readers ignore unknown material keys.

### 4.4 Chunk entry

```
(chunk (cx a) (cy b) (cz c) (layers ...))        ; inline
(chunk (cx a) (cy b) (cz c) (file "c.scm"))      ; external file = one (layers ...) datum
```

## 5. Coordinate mapping

Pixel `(px, py)` of a layer image (px: left→right 0..w-1, py: top→bottom 0..h-1) maps to voxel:

| axis | slice key | rect keys | voxel |
|---|---|---|---|
| `z` | `(z s)` | `(x a) (y b)` | `(a+px, b+py, s)` |
| `y` | `(y s)` | `(x a) (z b)` | `(a+px, s, b+py)` |
| `x` | `(x s)` | `(y a) (z b)` | `(s, a+px, b+py)` |

## 6. Layers and QOI

- Each layer is a **complete QOI v1.0 stream**: magic `qoif`, `channels = 4` (RGBA), `colorspace` 0 (sRGB, recommended) or 1, 8-byte end marker present. Manifest `w`,`h` MUST equal the QOI header width/height.
- **Occupancy is the alpha channel only:** `A = 0` → empty voxel; `A = 255` → solid; any other alpha value is a validation error.
- `(encoding rgba)`: RGB = voxel color.
- `(encoding palette)`: R = palette index (MUST exist in `(palette ...)` for solid pixels); G,B ignored by readers.

## 7. Chunks

If `(chunk-size n)` present: the volume is split into `ceil(dim/n)` chunks per axis. Chunk `(cx cy cz)` has origin `(cx·n, cy·n, cz·n)` and dims `min(n, dim − c·n)` per axis. **All coordinates inside a chunk are relative to the chunk origin.** A chunk not listed in `(chunks ...)` is entirely empty.

## 8. Animation (optional)

```
(anim (fps n) (frame (layers ...))+)
```

Each frame is a complete layer list; `fps > 0`. Frames' layers participate in packed document order like any others.

## 9. Pack / unpack

- `vax-pack dir.vax/ → name.vax`: validate (§10), serialize manifest canonically, write §3 layout with QOI bytes in document order.
- `vax-unpack name.vax → dir.vax/`: parse, walk the tree depth-first, decode each QOI stream, write `layers/NNN.qoi` (sequential NNN), rewrite the manifest in folder form with `(file "layers/NNN.qoi")`.

## 10. Validator (`vax-check`) rules

Exit 0 = ok, 1 = validation errors, 2 = parse/IO error. Checks:

1. Grammar (§4); top-level `vax`; `(version 1)` first key.
2. `dims` positive, within limits (§11); axis/encoding symbols valid.
3. Every layer: `w,h` ≥ 1, rect within volume/chunk bounds (§5, §7), slice index unique within its layer list, QOI file exists / stream present.
4. QOI header: magic `qoif`, channels 4, `w,h` match entry, alpha values ∈ {0, 255}.
5. Palette mode: every solid pixel's R is a defined palette index.
6. Paths: relative, no `..`, no absolute (§2).
7. Chunk coords within chunk-grid ranges; chunk files parse as `(layers ...)`.
8. Duplicate keys flagged (§4).

## 11. Limits (readers MUST enforce)

| Limit | Value |
|---|---|
| dims per axis | ≤ 2^24 |
| dims product | ≤ 2^40 |
| layers per layer list / chunks / file refs | ≤ 2^20 |
| manifest size | ≤ 64 MiB |
| string length | ≤ 4096 chars |
| nesting depth | ≤ 64 |
| layer pixels (w·h) | ≤ 2^26 |

Implementations MAY impose lower limits.

## 12. Versioning

`(version n)`: readers reject `n` greater than their supported major version. New optional keys do not bump the version. Breaking changes → next major version.

## 13. Security notes

Enforce §11 limits before allocating. Never resolve `(file ...)` paths outside the `.vax` directory; reject absolute paths, `..`, and symlinks escaping the root (or refuse symlinks). Treat all counts and sizes as untrusted.

## 14. Example

Folder form, sparse slices (z 2–46 absent = empty), cropped rects, palette encoding:

```scheme
; tree.vax/manifest.scm
(vax
  (version 1)
  (name "tree")
  (dims 64 64 96)
  (axis z)
  (encoding palette)
  (palette
    (1 (color 92 62 40)  (material (roughness 0.9)))
    (2 (color 44 158 66) (material (roughness 0.6) (emissive 0.05))))
  (layers
    (layer (z 0)  (x 20) (y 20) (w 24) (h 24) (file "layers/000.qoi"))
    (layer (z 1)  (x 21) (y 21) (w 22) (h 22) (file "layers/001.qoi"))
    (layer (z 47) (x 30) (y 30) (w 4)  (h 4)  (file "layers/047.qoi"))))
```

Packed form is the same manifest (canonically serialized) prefixed by `VAX1` + u64 length, with the three `.qoi` files concatenated after it in that order.

## 15. Non-goals for v1

LOD/mip levels and per-voxel attributes beyond one palette index are out of scope for v1; both slot in later as new optional keys (e.g. `(lods ...)`, extra aligned QOI layers), which the ignore-unknown-keys rule (§4) already permits.

## 16. References

- QOI — “Quite OK Image” format v1.0, https://qoiformat.org (header `qoif`, RGBA, end marker `00 00 00 00 00 00 00 01`)
