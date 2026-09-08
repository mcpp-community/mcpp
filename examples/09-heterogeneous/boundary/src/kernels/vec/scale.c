/* A second island, one directory deeper, and the whole reason this directory
   exists: the generated namespace mirrors the tree. `vec/` is what puts this
   entry point in `boundary::kernels::vec` while the file above it is in
   `boundary::kernels`.

   The file name reaches no name. An island holds zero, one or many marked
   entry points and each carries its own -- which is the difference from the
   shader lane, where a file name becomes the identifier because a payload has
   no name of its own. */

MCPP_EXPORT_C
int boundary_scale(float a, float* out, unsigned n) {
    for (unsigned i = 0; i < n; ++i) out[i] = a * out[i];
    return 0;
}
