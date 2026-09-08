/* The island. An ordinary C file here, so this example runs on any machine;
   substituting a `.cu` and a device rule is what `../cuda` shows. What makes
   this an island is the property the boundary exists for: it is compiled
   separately, it cannot import a module, and its interface is `extern "C"`.

   The entry points carry a package prefix because the symbol they define is
   global to the whole program. The namespace they arrive in does not change
   that, which is why `strip_prefix` exists: `boundary::kernels::saxpy` is the
   short spelling of `boundary_saxpy`, and both name one entity. */

static const char* g_ran_on = "";

MCPP_EXPORT_C
int boundary_saxpy(float a, const float* x, const float* y,
                   float* out, unsigned n) {
    for (unsigned i = 0; i < n; ++i) out[i] = a * x[i] + y[i];
    g_ran_on = "cpu (this example's island is an ordinary C file)";
    return 0;
}

MCPP_EXPORT_C
const char* boundary_ran_on(void) { return g_ran_on; }
