#include "workqueue.hpp"

WQ_WORKQUEUE_VAR();

class LifecycleTimer {
public:
    // 构造函数，记录创建时间
    explicit LifecycleTimer(uint32_t count)
        : times_(times++)
        , count_(count)
        , start_time_(std::chrono::steady_clock::now()) {
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
    static std::atomic<uint32_t> times;
    uint32_t times_;
    uint32_t count_;
    std::chrono::steady_clock::time_point start_time_; // 创建时间
};

std::atomic<uint32_t> LifecycleTimer::times{0};

static uint32_t NR_WORKER = 2;
constexpr uint32_t NR_WORK   = 1U << 16;

int main(int32_t argc, char **argv) {
    if (argc == 2) {
        NR_WORKER = std::atoi(argv[1]);
    }

    fprintf(stderr, "---- 工作队列性能测试 ----\n");
    wq::config cfg;
    cfg.max_thds_ = 10;
    auto wq = std::make_shared<wq::workqueue>(cfg);

    std::vector<std::thread> threads;
    volatile std::atomic<uint32_t> nr_finish{0};

    for (size_t i = 0; i < NR_WORKER; i++) {
        /* code */
        threads.push_back(std::thread([&, wq](void) {
            std::vector<std::shared_ptr<wq::work>> tasks;
            for (size_t i = 0; i < NR_WORK; i++) {
                /* code */
                tasks.push_back(std::make_shared<wq::work>(wq, [&](void){
                    nr_finish++;
                }));
            }
            LifecycleTimer cost(tasks.size());
            for (auto &&task : tasks) {
                task->queue();
            }
            for (auto &&task : tasks) {
                task->flush();
            }
        }));
    }

    for (auto &&worker : threads) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    assert(NR_WORKER * NR_WORK == nr_finish);

    wq->stop();
    fprintf(stderr, "workqueue : %zu\n", wq.use_count());

    return EXIT_SUCCESS;
}