#include <chrono>
#include <iostream>
#include "ringbuffer.hpp"

struct Object {

    Object(void) {
        auto nr = ++nr_objs;
#ifdef DEBUG
        fprintf(stdout, "-->> %d\n", nr);
#endif
    }

    virtual ~Object(void) {
        auto nr = --nr_objs;
#ifdef DEBUG
        fprintf(stdout, "<<-- %d\n", nr);
#endif
    }

    int32_t id{0};

    static int32_t nr_objs;
};

int32_t Object::nr_objs = 0;

int main(void)
{
    /* buffer 基准测试*/
    int32_t i;
    /*容器*/
    if (1)
{
    std::cout << "=== 与容器的交互 ===" << std::endl;
    ring::buffer<std::vector<char>, 10> queue;

    const std::vector<char> src{'0', '1', '2', '3', '4', '\0'};
    queue.push(src);

{
    std::vector<char> data{'0', '1', '2', '3', '4', '\0'};
    queue.push(std::move(data));
}

    for (i = 2; i < 1024; i++) {
        if (!queue.push({'0', '1', '2', '3', '4', '\0'}))
            break;
    }

    assert(i == 1023);

    for (i = 0; i < 1024; i++) {
        std::vector<char> data;
        if (!queue.pop(data))
            break;
        assert(data == src);
#ifdef DEBUG
        fprintf(stdout, "--> %d : %s\n", i, data.data());
#endif
    }

    assert(i == 1023);
}

    /*基础类型*/
    if (1)
{
    std::cout << "=== 与指针的交互 ===" << std::endl;
    ring::buffer<Object*, 10> queue;

    for (i = 0; i < 1024; i++) {
        auto data = new Object();
        if (!queue.push(data)) {
            delete data;
            break;
        }
    }

    assert(i == 1023);
    assert(Object::nr_objs == i);

    for (i = 0; i < 1024; i++) {
        Object * data;
        if (!queue.pop(data))
            break;
        delete data;
    }

    assert(i == 1023);
    assert(Object::nr_objs == 0);
}

    /*对象*/
    if (1)
{
    std::cout << "=== 与对象(shared_ptr<>)的交互 ===" << std::endl;
    ring::buffer<std::shared_ptr<Object>, 10> queue;

    for (i = 0; i < 1024; i++) {
        if (!queue.push(std::make_shared<Object>())) {
            break;
        }
    }

    assert(i == 1023);
    assert(Object::nr_objs == i);

    for (i = 0; i < 1024; i++) {
        std::shared_ptr<Object> data;
        if (!queue.pop(data))
            break;
    }

    assert(i == 1023);

    assert(Object::nr_objs == 0);
}

    /*循环所有的slot测试*/
    if (1)
{
    std::cout << "=== 循环整个slot ===" << std::endl;
    ring::buffer<int32_t, 10> queue;
    for (i = 0; i < 1U << 20;) {
        if (!queue.push(i)) {
            int32_t v;
            queue.pop(v);
        } else {
            i++;
        }
    }

    assert(queue.size() == queue.capacity());
    i -= queue.size();

    int32_t j;
    for (j = 0; j < queue.capacity(); j++, i++) {
        int32_t v;
        if (!queue.pop(v)) {
            break;
        }
        assert(v == i);
    }

    assert(j == queue.capacity());
}

    /*blocking_buffer 基准测试（仅超时）*/
    if (1)
{
    std::cout << "=== 阻塞验证 ===" << std::endl;
    ring::blocking_buffer<int32_t, 4> queue("ringbuffer_benchmark");

    int32_t val;
    auto start_time = std::chrono::steady_clock::now();
    auto rc = queue.pop(val, 100);
    assert(!rc);
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time).count();
    std::cout << "\t阻塞弹出耗时(毫秒):" << duration << std::endl;
    assert(duration >= 100);

    rc = queue.push(100);
    assert(rc);

    start_time = std::chrono::steady_clock::now();
    rc = queue.pop(val, 100);
    assert(rc);
    end_time = std::chrono::steady_clock::now();
    duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            end_time - start_time).count();
    
    std::cout << "\t非阻塞弹出耗时(纳秒):" << duration << std::endl;
    assert(duration < 1000);
}

    return EXIT_SUCCESS;
}
