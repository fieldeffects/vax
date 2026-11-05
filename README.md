# vax

voxel asset exchange file format

See [SPEC.md](SPEC.md) for the format specification.

## Reference parser (C23)

`include/vax.h` + `src/` implement the `vaxscm` manifest parser, the packed
`.vax` container reader, the QOI layer decoder and the validation rules of
SPEC.md section 10.

```sh
make          # builds build/libvax.a and build/vax-check
make test     # unit tests (tests/test_vax.c)
build/vax-check name.vax    # packed file or name.vax/ folder; exit 0 ok, 1 issues, 2 error
```
