// mcpp.pack.compress — the compression of a package's container (SPEC-014 §5).
//
// `mcpp pack` writes an archive in one of three compressions or none, chosen by
// `--compress <algorithm>[:<level>]` or `[pack] compress`: gzip (the default of
// `tar`, as every earlier release wrote it), zstd and xz. A zip's members are
// deflated or stored; a single file's payload is zstd. The codecs are the
// reference libraries (zlib, zstd, liblzma), dependencies of mcpp, so a
// package is written the same way on every host and nothing is looked for on
// PATH.
//
// Deterministic by construction: a gzip member carries no name and no time, a
// zstd frame states its content size and its checksum, an xz stream checks
// with CRC64. Two packs of one tree produce the same bytes.

export module mcpp.pack.compress;

import std;

export namespace mcpp::pack::compress {

enum class Algorithm { None, Gzip, Zstd, Xz };

struct Choice {
    Algorithm algorithm = Algorithm::Gzip;
    int       level     = 0;      // 0: the codec's own default
    bool      stated    = false;  // written by the user (flag or manifest)
};

// `none`, `gzip`, `zstd`, `xz`, each optionally `:<level>` (gzip 1-9, zstd
// 1-22, xz 0-9).
std::expected<Choice, std::string> parse(std::string_view text);

std::string_view name(Algorithm a);

// The extension of a tar archive under `a`: `.tar.gz`, `.tar.zst`, `.tar.xz`,
// `.tar`.
std::string_view tar_extension(Algorithm a);

// The whole of `in`, compressed to `out` under `c` (a gzip member, a zstd
// frame, an xz stream; a copy for None).
std::expected<void, std::string> file(const std::filesystem::path& in,
                                      const std::filesystem::path& out, Choice c);

// `in`, compressed under `c`.
std::expected<std::string, std::string> bytes(std::string_view in, Choice c);

// `in` as raw DEFLATE (a zip member's method 8), at `level` (0: default).
std::expected<std::string, std::string> deflate_raw(std::string_view in, int level);

// A zstd frame's content, for the readers of what mcpp wrote (the tests, and
// `--merge`, which reads a package it wrote).
std::expected<std::string, std::string> zstd_decompress(std::string_view frame);

// What `bytes` compressed under `a`: a gzip member, a zstd frame, an xz
// stream, or the bytes themselves. `--merge` reads the archives it merges so.
std::expected<std::string, std::string> decompress(std::string_view in, Algorithm a);

// A raw DEFLATE stream's content, `size` bytes long (a zip member's method 8).
std::expected<std::string, std::string> inflate_raw(std::string_view in, std::size_t size);

}  // namespace mcpp::pack::compress

// The codecs themselves, in compress_codecs.cpp: the only unit that sees the
// libraries' headers.
namespace mcpp::pack::compress::codec {
std::expected<std::string, std::string> gzip(std::string_view in, int level);
std::expected<std::string, std::string> zstd(std::string_view in, int level);
std::expected<std::string, std::string> xz(std::string_view in, int level);
std::expected<std::string, std::string> deflate(std::string_view in, int level);
std::expected<std::string, std::string> unzstd(std::string_view frame);
std::expected<std::string, std::string> gunzip(std::string_view in);
std::expected<std::string, std::string> unxz(std::string_view in);
std::expected<std::string, std::string> inflate(std::string_view in, std::size_t size);
}  // namespace mcpp::pack::compress::codec

namespace mcpp::pack::compress {

std::string_view name(Algorithm a) {
    switch (a) {
        case Algorithm::None: return "none";
        case Algorithm::Gzip: return "gzip";
        case Algorithm::Zstd: return "zstd";
        case Algorithm::Xz:   return "xz";
    }
    return "gzip";
}

std::string_view tar_extension(Algorithm a) {
    switch (a) {
        case Algorithm::None: return ".tar";
        case Algorithm::Gzip: return ".tar.gz";
        case Algorithm::Zstd: return ".tar.zst";
        case Algorithm::Xz:   return ".tar.xz";
    }
    return ".tar.gz";
}

std::expected<Choice, std::string> parse(std::string_view text) {
    Choice c;
    c.stated = true;
    std::string_view alg = text;
    std::string_view lvl;
    if (auto colon = text.find(':'); colon != std::string_view::npos) {
        alg = text.substr(0, colon);
        lvl = text.substr(colon + 1);
    }
    struct Row { std::string_view word; Algorithm a; int lo, hi; };
    constexpr Row rows[] = {
        {"none", Algorithm::None, 0, 0},
        {"gzip", Algorithm::Gzip, 1, 9},
        {"zstd", Algorithm::Zstd, 1, 22},
        {"xz",   Algorithm::Xz,   0, 9},
    };
    const Row* row = nullptr;
    for (auto const& r : rows) if (r.word == alg) row = &r;
    if (!row)
        return std::unexpected(std::format(
            "compression '{}' is not one of none | gzip | zstd | xz (each with an optional "
            ":<level>)", alg));
    c.algorithm = row->a;
    if (!lvl.empty()) {
        if (row->a == Algorithm::None)
            return std::unexpected(std::string("compression 'none' takes no level"));
        int v = 0;
        auto [p, ec] = std::from_chars(lvl.data(), lvl.data() + lvl.size(), v);
        if (ec != std::errc{} || p != lvl.data() + lvl.size() || v < row->lo || v > row->hi)
            return std::unexpected(std::format(
                "the level of {} is {} to {}; '{}' is not", row->word, row->lo, row->hi, lvl));
        c.level = v;
    }
    return c;
}

std::expected<std::string, std::string> bytes(std::string_view in, Choice c) {
    switch (c.algorithm) {
        case Algorithm::None: return std::string(in);
        case Algorithm::Gzip: return codec::gzip(in, c.level);
        case Algorithm::Zstd: return codec::zstd(in, c.level);
        case Algorithm::Xz:   return codec::xz(in, c.level);
    }
    return std::string(in);
}

std::expected<void, std::string> file(const std::filesystem::path& in,
                                      const std::filesystem::path& out, Choice c) {
    std::string data;
    {
        std::ifstream is(in, std::ios::binary);
        if (!is) return std::unexpected(std::format("cannot read '{}'", in.string()));
        data.assign(std::istreambuf_iterator<char>(is), {});
    }
    auto packed = bytes(data, c);
    if (!packed) return std::unexpected(std::format("{}: {}", in.string(), packed.error()));
    std::error_code ec;
    if (out.has_parent_path()) std::filesystem::create_directories(out.parent_path(), ec);
    std::ofstream os(out, std::ios::binary | std::ios::trunc);
    if (!os) return std::unexpected(std::format("cannot write '{}'", out.string()));
    os.write(packed->data(), static_cast<std::streamsize>(packed->size()));
    if (!os) return std::unexpected(std::format("failed writing '{}'", out.string()));
    return {};
}

std::expected<std::string, std::string> deflate_raw(std::string_view in, int level) {
    return codec::deflate(in, level);
}

std::expected<std::string, std::string> zstd_decompress(std::string_view frame) {
    return codec::unzstd(frame);
}

std::expected<std::string, std::string> decompress(std::string_view in, Algorithm a) {
    switch (a) {
        case Algorithm::None: return std::string(in);
        case Algorithm::Gzip: return codec::gunzip(in);
        case Algorithm::Zstd: return codec::unzstd(in);
        case Algorithm::Xz:   return codec::unxz(in);
    }
    return std::string(in);
}

std::expected<std::string, std::string> inflate_raw(std::string_view in, std::size_t size) {
    return codec::inflate(in, size);
}

}  // namespace mcpp::pack::compress
