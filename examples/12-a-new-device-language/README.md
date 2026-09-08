# 12 — A device language the engine has never heard of

A rule package teaches mcpp to compile `.toy`, a small language with its own
compiler, and a project uses it. No mcpp release is involved: `.toy` is not in
the engine's built-in device-extension table and will never be.

```
cd examples/12-a-new-device-language/app
mcpp run
```

```
       Rules example.rules.toy (example:rules-toy)
    Building host tool toyc:toyc from toyc v0.1.0 (once per package version × host toolchain)
   Compiling toyapp v0.1.0 (.)
    Finished dev [unoptimized + debuginfo] in 0.65s

gcd(1071, 462) = 21
scale(21, 2)   = 42
answer()       = 42
```

## The three packages

| directory | what it is | who builds it |
|---|---|---|
| `toyc/` | the compiler for `.toy`: a lexer, a recursive-descent parser, semantic checks and a C++ emitter | mcpp, **for the build machine**, as a host tool |
| `rules-toy/` | the rule: it declares the extension, the module a consumer imports, and one action per `.toy` | the consumer's build program |
| `app/` | the project | mcpp, for the target |

## The language

`.toy` has integers, `let`, assignment, `if` / `else`, `while`, calls between
kernels, and the arithmetic and comparison operators. Every kernel takes and
returns an integer.

```
program := { kernel }
kernel  := 'kernel' ident '(' [ ident { ',' ident } ] ')' block
block   := '{' { stmt } '}'
stmt    := 'let' ident '=' expr ';'
         | ident '=' expr ';'
         | 'return' expr ';'
         | 'if' '(' expr ')' block [ 'else' block ]
         | 'while' '(' expr ')' block
expr    := cmp
cmp     := sum { ('<' | '>' | '<=' | '>=' | '==' | '!=') sum }
sum     := term { ('+' | '-') term }
term    := unary { ('*' | '/' | '%') unary }
unary   := [ '-' ] primary
primary := number | ident | ident '(' [ expr { ',' expr } ] ')' | '(' expr ')'
```

`src/kernels/answer.toy` is Euclid's algorithm and a caller:

```
kernel gcd(a, b) {
    while (b != 0) {
        let t = b;
        b = a % b;
        a = t;
    }
    return a;
}

kernel answer() {
    return scale(gcd(1071, 462), 2);
}
```

`toyc` emits one `extern "C"` function per kernel, and forward-declares them
all first so kernels may call each other in any order:

```cpp
extern "C" int toy_gcd(int v_a, int v_b) {
    while ((v_b != 0)) {
        int v_t = v_b;
        v_b = (v_a % v_b);
        v_a = v_t;
    }
    return v_a;
}
```

Three things in that output are decisions rather than accidents. **`extern "C"`**,
because the two sides are produced by different compilers and share no C++ ABI
— the same reason a device island's boundary is. **`v_` on every local**,
because a kernel that names a variable `class` must not become a C++ file that
fails to compile for a reason the toy source cannot express. **Parentheses
around every binary expression**, because the AST already holds the grouping
and the emitter does not reproduce C++'s precedence table.

The compiler rejects what it cannot compile, and says where. Each of these was
produced by running `toyc` on a file with that one defect:

```console
$ toyc bad.toy -o bad.cpp
bad.toy:1:1: error: kernel `scale` can reach its end without a `return`
bad2.toy:2:12: error: `b` is not a kernel in this file
bad3.toy:2:12: error: `x` is not declared
bad4.toy:3:5: error: expected `;`, found `return`
```

The first is the one worth the code it takes. A kernel returns an integer on
every path, and a block satisfies that if it ends in a `return` or in an
`if`/`else` whose branches both do — `while` never counts, because the language
cannot state that a loop runs at all. Emitting `return 0;` at the end instead
would have compiled everything and given a wrong answer for the kernel whose
author forgot a branch.

## The two manifest keys

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

Device extensions are not in the default source glob. A `.toy` is compiled
because the manifest names it:

```toml
[dependencies]
rules-toy = { path = "../rules-toy", features = ["rules-toy"] }

[build]
sources = ["src/*.cpp", "src/kernels/*.toy"]
```

## The compiler is a package, built for the build machine

The rule contains no compiler. `toyc` is an ordinary mcpp package with a
`kind = "bin"` target, and one line brings it into the graph:

```toml
[feature-deps.rules-toy]
toyc = { path = "../toyc", tools = ["toyc"], reexport = true }
```

| part | what it does |
|---|---|
| `tools = ["toyc"]` | mcpp builds that target **for the build machine**, even when the project around it is cross-compiling |
| `reexport = true` | the tool reaches whoever activated the feature. Without it the tool stays with this package, which is the supply-chain default: an arbitrary transitive dependency must not put entries in a build program's tool namespace |
| on `[feature-deps]` | a project that depends on this package **without** activating the rule builds no compiler |

The rule reads the path back with `mcpp::dep_bin("toyc", "toyc")`, under the
name of the manifest entry that **declared** the tool rather than the
consumer's spelling of anything.

That gating was measured. With `features = ["rules-toy"]` removed from the
consumer, the build stops before any tool is built:

```
error: build.mcpp imports 'example.rules.toy', and no dependency provides it as a host module.
       declared without `host-module = true`: rules-toy (in [dependencies])
```

and the tool store holds no `toyc` entry afterwards.

## What this example is the first to demonstrate

Every other rule in this repository is built into `mcpp:plugins` and compiles an
extension the engine already knows. This one adds a language from outside — the
property the accelerator design is built on — and it is the tree's only example
of a **dependency that produces a host tool**, which is how a rule package ships
a real compiler rather than a script.

## Criteria

| criterion | measured |
|---|---|
| a `.toy` is compiled and its output joins the link, on an engine that does not know the extension | `answer() = 42` |
| the language is executed rather than pattern-matched | `gcd(1071, 462) = 21`, computed by the emitted loop |
| editing the `.toy` reaches the artifact | `scale(…, 2)` → `scale(…, 3)`: `42` → `63` |
| the compiler is built for the build machine, on demand, and only when the rule is active | the `Building host tool` line above; no store entry without the feature |

## The boundary this example measured: a host tool is cached by version

Four changes were made one at a time, each from the same starting state:

| what changed | the artifact | how it was changed |
|---|---|---|
| the `.toy` source | follows: `42` → `63` | `scale(…, 2)` → `scale(…, 3)` |
| the compiler's **bytes**, at the path the action names | follows: `42` → `168` | overwriting the binary in the tool store |
| the compiler's **sources**, its version unchanged | does not follow: the previous answer stands | editing the emitter |
| the compiler's **version** | follows: `42` → `168`, and the tool is rebuilt | `0.1.0` → `0.1.1` |

Rows two and three are the whole finding, and they separate two things that are
easy to merge. **The action's input tracking works**: `rules-toy` declares the
compiler beside the source, and changing that file's bytes re-runs the edge.
**What does not happen is the rebuild that would change those bytes.** The tool
store's key is the tool package's identity, version, host triple, compiler
identity, profile, features and the versions of its transitive dependencies —
it holds no source content. For a package that arrives from an index the key is
exact, because a published version is immutable; for a `path` dependency being
edited it is not.

`mcpp run` prints `Finished dev in 0.00s` in row three, and that line is mcpp's
own summary rather than evidence: row two prints it too, and the artifact
changed.

**Row four does not test row two, which is why the difference is worth stating.**
The tool's path is on the action's command line, so a new version re-runs the
edge whether or not the compiler is also a declared input. Removing
`a.input(compiler)` from `rules-toy` and bumping the version left the artifact
following anyway. The isolating change is different bytes at the *same* path —
row two — and it takes both of its directions: with the input removed, the
artifact followed the overwrite and then stopped following the restore. CI runs
that pair.

Two ways out: bump the tool package's version, or empty the build cache with
`mcpp cache clean` — the tool store lives inside it, at
`<mcpp cache dir>/tool/<index>/<name>@<version>/`.

**One more trap sits behind them.** Going back from `0.1.1` to `0.1.0`, whose
clean tool was still in the store, left the artifact at `168`. The build program
did not re-run, so the plan still named the `0.1.1` binary, which had not
changed. `rm -rf target` cleared it. Iterating on a compiler means the version
goes forward only.

## Three things the first version got wrong

Recorded because each is a mistake a rule author will make once.

**An action's command does not run from the package root.**
`mcpp::device_sources()` answers with package-root-relative paths, and the
command runs from the build directory. The relative path reached `toyc`
unchanged and the read failed there. The rule joins `mcpp::manifest_dir()` to
each path.

**The compiler was a shell script, and its exit status lied.** The first `toyc`
summed with `sed … | grep … | paste -sd+ - | bc`. A pipeline's status is its
last command's: when an earlier stage produced nothing, `bc` still exited 0, so
`set -e` never fired and the script wrote a program that compiled, linked, ran
and printed `0`. Replacing the script with a compiled program removed the
class, not just the instance.

**The strings must outlive the action.** `a.id = ("toy:" + stem).c_str()` hands
`submit()` a pointer into a temporary that is already gone.
