#include "cyber/imageio/stream.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <vector>

#include "detail.hpp"
#include "exr_header.hpp"

namespace cyber::imageio {

namespace {

using detail::Bytes;

// An output file held open across many appends.
class OutFile {
public:
    explicit OutFile(const std::string& path) : file_(path, std::ios::binary | std::ios::trunc) {}

    [[nodiscard]] bool ok() const { return static_cast<bool>(file_); }

    bool write(const std::uint8_t* data, std::size_t size) {
        if (size != 0) {
            file_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        }
        return static_cast<bool>(file_);
    }
    bool write(const Bytes& data) { return write(data.data(), data.size()); }

    // close() flushes and latches failbit if that flush fails, so a full disk is
    // reported here rather than lost in the destructor.
    bool close() {
        file_.close();
        return static_cast<bool>(file_);
    }

private:
    std::ofstream file_;
};

// Running Adler-32: the zlib stream inside a PNG is checksummed over the WHOLE
// image while a band writer only ever holds one band of it. Same arithmetic as
// detail::adler32.
class Adler32 {
public:
    void update(const std::uint8_t* data, std::size_t len) {
        constexpr std::uint32_t kMod = 65521u;
        for (std::size_t i = 0; i < len; ++i) {
            a_ = (a_ + data[i]) % kMod;
            b_ = (b_ + a_) % kMod;
        }
    }
    [[nodiscard]] std::uint32_t value() const { return (b_ << 16) | a_; }

private:
    std::uint32_t a_ = 1u;
    std::uint32_t b_ = 0u;
};

constexpr std::size_t kMaxBlock = 65535;

// The exact size of the zlib stream `raw` bytes encode to as STORED blocks: the
// two header bytes, five per block, the bytes, the Adler-32. Known before a row
// is written, which is what lets the whole stream live in ONE IDAT chunk whose
// length precedes it -- the shape the one-shot writer produces.
std::size_t zlibStoredSize(std::size_t raw) {
    const std::size_t blocks = raw == 0 ? 1 : (raw + kMaxBlock - 1) / kMaxBlock;
    return 2 + blocks * 5 + raw + 4;
}

// 8-bit PNG, RGB or RGBA. The deflate block split is over the RUNNING raw
// stream, not over the band: bytes are held back until strictly more than one
// block's worth is pending, so a raw stream that is an exact multiple of the
// block size keeps its last full block for finish() to mark FINAL -- which is
// what the one-shot encoder does.
class PngStream final : public ImageStreamWriter {
public:
    PngStream(const std::string& path, int width, int height, int channels)
        : file_(path),
          width_(static_cast<std::size_t>(width)),
          height_(static_cast<std::size_t>(height)),
          sourceChannels_(static_cast<std::size_t>(channels)),
          fileChannels_(channels == 1 ? 3u : static_cast<std::size_t>(channels)) {
        ok_ = file_.ok() && file_.write(prologue(width, height));
    }

    [[nodiscard]] bool ok() const { return ok_; }

    // Whether the IDAT length fits the chunk's 32-bit field.
    [[nodiscard]] static bool fits(int width, int height, int channels) {
        const auto w = static_cast<std::size_t>(width);
        const auto ch = static_cast<std::size_t>(channels == 1 ? 3 : channels);
        const std::size_t limit = std::numeric_limits<std::uint32_t>::max() / 2;
        return w <= limit / ch && (w * ch + 1) <= limit / static_cast<std::size_t>(height) &&
               zlibStoredSize((w * ch + 1) * static_cast<std::size_t>(height)) <= limit;
    }

    bool writeRows(const float* rows, int count) override {
        if (!ok_ || finished_ || rows == nullptr || count < 0 ||
            static_cast<std::size_t>(count) > height_ - rowsWritten_) {
            return fail();
        }
        for (int y = 0; y < count; ++y) {
            appendRow(rows + static_cast<std::size_t>(y) * width_ * sourceChannels_);
            while (pending_.size() > kMaxBlock) {
                if (!emitBlock(kMaxBlock, false)) {
                    return false;
                }
            }
        }
        rowsWritten_ += static_cast<std::size_t>(count);
        return true;
    }

    bool finish() override {
        if (!ok_ || finished_ || rowsWritten_ != height_) {
            return fail();
        }
        finished_ = true;
        if (!emitBlock(pending_.size(), true)) {
            return false;
        }
        Bytes tail;
        detail::appendU32BE(tail, adler_.value());
        crc_ = detail::crc32Update(crc_, tail.data(), tail.size());
        detail::appendU32BE(tail, crc_ ^ 0xffffffffu);
        // IEND: zero length, type, CRC of the type.
        const std::array<std::uint8_t, 4> iend{'I', 'E', 'N', 'D'};
        detail::appendU32BE(tail, 0);
        tail.insert(tail.end(), iend.begin(), iend.end());
        detail::appendU32BE(tail, detail::crc32(iend.data(), iend.size()));
        return (file_.write(tail) && file_.close()) || fail();
    }

private:
    // Signature, IHDR, and the IDAT chunk's length, type and zlib header.
    Bytes prologue(int width, int height) {
        Bytes out{137, 80, 78, 71, 13, 10, 26, 10};
        Bytes ihdr{'I', 'H', 'D', 'R'};
        detail::appendU32BE(ihdr, static_cast<std::uint32_t>(width));
        detail::appendU32BE(ihdr, static_cast<std::uint32_t>(height));
        ihdr.push_back(8);                                                      // bit depth
        ihdr.push_back(static_cast<std::uint8_t>(fileChannels_ == 4 ? 6 : 2));  // color type
        ihdr.push_back(0);                                                      // compression
        ihdr.push_back(0);                                                      // filter
        ihdr.push_back(0);                                                      // interlace
        detail::appendU32BE(out, 13);
        detail::appendBytes(out, ihdr);
        detail::appendU32BE(out, detail::crc32(ihdr));

        const std::size_t raw = (width_ * fileChannels_ + 1) * height_;
        detail::appendU32BE(out, static_cast<std::uint32_t>(zlibStoredSize(raw)));
        const Bytes idat{'I', 'D', 'A', 'T', 0x78, 0x01};
        detail::appendBytes(out, idat);
        crc_ = detail::crc32Update(crc_, idat.data(), idat.size());
        return out;
    }

    // One row, filter byte first, tone-mapped exactly as writePngTonemapped
    // does, a 1-channel source written to all three channels.
    void appendRow(const float* row) {
        const std::size_t start = pending_.size();
        pending_.push_back(0);  // filter type: None
        for (std::size_t x = 0; x < width_; ++x) {
            for (std::size_t c = 0; c < fileChannels_; ++c) {
                const std::size_t source = sourceChannels_ == 1 ? 0 : c;
                const float clamped =
                    std::min(1.0f, std::max(0.0f, row[x * sourceChannels_ + source]));
                pending_.push_back(static_cast<std::uint8_t>(std::lround(clamped * 255.0f)));
            }
        }
        adler_.update(pending_.data() + start, pending_.size() - start);
    }

    // One STORED deflate block of the first `size` pending bytes.
    bool emitBlock(std::size_t size, bool final) {
        Bytes block;
        block.reserve(size + 5);
        block.push_back(final ? 0x01 : 0x00);
        const auto len = static_cast<std::uint16_t>(size);
        detail::appendU16LE(block, len);
        detail::appendU16LE(block,
                            static_cast<std::uint16_t>(~static_cast<unsigned>(len) & 0xffffu));
        block.insert(block.end(), pending_.begin(),
                     pending_.begin() + static_cast<std::ptrdiff_t>(size));
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(size));
        crc_ = detail::crc32Update(crc_, block.data(), block.size());
        return file_.write(block) || fail();
    }

    bool fail() {
        ok_ = false;
        return false;
    }

    OutFile file_;
    std::size_t width_;
    std::size_t height_;
    std::size_t sourceChannels_;
    std::size_t fileChannels_;
    bool ok_ = false;
    bool finished_ = false;
    std::size_t rowsWritten_ = 0;
    std::uint32_t crc_ = 0xffffffffu;  // running, over "IDAT" + the zlib stream
    Adler32 adler_;
    Bytes pending_;  // filtered raw bytes not yet emitted in a block
};

// Scanline OpenEXR, NO_COMPRESSION, 32-bit float. Every scanline block has the
// same size, so the offset table is exact before a pixel has been seen -- the
// whole reason this format streams.
class ExrStream final : public ImageStreamWriter {
public:
    ExrStream(const std::string& path, int width, int height, int channels)
        : file_(path),
          width_(static_cast<std::size_t>(width)),
          height_(static_cast<std::size_t>(height)),
          channels_(static_cast<std::size_t>(channels)),
          layout_(detail::exrChannelLayout(channels)) {
        Bytes out = detail::exrHeader(width, height, layout_);
        const std::size_t blockStride = 8 + channels_ * width_ * 4;
        const std::size_t firstBlock = out.size() + height_ * 8;
        for (std::size_t y = 0; y < height_; ++y) {
            detail::appendU64LE(out, static_cast<std::uint64_t>(firstBlock + y * blockStride));
        }
        ok_ = file_.ok() && file_.write(out);
    }

    [[nodiscard]] bool ok() const { return ok_; }

    bool writeRows(const float* rows, int count) override {
        if (!ok_ || finished_ || rows == nullptr || count < 0 ||
            static_cast<std::size_t>(count) > height_ - rowsWritten_) {
            return fail();
        }
        Bytes block;
        for (std::size_t y = 0; y < static_cast<std::size_t>(count); ++y) {
            block.clear();
            detail::appendI32LE(block, static_cast<std::int32_t>(rowsWritten_ + y));
            detail::appendU32LE(block, static_cast<std::uint32_t>(channels_ * width_ * 4));
            const float* row = rows + y * width_ * channels_;
            for (const detail::ExrChannel& c : layout_) {
                const auto source = static_cast<std::size_t>(c.source);
                for (std::size_t x = 0; x < width_; ++x) {
                    detail::appendFloatLE(block, row[x * channels_ + source]);
                }
            }
            if (!file_.write(block)) {
                return fail();
            }
        }
        rowsWritten_ += static_cast<std::size_t>(count);
        return true;
    }

    bool finish() override {
        if (!ok_ || finished_ || rowsWritten_ != height_) {
            return fail();
        }
        finished_ = true;
        return file_.close() || fail();
    }

private:
    bool fail() {
        ok_ = false;
        return false;
    }

    OutFile file_;
    std::size_t width_;
    std::size_t height_;
    std::size_t channels_;
    std::vector<detail::ExrChannel> layout_;
    bool ok_ = false;
    bool finished_ = false;
    std::size_t rowsWritten_ = 0;
};

}  // namespace

std::unique_ptr<ImageStreamWriter> ImageStreamWriter::open(const std::string& path, int width,
                                                           int height, int channels,
                                                           ImageFormat format) {
    // Validated BEFORE the file is opened, so an unusable request leaves no
    // empty file behind -- the one-shot writers' behaviour.
    if (width <= 0 || height <= 0 || (channels != 1 && channels != 3 && channels != 4)) {
        return nullptr;
    }
    if (format == ImageFormat::Exr) {
        auto writer = std::make_unique<ExrStream>(path, width, height, channels);
        return writer->ok() ? std::move(writer) : nullptr;
    }
    if (!PngStream::fits(width, height, channels)) {
        return nullptr;
    }
    auto writer = std::make_unique<PngStream>(path, width, height, channels);
    return writer->ok() ? std::move(writer) : nullptr;
}

}  // namespace cyber::imageio
