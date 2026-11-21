// scalaSeq trace build (compile with -DSCALASEQ_TRACE=1; diagnostic only, never released).
//
// Records every call between the MPC host and this plugin, with its start time, duration and
// thread: processReplacing, the dispatcher opcodes, setParameter, getParameter, and the plugin's
// calls into the host (for example audioMasterAutomate from the display thread). A separate thread
// appends the records to /tmp/ssq_trace.bin every 200 ms. tools: tests/trace_report.py.
//
// The audio thread only writes a few numbers into a preallocated ring (no lock, no allocation,
// no I/O). The ring holds 2^19 records; the writer thread checks each record's sequence number,
// so records overwritten before they were written out are counted as lost.
#ifndef SCALASEQ_TRACE_H
#define SCALASEQ_TRACE_H

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <thread>
#include <time.h>
#if defined(__linux__)
  #include <sys/syscall.h>
  #include <unistd.h>
  #include <pthread.h>
#endif

namespace ssqtrace {

enum Kind : uint16_t { K_PROCESS = 0, K_DISPATCH = 1, K_SETPARAM = 2, K_GETPARAM = 3, K_HOSTCB = 4, K_MARK = 5 };

struct Rec
{
    uint64_t t;      // start, CLOCK_MONOTONIC ns
    uint32_t dur;    // ns
    int32_t tid;     // kernel thread id
    uint16_t kind;
    int16_t op;      // dispatcher / host opcode
    int32_t index;   // parameter index, or frames for process
    std::atomic<uint32_t> seq;
};

constexpr uint32_t kSize = 1u << 19;
inline Rec* ring() { static Rec* r = new Rec[kSize](); return r; }
inline std::atomic<uint32_t>& head() { static std::atomic<uint32_t> h{0}; return h; }

inline uint64_t nowNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

inline int32_t tid()
{
#if defined(__linux__)
    static thread_local int32_t t = (int32_t)syscall(SYS_gettid);
    return t;
#else
    return 0;
#endif
}

inline void put(Kind k, int op, int index, uint64_t t0)
{
    const uint64_t t1 = nowNs();
    const uint32_t i = head().fetch_add(1, std::memory_order_relaxed);
    Rec& r = ring()[i & (kSize - 1)];
    r.t = t0; r.dur = (uint32_t)std::min<uint64_t>(t1 - t0, 0xffffffffu); r.tid = tid();
    r.kind = (uint16_t)k; r.op = (int16_t)op; r.index = index;
    r.seq.store(i + 1, std::memory_order_release);
}

inline std::atomic<bool>& quit() { static std::atomic<bool> q{false}; return q; }
inline std::atomic<int>& instances() { static std::atomic<int> n{0}; return n; }
inline std::thread& writer() { static std::thread t; return t; }

// The writer thread: appends finished records (t, dur, tid, kind, op, index: 24 bytes each).
// It must stop before the host unloads the library (measured: a thread left running in the
// unloaded library crashed the MPC, signal 11 in "ssqTrace"), so the last effClose joins it.
inline void writerLoop()
{
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "ssqTrace");
#endif
    std::FILE* f = std::fopen("/tmp/ssq_trace.bin", "ab");
    if (!f) return;
    uint32_t next = 0, lost = 0;
    for (bool last = false; !last;)
    {
        for (int k = 0; k < 20 && !quit().load(); ++k) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        last = quit().load();
        const uint32_t h = head().load(std::memory_order_acquire);
        if (h - next > kSize) { lost += h - next - kSize; next = h - kSize; }
        while (next != h)
        {
            Rec& r = ring()[next & (kSize - 1)];
            if (r.seq.load(std::memory_order_acquire) != next + 1) break;   // still being written
            struct { uint64_t t; uint32_t dur; int32_t tid; uint16_t kind; int16_t op; int32_t index; } o
                {r.t, r.dur, r.tid, r.kind, r.op, r.index};
            std::fwrite(&o, sizeof o, 1, f);
            ++next;
        }
        if (lost)
        {
            struct { uint64_t t; uint32_t dur; int32_t tid; uint16_t kind; int16_t op; int32_t index; } o
                {nowNs(), lost, 0, 99, 0, 0};   // kind 99: records lost
            std::fwrite(&o, sizeof o, 1, f);
            lost = 0;
        }
        std::fflush(f);
    }
    std::fclose(f);
}

// Called for each new instance; the first one starts the writer.
inline void startWriter()
{
    if (instances().fetch_add(1) == 0)
    {
        ring();
        quit().store(false);
        writer() = std::thread(writerLoop);
    }
}
// Called after each effClose; the last one stops and joins the writer.
inline void stopWriter()
{
    if (instances().fetch_sub(1) == 1)
    {
        quit().store(true);
        if (writer().joinable()) writer().join();
    }
}

// ---- wrappers around the framework's entry points ----
inline AEffectDispatcherProc& origDispatcher() { static AEffectDispatcherProc p = nullptr; return p; }
inline audioMasterCallback& origHost() { static audioMasterCallback p = nullptr; return p; }

inline VstIntPtr dispatcherW(AEffect* e, VstInt32 op, VstInt32 index, VstIntPtr value, void* ptr, float opt)
{
    const uint64_t t0 = nowNs();
    const VstIntPtr r = mpc::detail::dispatcher(e, op, index, value, ptr, opt);
    put(K_DISPATCH, op, index, t0);
    if (op == effClose) stopWriter();
    return r;
}
inline void processW(AEffect* e, float** in, float** out, VstInt32 frames)
{
    const uint64_t t0 = nowNs();
    mpc::detail::processReplacing(e, in, out, frames);
    put(K_PROCESS, 0, frames, t0);
}
inline void setParamW(AEffect* e, VstInt32 index, float v)
{
    const uint64_t t0 = nowNs();
    mpc::detail::setParameter(e, index, v);
    put(K_SETPARAM, 0, index, t0);
}
inline float getParamW(AEffect* e, VstInt32 index)
{
    const uint64_t t0 = nowNs();
    const float r = mpc::detail::getParameter(e, index);
    put(K_GETPARAM, 0, index, t0);
    return r;
}
inline VstIntPtr hostW(AEffect* e, VstInt32 op, VstInt32 index, VstIntPtr value, void* ptr, float opt)
{
    const uint64_t t0 = nowNs();
    const VstIntPtr r = origHost() ? origHost()(e, op, index, value, ptr, opt) : 0;
    put(K_HOSTCB, op, index, t0);
    return r;
}

inline AEffect* wrap(AEffect* e)
{
    if (!e) return e;
    startWriter();
    mpc::Plugin* p = static_cast<mpc::Plugin*>(e->object);
    origHost() = p->host_;
    p->host_ = &hostW;
    e->dispatcher = &dispatcherW;
    e->processReplacing = &processW;
    e->setParameter = &setParamW;
    e->getParameter = &getParamW;
    return e;
}

} // namespace ssqtrace

#define SSQ_TRACE_ENTRY(PluginClass)                                                         \
    extern "C" MPC_EXPORT AEffect* VSTPluginMain(audioMasterCallback host)                   \
    {                                                                                        \
        if (host != nullptr && host(nullptr, audioMasterVersion, 0, 0, nullptr, 0.0f) == 0)  \
            return nullptr;                                                                  \
        return ssqtrace::wrap(mpc::detail::createEffect(new PluginClass(), host));           \
    }

#endif
