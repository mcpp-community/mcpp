// mcpp.freestanding.runner — how `mcpp run` executes an artifact that cannot
// run on this machine.
//
// A bare-metal image is not executable here by construction: wrong ISA, no
// loader, and it expects to own the whole address space. Something has to
// stand between `mcpp run` and the artifact, and WHICH something is a board
// fact — the emulator's machine model, its firmware mode, whether output
// arrives over a UART or through semihosting. Two boards on the same ISA need
// different argv:
//
//   qemu-system-riscv64 -machine virt -bios default    -kernel <img>   (OpenSBI)
//   qemu-system-riscv64 -machine virt -bios none -semihosting -kernel <img>
//
// So mcpp carries a TEMPLATE and never a default. There is deliberately no
// built-in "if riscv64 then qemu" rule: the moment the engine guesses, a board
// that needs the other spelling has to fight it, and `mcpp run` silently doing
// the wrong thing is worse than saying it does not know.

export module mcpp.freestanding.runner;

import std;

export namespace mcpp::freestanding {

// Where the artifact goes in the argv.
//
// `{}` is substituted when present; otherwise the path is appended. Appending
// is the common shape (`-kernel <img>` ends the line) and making it the
// default keeps the simple case free of punctuation, while `{}` covers the
// emulator that wants the image in the middle.
inline constexpr std::string_view kArtifactPlaceholder = "{}";

inline std::vector<std::string> expand(std::span<const std::string> tmpl,
                                       const std::filesystem::path& artifact)
{
    std::vector<std::string> argv;
    argv.reserve(tmpl.size() + 1);
    bool substituted = false;
    for (auto const& tok : tmpl) {
        auto pos = tok.find(kArtifactPlaceholder);
        if (pos == std::string::npos) { argv.push_back(tok); continue; }
        std::string out = tok;
        out.replace(pos, kArtifactPlaceholder.size(), artifact.string());
        argv.push_back(std::move(out));
        substituted = true;
    }
    if (!substituted) argv.push_back(artifact.string());
    return argv;
}

// The diagnostic for "this target needs a runner and none is configured".
//
// Its own function so the wording is asserted once. It names the exact key and
// shows a working value, because the user reading it has just been told their
// build succeeded and their run did not — the gap between those two is the
// whole content of the message.
inline std::string no_runner_message(std::string_view triple) {
    return std::format(
        "no runner is configured for '{}' — a freestanding artifact cannot "
        "execute on this machine.\n"
        "       Declare how to run it:\n"
        "\n"
        "           [target.{}]\n"
        "           runner = [\"qemu-system-riscv64\", \"-machine\", \"virt\",\n"
        "                     \"-nographic\", \"-no-reboot\", \"-bios\", "
        "\"default\", \"-kernel\"]\n"
        "\n"
        "       The artifact path is appended, or substituted for `{{}}` if the "
        "template contains it.\n"
        "       A board-support package normally supplies this so you do not "
        "have to.",
        triple, triple);
}

} // namespace mcpp::freestanding
