// rt_util.h - small real-time helpers shared by the blackdingus MPC plugins.
//
// SPDX-License-Identifier: MIT
//
//   MsgRing   single-producer / single-consumer queue of 3-byte MIDI messages (audio -> worker)
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
