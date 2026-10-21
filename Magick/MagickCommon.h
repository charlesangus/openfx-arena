#pragma once

#include <cassert>
#include <condition_variable>
#include <mutex>
#include <Magick++.h>
#ifdef IsNaN
#undef IsNaN
#endif

#include "ofxsImageEffect.h"

// ImageMagick's random seed is process-global, so every effect that seeds it must hold this
// across the seed and the calls that draw from it. The function-local static is shared by all
// translation units in the plug-in binary because the function is inline.
inline std::mutex &magickRandomMutex()
{
    static std::mutex mutex;
    return mutex;
}

// ImageMagick's thread limit is process-global, and many of its operations read it once to size
// per-thread scratch arrays and again to pick the worker count or to free those arrays, so
// changing it while another call runs can index or free past the end of an array. A scope
// holds the limit fixed for its lifetime: scopes asking for the same limit run together, a
// scope asking for a different one waits until the current holders leave, and a scope asking
// for 0 runs under whatever limit is in force without ever waiting. Release the scope around
// host calls that can render upstream effects, or a nested Magick render can wait on it forever.
class MagickThreadLimitScope
{
public:
    explicit MagickThreadLimitScope(unsigned int threads)
        : _threads(threads)
        , _held(false)
    {
        acquire();
    }

    ~MagickThreadLimitScope()
    {
        release();
    }

    void acquire()
    {
        if (_held) {
            return;
        }
        State &s = state();
        std::unique_lock<std::mutex> lock(s.mutex);
        if (_threads != 0) {
            // A newcomer may not join a matching group while others wait, or a steady stream
            // of one limit would starve the other; it joins only a group formed after it arrived.
            if (s.active != 0 && !(s.limit == _threads && s.waiting == 0)) {
                const unsigned long arrival = s.epoch;
                ++s.waiting;
                s.changed.wait(lock, [&] {
                    return s.active == 0 || (s.limit == _threads && s.epoch != arrival);
                });
                --s.waiting;
            }
            if (s.active == 0) {
                ++s.epoch;
                if (s.limit != _threads) {
                    Magick::ResourceLimits::thread(_threads);
                    s.limit = _threads;
                }
                if (s.waiting != 0) {
                    s.changed.notify_all();
                }
            }
        }
        ++s.active;
        _held = true;
    }

    void release()
    {
        if (!_held) {
            return;
        }
        _held = false;
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (--s.active == 0) {
            s.changed.notify_all();
        }
    }

private:
    MagickThreadLimitScope(const MagickThreadLimitScope &);
    MagickThreadLimitScope &operator=(const MagickThreadLimitScope &);

    struct State
    {
        std::mutex mutex;
        std::condition_variable changed;
        unsigned int limit = 0;
        unsigned int active = 0;
        unsigned int waiting = 0;
        unsigned long epoch = 0;
    };

    static State &state()
    {
        static State s;
        return s;
    }

    unsigned int _threads;
    bool _held;
};

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
