#include <thread>
#include <chrono>
#include <vector>
#include <iostream>
#include <algorithm>
#include <sys/wait.h>
#include "shareMemoryQueue.hpp"

static const std::string qName = "/shm_queue_perf";
constexpr uint64_t tCount = (1UL << 21);
constexpr uint64_t sFreq  = 256;
constexpr size_t   pSize  = 128;

struct LatencyRing {
    static constexpr size_t N = tCount/sFreq;
    uint64_t data_[N];
    size_t  idx_{0};

    void push(uint64_t v) {
        auto idx = idx_++;
        data_[idx] = v;
        if (unlikely(idx == 0)) {
            fprintf(stderr, "first message latency : %llu\n", v);
        }
        if (unlikely(/*idx_ == 4096 ||*/ idx_ == N)) {
            fprintf(stderr, "last  message latency : %llu\n", v);
            showLatency();
            idx_ = 0;
        }
    }

    void showLatency(void) {
        if (idx_ == 0) {
            return;
        }
        std::vector<uint64_t> samples;
        samples.reserve(N);

        for (size_t i = 0; i < idx_; ++i) {
            auto v = data_[i];
            samples.push_back(v);
            // fprintf(stderr, "message latency : %llu\n", v);
        }
        std::sort(samples.begin(), samples.end());
        auto p50  = samples[samples.size() * 50 / 100];
        auto p90  = samples[samples.size() * 90 / 100];
        auto p99  = samples[samples.size() * 99 / 100];
        auto p999 = samples[samples.size() * 999 / 1000];

        fprintf(stdout,
            "====== latency ======\n"
            "samples : %zu\n"
            "   P50  : %llu\n"
            "   P90  : %llu\n"
            "   P99  : %llu\n"
            "   P999 : %llu\n"
            "=====================\n"
            ,idx_, p50, p90, p99, p999
        );
    }

    ~LatencyRing(void) {
        showLatency();
    }
};

static inline uint64_t sys_clock(void)
{
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    return static_cast<uint64_t>(duration);
}

static void sendMessage(bool fast) {
    shm::ShareMemoryQueue queue;

    std::cout << "\t[*] 打开队列"<< std::endl;
    auto rc = queue.open(qName);
    assert(rc);

    /*休眠一会儿，防止接收方还在准备，队列被满*/
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::cout << "\t[*] 开始发送"<< std::endl;

    struct Payload {
        char buff[pSize];
    };

    Payload payload;

    uint64_t msgId = 0;
    while (msgId < tCount) {
        uint64_t ts = 0;
        if (unlikely((msgId & 255) == 0)) {
            ts = sys_clock();
        }

        while (!queue.pack(msgId, ts, payload)) {
            //std::cout << "\t[!] 队列已满" << std::endl;
            std::this_thread::yield();
        }

        if (!fast) {
            if (unlikely((msgId & 31) == 0)) {
                std::this_thread::yield();
            }
        }
    
        msgId += 1;
    }
    std::cout << "\t[*] 发送完成"<< std::endl;
}

static void recvMessage(shm::ShareMemoryQueue & queue) {
    std::cout << "\t[*] 开始接收"<< std::endl;
    uint64_t start;
    uint64_t count{0};

    struct Msg {
        uint64_t seq;
        uint64_t ts;
        char data[pSize];
    };

    LatencyRing ring;

    for (uint64_t msgId = 0; msgId < tCount; msgId++) {
        size_t size;
        auto rc = queue.pop(nullptr, size, 1000);
        if (unlikely(!rc)) {
            std::cout << "\t[!] 测试失败，接收发生了超时"<< std::endl;
            return;
        }

        Msg data;
        rc = queue.unpack(data);
        assert(rc);
        if (unlikely(data.seq != msgId)) {
            std::cout << "\t[!] 测试失败，接收发生了丢失"<< std::endl;
            return;
        }
        auto ts = data.ts;
        if (unlikely(ts != 0)) {
            /*采样*/
            ring.push(sys_clock()-ts);
        }
    }

    std::cout << "\t[*] 接收完成"<< std::endl;
}

int main(int32_t argc, char **argv) {

    std::cout << "=== 共享队列延迟测试 ===" << std::endl;
    if (argc != 1) {
        std::cout << "\t[!] 快速发送模式"<< std::endl;
    }

    std::cout << "\t[*] 创建队列"<< std::endl;
    /*队列大小将严重影响 P99+ 延迟，抖动厉害*/
    shm::ShareMemoryQueue queue(argc!=1?(1U<<std::atoi(argv[1])):(1U<<12));

    auto rc = queue.create(qName);
    assert(rc);

    std::cout << "\t[*] 创建进程"<< std::endl;
    auto pid = ::fork();
    assert(pid > -1);
    if (pid == 0) {
        /*子进程，作为发送数据方*/
        sendMessage(false);
        _exit(EXIT_SUCCESS);
    }

    recvMessage(queue);

    std::cout << "\t[*] 回收进程"<< std::endl;
    ::wait(nullptr);
    std::cout << "\t[*] 完成测试"<< std::endl;

    return EXIT_SUCCESS;
}