//
//  workqueue.hpp
//
//  Copyright © 2025 zhoukai. All rights reserved.
//

#ifndef workqueue_hpp
#define workqueue_hpp

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>

// #define WQ_DEBUG

#ifndef likely
# define likely(x)   __builtin_expect(!!(x), 1)
#endif

#ifndef unlikely
# define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#include <errno.h>
#include <signal.h>
#include <pthread.h>
#ifdef _QNX_
#include <process.h>
#else
#include <sys/syscall.h>
#endif

#include <list>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <utility>
#include <algorithm>
#include <functional>
#include <system_error>
#include <condition_variable>


#define WQ_WORKQUEUE_VAR() \
    thread_local std::shared_ptr<wq::worker> wq::worker::current_

namespace wq {

struct work;
struct worker;
struct barrier;
struct workqueue;
using WorkPtr      = std::shared_ptr<work>;
using WorkerPtr    = std::shared_ptr<worker>;
using WorkQueuePtr = std::shared_ptr<workqueue>;
using UniqueMutex  = std::unique_lock<std::mutex>;

#define WQ_THROW(what) \
    throw std::invalid_argument(std::string("Incorrect status : [ ") + \
        what + " ], at (" + __PRETTY_FUNCTION__ + \
        std::to_string(__LINE__) + ")")

struct config {
    bool    ordered_{false};
    int8_t  nice_{0};
    uint8_t max_idle_{4};
    uint8_t min_idle_{2};
    uint8_t max_thds_{32};
    uint8_t keep_alive{2};
};

struct workqueue : public std::enable_shared_from_this<workqueue> {
    friend struct work;
    friend struct worker;
    friend struct barrier;
    /*默认使用CPU的核心数构建初始化的工作线程*/
    explicit workqueue(const config & cfg = config()) : cfg_(cfg) {
        if (!cfg_.max_thds_) cfg_.max_thds_ = 32;
        if (cfg_.ordered_) { cfg_.max_idle_ = cfg_.min_idle_ = 1; }
    }
    virtual ~workqueue(void);

    static uint64_t tick(void);
    /*如果当前是工作线程上下文，且需要进行比较耗时的计算，那么应该调用此函数，通知工作队列*/
    static void maybe_sleep(void);

    void launch(uint8_t nr = 0);
    bool flush(uint32_t ms = (-1U), bool wf = false);

    void stop(void);
    bool is_stopping(void) { return flags_ & STOPPING; }

    size_t nr_worker(void) const { return workers_.size(); }
    uint16_t nr_work(void) const { return insert_seq_ - remove_seq_; }
    uint8_t nr_idle(void) const { return nr_worker() - nr_running_; }
    uint8_t nr_active(void) const {
        if (unlikely(nr_running_ < (nr_flusher_ + nr_sleeping_))) {
            if (cfg_.ordered_) { return nr_running_; }
            return likely(nr_worker() && !nr_running_)?1:0;
        }
        return nr_running_ - (nr_flusher_ + nr_sleeping_);
    }
private:
    void notify(void);
    void show_info(const std::string &msg);
    bool wait(UniqueMutex &lck, uint32_t ms = 5000);
    UniqueMutex lock(void) { return UniqueMutex(mutex_); }
    void launch_worker(bool temp = false);
    bool remove_worker(WorkerPtr);
    bool do_sleep(bool sleep = true);
    void finish_sleep(bool sleep = true);
    void incr_worker(bool more = false);
    void wakeup_worker(void);
    uint8_t need_incr(void);
    bool need_more(uint8_t hint) const {
        if (cfg_.ordered_ && nr_running_ > 0) { return false; }
        return works_.size() && nr_active() <= hint;
    }
    void maybe_incr_worker(void);
private:
    enum {
        STOPPING = 0x1,
        PICKING  = 0x2,
    };

    struct waiter {
        std::condition_variable cond_;
        uint16_t insert_seq_;
        uint16_t remove_seq_;
        bool wait(UniqueMutex &lk, uint32_t ms) {
            auto end = std::chrono::steady_clock::now();
            end += std::chrono::milliseconds(ms);
            auto rc = cond_.wait_until(lk, end, [this]() {
                return insert_seq_ == remove_seq_;
            });
            return rc;
        }
    };

    using workList   = std::list<WorkPtr>;
    using waiterList = std::list<waiter*>;
    using workerMap  = std::unordered_map<std::thread::id, WorkerPtr>;

    config     cfg_;
    workList   works_;
    workerMap  workers_;
    waiterList waiters_;
    uint8_t    flags_{0};
    uint8_t    nr_running_{0};
    uint8_t    nr_flusher_{0};
    uint8_t    nr_sleeping_{0};

    std::atomic<uint16_t> insert_seq_{0};
    std::atomic<uint16_t> remove_seq_{0};

    std::mutex mutex_;
    std::condition_variable cond_;
    std::condition_variable sync_;

private:
    static inline uint8_t cpu_cores(void) {
        return (uint8_t)sysconf(_SC_NPROCESSORS_CONF);
    }
};

struct worker : public std::enable_shared_from_this<worker> {
    friend struct work;
    friend struct barrier;
    friend struct workqueue;
    static WorkerPtr & current(void) { return current_; }
    static bool is_sleeper(void) {
        return current_ && (current_->flags_ & worker::MAYBE_SLEEP);
    }
    static WorkQueuePtr current_wq(void) {
        if (current_) { return current_->wq_.lock(); } return nullptr;
    }
protected:
    enum {
        IS_TEMP     = 0x1U,
        IN_WQ       = 0x4U,
        MAYBE_SLEEP = 0x8U,
    };
    static WorkerPtr make_one(WorkQueuePtr wq, bool temp = false) {
        return std::shared_ptr<worker>(new worker(wq, temp));
    }
    explicit worker(WorkQueuePtr, bool temp);
    void process_barrier(void);
    bool move_barrier(WorkPtr);
    void process_work(WorkPtr &, bool move = true);
private:
    void task(void);
private:
    using workList  = std::list<WorkPtr>;
    uint8_t   flags_{0};
    uint64_t  nr_done_{0};
    workList  sched_works_;
    std::thread::id id_;
    std::thread thread_;
    work *    work_{nullptr}; /*当前运行的工作*/
    std::weak_ptr<workqueue> wq_;
    static thread_local WorkerPtr current_;
};

struct work : public std::enable_shared_from_this<work> {
    friend struct worker;
    friend struct workqueue;
    enum {
        PENDING_WQ = 0x1, /*已排队在 workqueue 内*/
        PENDING_WR = 0x2, /*已排队在 worker 调度内*/
        RUNNING    = 0x4, /*在运行*/
        FLUSHING   = 0x8, /*被等待*/
    };
    explicit work(WorkQueuePtr wq, std::function<void(void)> &&func = nullptr)
        : wq_(wq), func_(std::move(func)) { assert(wq); }
    virtual ~work(void) {}
    bool queue(void);
    uint32_t busy(void);
    bool flush(void) {
        auto wq = wq_.lock();
        if (unlikely(!wq)) { return false; }
        auto lk = wq->lock();
        return do_flush(wq, lk);
    }
    bool cancel(bool async = true);
    bool queue(std::function<void(void)> &&func) {
        func_ = std::move(func);
        return queue();
    }
    WorkQueuePtr wq(void) { return wq_.lock(); }
    static inline void exec(WorkQueuePtr wq, std::function<void(void)> && fn) {
        auto async = std::make_shared<work>(wq);
        async->queue(std::forward<std::function<void(void)>>(fn));
    }
protected:
    explicit work(void) { }
    virtual void task(void) { func_(); }
    bool on_flushing(void);
    bool do_cancel(bool async = true);
    bool do_flush(WorkQueuePtr &, UniqueMutex &);
    std::weak_ptr<worker>     wr_;
    std::weak_ptr<workqueue>  wq_;
    std::function<void(void)> func_;
private:
    using workList  = std::list<WorkPtr>;
    uint32_t flags_{0};
    workList waiters_;
};

struct barrier : public work {
    barrier(WorkPtr wk) : target_(wk) {}
    WorkPtr target(void) { return target_; }
    void wait(UniqueMutex & lk) {
        cond_.wait(lk, [this](void) { return nofity_; });
    }
private:
    bool    nofity_{false};
    WorkPtr target_;
    std::condition_variable cond_;
    void task(void) override {
        auto & wr = worker::current();
        if (unlikely(wr != wr_.lock())) {
            WQ_THROW("The owning worker is not the current.");
        }
        auto wq = wr->wq_.lock();
        if (likely(wq)) {
            auto lk = wq->lock();
            nofity_ = true;
            cond_.notify_all();
        }
    }
};

inline void workqueue::launch(uint8_t nr)
{
    auto lk = lock();
    if (nr == 0) { nr = cpu_cores(); }
    if (nr > 4 || unlikely(nr == 0)) { nr = 4; }
    if (cfg_.ordered_) { nr = 1; }
    for (uint8_t i = 0; i < nr; i++) { launch_worker(); }
    if (workers_.empty()) { WQ_THROW("can't create thread");}
    flags_ &= ~STOPPING;
}

inline void workqueue::stop(void)
{
    auto lk = lock();
    flags_ |= STOPPING;
    cond_.notify_all();
    sync_.wait(lk, [this](void) { return !workers_.size(); });
}

inline workqueue::~workqueue(void)
{
    auto mutex = lock();
    fprintf(stderr, "workqueue to been destroy\n");
    flags_ |= STOPPING;
    while (nr_worker()) {
        auto worker = (*workers_.begin()).second;
        workers_.erase(workers_.begin());
        worker->flags_ &= ~worker::IN_WQ;
        cond_.notify_all();
        mutex.unlock();
        worker->thread_.join();
        mutex.lock();
    }
    if (works_.size()) { fprintf(stderr, "Oops!!! works > 0.\n"); }
    if (nr_worker()) { fprintf(stderr, "Oops!!! workers > 0.\n"); }
    if (insert_seq_ != remove_seq_) {
        fprintf(stderr, "Oops!!! I[%d] != R[%d].\n",
                insert_seq_.load(), remove_seq_.load());
    }
}

inline void workqueue::show_info(const std::string &msg)
{
#ifdef WQ_DEBUG
    static thread_local long tid = -1;
    if (unlikely(tid == -1)) {
#if defined(_QNX_)
        tid = ::gettid();
#elif defined(__APPLE__)
        tid = ::syscall(SYS_thread_selfid);
#else /*LINUX*/
        tid = ::syscall(SYS_gettid);
#endif
    }
    auto & wk = worker::current();
    fprintf(stderr, "[%llu] %s[%ld%c]:worker[%d], works[%d], running[%d/%d],"
        " flusher[%d], idle[%d]\n", static_cast<unsigned long long>(tick()),
        msg.c_str(),tid, wk ?'*':'?',
        static_cast<int32_t>(workers_.size()), static_cast<int32_t>(works_.size()),
        nr_running_,nr_sleeping_,nr_flusher_,nr_idle());
#endif
}

inline void workqueue::notify(void)
{
    auto seq = ++remove_seq_;
    if (likely(!waiters_.size())) return;
    waiters_.remove_if([=](waiter* wt) {
        if (wt->insert_seq_ != seq) { return false; }
        show_info(std::to_string((uintptr_t)this) + " notify to " + 
            std::to_string((uintptr_t)wt) + "at I[" +
            std::to_string(wt->insert_seq_) + "], R[" +
            std::to_string(wt->remove_seq_) +"]");
        wt->remove_seq_ = seq;
        wt->cond_.notify_one();
        return true;
    });
}

inline bool workqueue::wait(UniqueMutex &lk, uint32_t ms)
{
    auto end = std::chrono::steady_clock::now();
    end += std::chrono::milliseconds(ms);
    nr_running_--;
    auto rc = cond_.wait_until(lk, end, [this]() {
        return works_.size() || (flags_ & STOPPING);
    });
    nr_running_++;
    return rc;
}

inline void workqueue::launch_worker(bool temp)
{
    auto wr = worker::make_one(this->shared_from_this(), temp);
    if (likely(wr->flags_ & worker::IN_WQ)) { workers_.emplace(wr->id_, wr); }
}

inline bool workqueue::remove_worker(WorkerPtr wr)
{
    if (workers_.erase(wr->id_)) { wr->flags_ &= ~worker::IN_WQ; return true; }
    return false;
}

inline uint8_t workqueue::need_incr(void)
{
    auto nr_worker = this->nr_worker();
    if (unlikely(nr_worker >= cfg_.max_thds_)) {
#ifdef WQ_DEBUG
        show_info("Too many threads " + std::to_string(cfg_.max_thds_));
#else
        //fprintf(stderr, "Too many threads [%zu/%d]\n", nr_worker, cfg_.max_thds_);
#endif
        return 0;
    }
    auto nr_idle = nr_worker - nr_running_;
    if (worker::is_sleeper()) { return likely(nr_idle > 0) ? 0 : 1; }
    uint8_t idle = 0;
    auto & wr = worker::current();
    if (wr) {
        if (cfg_.ordered_) { return 0; }
        if (!wr->work_) { idle = 1; } /*worker 中的回收测试路径，算idle */
    }
    if (likely((nr_idle + idle) >= cfg_.min_idle_)) { return 0; }
    return cfg_.min_idle_ - (nr_idle + idle);
}

inline void workqueue::incr_worker(bool more)
{
    uint8_t i;
    auto nr = need_incr();
    for (i = 0; (unlikely(more) && i < cfg_.max_idle_) || i < nr; i++) {
        launch_worker(true);
    }
    if (i == 0 && need_more(1)) { cond_.notify_one(); }
}

inline void workqueue::maybe_incr_worker(void)
{
    /*TODO: 需要一个更好的检查是否需要启动增长的算法*/
    if (cfg_.ordered_) { return; }
    if (unlikely(nr_idle() == 0)) { incr_worker(); }
    auto hint = static_cast<uint8_t>(works_.size());
    if (need_more(hint)) { incr_worker(); }
}

inline void workqueue::wakeup_worker(void)
{
    /*延迟创建*/
    if (unlikely(!nr_worker())) { incr_worker(true); return; }
    auto nr_act = nr_active();
    if (cfg_.ordered_ && nr_act) { return; }
    auto works = works_.size();
    /*活动线程少，TODO:创建标志？异步创建？*/
    if (unlikely((nr_act << 1) < works)) { incr_worker(); return; }
    /*太多活动的任务线程，或没有活动线程则不唤醒*/
    if (!nr_idle() || nr_act > works) { return; }
    show_info("wakeup worker");
    cond_.notify_one();
}

inline uint64_t workqueue::tick(void)
{
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();
    return static_cast<uint64_t>(duration);
}

inline bool workqueue::do_sleep(bool sleep)
{
    auto & wk = worker::current();
    if (!wk || unlikely((wk->flags_ & worker::MAYBE_SLEEP))) { return false; }
    /*退出后自动解除*/
    if (sleep) { nr_sleeping_ += 1; } else { nr_flusher_  += 1; }
    wk->flags_ |= worker::MAYBE_SLEEP;
    show_info("maybe sleep");
    maybe_incr_worker();
    return true;
}

inline void workqueue::finish_sleep(bool sleep)
{
    auto & wk = worker::current();
    if (unlikely(!wk) || !(wk->flags_ & worker::MAYBE_SLEEP)) { return; }
    wk->flags_ &= ~worker::MAYBE_SLEEP;
    if (sleep) { nr_sleeping_ -= 1; } else { nr_flusher_ -= 1; }
}

inline void workqueue::maybe_sleep(void)
{
    auto & wk = worker::current();
    if (wk && likely(!(wk->flags_ & worker::MAYBE_SLEEP))) {
        auto wq = wk->wq_.lock();
        auto lk = wq->lock();
        wq->do_sleep();
    }
}

inline bool workqueue::flush(uint32_t ms, bool wf)
{
    auto & wk = worker::current();
    if (unlikely(wk && wk->wq_.lock() == shared_from_this())) {
        WQ_THROW("wait for itself in a worker within its own workqueue.");
    }
    bool rc = true;
    auto lk = lock();
    bool do_sleep = this->do_sleep();
    do {
        waiter wt;
        wt.insert_seq_ = insert_seq_;
        wt.remove_seq_ = remove_seq_;
        waiters_.push_back(&wt);
        show_info(std::string("flush wq : I[") + std::to_string(wt.insert_seq_)
                + "], R[" + std::to_string(wt.remove_seq_) + "]");
        rc = wt.wait(lk, ms);
        waiters_.remove(&wt);
        show_info(std::string("flush finish :") + (rc?"success":"failed"));
        if (!wf || works_.empty()) { break; }
    } while (true);
    if (do_sleep) { finish_sleep(); }
    return rc;
}

inline worker::worker(std::shared_ptr<workqueue> wq, bool temp)
    : flags_(temp?IS_TEMP:0)
    , wq_(wq)
{
    thread_ = std::thread(std::bind(&worker::task, this));
    if (unlikely(!thread_.joinable())) {
        fprintf(stderr, "create thread failed : %s\n", strerror(errno));
        return;
    }
    wq->show_info(std::string("create [") + (temp?"T":"*") + "] worker");
    flags_ |= IN_WQ;
    id_     = thread_.get_id();
}

inline bool worker::move_barrier(std::shared_ptr<work> wk)
{
    auto wr = wk->wr_.lock();

    if (wr && unlikely(wr.get() != this && wk.get() == wr->work_)) {
        wr->wq_.lock()->show_info("collision[!!]");
    } else {
        wr = nullptr;
    }

    auto _this = this->shared_from_this();
    if (!wr) { wr = _this; }
    wr->sched_works_.splice(wr->sched_works_.cend(), wk->waiters_);
    if (wr != _this) {
        wk->flags_ &= ~work::PENDING_WQ;
        wk->flags_ |=  work::PENDING_WR;
        wr->sched_works_.push_front(wk);
        wr->wq_.lock()->show_info("collision[ok]");
        return false;
    }

    return true;
}

inline void worker::process_work(std::shared_ptr<work> & wk, bool move)
{
    bool isbar = false;
    if (!wk->wq_.lock()) {
        isbar = true;
    } else if (move && !move_barrier(wk)) {
        return;
    }
    wk->wr_ = this->shared_from_this();
    work_   = wk.get();
    auto wq = wq_.lock();
    wq->show_info(std::string("exec start") + (unlikely(isbar)?"[B]":"[T]"));
    wk->flags_ &= ~(work::PENDING_WQ|work::PENDING_WR);
    wq->mutex_.unlock();
    nr_done_++;
    try { wk->task(); } catch(...) { fprintf(stderr, "Oops!!!execute."); }
    wk.reset();
    wq->mutex_.lock();
    wq->finish_sleep();
    work_   = nullptr;
    wq->show_info("exec end");
    /*因为此处，所以从队列中删除时，可能不需要递增移除计数和通知*/
    if (!isbar) { wq->notify(); }
}

inline void worker::process_barrier(void)
{
    while (sched_works_.size()) {
        auto wk = sched_works_.front();
        sched_works_.pop_front();
        process_work(wk, false);
    }
}

inline void worker::task(void)
{
    sigset_t full;
    sigfillset(&full);
    sigdelset(&full, SIGILL);
    sigdelset(&full, SIGBUS);
    sigdelset(&full, SIGSEGV);
    pthread_sigmask(SIG_BLOCK, &full, NULL);
    auto wq = wq_.lock();
    /*设置优先级*/
    if (wq->cfg_.nice_) {
        int32_t type = 0;
        struct sched_param params;
        pthread_getschedparam(pthread_self(), &type, &params);
        auto min_prio = sched_get_priority_min(type);
        auto max_prio = sched_get_priority_max(type);
        int32_t prio = params.sched_priority - wq->cfg_.nice_;
        prio = std::max(min_prio, std::min(max_prio, prio));
#ifdef _QNX_
        if (prio > 50) { prio = 50; }
#endif
        params.sched_priority = prio;
        if (pthread_setschedparam(pthread_self(), type, &params)) {
            fprintf(stderr, "Can't set priority of worker to %d\n", prio);
        }
    }
    auto lk = wq->lock();
    int32_t to = flags_&IS_TEMP?(wq->cfg_.keep_alive * 1000):-1;
    wq->nr_running_++;
    current_ = this->shared_from_this();
#ifdef WQ_DEBUG
    wq->show_info("one worker start.");
#else
    fprintf(stderr, "one worker start.\n");
#endif
    do {
        /*执行完才会退出循环*/
        if (unlikely(wq->is_stopping()) && wq->works_.empty()) { break; }
        if (wq->cfg_.ordered_ && wq->nr_active() > 1) { continue; }
        std::shared_ptr<work> wk; /*延迟释放*/
        if (wq->works_.size()) {
            wk = wq->works_.front();
            wq->works_.pop_front();
            process_work(wk);
            process_barrier();
        }
        if (!wq->wait(lk, to)) { if (to > 0 && !wq->need_incr()) { break; }}
    } while (1);
#ifdef WQ_DEBUG
    wq->show_info("one worker exit [" + std::to_string(nr_done_) + "]");
#else
    fprintf(stderr, "one worker exit [%llu].\n", nr_done_);
#endif
    wq->nr_running_--;
    if (likely(flags_ & IN_WQ)) {
        wq->remove_worker(current_);
        thread_.detach();
    }
    wq->sync_.notify_all();
    current_ = nullptr;
}

uint32_t work::busy(void)
{
    uint32_t stat = 0;
    auto wq = wq_.lock();
    if (!wq) { return stat; }
    stat = flags_ & (PENDING_WQ|PENDING_WR);
    if (on_flushing()) { stat |= FLUSHING; }
    auto wr = wr_.lock();
    if (wr && wr->work_ == this) { stat |= RUNNING; }
    return stat;
}

/*是否正在被 flush*/
inline bool work::on_flushing(void)
{
    auto wr = wr_.lock();
    if (flags_ & PENDING_WQ) {
        if (waiters_.size()) return true;
    } else if (wr) {
        if (!wr->sched_works_.size()) {
            if (unlikely(flags_ & PENDING_WR)) { WQ_THROW("Not in a worker."); }
            return false;
        }
        /*
         * 必定在最后一个，因为 worker 调度队列只能有一个 work，且为当前worker上
         * 运行的 work ， 其他的应该全是 当前运行的 barrier
         */
        auto last  = wr->sched_works_.back();
        auto _this = this->shared_from_this();
        if (!last->wq_.lock()) {
            /*没有 wq 则为一个 barrier */
            return std::static_pointer_cast<barrier>(last)->target() == _this;
        } else if (last == _this) {
            if (unlikely(!(flags_ & PENDING_WQ))){WQ_THROW("Not in a worker.");}
        }
    }
    return false;
}

inline bool work::do_flush(WorkQueuePtr & wq, UniqueMutex & lk)
{
    std::shared_ptr<barrier> bar;
    if (flags_ & work::PENDING_WQ) {
        bar = std::make_shared<barrier>(this->shared_from_this());
        waiters_.push_back(bar);
    } else {
        auto wr = wr_.lock();
        if (wr && (this == wr->work_ ||
                unlikely(flags_ & work::PENDING_WR))) {
            bar = std::make_shared<barrier>(this->shared_from_this());
            wr->sched_works_.push_back(bar);
        } else {
            return false;
        }
    }
    bool do_sleep = false;
    wq->show_info("flush start");
    auto & wr = worker::current();
    if (wr) {
        /*不能在自身任务中阻塞自己*/
        if (unlikely(wr->work_ == this)) { WQ_THROW("Recursive waiting."); }
        do_sleep = wq->do_sleep(false);
    }
    bar->wait(lk);
    if (do_sleep) {
        wq->finish_sleep(false);
    }
    wq->show_info("flush end");
    return true;
}

inline bool work::do_cancel(bool async)
{
    auto wq = wq_.lock();
    if (unlikely(!wq)) { WQ_THROW("Not in a workqueue."); }
    if (!(flags_ & (PENDING_WQ|PENDING_WR))) { return false; }
    auto wr = wr_.lock();
    if ((flags_ & PENDING_WR) && unlikely(!wr)){ WQ_THROW("Not in a worker."); }
    auto & works = flags_ & PENDING_WQ ? wq->works_ : wr->sched_works_;
    auto it = std::find(works.begin(),works.end(),this->shared_from_this());
    if ((flags_ & PENDING_WQ) && unlikely(it == works.end())) {
        WQ_THROW("Not in a workqueue.");
    } else if ((flags_ & PENDING_WR) && unlikely(it != works.begin())) {
        WQ_THROW("Not in a worker.");
    }
    works.erase(it);
    flags_ &= ~(PENDING_WQ|PENDING_WR);
    /*从队列上删除成功，则递增移除计数器，并通知*/
    wq->notify();
    return true;
}

inline bool work::cancel(bool async)
{
    bool cl = false, fl = false;
    auto wq = wq_.lock();
    if (unlikely(!wq)) { return false; }
    auto lk = wq->lock();
    /*根据是否同步来决定是否延迟递增移除计数器，保证wq.flush语义正确*/
    cl = do_cancel(async);
    if (!async) { fl = do_flush(wq, lk); }
    return cl?cl:fl;
}

inline bool work::queue(void)
{
    auto wq    = wq_.lock();
    auto mutex = wq->lock();
    /*已经在队列中*/
    if (flags_ & (PENDING_WQ | PENDING_WR)) {
        wq->show_info("Work in a worker already.");
        return false;
    }
    /*正在被等待*/
    if (on_flushing()) {
        wq->show_info("Work on flushing.");
        return false;
    }
    /*排队*/
    flags_ |= PENDING_WQ;
    wq->show_info("queue start");
    wq->insert_seq_++;
    wq->works_.push_back(this->shared_from_this());
    wq->wakeup_worker();
    wq->show_info("queue end");
    return true;
}

}

#endif
