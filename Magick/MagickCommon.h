#pragma once

#include <cassert>
#include <condition_variable>
#include <deque>
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
// holds the limit fixed for its lifetime; scopes holding at once form a group sharing one limit.
// When nobody waits, a scope joins the current group if its limit matches or it asks for 0
// (meaning whatever limit is in force), and otherwise queues. Once anybody queues, every
// newcomer queues too, so a stream of arrivals cannot keep a group alive. When a group
// empties, the longest waiter's limit forms the next group, which admits every queued scope
// asking for that limit or for 0; a queued scope is therefore admitted no later than the group
// formed for it when it reaches the head of the queue. A scope nested on a thread that
// already holds one joins at once when its limit matches, since queueing behind itself would
// never end. Release the scope around host calls that can render upstream effects, or a nested
// Magick render on another thread can wait on it forever.
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

    MagickThreadLimitScope(const MagickThreadLimitScope &) = delete;
    MagickThreadLimitScope &operator=(const MagickThreadLimitScope &) = delete;

    void acquire()
    {
        if (_held) {
            return;
        }
        State &s = state();
        std::unique_lock<std::mutex> lock(s.mutex);
        const bool matches = _threads == 0 || s.limit == _threads;
        if (s.active == 0 && s.queue.empty()) {
            setLimit(s, _threads);
            ++s.active;
        } else if (matches && (s.queue.empty() || heldOnThisThread() != 0)) {
            ++s.active;
        } else {
            Waiter self;
            self.threads = _threads;
            s.queue.push_back(&self);
            if (s.active == 0) {
                formGroup(s);
            }
            s.changed.wait(lock, [&] { return self.admitted; });
        }
        ++heldOnThisThread();
        _held = true;
    }

    void release()
    {
        if (!_held) {
            return;
        }
        _held = false;
        --heldOnThisThread();
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (--s.active == 0 && !s.queue.empty()) {
            formGroup(s);
        }
    }

private:
    struct Waiter
    {
        unsigned int threads = 0;
        bool admitted = false;
    };

    struct State
    {
        std::mutex mutex;
        std::condition_variable changed;
        unsigned int limit = 0;
        unsigned int active = 0;
        std::deque<Waiter *> queue;
    };

    static State &state()
    {
        static State s;
        return s;
    }

    static unsigned int &heldOnThisThread()
    {
        static thread_local unsigned int held = 0;
        return held;
    }

    static void setLimit(State &s, unsigned int threads)
    {
        if (threads != 0 && s.limit != threads) {
            Magick::ResourceLimits::thread(threads);
            s.limit = threads;
        }
    }

    // Called with the mutex held, no holders and a non-empty queue. Admission is recorded in
    // each waiter rather than signalled, so a waiter that has not started waiting yet still
    // sees it.
    static void formGroup(State &s)
    {
        assert(s.active == 0 && !s.queue.empty());
        setLimit(s, s.queue.front()->threads);
        std::deque<Waiter *>::iterator kept = s.queue.begin();
        for (std::deque<Waiter *>::iterator it = s.queue.begin(); it != s.queue.end(); ++it) {
            if ((*it)->threads == 0 || (*it)->threads == s.limit) {
                (*it)->admitted = true;
                ++s.active;
            } else {
                *kept++ = *it;
            }
        }
        s.queue.erase(kept, s.queue.end());
        s.changed.notify_all();
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
