// The codecs of mcpp.pack.compress: zlib (gzip, raw deflate), zstd and liblzma
// (xz), each through its one-shot interface. The only unit that includes the
// libraries' headers.
module;
#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

module mcpp.pack.compress;

import std;

namespace mcpp::pack::compress::codec {

namespace {

// zlib's deflate with `windowBits`: 15 + 16 is a gzip member, -15 is raw
// DEFLATE. A gzip member's header is stated rather than left to zlib, whose own
// header names the system zlib was built for (3 on Unix, 10 or 11 on Windows):
// no name, no time, the system 255 (unknown), so the same tree is the same
// bytes on every host.
std::expected<std::string, std::string> zlib_deflate(std::string_view in, int level,
                                                     int windowBits) {
    z_stream s{};
    const int lvl = level < 0 ? Z_DEFAULT_COMPRESSION : level;
    if (deflateInit2(&s, lvl, Z_DEFLATED, windowBits, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return std::unexpected(std::string("zlib: deflateInit2 failed"));
    gz_header header{};
    header.os = 255;
    if (windowBits > 15 && deflateSetHeader(&s, &header) != Z_OK) {
        deflateEnd(&s);
        return std::unexpected(std::string("zlib: deflateSetHeader failed"));
    }
    std::string out;
    out.resize(deflateBound(&s, static_cast<uLong>(in.size())) + 64);
    s.next_in   = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    s.avail_in  = static_cast<uInt>(in.size());
    s.next_out  = reinterpret_cast<Bytef*>(out.data());
    s.avail_out = static_cast<uInt>(out.size());
    const int rc = ::deflate(&s, Z_FINISH);
    const auto written = s.total_out;
    deflateEnd(&s);
    if (rc != Z_STREAM_END) return std::unexpected(std::format("zlib: deflate returned {}", rc));
    out.resize(written);
    return out;
}

}  // namespace

std::expected<std::string, std::string> gzip(std::string_view in, int level) {
    if (in.size() > std::numeric_limits<uInt>::max())
        return std::unexpected(std::string("gzip: an input over 4 GiB"));
    return zlib_deflate(in, level, 15 + 16);
}

std::expected<std::string, std::string> deflate(std::string_view in, int level) {
    if (in.size() > std::numeric_limits<uInt>::max())
        return std::unexpected(std::string("deflate: an input over 4 GiB"));
    return zlib_deflate(in, level, -15);
}

std::expected<std::string, std::string> zstd(std::string_view in, int level) {
    ZSTD_CCtx* cx = ZSTD_createCCtx();
    if (!cx) return std::unexpected(std::string("zstd: cannot create a context"));
    ZSTD_CCtx_setParameter(cx, ZSTD_c_compressionLevel, level < 0 ? 19 : level);
    ZSTD_CCtx_setParameter(cx, ZSTD_c_checksumFlag, 1);
    ZSTD_CCtx_setParameter(cx, ZSTD_c_contentSizeFlag, 1);
    std::string out;
    out.resize(ZSTD_compressBound(in.size()));
    const std::size_t n = ZSTD_compress2(cx, out.data(), out.size(), in.data(), in.size());
    ZSTD_freeCCtx(cx);
    if (ZSTD_isError(n)) return std::unexpected(std::format("zstd: {}", ZSTD_getErrorName(n)));
    out.resize(n);
    return out;
}

std::expected<std::string, std::string> unzstd(std::string_view frame) {
    const auto size = ZSTD_getFrameContentSize(frame.data(), frame.size());
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN)
        return std::unexpected(std::string("zstd: the frame does not state its content size"));
    std::string out;
    out.resize(static_cast<std::size_t>(size));
    const std::size_t n = ZSTD_decompress(out.data(), out.size(), frame.data(), frame.size());
    if (ZSTD_isError(n)) return std::unexpected(std::format("zstd: {}", ZSTD_getErrorName(n)));
    out.resize(n);
    return out;
}

std::expected<std::string, std::string> xz(std::string_view in, int level) {
    std::string out;
    out.resize(lzma_stream_buffer_bound(in.size()));
    std::size_t pos = 0;
    const lzma_ret rc = lzma_easy_buffer_encode(
        static_cast<std::uint32_t>(level < 0 ? 6 : level), LZMA_CHECK_CRC64, nullptr,
        reinterpret_cast<const std::uint8_t*>(in.data()), in.size(),
        reinterpret_cast<std::uint8_t*>(out.data()), &pos, out.size());
    if (rc != LZMA_OK) return std::unexpected(std::format("xz: lzma_easy_buffer_encode returned {}",
                                                          static_cast<int>(rc)));
    out.resize(pos);
    return out;
}

namespace {

// zlib's inflate until the stream ends: a gzip member (windowBits 15 + 32
// detects the header), its output growing, or raw DEFLATE (-15) of a stated
// `size`, its output never more than that.
std::expected<std::string, std::string> zlib_inflate(std::string_view in, int windowBits,
                                                     std::optional<std::size_t> size) {
    if (in.size() > std::numeric_limits<uInt>::max())
        return std::unexpected(std::string("zlib: an input over 4 GiB"));
    // DEFLATE expands at most 1032 times (a run of one byte): a stated size
    // beyond that is a lie, refused before anything is allocated, and a stream
    // that grows past it is refused as it grows.
    const std::size_t most = in.size() * 1032 + 64;
    if (size && *size > most)
        return std::unexpected(std::format("zlib: {} bytes cannot hold the {} it states", in.size(), *size));
    z_stream s{};
    if (inflateInit2(&s, windowBits) != Z_OK)
        return std::unexpected(std::string("zlib: inflateInit2 failed"));
    std::string out;
    // One byte past a stated size: a stream that fills it holds more than it
    // said.
    out.resize(size ? *size + 1 : in.size() * 4 + 1024);
    s.next_in  = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    s.avail_in = static_cast<uInt>(in.size());
    int rc = Z_OK;
    while (rc == Z_OK) {
        if (s.total_out >= out.size()) {
            if (size) {
                inflateEnd(&s);
                return std::unexpected(std::format("zlib: the stream holds more than its {} bytes",
                                                   *size));
            }
            if (out.size() > most) {
                inflateEnd(&s);
                return std::unexpected(std::string("zlib: the stream expands beyond what DEFLATE can"));
            }
            out.resize(std::min(out.size() * 2, most + 1));
        }
        s.next_out  = reinterpret_cast<Bytef*>(out.data() + s.total_out);
        s.avail_out = static_cast<uInt>(std::min<std::size_t>(out.size() - s.total_out,
                                                              std::numeric_limits<uInt>::max()));
        rc = ::inflate(&s, Z_NO_FLUSH);
    }
    const auto written = s.total_out;
    inflateEnd(&s);
    if (rc != Z_STREAM_END) return std::unexpected(std::format("zlib: inflate returned {}", rc));
    out.resize(written);
    return out;
}

}  // namespace

std::expected<std::string, std::string> gunzip(std::string_view in) {
    return zlib_inflate(in, 15 + 32, std::nullopt);
}

std::expected<std::string, std::string> inflate(std::string_view in, std::size_t size) {
    return zlib_inflate(in, -15, size);
}

std::expected<std::string, std::string> unxz(std::string_view in) {
    lzma_stream s = LZMA_STREAM_INIT;
    if (lzma_stream_decoder(&s, UINT64_MAX, 0) != LZMA_OK)
        return std::unexpected(std::string("xz: cannot start a decoder"));
    std::string out;
    out.resize(in.size() * 4 + 1024);
    s.next_in  = reinterpret_cast<const std::uint8_t*>(in.data());
    s.avail_in = in.size();
    lzma_ret rc = LZMA_OK;
    while (rc == LZMA_OK) {
        if (s.total_out >= out.size()) out.resize(out.size() * 2);
        s.next_out  = reinterpret_cast<std::uint8_t*>(out.data() + s.total_out);
        s.avail_out = out.size() - s.total_out;
        rc = lzma_code(&s, LZMA_FINISH);
    }
    const auto written = s.total_out;
    lzma_end(&s);
    if (rc != LZMA_STREAM_END)
        return std::unexpected(std::format("xz: lzma_code returned {}", static_cast<int>(rc)));
    out.resize(written);
    return out;
}

}  // namespace mcpp::pack::compress::codec
