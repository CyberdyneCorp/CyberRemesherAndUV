#pragma once

#include <memory>
#include <string>

#include "cyber/imageio/image.hpp"

// Writing a baked map ONE HORIZONTAL BAND AT A TIME (mesh-io spec, "Baked maps
// are writable one band at a time").
//
// The one-shot encoders take the whole image as a buffer, which is exactly what
// a 16384-square map cannot be: three gigabytes of floats before a byte reaches
// the disk. A regioned bake (surface-baking, "Regioned baking with a bounded
// working set") produces the map band by band and needs a destination shaped
// the same way.
//
// The file is BYTE-IDENTICAL to the one-shot writer's (saveImage) for the same
// pixels, whatever the band sizes -- a property the tests pin, since the two are
// separate encoders.
namespace cyber::imageio {

class ImageStreamWriter {
public:
    ImageStreamWriter() = default;
    ImageStreamWriter(const ImageStreamWriter&) = delete;
    ImageStreamWriter& operator=(const ImageStreamWriter&) = delete;
    ImageStreamWriter(ImageStreamWriter&&) = delete;
    ImageStreamWriter& operator=(ImageStreamWriter&&) = delete;
    virtual ~ImageStreamWriter() = default;

    // Opens `path` for an image of exactly `width` x `height` x `channels`
    // float pixels. PNG accepts 1 (expanded to grey RGB, as saveImage does), 3
    // and 4 channels and tone-maps [0,1] to 8 bits; EXR accepts 1, 3 and 4 and
    // keeps the floats. Returns null -- without creating the file -- when the
    // arguments are unusable, and null when the file cannot be opened.
    [[nodiscard]] static std::unique_ptr<ImageStreamWriter> open(const std::string& path, int width,
                                                                 int height, int channels,
                                                                 ImageFormat format);

    // Appends `count` rows (`count * width * channels` floats, row-major),
    // continuing where the previous call stopped. False on an I/O failure or
    // when the rows would run past `height` -- reported AT THE BAND that failed.
    [[nodiscard]] virtual bool writeRows(const float* rows, int count) = 0;

    // Completes the file. False when fewer than `height` rows were written or
    // on an I/O failure; a writer destroyed without finish() leaves an
    // incomplete file, exactly as an abandoned bake leaves no map.
    [[nodiscard]] virtual bool finish() = 0;
};

}  // namespace cyber::imageio
