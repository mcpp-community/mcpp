# Build Api Reference

Interface names and declarations for mcpp 2026.10.31.3.

## Public C++ declarations

| Interface | Details |
| --- | --- |
| `action& arg(const char* a)` | action public method |
| `action& cwd(const char* dir)` | action public method |
| `action& env(const char* name, const char* value)` | action public method |
| `action& imports(const char* n)` | action public method |
| `action& input(const char* p)` | action public method |
| `action& output(const char* p)` | action public method |
| `action& output_dir(const char* p)` | action public method |
| `action& provides(const char* n)` | action public method |
| `action& target(const char* n)` | action public method |
| `void submit()` | action public method |
| `bool has_feature(const char* name)` | Build-program function |
| `bool xpkg_pending()` | Build-program function |
| `const char* abi_tool(const char* role)` | Build-program function |
| `const char* accel()` | Build-program function |
| `const char* compiler()` | Build-program function |
| `const char* cxx_abi_root()` | Build-program function |
| `const char* cxx_runtime()` | Build-program function |
| `const char* cxx_stdlib()` | Build-program function |
| `const char* dep_bin(const char* pkg, const char* tool)` | Build-program function |
| `const char* dep_dir(const char* name)` | Build-program function |
| `const char* dep_linkage(const char* name)` | Build-program function |
| `const char* device_sources()` | Build-program function |
| `const char* graph_file()` | Build-program function |
| `const char* host()` | Build-program function |
| `const char* manifest_dir()` | Build-program function |
| `const char* min_platform_version()` | Build-program function |
| `const char* msvc_crt_linkage()` | Build-program function |
| `const char* msvc_instance_dir()` | Build-program function |
| `const char* ninja_program()` | Build-program function |
| `const char* out_dir()` | Build-program function |
| `const char* pack_debug_symbols_dir()` | Build-program function |
| `const char* pack_format()` | Build-program function |
| `const char* pack_stage_dir()` | Build-program function |
| `const char* pack_strip()` | Build-program function |
| `const char* package_authors()` | Build-program function |
| `const char* package_description()` | Build-program function |
| `const char* package_license()` | Build-program function |
| `const char* package_name()` | Build-program function |
| `const char* package_namespace()` | Build-program function |
| `const char* package_repo()` | Build-program function |
| `const char* package_version()` | Build-program function |
| `const char* phase()` | Build-program function |
| `const char* pkg_config_libdir()` | Build-program function |
| `const char* profile()` | Build-program function |
| `const char* sysroot_dir()` | Build-program function |
| `const char* target()` | Build-program function |
| `const char* target_arch()` | Build-program function |
| `const char* target_builtins_lib()` | Build-program function |
| `const char* target_env()` | Build-program function |
| `const char* target_libc()` | Build-program function |
| `const char* target_libc_profile()` | Build-program function |
| `const char* target_os()` | Build-program function |
| `const char* tool(const char* role)` | Build-program function |
| `const char* tool_env()` | Build-program function |
| `const char* toolchain_binutils_dir()` | Build-program function |
| `const char* toolchain_dir()` | Build-program function |
| `const char* toolchain_sysroot()` | Build-program function |
| `const char* toolset_identity()` | Build-program function |
| `const char* xpkg_dir(const char* name)` | Build-program function |
| `const char* xpkg_dir(const char* ns, const char* name)` | Build-program function |
| `const char* xpkg_program(const char* name)` | Build-program function |
| `const char* xpkg_program(const char* ns, const char* name)` | Build-program function |
| `const char* xpkg_request(const char* ns, const char* name)` | Build-program function |
| `const char* xpkg_source(const char* name)` | Build-program function |
| `const char* xpkg_source(const char* ns, const char* name)` | Build-program function |
| `mcpp::action::blocking` | bool; default=false |
| `mcpp::action::depfile` | const char*; default="" |
| `mcpp::action::description` | const char*; default="" |
| `mcpp::action::id` | const char*; default="" |
| `mcpp::action::role` | const char*; default=roles::source |
| `mcpp::diagnostic::hint` | const char*; default="" |
| `mcpp::diagnostic::impact` | const char*; default="" |
| `mcpp::diagnostic::message` | const char*; default="" |
| `mcpp::diagnostic::severity` | const char*; default="warning" |
| `mcpp::action` | public type or role constant |
| `mcpp::diagnostic` | public type or role constant |
| `mcpp::roles::artifact` | public type or role constant |
| `mcpp::roles::check` | public type or role constant |
| `mcpp::roles::object` | public type or role constant |
| `mcpp::roles::prepare` | public type or role constant |
| `mcpp::roles::source` | public type or role constant |
| `void artifact_name(const char* kind, const char* pattern)` | Build-program function |
| `void cflag(const char* flag)` | Build-program function |
| `void cxxflag(const char* flag)` | Build-program function |
| `void define(const char* name)` | Build-program function |
| `void deploy(const char* from, const char* to)` | Build-program function |
| `void fact(const char* name, const char* version)` | Build-program function |
| `void floor(const char* spec)` | Build-program function |
| `void generated(const char* path)` | Build-program function |
| `void include_dir(const char* dir)` | Build-program function |
| `void include_dir_after(const char* dir)` | Build-program function |
| `void link_driver(const char* driver)` | Build-program function |
| `void link_flag(const char* flag)` | Build-program function |
| `void link_input(const char* path)` | Build-program function |
| `void link_input_from(const char* as, const char* token)` | Build-program function |
| `void link_lib(const char* name)` | Build-program function |
| `void link_script(const char* path)` | Build-program function |
| `void link_search(const char* dir)` | Build-program function |
| `void link_start(const char* symbol)` | Build-program function |
| `void provides_pack_format(const char* name)` | Build-program function |
| `void report(const diagnostic& d)` | Build-program function |
| `void rerun_if_changed(const char* path)` | Build-program function |
| `void rerun_if_changed_glob(const char* pattern)` | Build-program function |
| `void rerun_if_env_changed(const char* var)` | Build-program function |
| `void run_exclusive()` | Build-program function |
| `void run_host(const char* where)` | Build-program function |
| `void runner(const char* name, const char* token)` | Build-program function |
| `void runner(const char* token)` | Build-program function |
| `void runner_longlived(const char* name)` | Build-program function |
| `void runtime_search_dir(const char* dir)` | Build-program function |
| `void seal(int step, const char* token)` | Build-program function |
| `void source(const char* path)` | Build-program function |
| `void toolchain(const char* key, const char* value)` | Build-program function |
| `void warning(const char* message)` | Build-program function |
| `void windows_entry(const char* target, const char* value)` | Build-program function |
| `void windows_subsystem(const char* target, const char* value)` | Build-program function |

## Build-program directives

| Interface | Details |
| --- | --- |
| `mcpp:action` | scope=GraphNode; transform=Verbatim; since protocol=1; requires output=false |
| `mcpp:artifact-name` | scope=LinkGlobal; transform=Verbatim; since protocol=17; requires output=false |
| `mcpp:cfg` | scope=PackagePrivate; transform=DefinePrefix; since protocol=1; requires output=false |
| `mcpp:cflag` | scope=PackagePrivate; transform=Verbatim; since protocol=1; requires output=false |
| `mcpp:cxxflag` | scope=PackagePrivate; transform=Verbatim; since protocol=1; requires output=false |
| `mcpp:decision` | scope=Advisory; transform=Verbatim; since protocol=15; requires output=false |
| `mcpp:deploy` | scope=LinkGlobal; transform=Deploy; since protocol=11; requires output=false |
| `mcpp:diagnostic` | scope=Advisory; transform=Verbatim; since protocol=14; requires output=false |
| `mcpp:fact` | scope=Claim; transform=Verbatim; since protocol=7; requires output=false |
| `mcpp:floor` | scope=Claim; transform=Verbatim; since protocol=7; requires output=false |
| `mcpp:generated` | scope=SourceSet; transform=Verbatim; since protocol=1; requires output=true |
| `mcpp:include-dir` | scope=PackagePrivate; transform=AbsPath; since protocol=1; requires output=false |
| `mcpp:include-dir-after` | scope=PackagePrivate; transform=AbsPath; since protocol=1; requires output=false |
| `mcpp:link-driver` | scope=LinkGlobal; transform=Verbatim; since protocol=17; requires output=false |
| `mcpp:link-flag` | scope=LinkGlobal; transform=Verbatim; since protocol=8; requires output=false |
| `mcpp:link-input` | scope=LinkGlobal; transform=Verbatim; since protocol=17; requires output=false |
| `mcpp:link-lib` | scope=LinkGlobal; transform=LibFlag; since protocol=1; requires output=false |
| `mcpp:link-script` | scope=LinkGlobal; transform=LinkerScript; since protocol=3; requires output=false |
| `mcpp:link-search` | scope=LinkGlobal; transform=LibSearchPath; since protocol=1; requires output=false |
| `mcpp:link-start` | scope=LinkGlobal; transform=Verbatim; since protocol=16; requires output=false |
| `mcpp:pack-format` | scope=Claim; transform=Verbatim; since protocol=9; requires output=false |
| `mcpp:rerun-if-changed` | scope=RerunKey; transform=Verbatim; since protocol=1; requires output=false |
| `mcpp:rerun-if-changed-glob` | scope=RerunKey; transform=Verbatim; since protocol=2; requires output=false |
| `mcpp:rerun-if-env-changed` | scope=RerunKey; transform=Verbatim; since protocol=1; requires output=false |
| `mcpp:run-exclusive` | scope=RunGlobal; transform=Verbatim; since protocol=6; requires output=false |
| `mcpp:run-host` | scope=RunGlobal; transform=Verbatim; since protocol=17; requires output=false |
| `mcpp:runner` | scope=RunGlobal; transform=Verbatim; since protocol=4; requires output=false |
| `mcpp:runner-longlived` | scope=RunGlobal; transform=Verbatim; since protocol=6; requires output=false |
| `mcpp:runner-named` | scope=RunGlobal; transform=Verbatim; since protocol=6; requires output=false |
| `mcpp:runtime-search-dir` | scope=LinkGlobal; transform=AbsPath; since protocol=12; requires output=false |
| `mcpp:seal` | scope=LinkGlobal; transform=Verbatim; since protocol=17; requires output=false |
| `mcpp:source` | scope=SourceSet; transform=Verbatim; since protocol=1; requires output=true |
| `mcpp:toolchain` | scope=Claim; transform=Verbatim; since protocol=15; requires output=false |
| `mcpp:warning` | scope=Advisory; transform=Verbatim; since protocol=5; requires output=false |
| `mcpp:windows-entry` | scope=TargetLink; transform=Verbatim; since protocol=10; requires output=false |
| `mcpp:windows-subsystem` | scope=TargetLink; transform=Verbatim; since protocol=10; requires output=false |
| `mcpp:xpkg-request` | scope=Claim; transform=Verbatim; since protocol=15; requires output=false |

## Source

- [modules/buildmcpp/src/directives.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/modules/buildmcpp/src/directives.cppm)
- [src/build/hostprogram.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/src/build/hostprogram.cppm)
