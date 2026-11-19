// mcpp.pm.descriptor — the manifest a descriptor states (SPEC-001 §3.5).
//
// A descriptor's `mcpp` field has two forms. The pointer form,
// `mcpp = "<glob>"`, says the payload carries its manifest: the glob is
// relative to the version directory and matches exactly one `mcpp.toml`, whose
// directory is the package's root; with no field the two conventional places
// are tried. The inline form, `mcpp = { ... }`, says the payload carries none
// and the table is the manifest.
//
// Six places read the field, each with a branch of its own on its kind, and
// the writer (`mcpp publish`) wrote a table that named a manifest inside the
// payload: a third form none of the six read (mcpp#960). The forms are decided
// here once, so a reader asks for the manifest and its root, and a reader that
// needs only one of them cannot disagree with another about which form it met.
export module mcpp.pm.descriptor;

import std;
import mcpp.manifest;
import mcpp.toolchain.platform;

export namespace mcpp::pm {

struct DescriptorManifest {
    enum class Form { Pointer, Inline };
    Form form = Form::Pointer;
    // The package's root: the directory of the manifest the pointer names, or
    // the version directory of an inline descriptor.
    std::filesystem::path packageRoot;
    // Pointer: the manifest inside the payload. The caller loads it, since
    // what a manifest inherits depends on where the caller found the package.
    std::filesystem::path manifestPath;
    // Inline: the table, read as a manifest for `platform`.
    std::optional<mcpp::manifest::Manifest> manifest;
};

// Whether the field is the inline form. The one question a reader may ask
// before a payload exists: an inline descriptor states its dependencies in the
// index, and a pointer one states them in a manifest not yet fetched.
bool descriptor_is_inline(std::string_view lua);

// Whether the payload at `versionDir` holds what the descriptor names: asked
// of an installed payload before it is used, since one that does not goes back
// to the provider. A pointer is answered by any match --- two matches are a
// descriptor's error, reported when the manifest is read, and reinstalling
// does not repair them.
bool descriptor_payload_present(std::string_view lua, const std::filesystem::path& versionDir,
                                std::string_view name, std::string_view version,
                                const mcpp::toolchain::platform::PlatformKey& platform);

// The manifest `lua` states for the payload installed at `versionDir`. The
// error names what was looked for and where, without the package's name: the
// caller says whose descriptor it is.
std::expected<DescriptorManifest, std::string>
descriptor_manifest(std::string_view lua, const std::filesystem::path& versionDir,
                    std::string_view name, std::string_view version,
                    const mcpp::toolchain::platform::PlatformKey& platform);

}  // namespace mcpp::pm
