#undef DEBUG
#include <vector>
#include <iostream>
#include <algorithm>

#include <sys/wait.h>

#include "shmRpcTopic.hpp"

QEVENT_REACTOR_VAR();
EZLOG_HOOKER_VAR();
RPCTOPIC_RUNTIME_VAR();

//#define RPC_BATCH

using namespace rpc_topic;

constexpr const char * RpcName        = "rpcPerf";
constexpr const char * ServiceName    = "rpcPerfTest";
constexpr const char * ProxyAppName   = "rpcPerfTestPxy";
constexpr const char * ServiceAppName = "rpcPerfTestSrv";

constexpr const uint32_t RpcQueueSize      = 1U << 12; /*RPC队列的共享内存大小*/
constexpr const uint32_t RpcPayloadSize    = 32; /*RPC的负载大小*/
constexpr const uint32_t StatisticPerCount = 128; /*每多少条数据统计一次*/
constexpr const uint32_t ShowPerCount      = 4096; /*每多少条数据统计一次*/
#ifdef RPC_BATCH
constexpr const uint32_t SendCount         = 1U << 16; /*请求数据条数*/
#else
constexpr const uint32_t SendCount         = 1U << 19; /*请求数据条数*/
#endif

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
//            fprintf(stderr, "message latency : %llu\n", v);
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
    CString<RpcPayloadSize> data;
    bool valid(void) const { return magic == 0xabadbeefU; }
};

static void launch_request(IpcKey pxyId, bool & running) {
    static bool launched = false;

    if (launched) {
        std::cout << "\t[!]已经启动了请求\n" << std::endl;
        return;
    }

    launched = true;

    std::thread sender;
    sender = std::thread([&, PxyId=pxyId](void) {
        auto pxy = Runtime::get()->findProxy(PxyId);
        assert(pxy);
        LatencyVector vector;
        uint32_t RecvCount = 0;
#ifndef RPC_BATCH
        bool finish;
        std::mutex mutex;
        std::condition_variable cond;
#endif
        for (uint32_t i = 0; i < SendCount && running; i++) {
            Message msg;
            if (unlikely((i & (StatisticPerCount-1)) == 0)) {
                /*间隔一些消息来统计延迟，防止 获取 系统单调时间 带来的干扰*/
                msg.ts = qevent::sys_clock();
            }
#ifndef RPC_BATCH
            finish = false;
#endif
            /*保证请求发送成功，即使队列暂时满了*/
            do {
                auto rc = pxy->requestAsync<fnv1a_64(RpcName)>(
                    [&](bool code, const uint64_t & ts) {
                        /*超时表示性能严重不达标，可以退出*/
                        if (!code) {
                            running = false;
                            std::cout << "\t[!] 请求超时" << std::endl;
                        } else if (unlikely(ts != 0)) {
                            auto latency = qevent::sys_clock() - ts;
                            vector.push(latency);
                        }
                        RecvCount++;
#ifndef RPC_BATCH
                        std::lock_guard<std::mutex> g(mutex);
                        finish = true;
                        cond.notify_one();
#endif
                    }
                    , msg
                );
                if (likely(rc)) {
                    break;
                }
            } while (true && running);
            /*等待完成，再发送？*/
#ifndef RPC_BATCH
            std::unique_lock<std::mutex> lock(mutex);
            cond.wait(lock, [&](void) { return finish; });
#else
            /*防止快速塞满服务端的队列，带来调度上的严重干扰，实际业务中，也不可能一直死循环请求*/
            std::this_thread::yield();
#endif
        }
        /*发送一条不响应的消息，通知对端退出*/
        pxy->request<fnv1a_64(RpcName)>();
        for (int32_t i = 0; i < 8 && RecvCount != SendCount; i++) {
            /*等待所有响应到达*/
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        running = false;
        std::cout << "发送完毕" << std::endl;
    });

    sender.detach();
}

static void run_proxy() {
    RuntimeConfig cfg;
    cfg.rpcQueueSize = RpcQueueSize;

    Runtime::init(ProxyAppName, cfg);

    auto pxy = Runtime::get()->buildProxy<>(ServiceAppName, ServiceName);
    assert(pxy);

    auto pxyId = pxy->ipcId();

    LatencyVector vector;

    /*启动一个客户端*/
    auto id = pxy->buildRpcClient(RpcName, 10);
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
            /*启动请求*/
            launch_request(pxyId, running);
        }
    });

    do {
        loop->run();
    } while (running);

    /*手动移除，解除回调中的引用，防止智能指针的环形引用*/
    pxy->removeRpcClient(id);
}

static void run_service() {
    Runtime::init(ServiceAppName);

    /*创建服务*/
    auto srv = Runtime::get()->buildService<>(ServiceName);
    assert(srv);

    bool running = true;
    /*创建一个回调模式的RpcServer, 可以响应请求*/
    auto id = srv->buildRpcServer<>(RpcName,
        [&](const RpcServer::ReqId reqId, PackBuffer & buff)
    {
        if (!reqId.second) { 
            /*最后一条消息*/
            running = false;
            return;
        }
        /*判断有效性*/
        auto msg = buff.data<Message>();
        assert(msg && msg->valid());
        /*响应时间戳，以便对端统计*/
        auto rc = srv->response<fnv1a_64(RpcName)>(reqId, msg->ts);
        /*
         * 这个测试中，Proxy的队列远大于 Service 的队列，不可能满
         * 如果失败必然是对端提前离线，或其他逻辑错误
         */
        assert(rc);
    });

    assert(id);


    auto & loop = Runtime::get()->loop();

    /*5秒内必须有订阅请求，并测试完毕，否则退出*/
    loop->addTimer(5000, [&](int32_t) { running = false; });

    loop->addSignal(SIGINT, [&](int32_t){ running = false; });

    do {
        loop->run();
    } while (running);

    /*手动移除，解除回调中的引用，防止智能指针的环形引用*/
}

int main(void) {
    ezlog::initialize(nullptr, ezlog::EZLOG_INFO);

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
