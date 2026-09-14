// mcpp.pack.stage_tree — what a staged tree promises an artifact action, and
// the one file that says so.
//
// `mcpp pack` has always computed a staged tree: the dependency closure after
// the strip policy, the debug-symbol split and `include`/`exclude`. Until
// `${mcpp.stage_dir}` it then compressed the tree and the directory was gone,
// so a `.deb`, an AppImage, a `.app` and an `.msi` each had to rebuild the same
// closure. Exposing it is one addition that serves every format and encodes no
// format's knowledge, which is the test for whether something belongs in the
// engine at all.
//
// WHY THERE IS A MANIFEST FILE AND NOT JUST A DIRECTORY. ninja identifies an
// input by a path and compares an mtime. A directory's mtime moves when its
// immediate entries change and not when a file two levels down is replaced, so
// naming the directory as an input would make the dist edge dirty for the wrong
// reasons and clean for the wrong reasons. The manifest is CONTENT-BEARING —
// one `<size> <relative path>` line per staged file, sorted — so it changes
// exactly when the staged set or any staged file's length changes, and it is a
// single ordinary file that ninja can compare.
//
// IT IS A SIBLING OF THE TREE, NOT A MEMBER OF IT. A file inside the staged
// directory would be collected by every format that packages the directory
// wholesale, and would then ship inside the user's installer. The sibling
// spelling is what keeps a mechanism the engine added from appearing in a
// product it does not own.
//
// The manifest deliberately records SIZES rather than content hashes. The tree
// can be hundreds of megabytes and is rebuilt on every pack; hashing it would
// make the common case pay for a distinction the uncommon case does not need,
// because a staged file whose length is unchanged and whose bytes differ can
// only have come from a rebuild, and a rebuild moved the link output that the
// dist edge also depends on.
//
// THE HEADER LINE. `mcpp pack` stages the program and its declared runtime
// files before it asks whether this host can walk the artifact's dependency
// closure (§3 of
// .agents/docs/2026-09-13-630-what-a-framework-still-hits-in-the-engine.md),
// so a tree can exist without one -- a Mach-O program today, a non-PE
// artifact on a Windows host. `closure = walked | not-walked` is the one fact
// the manifest states that the sorted file list cannot: a provider that needs
// the closure reads this line and decides for itself rather than discovering
// the gap by what is absent from `lib/`.
//
// THE `needs` LINES (#634 A3). `closure = walked` says the closure was
// resolved; it does not say what the closure was, and a provider that places
// libraries itself (`dist-apk` under `lib/<abi>/`, `dist-apple` under
// `Frameworks/`) would otherwise have to tell a staged library from a staged
// resource by its extension. One line per needed name states it:
//
//     needs<TAB><name><TAB><staged path>   the tree carries it at that path
//     needs<TAB><name><TAB>platform        the target provides it
//     needs<TAB><name><TAB>unresolved      neither; the closure is not-walked
//
// TAB-separated because both fields are names a loader reads and either may
// contain a space (a Mach-O install name, a Windows directory).

module;
#include <cstdio>

export module mcpp.pack.stage_tree;

import std;

export namespace mcpp::pack {

// The manifest that describes the tree staged at `stagingRoot`.
//
// A SIBLING, DERIVED FROM THE NAME RATHER THAN PLACED INSIDE. Spelled in one
// function because two readers need the same answer for different reasons:
// `prepare` names it as an implicit input while the file does not yet exist,
// and `pack::run` writes it. A second derivation of a path is a path that
// disagrees on the platform whose separator the author did not test.
std::filesystem::path stage_manifest_path(const std::filesystem::path& stagingRoot) {
    auto p = stagingRoot;
    p += ".stage-manifest";
    return p;
}

// Whether `mcpp pack` resolved the staged tree's dependency closure, and why
// not when it did not.
//
// The default (`walked = true`, empty `reason`) is what every archive format
// and every successful ELF/PE pack reports. `walked = false` reaches a
// manifest only for a DISPATCHED format -- `pack::closure_unavailable_outcome`
// turns the same condition into a hard failure for `--format tar` and
// `--format dir`, so those two never write a `not-walked` manifest.
// One name the staged program or a staged library needs, and what satisfies
// it. See the `needs` lines in the header comment.
struct ClosureNeed {
    enum class Kind { Staged, Platform, Unresolved };
    std::string name;                 // as the needing object spells it
    Kind        kind = Kind::Staged;
    // `Kind::Staged` only: the member's path relative to the staged tree,
    // `/`-separated on every host.
    std::string staged;
};

struct ClosureStatus {
    bool        walked = true;
    // Populated only when `!walked`. A SINGLE LINE: the manifest is a plain
    // list of one entry per line, and the reason text pack::run produces
    // carries its own embedded newlines (it doubles as a CLI diagnostic), so
    // `write_stage_manifest` folds them to spaces before writing.
    std::string reason;
    // Every name the closure resolved, or failed to. Empty when no closure was
    // read: a mode that bundles nothing (`system`, `static`), or a row whose
    // closure this host cannot read.
    std::vector<ClosureNeed> needs;
};

// The `needs` line for `need`, without its newline.
std::string render_closure_need(const ClosureNeed& need) {
    switch (need.kind) {
        case ClosureNeed::Kind::Staged:
            return std::format("needs\t{}\t{}", need.name, need.staged);
        case ClosureNeed::Kind::Platform:
            return std::format("needs\t{}\tplatform", need.name);
        case ClosureNeed::Kind::Unresolved:
            break;
    }
    return std::format("needs\t{}\tunresolved", need.name);
}

// Write the manifest for the tree now on disk at `stagingRoot`.
//
// Best-effort by construction and deliberately so: the manifest is a
// dependency-tracking convenience, and a pack that produced a correct tree must
// not fail because a sibling bookkeeping file could not be written. A missing
// manifest makes the dist edge fail with ninja's own "missing and no known rule
// to make it", which names the file — a legible failure rather than a silent
// staleness.
bool write_stage_manifest(const std::filesystem::path& stagingRoot,
                          ClosureStatus closure = {}) {
    std::error_code ec;
    if (!std::filesystem::is_directory(stagingRoot, ec)) return false;

    std::vector<std::string> lines;
    for (auto const& entry :
         std::filesystem::recursive_directory_iterator(
             stagingRoot, std::filesystem::directory_options::skip_permission_denied, ec))
    {
        if (ec) break;
        // Symlinks are recorded by NAME AND NOT FOLLOWED. `bundle-all`
        // dereferences a soname link while staging, so what remains is a real
        // file; a link that survives points outside the tree, and following it
        // would make the manifest describe a file the package does not carry.
        if (!entry.is_regular_file(ec) || entry.is_symlink()) {
            if (entry.is_symlink())
                lines.push_back(std::format("link {}",
                    std::filesystem::relative(entry.path(), stagingRoot, ec).generic_string()));
            continue;
        }
        auto rel = std::filesystem::relative(entry.path(), stagingRoot, ec).generic_string();
        if (ec || rel.empty()) continue;
        lines.push_back(std::format("{} {}",
            std::filesystem::file_size(entry.path(), ec), rel));
    }
    // Sorted, because a directory iteration order is not a promise. Two packs
    // of one tree must produce identical bytes or the dist edge is dirty on
    // every run for no reason.
    std::ranges::sort(lines);

    // The header, ahead of the sorted file list and NOT part of it -- it is a
    // property of the whole tree, not an entry in it, and mixing the two
    // would put "closure = walked" through the same alphabetical sort as a
    // path and make its position in the file a function of what got staged.
    std::string text = closure.walked ? "closure = walked\n" : "closure = not-walked\n";
    if (!closure.walked) {
        // Folded to one line: see the field comment on `ClosureStatus::reason`.
        std::string reason = closure.reason;
        std::ranges::replace(reason, '\n', ' ');
        std::string folded;
        bool lastWasSpace = false;
        for (char c : reason) {
            bool isSpace = (c == ' ' || c == '\t');
            if (isSpace && lastWasSpace) continue;
            folded.push_back(isSpace ? ' ' : c);
            lastWasSpace = isSpace;
        }
        while (!folded.empty() && folded.front() == ' ') folded.erase(folded.begin());
        while (!folded.empty() && folded.back()  == ' ') folded.pop_back();
        text += std::format("reason = {}\n", folded);
    }
    // The closure's lines follow the header and precede the file list, as a
    // block of their own for the reason the header is one: they describe what
    // the tree's files are for, and sorting them into the file list would
    // interleave the two. Sorted and deduplicated, because a several-ABI tree
    // reads the same platform name once per leg.
    std::vector<std::string> needs;
    for (auto const& n : closure.needs) needs.push_back(render_closure_need(n));
    std::ranges::sort(needs);
    needs.erase(std::unique(needs.begin(), needs.end()), needs.end());
    for (auto const& l : needs) { text += l; text.push_back('\n'); }
    for (auto const& l : lines) { text += l; text.push_back('\n'); }

    auto out = stage_manifest_path(stagingRoot);
    // Compared before writing, for the reason `mcpp.build.stage` gives at
    // length: rewriting identical bytes moves the mtime, and a moved mtime on
    // an input is indistinguishable from a changed input. A pack that staged
    // the same tree twice would rebuild the distributable both times.
    if (std::ifstream in(out, std::ios::binary); in) {
        std::string old((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
        if (old == text) return true;
    }
    std::ofstream os(out, std::ios::binary | std::ios::trunc);
    if (!os) return false;
    os.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(os);
}

// The two names `--format` answers for without consulting the graph.
//
// Held here rather than in the CLI because two layers need the same list: the
// parser decides whether a value is a built-in or a dispatch, and the
// declaration check refuses a package that claims one of them. A package
// claiming `tar` would be silently unreachable, since the built-in wins.
constexpr std::array<std::string_view, 2> kBuiltinPackFormats{"tar", "dir"};

bool is_builtin_pack_format(std::string_view name) {
    return std::ranges::find(kBuiltinPackFormats, name) != kBuiltinPackFormats.end();
}

// #622 A5 — the wasm32-emscripten stem family, read from the directory the
// link wrote into rather than assumed from a fixed extension list.
//
// THE RULE: the launcher (`<name>.js`) is the executable; the family is every
// other `<name>.<anything>` the SAME link also wrote. `.wasm` is required —
// the link edge declares it as an implicit output (see ninja_backend.cppm),
// so a missing one names a build directory that does not match the graph,
// not a program that legitimately produced none. A `.data`
// (`--preload-file`), a `.worker.js` (`-pthread`) or a `.wasm.map` are
// optional and travel exactly when the link wrote them — this asks the link,
// never a list this module would have to keep in step with emcc's.
struct EmscriptenStemFamily {
    std::vector<std::string> siblings;    // sorted; excludes the launcher itself
    bool                     hasWasm = false;
};

EmscriptenStemFamily emscripten_stem_family(const std::filesystem::path& builtDir,
                                            std::string_view launcherName) {
    EmscriptenStemFamily out;
    const auto stem = std::filesystem::path(launcherName).stem().string();
    const auto prefix = stem + ".";
    std::error_code ec;
    for (auto const& entry : std::filesystem::directory_iterator(builtDir, ec)) {
        if (ec) break;
        auto name = entry.path().filename().string();
        if (name == launcherName || !name.starts_with(prefix)) continue;
        std::error_code fec;
        if (!entry.is_regular_file(fec)) continue;
        if (name == stem + ".wasm") out.hasWasm = true;
        out.siblings.push_back(std::move(name));
    }
    std::ranges::sort(out.siblings);
    return out;
}

} // namespace mcpp::pack
