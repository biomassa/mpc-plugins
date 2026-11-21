// rt_util.h - small real-time helpers shared by the blackdingus MPC plugins.
//
// SPDX-License-Identifier: MIT
//
//   MsgRing   single-producer / single-consumer queue of 3-byte MIDI messages (audio -> worker)
//   TimedRing the same with a delivery time (monotonic ns) for each message
//   SampleClock  steady mapping from the audio sample count to monotonic time
//   Wakeup    wakes the worker thread; post() is safe on the audio thread (sem_post on Linux)
//   listFiles .scl / .kbm files in the plugin's Content/Scales folder, sorted by name

#ifndef BLACKDINGUS_RT_UTIL_H
#define BLACKDINGUS_RT_UTIL_H

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <dirent.h>
#include <time.h>
#if defined(__linux__)
  #include <semaphore.h>
#endif

#include "mpc_plugin.h"

namespace rt {

class MsgRing
{
public:
    bool push(uint8_t a, uint8_t b, uint8_t c)
    {
        const uint32_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) >= kSize) return false;   // full: drop
        uint8_t* m = buf_[h & (kSize - 1)];
        m[0] = a; m[1] = b; m[2] = c;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    bool pop(uint8_t out[3])
    {
        const uint32_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        const uint8_t* m = buf_[t & (kSize - 1)];
        out[0] = m[0]; out[1] = m[1]; out[2] = m[2];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
private:
    static constexpr uint32_t kSize = 4096;   // power of two
    uint8_t buf_[kSize][3];
    std::atomic<uint32_t> head_{0}, tail_{0};
};

// Single-producer / single-consumer queue of 3-byte MIDI messages, each with a time (ns).
class TimedRing
{
public:
    bool push(uint8_t a, uint8_t b, uint8_t c, int64_t tNs)
    {
        const uint32_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) >= kSize) return false;   // full: drop
        Msg& m = buf_[h & (kSize - 1)];
        m.b[0] = a; m.b[1] = b; m.b[2] = c; m.t = tNs;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    bool pop(uint8_t out[3], int64_t& tNs)
    {
        const uint32_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        const Msg& m = buf_[t & (kSize - 1)];
        out[0] = m.b[0]; out[1] = m.b[1]; out[2] = m.b[2]; tNs = m.t;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
private:
    struct Msg { uint8_t b[3]; int64_t t; };
    static constexpr uint32_t kSize = 4096;   // power of two
    Msg buf_[kSize];
    std::atomic<uint32_t> head_{0}, tail_{0};
};

inline int64_t monoNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Steady time of an audio sample. The host computes blocks at uneven moments (measured on the
// MPC One G2: up to about 10 ms apart from the even grid), but never earlier than their real
// time allows. So the earliest observed (block time - block sample / rate) is the steady
// anchor; it may rise slowly (leak) so that it follows clock drift.
class SampleClock
{
public:
    // call once per block, at the start of process(), with the sample count of the block start
    void update(int64_t nowNs, uint64_t sample, double sr)
    {
        sr_ = sr > 0 ? sr : 44100.0;
        const int64_t cand = nowNs - (int64_t)((double)sample * 1e9 / sr_);
        if (!valid_ || cand < anchor_) { anchor_ = cand; valid_ = true; lastNs_ = nowNs; return; }
        anchor_ += (int64_t)((double)(nowNs - lastNs_) * kLeak);   // follows drift (<= 500 ppm)
        lastNs_ = nowNs;
        if (cand < anchor_) anchor_ = cand;
    }
    int64_t timeOf(uint64_t sample) const { return anchor_ + (int64_t)((double)sample * 1e9 / sr_); }
    bool valid() const { return valid_; }
private:
    static constexpr double kLeak = 0.0005;
    int64_t anchor_ = 0, lastNs_ = 0;
    double sr_ = 44100.0;
    bool valid_ = false;
};

class Wakeup
{
public:
#if defined(__linux__)
    Wakeup() { sem_init(&sem_, 0, 0); }
    ~Wakeup() { sem_destroy(&sem_); }
    void post() { sem_post(&sem_); }
    void wait() { while (sem_wait(&sem_) != 0) {} }
    void waitMs(int ms)
    {
        timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (long)ms * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
        sem_timedwait(&sem_, &ts);
    }
private:
    sem_t sem_;
#else
    // macOS (host simulator and tests only): poll
    void post() { flag_.store(true); }
    void wait() { waitMs(2); }
    void waitMs(int ms)
    {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait_for(l, std::chrono::milliseconds(ms < 1 ? 1 : (ms > 2 ? 2 : ms)), [this] { return flag_.load(); });
        flag_.store(false);
    }
private:
    std::mutex m_;
    std::condition_variable cv_;
    std::atomic<bool> flag_{false};
#endif
};

struct FileEntry { std::string name, path; };

inline std::vector<FileEntry> listFiles(const char* ext)
{
    std::vector<FileEntry> out;
    const std::string dir = mpc::contentPath("Scales");
    if (DIR* d = opendir(dir.c_str()))
    {
        while (dirent* e = readdir(d))
        {
            std::string n = e->d_name;
            if (n.empty() || n[0] == '.' || n.size() <= 4) continue;
            std::string x = n.substr(n.size() - 4);
            for (char& c : x) c = (char)std::tolower((unsigned char)c);
            if (x != ext) continue;
            out.push_back({n.substr(0, n.size() - 4), dir + "/" + n});
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end(), [](const FileEntry& a, const FileEntry& b) {
        std::string x = a.name, y = b.name;
        for (char& c : x) c = (char)std::tolower((unsigned char)c);
        for (char& c : y) c = (char)std::tolower((unsigned char)c);
        return x < y;
    });
    return out;
}

} // namespace rt

#endif
