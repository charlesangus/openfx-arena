#pragma once

#include <cassert>
#include <Magick++.h>
#ifdef IsNaN
#undef IsNaN
#endif

#include "ofxsImageEffect.h"

// A one-channel (Alpha) stream is either a coverage matte, for geometric
// effects whose result is the matte itself, or a grayscale picture, for
// content effects that filter pixel values.
enum MagickAlphaMode {
    eMagickAlphaMatte,
    eMagickAlphaGray
};

// Fills image from a float buffer of width*height*nComponents values.
// Alpha sources are read as intensity; in matte mode the intensity is then
// copied into the alpha channel so I == A.
static inline void magickReadPixels(Magick::Image &image,
                                    int width,
                                    int height,
                                    OFX::PixelComponentEnum components,
                                    MagickAlphaMode mode,
                                    const void *data)
{
    if (components == OFX::ePixelComponentAlpha) {
        image.read(width, height, "I", Magick::FloatPixel, (void*)data);
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

// Exports the processed image straight into a one-component float buffer;
// the buffer must hold w*h floats.
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
