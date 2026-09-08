# 13 — A device language the engine has never heard of

A rule package teaches mcpp to compile `.toy`, and a project uses it. No mcpp
release is involved: `.toy` is not in the engine's built-in device-extension
table and will never be.

```
cd examples/13-a-new-device-language/app
mcpp run                 # toy_answer() = 42
```

## The two keys

`rules-toy/mcpp.toml` declares them on the feature that selects the rule:

```toml
[features.rules-toy]
sources           = ["src/rules-toy.cppm"]
rule_module       = "example.rules.toy"
device_extensions = [".toy"]
```

| key | effect |
|---|---|
| `device_extensions` | a consumer that activates the feature gets `.toy` classified as a **device source**: never scanned for imports, never producing a BMI, and refused if no action claims it |
| `rule_module` | the module the consumer's build program imports. It implies `host-module = true`, so the consumer writes the feature and nothing else |

The consumer's whole declaration:

```toml
[dependencies]
rules-toy = { path = "../rules-toy", features = ["rules-toy"] }

[build]
sources = ["src/*.cpp", "src/kernels/*.toy"]
```

Device extensions are not in the default source glob. A `.toy` is compiled
because the manifest names it.

## The compiler

`rules-toy/tools/toyc.sh` is the entire toolchain for `.toy`: a file is a list
of integers and the entry point returns their sum. A shell script rather than a
vendor toolkit, because the subject here is the build graph — how a language the
engine does not know reaches the link — and a real device compiler would only
add a download to it.

The generated C++ declares the entry point `extern "C"`, for the same reason a
device island's boundary is: the two sides are produced by different compilers
and do not share a C++ ABI.

## What this example is the first to demonstrate

Every other rule in this repository is built into `mcpp:plugins` and compiles an
extension the engine already knows. This one adds a language from outside, which
is the property the accelerator design is built on and which nothing in the tree
exercised.

## Criteria

Three, each measured while this example was written, and each corresponding to
a defect the first version had:

| criterion | measured |
|---|---|
| a `.toy` is compiled and its output joins the link, on an engine that does not know the extension | `toy_answer() = 42` |
| editing the `.toy` reaches the artifact | adding `1` to the file: `42` → `43` |
| **editing the compiler reaches the artifact** | changing `toyc.sh` to add 100: `42` → `142` |

The third is the one that fails silently. An action whose only declared input is
its source leaves every edge clean when the compiler changes, so the artifact
keeps the bytes the previous compiler produced. `rules-toy` declares the script
as an input alongside the source.

## Three things the first version got wrong

Recorded because each is a mistake a rule author will make once.

**An action's command does not run from the package root.**
`mcpp::device_sources()` answers with package-root-relative paths, and the
command runs from the build directory. The relative path reached `toyc`
unchanged and the read failed there. The rule passes an absolute path.

**A pipeline's exit status is its last command's.** The first `toyc` summed with
`sed … | grep … | paste -sd+ - | bc`. When an earlier stage produced nothing,
`bc` still exited 0, so `set -e` never fired and the script wrote a program that
compiled, linked, ran, and printed `0`. It sums with one `awk` now.

**A rule finds its own files through `mcpp::dep_dir`, under the name the
consumer declared.** `dep_dir("rules-toy")` answers; `dep_dir("example.rules-toy")`
and `dep_dir("example:rules-toy")` return empty, because the key is the spelling
in the consumer's `[dependencies]`. The rule exposes `options::rule_dir` so a
consumer that declares the edge under another key can say so, and refuses with a
message naming what it looked for rather than running `sh /tools/toyc.sh`.
