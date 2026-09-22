# mcpp-bin

The prebuilt [mcpp](https://github.com/mcpp-community/mcpp) {version} release
binary, packaged for `pip`. mcpp is a module-first build tool for modern C++:
`import std`, module interface units and partitions, a package index,
toolchain management and cross-compilation from one command.

```bash
pip install mcpp-bin
mcpp new hello && cd hello && mcpp build && mcpp run
```

The package installs the `mcpp` command. The PyPI name is `mcpp-bin` because
`mcpp` belongs to an unrelated project.

Wheels are published for Linux x86_64 and aarch64 (fully static, glibc or
musl), macOS 14+ on Apple silicon, and Windows x86_64. Each wheel carries the
same release payload as `install.sh`, Homebrew and the AUR `mcpp-bin` package.

mcpp keeps its registry sandbox, caches and downloaded toolchains in
`~/.mcpp` (`%USERPROFILE%\.mcpp` on Windows), not in the Python environment.
Set `MCPP_HOME` to use another directory. The first `mcpp build` initialises
that directory and fetches a toolchain, which takes a while once per user.
`pip uninstall mcpp-bin` removes the command; delete `~/.mcpp` to remove the
per-user data as well.

Documentation: <https://github.com/mcpp-community/mcpp/tree/main/docs>
