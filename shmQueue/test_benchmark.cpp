#include <thread>
#include <chrono>
#include <iostream>
#include "shareMemoryQueue.hpp"

int main(void)
{
    if (1)
{
    /*为了保证没有遗留*/
    shm::ShareMemoryQueue::unlink("invalid_name");
    shm::ShareMemoryQueue::unlink("/one_shm_queue");
}

    // open & create benchmark
    if (1)
{
    std::cout << "=== 创建、打开与关闭 ===" << std::endl;
    shm::ShareMemoryQueue queue(512);

    std::cout << "\t打开不存在的队列" << std::endl;
    auto rc = queue.open("/one_shm_queue");
    assert(rc == false);

    std::cout << "\t创建(to owner)" << std::endl;

    rc = queue.create("/one_shm_queue");
    assert(rc == true);
    assert(queue.size() == 512);

{
    std::cout << "\t创建(to user)" << std::endl;
    shm::ShareMemoryQueue queue(1024);
    rc = queue.create("/one_shm_queue", false);
    assert(rc == true);
    assert(queue.size() == 512);
}

{
    shm::ShareMemoryQueue user(1024);
    std::cout << "\t打开(to user)" << std::endl;
    rc = user.open("/one_shm_queue");
    assert(rc == true);
    assert(user.size() == 512);
}

    std::cout << "\t关闭(owner)" << std::endl;
    queue.close();

{
    shm::ShareMemoryQueue user;
    std::cout << "\t打开(user)" << std::endl;
    rc = user.open("/one_shm_queue");
    assert(rc == false);
}

    rc = queue.create("invalid_name");
    if (rc) {
        std::cout << "\t共享内存对象名可以不以 '/' 作为前缀" << std::endl;
    }
}

    /*move & copy*/
    if (1)
{
    std::cout << "=== 移动与构造 ===" << std::endl;
    shm::ShareMemoryQueue shm1(512);
    shm::ShareMemoryQueue shm2(512);

    std::cout << "\t[*] 创建" << std::endl;
    auto rc = shm1.create("/one_shm_queue");
    assert(rc == true);

    assert(shm1.valid() && !shm2.valid());
    std::cout << "\t[*] 移动" << std::endl;
    shm2 = std::move(shm1);
    assert(shm2.valid() && !shm1.valid());

{
    /*移动后，会在此块中被销毁*/
    std::cout << "\t[*] 销毁" << std::endl;
    shm::ShareMemoryQueue shm3(std::move(shm2));
    assert(shm3.valid() && !shm2.valid());
}

    std::cout << "\t[*] 打开" << std::endl;
    rc = shm1.open("/one_shm_queue");
    assert(rc == false);
}

    // vfs lock to atomic operate
    if (1)
{
    shm::ShareMemoryQueue user;
    shm::ShareMemoryQueue queue(512);

    std::atomic_bool creating{false};

    std::cout << "=== 原子执行 ===" << std::endl;
    auto thd1 = std::thread([&](void) {
        std::cout << "\t[*] 启动创建" << std::endl;
        auto rc = shm::ShareMemoryQueue::atomicExec(true, [&](void) {
            std::cout << "\t[1] 通知打开" << std::endl;
            creating = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::cout << "\t[3] 开始创建" << std::endl;
            return queue.create("/one_shm_queue");
        });
        assert(rc == true);
        std::cout << "\t[5] 完成创建" << std::endl;
    });

    auto thd2 = std::thread([&](void) {
        std::cout << "\t[*] 启动打开,并等待通知" << std::endl;
        while (!creating) { std::this_thread::yield(); }
        std::cout << "\t[2] 启动打开" << std::endl;
        auto rc = shm::ShareMemoryQueue::atomicExec(false, [&](void) {
            std::cout << "\t[4] 开始打开" << std::endl;
            return user.open("/one_shm_queue");
        });
        assert(rc == true);
        std::cout << "\t[6] 完成打开" << std::endl;
    });

    thd1.join();
    thd2.join();
}

    // enqueu & dequeue
    if (1)
{
    std::cout << "=== 出入队列操作 ===" << std::endl;
    shm::ShareMemoryQueue user(512);
    size_t size;
    char buff[] = "hello";
{
    shm::ShareMemoryQueue queue(512);
    auto rc = queue.create("/one_shm_queue");
    assert(rc == true);

    size = sizeof(buff);
    std::cout << "\t[*] 压入" << std::endl;
    queue.push(buff, size);
    memset(buff, 0, size);
    size = sizeof(buff);
    std::cout << "\t[*] 弹出" << std::endl;
    queue.pop(buff, size);
    assert(!std::memcmp(buff, "hello", sizeof(buff)));
    size = sizeof(buff);
    std::cout << "\t[*] 弹出超时" << std::endl;
    assert(!queue.pop(buff, size, 10));
    assert(errno == ETIMEDOUT);

    std::cout << "\t[*] 使用者打开" << std::endl;
    rc = user.open("/one_shm_queue");
    assert(rc);

    std::cout << "\t[*] 关闭创建者" << std::endl;
}
    size = sizeof(buff);
    std::cout << "\t[*] 队列仍可用" << std::endl;
    assert(!user.pop(buff, size, 10));
    assert(errno == ETIMEDOUT);
}

    if (1)
{
    std::cout << "=== 队列序列化操作 ===" << std::endl;
    shm::AnonMemoryQueue queue("/one_anon_queue");

    std::cout << "\t[*] 打包整数和浮点数" << std::endl;
    auto rc = queue.pack(32U, 1.1f);
    assert(rc == true);

    int32_t i;
    float f;

    std::cout << "\t[*] 解包整数和浮点数" << std::endl;
    rc = queue.unpack(i, f);

    assert(rc == true);
    assert(i = 32);
    assert(f == 1.1f);

    struct Data {
        uint8_t type;
        /*填充*/
        uint16_t version;
        uint32_t length;
        char payload[32];
    };

    Data data;

    std::cout << "\t[*] 没有数据时解包" << std::endl;
    rc = queue.unpack(i, data); /*可以打包、解包多个POD和基础类型的数据*/
    assert(rc == false);

    data.type    = 0xf1;
    data.version = 0xabad;
    data.length  = snprintf(data.payload, sizeof(data.payload),
        "%d, %s", 1, "helloworld");
    
    std::cout << "\t[*] 打包结构体" << std::endl;
    rc = queue.pack(data);
    assert(rc == true);

#if 0
    /*窥视有BUG*/
    std::cout << "\t[*] 窥视结构体" << std::endl;
    auto res = queue.peek<Data>(-1);
    assert(res.first == true);

    assert(res.second.type    == data.type);
    assert(res.second.version == data.version);
    assert(res.second.length  == data.length);
    assert(!std::memcmp(res.second.payload, data.payload, data.length));
    std::cout << "\t[*] 弹出丢弃" << std::endl;
    rc = queue.pop();
    assert(rc == true);
#else

    Data tmp;

    std::cout << "\t[*] 解包结构体" << std::endl;

    rc = queue.unpack(tmp);
    assert(rc == true);
    assert(tmp.type    == data.type);
    assert(tmp.version == data.version);
    assert(tmp.length  == data.length);
    assert(!std::memcmp(tmp.payload, data.payload, data.length));
#endif

    rc = queue.pop(0);
    assert(rc == false);
}

    if (1)
{
    std::cout << "=== 队列对象的ABA检查 ===" << std::endl;
    /*观察共享对象的ABA问题*/
    shm::ShareMemoryQueue user;
    shm::ShareMemoryQueue queue(512);

    std::cout << "\t[*] 创建" << std::endl;
    auto rc = queue.create("/one_shm_queue");
    assert(rc);

    std::cout << "\t[*] 打开" << std::endl;
    rc = user.open("/one_shm_queue");
    assert(rc);

    std::cout << "\t[*] 弹出超时" << std::endl;
    size_t  size;
    rc = user.pop(nullptr, size, 100);
    assert(!rc);

    std::cout << "\t[*] 弹出超时" << std::endl;
    rc = queue.push("1", 1);
    assert(rc);

    queue.close();

    rc = queue.create("/one_shm_queue");
    assert(rc);

    rc = queue.push("2", 1);
    assert(rc);

    uint8_t data;
    size = 1;
    rc = user.pop(&data, size, 0);
    assert(rc);
    /*应该是第一次的数据*/
    assert(data == '1');

    /*看不到第二次的数据*/
    rc = user.pop(nullptr, size, 100);
    assert(!rc);

    user.close();

    /*重新打开*/
    rc = user.open("/one_shm_queue");
    assert(rc);

    size = 1;
    rc = user.pop(&data, size, 0);
    assert(rc);
    /*应该是第二次的数据*/
    assert(data == '2');
}

    // sync share memory benchmark
    if (1)
{
    std::cout << "=== 进程间共享内存通信 ===" << std::endl;
    shm::ShareMemoryQueue queue(512);
    auto rc = queue.create("/one_shm_queue");
    assert(rc == true);

    const char *DATAS[] = {"Message 1", "Message 2", "Message 3"};
    pid_t pid = ::fork();
    if (pid < 0) {
        perror("fork");
        return EXIT_FAILURE;
    } else if (pid == 0) { // 子进程（生产者）
        /*以打开模式创建*/
        shm::ShareMemoryQueue user;
        auto rc = user.open("/one_shm_queue");
        assert(rc == true);
        for (int i = 0; i < 16; i++) {
            auto j = i % 3;
            user.push(DATAS[j], strlen(DATAS[j]) + 1);
            printf("\t[*] Child  pushed: %s\n", DATAS[j]);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        ::_exit(0);
    } else { // 父进程（消费者）
        char data[32];
        for (int i = 0; i < 16; i++) {
            size_t size = sizeof(data);
            auto j = i % 3;
            queue.pop(data, size);
            assert(std::strcmp(data, DATAS[j]) == 0);
            printf("\t[!] Parent popped: %.*s\n", static_cast<int32_t>(size), data);
        }
        ::wait(NULL);
        // 清理
    }
}

    if (1)
{
    std::cout << "=== 进程间匿名内存通信 ===" << std::endl;
    shm::AnonMemoryQueue queue("/one_anon_queue", 512);

    const char *DATAS[] = {"Message 1", "Message 2", "Message 3"};
    pid_t pid = ::fork();
    if (pid < 0) {
        perror("fork");
        return EXIT_FAILURE;
    } else if (pid == 0) { // 子进程（生产者）
        /*以打开模式创建*/
        for (int i = 0; i < 16; i++) {
            auto j = i % 3;
            queue.push(DATAS[j], strlen(DATAS[j]) + 1);
            printf("\t[*] Child  pushed: %s\n", DATAS[j]);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        ::_exit(0);
    } else { // 父进程（消费者）
        char data[32];
        for (int i = 0; i < 16; i++) {
            size_t size = sizeof(data);
            auto j = i % 3;
            queue.pop(data, size);
            assert(std::strcmp(data, DATAS[j]) == 0);
            printf("\t[!] Parent popped: %.*s\n", static_cast<int32_t>(size), data);
        }
        ::wait(NULL);
        // 清理
    }
}

    return EXIT_SUCCESS;
}