#include "workqueue.hpp"

WQ_WORKQUEUE_VAR();

#define NR_WORK   16
#define NR_WORKER 4

int main(void)
{
    wq::config cfg;

    auto wq1 = std::make_shared<wq::workqueue>(cfg);
    /*立即创建一些线程*/
    wq1->launch(NR_WORKER);

    /*等待启动完成*/
    cfg.ordered_ = true;
    /*延迟启动线程*/
    //auto wq2 = std::make_shared<wq::workqueue>(cfg);

    /*等待所有工作线程启动*/
    std::this_thread::sleep_for(std::chrono::microseconds(100));

    if (1)
{
    /*排队并等待任务完成*/
    fprintf(stderr, "--------排队等待任务完成--------\n");
    struct Testask : public wq::work {
        using work::work;
        volatile bool finish_{false};
    private:
        void task(void) override {
            finish_ = true;
            /*立即执行，flush 不会实际的发送等待*/
            fprintf(stderr, "\t任务执行\n");
        }
    };
    auto task = std::make_shared<Testask>(wq1);
    fprintf(stderr, "\t任务排队\n");
    auto rc = task->queue();
    assert(rc);
    fprintf(stderr, "\t完成排队\n");

    /*晚一点等待，finish返回一定指示work已经执行完成*/
    std::this_thread::sleep_for(std::chrono::microseconds(100));

    fprintf(stderr, "\t同步任务\n");
    rc = task->flush();
    assert(!rc);
    fprintf(stderr, "\t同步完成\n");

    assert(task->finish_);
}

    if (1)
{
    fprintf(stderr, "--------排队等待任务完成--------\n");
    struct Testask : public wq::work {
        using work::work;
        volatile bool finish_{false};
    private:
        void task(void) override {
            /*晚一点执行，finish会执行等待完成*/
            fprintf(stderr, "\t任务执行\n");
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            finish_ = true;
        }
    };
    auto task = std::make_shared<Testask>(wq1);

    fprintf(stderr, "\t任务排队\n");
    auto rc = task->queue();
    assert(rc);
    fprintf(stderr, "\t完成排队，开始同步\n");

    rc = task->flush();
    assert(rc);
    fprintf(stderr, "\t同步完成\n");

    assert(task->finish_);
    std::this_thread::sleep_for(std::chrono::microseconds(100));
}

    if (1)
{
    fprintf(stderr, "--------不能对已排队的任务再次排队--------\n");
    std::shared_ptr<wq::work> work;
    for (int8_t i = 0; i < NR_WORKER + 1; i++) {
        /*排队中的任务不能再次排队*/
        work = std::make_shared<wq::work>(wq1, [=](void) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });
        auto rc = work->queue();
        assert(rc);
    }

    auto rc = work->queue();
    assert(!rc);

    auto stat = work->busy();
    assert(stat & wq::work::PENDING_WQ);

    work->flush();
    std::this_thread::sleep_for(std::chrono::microseconds(100));
}

    if (1)
{
    /*
     * 最后一个排队的任务不会立即执行，因为当前上下文不是工作线程上下文
     * 而且为了锁的争抢和上下文切换，正在工作的线程数大于排队的任务数，
     * 则不会额外唤醒多
     */
    fprintf(stderr, "--------取消已排队的任务--------\n");
    volatile std::atomic<uint8_t> nr_exec{0};
    std::vector<std::shared_ptr<wq::work>> tasks;
    for (size_t i = 0; i < NR_WORKER + 1; i++) {
        auto task = std::make_shared<wq::work>(wq1, [&](void) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            nr_exec++;
        });
        tasks.push_back(std::move(task));
    }

    for (auto &&task : tasks) {
        task->queue();
    }
    
    /*休眠一段时间，最后一个任务也可取消*/
    std::this_thread::sleep_for(std::chrono::microseconds(500));
    auto stat = tasks.back()->busy();
    assert(stat == wq::work::PENDING_WQ);

    auto rc = tasks.back()->cancel(true);
    assert(rc);

    wq1->flush();
    assert(nr_exec = NR_WORKER);
    std::this_thread::sleep_for(std::chrono::microseconds(100));
}

    if (1)
{
    fprintf(stderr, "--------取消已执行的任务--------\n");
    volatile bool exec = false;
    auto task = std::make_shared<wq::work>(wq1, [&](void) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        exec = true;
    });
    auto rc = task->queue();
    assert(rc);
    while (task->busy() & wq::work::PENDING_WQ) { std::this_thread::yield(); }
    rc = task->cancel(true);
    assert(!rc);
    assert(!exec);
    /*取消已执行的任务，如果是同步取消，则与 flush 一样的效果*/
    rc = task->cancel(false);
    assert(rc);
    assert(exec);
    std::this_thread::sleep_for(std::chrono::microseconds(100));
}

    if (1)
{
    fprintf(stderr, "--------工作线程的增长与收缩--------\n");
    auto task = std::make_shared<wq::work>(wq1, [=]{
        /*模拟繁重的计算*/
        std::this_thread::sleep_for(std::chrono::seconds(1));
        fprintf(stderr, "\ttarget finish\n");
    });

    task->queue();
    for (int32_t i = 0; i < NR_WORKER - 1; i++) {
        /*在工作线程内等待其他任务，迫使线程自动增长*/
        auto waiter = std::make_shared<wq::work>(wq1, [=](void){
            task->flush();
            fprintf(stderr, "\twaiter finish\n");
        });
        waiter->queue();
        while (waiter->busy() & wq::work::PENDING_WQ) {
            std::this_thread::yield();
        }
    }

    /*等待所有任务都执行, 那么会因为task->flush一定会创建一些工作线程*/
    task->flush();

    assert(wq1->nr_worker() > NR_WORKER);

    /*默认两秒后就会回收临时的工作线程*/
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));

    assert(wq1->nr_worker() == NR_WORKER);
    std::this_thread::sleep_for(std::chrono::microseconds(100));
}

    if (1)
{
    /*并且可以解决执行冲突*/
    fprintf(stderr, "--------正在运行的任务可以递归排序--------\n");
    struct Testask : public wq::work {
        using work::work;
        uint8_t nr_{0};
    private:
        void task(void) override {
            if (++nr_ < 10) {
                this->queue();
            }
        }
    };

    std::vector<std::shared_ptr<Testask>> tasks;

    for (size_t i = 0; i < 10; i++)
    {
        auto task = std::make_shared<Testask>(wq1);
        tasks.push_back(std::move(task));
    }
    for (auto &&task: tasks) {
        task->queue();
    }

    /*最多等待500毫秒，否则视为失败*/
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    for (auto &&task: tasks) {
        task->flush();
        assert(task->nr_ == 10);
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
}

    wq1->stop();

    return EXIT_SUCCESS;
}