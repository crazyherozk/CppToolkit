#include <vector>
#include <iostream>
#include <algorithm>

#include <sys/wait.h>

#include "shmRpcTopic.hpp"

QEVENT_REACTOR_VAR();
EZLOG_HOOKER_VAR();
RPCTOPIC_RUNTIME_VAR();

using namespace rpc_topic;

constexpr const char * TopicName      = "topicPerf";
constexpr const char * ServiceName    = "topicPerfTest";
constexpr const char * ProxyAppName   = "topicPerfTestPxy";
constexpr const char * ServiceAppName = "topicPerfTestSrv";

constexpr const uint32_t TopicQueueSize    = 1U << 13; /*订阅队列的共享内存大小*/
constexpr const uint32_t TopicPayloadSize  = 64; /*订阅的负载大小*/
constexpr const uint32_t StatisticPerCount = 256; /*每多少条数据统计一次*/
constexpr const uint32_t ShowPerCount      = 4096; /*每多少条数据统计一次*/
constexpr const uint32_t SendCount         = 1U << 20; /*发布数据条数*/

struct LatencyVector {
    uint64_t data_[ShowPerCount];
    size_t  idx_{0};

    inline void push(uint64_t v) {
        auto idx = idx_++;
        data_[idx] = v;
        if (unlikely(idx == 0)) {
            fprintf(stderr, "first message latency : %llu\n", v);
        }
        if (unlikely(/*idx_ == 2048 || */idx_ == ShowPerCount)) {
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
        samples.reserve(ShowPerCount);

        for (size_t i = 0; i < idx_; ++i) {
            auto v = data_[i];
            samples.push_back(v);
            // fprintf(stderr, "message latency : %llu\n", v);
        }
        std::sort(samples.begin(), samples.end());
        auto p50  = samples[samples.size() * 50 / 100];
        auto p80  = samples[samples.size() * 80 / 100];
        auto p90  = samples[samples.size() * 90 / 100];
        auto p99  = samples[samples.size() * 99 / 100];
        auto p999 = samples[samples.size() * 999 / 1000];

        fprintf(stdout,
            "====== latency ======\n"
            "samples : %zu\n"
            "\tP50  : %llu\n"
            "\tP80  : %llu\n"
            "\tP90  : %llu\n"
            "\tP99  : %llu\n"
            "\tP999 : %llu\n",
            idx_, p50, p80, p90, p99, p999
        );
    }
    ~LatencyVector(void) {
        showLatency();
    }
};

struct Message {
    uint32_t    magic{0xabadbeefU};
    uint64_t    ts{0};
    CString<TopicPayloadSize> data;
    bool valid(void) const { return magic == 0xabadbeefU; }
    Message(void) :data("HelloWorld!") {}
};

static void run_proxy() {
    RuntimeConfig cfg;
    cfg.topicQueueSize = TopicQueueSize;

    Runtime::init(ProxyAppName, cfg);

    auto pxy = Runtime::get()->buildProxy<>(ServiceAppName, ServiceName);
    assert(pxy);

    LatencyVector vector;

    /*启动一个订阅*/
    auto id = pxy->launchSubscribe(TopicName, [&](const Message & msg) {
        assert(msg.valid());
        /*消息处理*/
        /*延迟统计*/
        if (unlikely(msg.ts != 0)) {
            auto latency = qevent::sys_clock() - msg.ts;
            vector.push(latency);
        }
    });

    assert(id);

    bool running = true;

    auto & loop = Runtime::get()->loop();

    auto timer = loop->addTimer(1000, [&](int32_t){
        /*一秒内不能探测服务，则退出*/
        running = false;
    });

    /*发起握手*/
    pxy->getProxyStatusEvent().subscribe([&](StatusValue stat){
        if (stat == StatusValue::APP_OFFLINE) {
            /*离线则停止*/
            running = false;
        } else if (stat == StatusValue::APP_ONLINE) {
            /*上线需要移除定时器*/
            loop->removeTimer(timer);
        }
    });

    do {
        loop->run();
    } while (running);

    /*手动移除，解除回调中的引用，防止智能指针的环形引用*/
    pxy->removeSubscriber(id);
}

static void launch_publish(bool & running) {
    static bool launched = false;

    if (launched) {
        std::cout << "\t[!]已经启动了分发\n" << std::endl;
        return;
    }

    launched = true;

    std::thread sender;
    sender = std::thread([&](void) {
        PublisherPtr pub;
        {
            /*查找服务和发布者对象，实现快速发布*/
            auto srv = Runtime::get()->findService(ServiceName);
            assert(srv);
            pub = srv->topicPublisher(fnv1a_64(TopicName));
            assert(pub);
        }
        /*等待一小会儿，让订阅者完全就绪，减少不必要的抖动干扰*/
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        for (uint64_t i = 0; i < SendCount && running; i++) {
            /*广播*/
            Message msg;
            if (unlikely((i & (StatisticPerCount-1)) == 0)) {
                /*间隔一些消息来统计延迟，防止 获取 系统单调时间 带来的干扰*/
                msg.ts = qevent::sys_clock();
            }
            /*保证分发成功，即使队列暂时满了*/
            do { if (pub->publish(msg)) break; } while (true && running);
#if 1
            /*防止快速塞满订阅端的队列，带来调度上的严重干扰，实际业务中，也不可能一直死循环发布数据*/
            std::this_thread::yield();
#endif
        }
        running = false;
    });

    sender.detach();
}

static void run_service() {
    Runtime::init(ServiceAppName);

    auto srv = Runtime::get()->buildService<>(ServiceName);
    assert(srv);

    bool running = true;

    auto & loop = Runtime::get()->loop();

    /*5秒内必须有订阅请求，并测试完毕，否则退出*/
    loop->addTimer(5000, [&](int32_t) { running = false; });

    auto id = srv->buildPublisher<>(TopicName, [&](IpcKey ipc) {
        /*有订阅请求，启动发送线程*/
        launch_publish(running);
    });

    loop->addSignal(SIGINT, [&](int32_t){ running = false; });

    do {
        loop->run();
    } while (running);

    /*手动移除，解除回调中的引用，防止智能指针的环形引用*/
    srv->removePublisher(id);
}

int main(void) {
    ezlog::initialize(nullptr, ezlog::EZLOG_INFO);

    /*虽然使用亲缘进程模拟发布端、订阅端，但是处了ezlog部分已经初始化，其余数据均为私用*/
    auto pid = ::fork();
    assert(pid != -1);

    if (pid == 0) {
        /*子进程*/
        run_proxy();
        exit(EXIT_SUCCESS);
    }

    run_service();

    ::wait(nullptr);

    return EXIT_SUCCESS;
}