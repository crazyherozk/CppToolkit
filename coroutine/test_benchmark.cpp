#include <random>
#include <chrono>
#include "coroutine.hpp"

COROUTINE_FUNC_IMPL();

static inline uint64_t tick(void)
{
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    return static_cast<uint64_t>(duration);
}

int main(void)
{
    std::random_device rd;
    std::mt19937 gen(rd());

    if (1)
{
    fprintf(stderr, "---- 切换测试 ----\n");
    volatile float pi = 3.1415926f;
    utils::coroutine task;
    task.bind([&](void) {
        std::uniform_real_distribution<> dis(.0f, 5.0f);
        for (int i = 0; i < 10; i++) {
            pi *= dis(gen);
            fprintf(stderr, "\t[%d] resume, pi : %lf ...\n", i, pi);
            task.yield();
        }
        fprintf(stderr, "\t[!] exited ...\n");
    });

#if 0
    while (!task.exited()) {
        task.resume();
        fprintf(stderr, "[-] coro was suspended\n");
    }
#else
    while (task.resume() != utils::CO_STATUS::CO_EXITED) {
        fprintf(stderr, "[-] coro was suspended\n");
    }
#endif
   
    fprintf(stderr, "=== 运行结果 pi : %lf ===\n", pi);
}

    return EXIT_SUCCESS;
}