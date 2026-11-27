import std;
import mcpplibs.mtest;
import mcpp.toolchain.target_rows;
import mcpp.toolchain.target_schema;

using namespace mcpplibs::mtest;

// THE STATIC TLS A ROW'S LOADER SERVES AN IMAGE LOADED BY `dlopen` (mcpp#907).
// The check after the link is the engine's; how much the loader serves is the
// row's (`dlopen-static-tls`), so the engine names no C library for it.

UTest the_glibc_rows_state_what_dlopen_serves{"DlopenStaticTls/TheGlibcRowsStateWhatDlopenServes", [] {
    // glibc's `glibc.rtld.optional_static_tls`, 512 bytes since 2.32.
    for (auto triple : { "x86_64-linux-gnu", "aarch64-linux-gnu", "i686-linux-gnu" }) {
        auto const* row = mcpp::toolchain::target::find_row(triple);
        expect(row != nullptr);
        if (row) expect(eq(row->dlopenStaticTls, std::int64_t{512}));
    }
}};

UTest a_row_that_states_no_bound_is_not_checked{"DlopenStaticTls/ARowThatStatesNoBoundIsNotChecked", [] {
    // musl and the systems that are not glibc's state nothing: 0 is unstated.
    for (auto triple : { "x86_64-linux-musl", "aarch64-macos", "x86_64-windows-msvc" }) {
        auto const* row = mcpp::toolchain::target::find_row(triple);
        expect(row != nullptr);
        if (row) expect(eq(row->dlopenStaticTls, std::int64_t{0}));
    }
}};
