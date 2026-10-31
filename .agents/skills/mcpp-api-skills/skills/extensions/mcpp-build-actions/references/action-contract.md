# Action Paths and Placeholders

Baseline: mcpp 2026.10.31.3. Source: `directives::prepare_actions` in
modules/buildmcpp/src/directives.cppm and action substitution in
src/build/prepare/plan.cpp.

## Path anchors

Literal inputs, outputs, depfile and prepare output_dir are resolved against
the declaring package root. A relative explicit cwd is package-root-relative.
The command's ordinary default working directory is the build directory.
Arguments are not all automatically made absolute: an argument such as
`tools/generate.py` must agree with the selected cwd or use an absolute path.

Changing cwd does not change how declared input/output paths are anchored.
The command must write the same file the output declaration names.

## Engine substitutions

| Token | Value and constraint |
|---|---|
| `${mcpp.self}` | Absolute current mcpp executable |
| `${mcpp.out_dir}` | Absolute configuration output directory; distinct from the build-program API's package-specific out_dir |
| `${mcpp.bin_dir}` | Product binary directory |
| `${mcpp.compile_db}` | Configuration compile database path |
| `${mcpp.contract}` | Configuration mcpp-contract.json path |
| `${mcpp.target_file:NAME}` | Link-unit output using its build-directory-relative spelling |
| `${mcpp.artifact:PACKAGE/TARGET}` | Artifact explicitly requested through a dependency's artifacts edge |
| `${mcpp.stage_dir}` | Absolute staged package closure, only in an artifact action of the applicable packaging pass |

Engine path substitutions use forward slashes on Windows too. Unknown product
or artifact references fail; feature-gated targets must be active. Duplicate
target names in unrelated members can be ambiguous.

The target-file form must keep the same graph node spelling as the link edge.
If the command uses a different cwd, account for that relative token explicitly.
An object action runs before the link: target() selects which link units receive
its objects; it is not a post-link target-file operation.

## Portable file copy

An action can invoke the engine rather than assume a shell copy command:

```cpp
mcpp::action a;
a.id = "copy-asset";
a.role = mcpp::roles::artifact;
a.input("assets/banner.txt");
a.output("${mcpp.bin_dir}/banner.txt");
a.arg("${mcpp.self}").arg("stage").arg("--verify").arg("content")
 .arg("--output").arg("${mcpp.bin_dir}/banner.txt").arg("assets/banner.txt")
 .cwd(".");
a.submit();
```

Provide the actual input file. Stage creates destination parents and writes
when content differs. Its action argument shape is a public contract; other
backend commands are not automatically public because they appear in help.

## Pack stage

Declare the provided pack format on all runs. Submit actions using stage_dir
only for the requested format/pass and with role artifact. A plain build has
no staged closure. Library-form/unsupported staging can also refuse that token;
handle the format's actual applicability rather than substituting an empty path.

Source/check/object/prepare cannot read a future staged tree. Do not generate
compile inputs from post-link packaging output.
