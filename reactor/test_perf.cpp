#include <thread>
#include <cstring>
#include <ctime>
#include <cassert>
#include <cstdio>
#include <cstdlib>

#include <random>
#include <vector>
#include <atomic>
#include <thread>
#include <memory>
#include <string>
#include <chrono>

#include <arpa/inet.h>
#include <sys/socket.h>

#include "reactor.hpp"

QEVENT_REACTOR_VAR();

class LifecycleTimer {
public:
    // 构造函数，记录创建时间
    explicit LifecycleTimer(uint32_t count)
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
    uint32_t count_;
    std::chrono::steady_clock::time_point start_time_; // 创建时间
};

int32_t LifecycleTimer::times_{0};

struct Fib {
    Fib(uint32_t n) : nr_(n) { }

    uint64_t doCalc(void) const {
        if (nr_ <= 1) return nr_;
        volatile uint64_t a = 0;
        volatile uint64_t b = 1;
        for (uint32_t i = 2; i <= nr_; ++i) {
            uint64_t temp = a + b;
            a = b;
            b = temp;
        }
        return b;
    }

    uint32_t nr_{0};
};

int main(void) {
    qevent::reactor loop;

    /*异步性能*/
    if (1)
{
    const uint32_t count = 1U << 20;
    fprintf(stderr, "异步任务性能测试\n");
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(50, 60);

    std::atomic<uint32_t> done{0};
    auto task = [&](void){
        ::usleep(500);
        for (uint32_t i = 0; i < count/2; i++)
        {
            Fib fib(dist(gen));
            loop.addAsync([=](uint32_t) {
                volatile auto rc = fib.doCalc();
                (void)rc;
            });
        }
        loop.addAsync([&](uint32_t){
            done++;
        });
    };

    LifecycleTimer cost(count);
    //std::vector<std::thread> T;
    std::thread T1(task);
    if (!T1.joinable()) { abort(); }
    std::thread T2(task);
    if (!T2.joinable()) { abort(); }

    do {
        loop.run(1);
    } while (done != 2);

    if (T1.joinable()) { T1.join(); }
    if (T2.joinable()) { T2.join(); }

    auto stat = loop.pollStatus();
    fprintf(stderr, "\t测试执行次数 : %d\n", count);
    fprintf(stderr, "\t内部事件次数(通知循环): %d\n", stat.nr_done_ - count);
}

    /*定时器性能*/
    if (1)
{
    fprintf(stderr, "定时任务性能测试\n");
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(50, 1000);

    const uint32_t count = 1U << 16;

    volatile uint32_t total = 0;
    volatile uint32_t done  = 0;
    const auto work = [&](int32_t diff) {
        done++;
        total += (diff>0)?diff:-diff;
    };
    /*插入1<<16个任务*/
    for (int i = 0; i < count; i++) {
        loop.addTimer(dist(gen), work);
    }

    LifecycleTimer cost(count);
    do { loop.run(); } while (done != count);

    fprintf(stderr, "\t测试执行次数 : %d\n", count);
    fprintf(stderr, "\t平均触发误差 : %.3f(ms)\n", (float)total/count);
}

    /*IO监控性能*/
    if (0)
{
    /*
     * iperf -c 10.75.100.14 -p 8000 -t 10 -i 1 -P <#thread>
     */
    fprintf(stderr, "IO任务性能测试\n");
    /*需要 iperf 辅助测试*/
    int32_t server_fd, client_fd;
    sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    // 设置 socket 选项：端口复用
    int32_t opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 绑定地址和端口
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;  // 监听所有网卡
    server_addr.sin_port = htons(8000);

    if (bind(server_fd, (sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        close(server_fd);
        fprintf(stderr, "bind 失败\n");
        return EXIT_FAILURE;
    }

    // 开始监听
    if (listen(server_fd, 10) == -1) {  //  backlog = 10
        close(server_fd);
        fprintf(stderr, "listen 失败\n");
        return EXIT_FAILURE;
    }

    fprintf(stderr, "\tTCP 服务端启动，监听端口: 8000\n");

    const auto setNonblock = [](int32_t fd)
    {
        int32_t v = ::fcntl(fd, F_GETFL, nullptr);
        if (unlikely(v < 0)) {
            fprintf(stderr, "\t[!] 设置为非阻塞失败\n");
            return;
        }
        v = ::fcntl(fd, F_SETFL,
        static_cast<uint32_t>(v) | static_cast<uint32_t>(O_NONBLOCK));
        if (unlikely(v < 0)) {
            fprintf(stderr, "\t[!] 设置为非阻塞失败\n");
            return;
        }
    };

    setNonblock(server_fd);

    static uint8_t buff[4096];

    loop.addEvent(server_fd, qevent::EV_READ, [&](int32_t) {
        for (int32_t i = 0; i < 4; i++) {
            client_fd = accept(server_fd, nullptr, nullptr);
            if (client_fd == -1) {
                if (errno != EAGAIN) {
                    fprintf(stderr, "[!] accept 失败\n");
                }
                break;
            }
            fprintf(stderr, "\t[*] 新的连接 [%d]\n", client_fd);
            setNonblock(client_fd);
            loop.addEvent(client_fd, qevent::EV_READ,
                    [&, cfd = client_fd](int32_t)
            {
                for (int i = 0; i < 8; i++) {
                    auto rc = read(cfd, buff, sizeof(buff));
                    if (rc < 1) {
                        if (rc == 0 || errno != EAGAIN) {
                            loop.removeEvent(cfd);
                            close(cfd);
                        }
                        break;
                    }
                    /*回射数据?*/
                    //write(cfd, buff, rc);
                    //fprintf(stderr, "\t读取字节数 [%d] : %ld\n", cfd, rc);
                }
            });
        }
    });

    do { loop.run(10); } while (true);

    loop.reset();

    ::close(server_fd);
}

    return EXIT_SUCCESS;
}