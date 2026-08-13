-- libarchive-xlings — reused verbatim in shape from xlings' own
-- `xmake/packages/libarchive.lua` (openxlings/xlings @ bb27e43), which is where
-- xlings keeps it. Kept as a local package definition here for the same reason
-- it is local there: it overrides a third-party package for this project's
-- needs, so it does not belong in mcpplibs-index.
--
-- WHY THE OVERRIDE IS NEEDED AT ALL. xmake-repo's `libarchive` configures with
-- libarchive's own defaults, and under the hermetic payload toolchain that
-- stops the install dead:
--
--     CMake Error at CMakeLists.txt:1349 (MESSAGE):
--       libgcc not found.
--
-- `-DENABLE_LibGCC=OFF` below is the line that fixes it. The rest of the OFFs
-- are the tools and test suites xlings never links — the same class of problem
-- the cmake arm hit from the other direction, where libarchive's test suite
-- could not even configure.
--
-- UPSTREAM'S OWN NOTE, kept because it is not obvious: the dependency list uses
-- `xz` rather than `lzma`, because libarchive probes via `find_package(LibLZMA)`
-- and that resolves to xz-utils' liblzma, not the 7-Zip LZMA SDK. With the wrong
-- one it silently falls back to fork-exec for `.tar.xz`, which is a correctness
-- difference, not a packaging preference.
package("libarchive-xlings")

    set_base("libarchive")

    add_versions("3.8.7", "4b787cca6697a95c7725e45293c973c208cbdc71ae2279f30ef09f52472b9166")
    add_versions("3.8.6", "213269b05aac957c98f6e944774bb438d0bd168a2ec60b9e4f8d92035925821c")

    add_deps("cmake")
    -- openssl is in the list even though `-DENABLE_OPENSSL=OFF` is passed
    -- below: `set_base("libarchive")` inherits the upstream package's openssl
    -- dependency, so `-lssl -lcrypto` reach libarchive's own link line anyway.
    -- Declaring it HERE is what puts openssl's lib dir on that line — a
    -- project-level `add_requires("openssl")` installs the package but does not
    -- reach into this package's build, and libarchive then fails with
    --     ld: cannot find -lssl: No such file or directory
    -- because the host ships libssl.so.3 without a `libssl.so` dev symlink.
    add_deps("zlib", "bzip2", "lz4", "zstd", "xz", "openssl")

    if is_plat("windows") then
        add_syslinks("advapi32")
    end

    on_install("windows", "linux", "macosx", function (package)
        local configs = {
            "-DENABLE_TEST=OFF",
            "-DENABLE_CAT=OFF",
            "-DENABLE_TAR=OFF",
            "-DENABLE_CPIO=OFF",
            "-DENABLE_OPENSSL=OFF",
            "-DENABLE_PCREPOSIX=OFF",
            "-DENABLE_LibGCC=OFF",
            "-DENABLE_CNG=OFF",
            "-DENABLE_ICONV=OFF",
            "-DENABLE_ACL=OFF",
            "-DENABLE_EXPAT=OFF",
            "-DENABLE_LIBXML2=OFF",
            "-DENABLE_LIBB2=OFF",
            "-DENABLE_ZLIB=ON",
            "-DENABLE_BZip2=ON",
            "-DENABLE_LZ4=ON",
            "-DENABLE_ZSTD=ON",
            "-DENABLE_LZMA=ON",
            -- NO `-DCMAKE_FIND_USE_CMAKE_SYSTEM_PATH=OFF` HERE. It looks like the
            -- right way to stop libarchive preferring the host's shared zlib/bz2
            -- over the static ones xrepo built, and it does — along with
            -- everything else libarchive legitimately probes for, so the package
            -- then fails to link at all. The runtime path is solved on the
            -- consumer side instead (see xmake.lua), not by blinding configure.
        }
        table.insert(configs, "-DCMAKE_BUILD_TYPE=" .. (package:debug() and "Debug" or "Release"))
        table.insert(configs, "-DBUILD_SHARED_LIBS=" .. (package:config("shared") and "ON" or "OFF"))
        if not package:config("shared") then
            package:add("defines", "LIBARCHIVE_STATIC")
        end
        import("package.tools.cmake").install(package, configs)
    end)

    on_test(function (package)
        assert(package:has_cfuncs("archive_version_number", {includes = "archive.h"}))
    end)

package_end()
