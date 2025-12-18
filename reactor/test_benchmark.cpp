#include <thread>
#include <chrono>
#include <iostream>
#include <cassert>
#include "reactor.hpp"

QEVENT_REACTOR_VAR();

int main(void)
{
    qevent::reactor loop;

    /*
     * 由于使用的是绝对时间，所以如果系统本身负载过高，可能导致定时器测试失败
     * 可以使用 nice 提高优先级来运行测试，但也并不保证一定成功
     */

    if (1)
{
    /*
     * 嵌入式系统或内部应用，对于socket而言，一般不会超过32个
     * 所以使用 poll 实现多路复用，性能和 epoll 几乎一样，但
     * 实现上简单可靠
     */
    std::cout << "IO基准测试" << std::endl;

    int32_t pfd[2];
    qevent::utils::pipe2(pfd, O_NONBLOCK);

    /*添加、删除、修改*/
    assert(!loop.removeEvent(pfd[1]));
    assert(loop.addEvent(pfd[1], POLLIN, [](uint32_t){}));
    assert(!loop.addEvent(pfd[1], POLLOUT, [](uint32_t){}));
    assert(loop.modEvent(pfd[1], POLLOUT));
    assert(loop.removeEvent(pfd[1]));

    std::cout << "\t可写性测试..." << std::endl;
    volatile bool wa = false;
    assert(loop.addEvent(pfd[1], POLLOUT, [&](uint32_t){ wa = true; }));
    /*写立即触发*/
    assert(loop.run(10) < 2);
    assert(wa);

    std::cout << "\t可写事件单次触发..." << std::endl;
    /*
     * 且只触发一次，因为大部分时候，IO写都是可行的，所以应该先尝试写，失败再加入事件循环，
     * 触发可写后，写入后，不再触发
     */
    wa = false;
    assert(loop.run(10) >= 10);
    assert(!wa);

    std::cout << "\t可读性测试..." << std::endl;
    volatile bool ra = false;
    assert(loop.addEvent(pfd[0], POLLIN, [&](int32_t){ ra = true; }));
    /*不可读*/
    assert(loop.run(10) >= 10);
    assert(!ra);

    std::cout << "\t写满PIPE : ";
    do {
        char buff[512];
        auto rc = ::write(pfd[1], buff, sizeof(buff));
        if (rc < 0) {
            assert(errno == EAGAIN);
            break;
        }
        std::cout << ".";
    } while (1);

    std::cout << std::endl;

    /*再次加入*/
    wa = false;
    ra = false;
    assert(!loop.modEvent(pfd[1], POLLOUT));
    /*不可写但可读*/
    std::cout << "\t不可写但可读..." << std::endl;
    assert(loop.run(10) < 2);
    assert(!wa && ra);

    ra = false;
    assert(loop.removeEvent(pfd[0]));
    assert(loop.addEvent(pfd[0], POLLIN, [&](int32_t){
        std::cout << "\t读空PIPE : ";
        do {
            char buff[512];
            auto rc = ::read(pfd[0], buff, sizeof(buff));
            if (rc < 1) {
                break;
            }
            ra = true;
            std::cout << ".";
        } while (1);
        std::cout << std::endl;
    }));

    loop.run(10);
    assert(ra);

    /*lambda的作用域为栈，重置reactor*/
    loop.reset();
    ::close(pfd[0]);
    ::close(pfd[1]);
}

    if (1)
{
    /*定时器测试*/
    std::cout << "定时器基准测试" << std::endl;

    /*随意添加一些不过期的定时器，参与超时判断*/
#if 1
    for (int i = 128; i < 512; i++) {
        /*嵌入式系统，一般也就200、300 个定时器*/
        loop.addTimer(i, [](int32_t){
            std::cout << "可能需要编译为Release模式进行测试" << std::endl;
        });
    }
#endif

    /*增删*/
    volatile bool res = false;
    qevent::reactor::timerId tid;
    assert(!loop.removeTimer(tid));
    tid = loop.addTimer(1, [&](int32_t diff) {
        res = true;
        if (diff < 1) {
            std::cout << "\t[*] 精准触发" << std::endl;
        } else {
            std::cout << "\t[*] 精准不足 [" << diff << "]" << std::endl;
        }
    });
    assert(tid.first);

    /*重置设置超时值*/
    assert(loop.resetTimer(tid, 10));

    auto escape = loop.run(5);
    std::cout << "\t第一次循环不触发 [" << escape << "]..." << std::endl;
    /*返回值的是循环开始和退出的流逝时间，按毫秒截断，如果第一次大于5毫秒，第二次可能就小于5毫秒了*/
    assert(escape >= 5);
    assert(!res);

    for (int i = 0; i < 2 && !res; i++) {
        /*poll精度不足，可能需要循环2次才能触发超时*/
        escape = loop.run(5);
        std::cout << "\t循环流逝时间 [" << escape << "]..." << std::endl;
    }
    assert(res);

    /*移除所有定时器*/
    loop.reset();
}

    if (1)
{
    using namespace std::chrono;
    /*定时器测试*/
    std::cout << "间隔定时器基准测试" << std::endl;

    /*普通重启定时器，不会有时间补偿*/
    qevent::restartTimer<qevent::reactor*> timer1(&loop);

    volatile bool broken = false;
    volatile uint32_t count = 0;
    timer1.launch(10, [&](int32_t) {
        count++;
        std::cout << "\t[*] 定时器触发 [" << count << "]" << std::endl;
    });

    std::cout << "\t间隔定时器受CPU负载影响 ..." << std::endl;
    loop.addTimer(100, [&](int32_t){ broken = true; });
    /*休眠模拟负载*/
    std::this_thread::sleep_for(milliseconds(50));
    /*循环一段时间，直到100毫秒到期*/
    do { loop.run(); } while (!broken);
    /*由于耽搁50毫秒，触发肯定小于6次*/
    assert(count = 5);
    /*停止计时器*/
    assert(timer1.stop());

    count = 0;
    broken = false;
    /*有时间补偿的重启定时器，比如间隔定为10毫秒，那么100毫秒内，无论CPU负载如何必定执行10次*/
    qevent::intervalTimer<qevent::reactor*> timer2(&loop);
    timer2.launch(10, [&](int32_t) {
        count++;
        std::cout << "\t[*] 定时器触发 [" << count << "]" << std::endl;
    });

    std::cout << "\t补偿间隔定时器不受CPU负载影响 ..." << std::endl;
    loop.addTimer(100, [&](int32_t){ broken = true; });

    std::this_thread::sleep_for(milliseconds(50));
    /*循环一段时间，直到100毫秒到期*/
    do { loop.run(); } while (!broken);
    /*
     * 耽搁50毫秒，不受影响
     * 在计算油耗、里程数、平均速度这样的算法中是必须的
     */
    assert(count < 12);

    loop.reset();
}

    if (1)
{
    /*信号测试*/
    std::cout << "信号基准测试" << std::endl;

    volatile bool res = false;
    /*信号一般在事件循环前就安装，后续基本上不会做任何改动，故接口简单*/
    loop.addSignal(SIGINT, [&](int32_t){
        std::cout << "\t[*] 信号触发" << std::endl;
        res = true;
    });

    /*信号已经被屏蔽，可以向自身发送信号，进程也不会退出*/
    ::kill(::getpid(), SIGINT);

    /*循环捕获信号*/
    for (int i = 0; i < 3; i++) {
        std::cout << "\t循环..." << std::endl;
        /*由于信号依赖内部的PIPE，所以事件需要至少两次触发才能执行信号的回调*/
        loop.run(1);
    }

    assert(res);
    loop.removeSignal(SIGINT);
}

    if (1)
{
    using namespace std::chrono;
    /*
     * 异步任务
     * 1. 为了保证业务的时序，所有触发的逻辑，都封装为任务
     * 2. 推入事件循环中执行
     */
    std::cout << "异步任务基准测试" << std::endl;

    volatile uint32_t count = 0;

    std::thread T([&](void) {
        for (int i = 0; i < 10; i++) {
            std::this_thread::sleep_for(milliseconds(50));
            loop.addAsync([&](int32_t) {
                count++;
                std::cout << "\t[*] 执行异步任务" << std::endl;
            });
        }
    });

    auto tid = loop.addTimer(550, [&](int32_t){
        std::cout << "异步任务测试失败" << std::endl;
        count = 100;
    });

    do { loop.run(); } while (count < 10);

    T.join();

    loop.reset();
}

    return EXIT_SUCCESS;
}