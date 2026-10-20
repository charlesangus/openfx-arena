#pragma once

#include <cassert>
#include <mutex>
#include <Magick++.h>
#ifdef IsNaN
#undef IsNaN
#endif

#include "ofxsImageEffect.h"

// ImageMagick's random seed is process-global, so every effect that seeds it must hold this
// across the seed and the calls that draw from it. The function-local static is shared by all
// translation units in the plug-in binary because the function is inline.
static inline std::mutex &magickRandomMutex()
{
    static std::mutex mutex;
    return mutex;
}

enum MagickAlphaMode {
    eMagickAlphaMatte,
    eMagickAlphaGray
};

static inline void magickReadPixels(Magick::Image &image,
                                    int width,
                                    int height,
                                    OFX::PixelComponentEnum components,
                                    MagickAlphaMode mode,
                                    const void *data)
{
    if (components == OFX::ePixelComponentAlpha) {
        image.read(width, height, "I", Magick::FloatPixel, (void*)data);
        // Geometric effects transform and composite coverage through ImageMagick's alpha
        // channel, and their result is read back from it, so the matte must live there too.
        if (mode == eMagickAlphaMatte) {
#if MagickLibVersion >= 0x700
            image.alphaChannel(Magick::CopyAlphaChannel);
#else
            image.alphaChannel(Magick::CopyOpacityChannel);
#endif
        }
    } else {
        image.read(width, height, "RGBA", Magick::FloatPixel, (void*)data);
    }
}

static inline void magickWriteAlphaPixels(Magick::Image &image,
                                          int x,
                                          int y,
                                          int w,
                                          int h,
                                          MagickAlphaMode mode,
                                          void *data)
{
    image.write(x, y, w, h, mode == eMagickAlphaMatte ? "A" : "I", Magick::FloatPixel, data);
}
