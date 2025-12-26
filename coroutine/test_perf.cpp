#include <chrono>
#include <random>
#include <iostream>
#include "coroutine.hpp"

class LifecycleTimer {
public:
    // 构造函数，记录创建时间
    explicit LifecycleTimer(uint64_t count)
        : count_(count), start_time_(std::chrono::steady_clock::now()) {
        times_++;
    }

    // 析构函数，计算并输出生命周期
    ~LifecycleTimer() {
        auto end_time = std::chrono::steady_clock::now();
        auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            end_time - start_time_).count();
        fprintf(stderr, "%d - total : %llu(ms)\n", times_,
                static_cast<uint64_t>(duration_ns)/1000000);
        fprintf(stderr, "%d - avg   : %.3lf(ns)\n", times_,
                static_cast<double>(duration_ns)/count_);
    }

private:
    static int32_t times_;
    uint64_t count_;
    std::chrono::steady_clock::time_point start_time_; // 创建时间
};

int32_t LifecycleTimer::times_{0};

COROUTINE_FUNC_IMPL();

static inline uint64_t tick(void)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

int main(void)
{

    if (1)
{
    utils::coroutine coro;

    coro.bind([&](void) {
        do {
            coro.yield();
        } while (true);
    });
    
    fprintf(stderr, "====== CoroutineSwapCtx 性能测试 ======\n");
    // 测试 resume/yield 的总时间
    constexpr uint64_t iterations = 1UL << 25;
    LifecycleTimer cost(iterations);
    for (uint64_t i = 0; i < iterations; i++) {
        coro.resume();
    }
}

    if (1)
{
    utils::coroutine coro;

    volatile uint64_t b = 1;
    coro.bind([&](void) {
        volatile uint64_t a = 0;
        do {
            uint64_t temp = a + b;
            a = b;
            b = temp;
            coro.yield();
        } while (true);
    });
    
    fprintf(stderr, "====== Coroutine 性能测试 ======\n");
    // 测试 resume/yield 的总时间
    constexpr uint64_t iterations = 1UL << 25;
    LifecycleTimer cost(iterations);
    for (uint64_t i = 0; i < iterations; i++) {
        coro.resume();
    }
    fprintf(stderr, "\tFib 计算结果 : %llu\n", b);
}

    fprintf(stderr, "注: 包括 resume/yield 各一次，实际 SwapCtx 是其中的一部分\n");

    return EXIT_SUCCESS;
}
