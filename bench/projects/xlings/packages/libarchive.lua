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
    add_deps("zlib", "bzip2", "lz4", "zstd", "xz")

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
