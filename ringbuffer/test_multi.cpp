#include <chrono>
#include <thread>
#include <random>
#include <iostream>
#include "ringbuffer.hpp"

class LifecycleTimer {
public:
    // 构造函数，记录创建时间
    explicit LifecycleTimer(uint32_t count)
        : count_(count), start_time_(std::chrono::steady_clock::now()) {
        fprintf(stderr, "\t[*] 启动了计时，次数 : %u\n", count_);
    }

    // 析构函数，计算并输出生命周期
    ~LifecycleTimer() {
        auto end_time = std::chrono::steady_clock::now();
        auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            end_time - start_time_).count();
        fprintf(stderr, "\t[*] 平均单次出入花费(ns) : %.3lf\n",
                static_cast<double>(duration_ns)/count_);
    }

    // 获取当前生命周期（纳秒）
    int64_t get_duration_ns() const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start_time_).count();
    }

    // 获取当前生命周期（微秒）
    int64_t get_duration_us() const {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start_time_).count();
    }

    // 获取当前生命周期（毫秒）
    int64_t get_duration_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time_).count();
    }

private:
    uint32_t count_;
    std::chrono::steady_clock::time_point start_time_; // 创建时间
};

int main(void) {

    if (1)
{
    std::cout << "=== 观察阻塞 ===" << std::endl;
    ring::blocking_buffer<std::string> queue("ringbuffer_multi");

    // fprintf(stderr, "capacity of queue : %zu\n", queue.capacity());

    constexpr uint32_t count = 1U << 6;
    const std::array<std::string, 3> msgs = {
        "Message1:hello China, Beijin",
        "Message2:hello world, China",
        "Message3:hello China, Chengdu"
    };
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(1, 10);

    auto doEnqueu = [&](void) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::cout << "\t[*] 启动生产者" << std::endl;
        for (int i = 0; i < count; i++) {
            size_t j = i % msgs.size();
            while (!queue.push(msgs[j])) {
                fprintf(stderr, "\t[%03d] 尝试推入\n", i);
                std::this_thread::yield();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(dist(gen)));
        }
    };

    std::thread T1(doEnqueu);

{
    std::cout << "\t[*] 启动消费者" << std::endl;
    for (int i = 0; i < count * 1; i++) {
        std::string result;
        while (!queue.pop(result, 5)) {
            fprintf(stderr, "\t[%03d] 尝试弹出\n", i);
        }
        size_t j = i % msgs.size();
        if (msgs[j] != result) {
            fprintf(stderr, "\t[!]测试失败 [%d]\n", i);
            ::abort();
        }
    }

    T1.join();
}
}

    if (1)
{
    std::cout << "=== SPSC 测试 ===" << std::endl;
    ring::blocking_buffer<std::string> queue("ringbuffer_multi");

    // fprintf(stderr, "capacity of queue : %zu\n", queue.capacity());

    constexpr uint32_t count = 1U << 21;
    const std::array<std::string, 3> msgs = {
        "Message1:hello China, Beijin",
        "Message2:hello world, China",
        "Message3:hello China, Chengdu"
    };
    LifecycleTimer cost(count);

    auto doEnqueu = [&](void) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::cout << "\t[*] 启动生产者" << std::endl;
        for (int i = 0; i < count; i++) {
            size_t j = i % msgs.size();
            std::string msg = msgs[j];
            while (!queue.push(std::move(msg))) {
                // fprintf(stderr, "try push again[%zu/%d]\n", j, i);
                std::this_thread::yield();
            }
        }
    };

    std::thread T1(doEnqueu);
{
    std::cout << "\t[*] 启动消费者" << std::endl;
    for (int i = 0; i < count * 1; i++) {
        std::string result;
        while (!queue.pop(result, 10)) { }
#if 1
        size_t j = i % msgs.size();
        assert(msgs[j] == result);
#endif
    }
}
    T1.join();
}

    if (1)
{
    std::cout << "=== MPSC 测试 ===" << std::endl;
    ring::blocking_buffer<std::string, 10> queue("ringbuffer_multi");

    // fprintf(stderr, "capacity of queue : %zu\n", queue.capacity());

    constexpr uint32_t count = 1U << 20;
    const std::array<std::string, 3> msgs = {
        "Message1:hello China, Beijin",
        "Message2:hello world, China",
        "Message3:hello China, Chengdu"
    };
    LifecycleTimer cost(count * 2);

    auto doEnqueu = [&](void) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::cout << "\t[*] 启动生产者" << std::endl;
        for (int i = 0; i < count; i++) {
            size_t j = i % msgs.size();
            std::string msg = msgs[j];
            while (!queue.push(std::move(msg))) {
//                fprintf(stderr, "try push again[%zu/%d]\n", j, i);
                std::this_thread::yield();
            }
        }
    };

    std::thread T1(doEnqueu);
    std::thread T2(doEnqueu);

{
    std::cout << "\t[*] 启动消费者" << std::endl;
    for (int i = 0; i < count * 2; i++) {
        std::string result;
        while (!queue.pop(result, 10)) {
            std::this_thread::yield();
        }
#if 0
        /*生产者乱序，没有办法验证弹出了*/
        size_t j = i % msgs.size();
        assert(msgs[j] == result);
#endif
    }
}
    T1.join();
    T2.join();
}

    if (1)
{
    std::cout << "=== SPSC 测试(仅出入队列) ===" << std::endl;
    ring::blocking_buffer<uint64_t> queue("ringbuffer_multi");

    // fprintf(stderr, "capacity of queue : %zu\n", queue.capacity());

    constexpr uint32_t count = 1U << 24;
    const std::array<uint64_t, 3> msgs = {
        0xfedcba9876543210ULL,
        0x0123456789abcdefULL,
        0x76543210fedcba98ULL,
    };
    LifecycleTimer cost(count);

    auto doEnqueu = [&](void) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::cout << "\t[*] 启动生产者" << std::endl;
        for (int i = 0; i < count; i++) {
            size_t j = i % msgs.size();
            while (!queue.push(msgs[j])) {
                // fprintf(stderr, "try push again[%zu/%d]\n", j, i);
                std::this_thread::yield();
            }
        }
    };

    std::thread T1(doEnqueu);

{
    std::cout << "\t[*] 启动消费者" << std::endl;
    for (int i = 0; i < count * 1; i++) {
        uint64_t result;
        while (!queue.pop(result, 0)) {
            std::this_thread::yield();
        }
#if 1
        size_t j = i % msgs.size();
        assert(msgs[j] == result);
#endif
    }
}
    T1.join();
}

    return EXIT_SUCCESS;
}