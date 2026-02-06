//
//  reactor.hpp
//
//
//  Created by zhoukai
//

#ifndef qevent_reactor_hpp
#define qevent_reactor_hpp

#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <assert.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/poll.h>
#include <sys/time.h>

#include <set>
#include <map>
#include <list>
#include <array>

#include <mutex>
#include <chrono>
#include <thread>
#include <atomic>
#include <bitset>
#include <memory>
#include <exception>
#include <algorithm>
#include <functional>
#include <system_error>
#include <unordered_map>
#include <condition_variable>

#ifndef likely
# define likely(x)   __builtin_expect(!!(x), 1)
#endif

#ifndef unlikely
# define unlikely(x) __builtin_expect(!!(x), 0)
#endif

/*
 * 必须在函数体外，源文件内包含一次，
 * 建议在可执行程序的 main() 函数所在文件中包含
 */
#define QEVENT_REACTOR_VAR() \
    std::mutex qevent::utils::all_mutex_; \
    std::set<qevent::reactor*> qevent::utils::all_reactors_; \
    thread_local std::atomic<uint8_t> qevent::utils::signal_depth_

namespace qevent {

constexpr int32_t EV_READ  = POLLIN;
constexpr int32_t EV_WRITE = POLLOUT;
constexpr int32_t EV_PRI   = POLLPRI;
constexpr int32_t EV_HUP   = POLLHUP;
constexpr int32_t EV_ERROR = POLLERR;

class reactor;
/*不变工具类*/
struct utils {
    friend class reactor;
private:
    static std::mutex                        all_mutex_;
    static std::set<reactor*>                all_reactors_;
    static thread_local std::atomic<uint8_t> signal_depth_;
    static inline void for_each(std::function<void(reactor*)> && func) {
        for (auto &reactor : all_reactors_) { func(reactor); }
    }

public:
    typedef void (*signalFunc)(int32_t);

    /*a绝对时间在b绝对时间之后*/
    template<typename A, typename B>
    static bool time_after64(A a, B b) { return ((int64_t)(b)-(int64_t)(a))<0; }
    /*a绝对时间在b绝对时间之前*/
    template<typename A, typename B>
    static bool time_before64(A a,B b) { return time_after64(b,a); }
    static inline void signalHandle(int32_t signo);
    static inline int32_t pipe2(int32_t fd[2], int32_t flags);
    static inline signalFunc setupSignal(int32_t signo, signalFunc cb);
};

static inline uint64_t sys_clock(struct timespec *ts = nullptr);

class reactor {
    friend struct utils;
    using lock_guard = std::lock_guard<std::mutex>;
    using unique_lock = std::unique_lock<std::mutex>;
public:
    using task = std::function<void(int32_t)>;
    using timerId = std::pair<uint32_t, uint64_t>;
    struct barrier {
        bool stat_{false};
        std::condition_variable cond_;
        void notify(void) { stat_ = true; cond_.notify_one(); }
        bool wait(unique_lock & lk, uint32_t ms = -1U) {
            auto end = std::chrono::steady_clock::now();
            end += std::chrono::milliseconds(ms);
            return cond_.wait_until(lk, end, [=]() { return stat_; });
        }
    };

    struct status {
        /*统计信息*/
        uint32_t version_{0};
        uint32_t nr_poll_{0};
        uint32_t nr_done_{0};
        uint32_t nr_write_{0};
        uint32_t nr_read_{0};
    };

    explicit reactor(void);
    ~reactor(void) noexcept;

    /*IO事件接口函数*/
    template<typename Task>
    bool addEvent(int32_t fd, int32_t ev, Task && func);
    bool removeEvent(int32_t fd) noexcept;
    int32_t modEvent(int32_t fd, std::function<int32_t(int32_t)> &&) noexcept;
    int32_t checkEvent(int32_t fd) {
        return modEvent(fd, [](int32_t v){return v;});
    }
    int32_t modEvent(int32_t fd, int32_t ev) {
        return modEvent(fd, [ev](int32_t){return ev;});
    }
    int32_t enableEvent(int32_t fd, int32_t ev) {
        return modEvent(fd, [ev](int32_t old){return ev|old;});
    }
    int32_t disableEvent(int32_t fd, int32_t ev) {
        return modEvent(fd, [ev](int32_t old){return (~ev) & old;});
    }

    /*定时器事件接口函数*/
    template<typename Task>
    timerId addTimer(uint32_t expire, Task && func);
    /*func用于重置成功时，修改其他需要保护的数据*/
    bool resetTimer(timerId & id, uint32_t expire, task && func = nullptr) noexcept;
    bool removeTimer(timerId &) noexcept;

    /*SysV信号事件接口函数*/
    template<typename Task>
    void addSignal(int32_t signo, Task && func);
    void removeSignal(int32_t signo) noexcept;

    /*异步任务接口函数*/
    /*0为最高优先级*/
    template<typename Task>
    bool addAsync(Task && func, bool wakeup = true, uint8_t prior = 0);

    /*循环控制接口*/
    int32_t run(int32_t timeout = -1) noexcept;
    void stop(void) noexcept;
    /*外部只能中断循环，而不能发送其他通知信息*/
    void notify(void) { notify(0); }
    /*同步，保证Async已经完成*/
    bool sync(bool force = false, uint32_t ms = -1U);
    bool isTaskCtx(void) const { return tid_ == std::this_thread::get_id(); }
    /*移除所有事件*/
    void reset(bool exec = false) noexcept;

    status pollStatus(bool reset = true) {
        lock_guard guard(mutex_);
        auto tmp = status_;
        if (reset) { status_ = status(); }
        return tmp;
    }

    /*在即刻起的未来相对时间添加任务*/
    template<typename Task>
    timerId doAddTimer(uint64_t expire, Task && func);
    int32_t nextTimeout(uint32_t, uint64_t) const noexcept;
private:
    struct timerTask {
        timerId     id_;
        task        task_;
        timerTask(timerId id, task && task)
            : id_(id), task_(std::move(task)) {}
        timerTask(timerTask && other) {
            this->id_   = other.id_; this->task_ = std::move(other.task_);
        }
    };
    struct ioTask {
        int32_t     ev_;
        int32_t     version_;
        task        task_;
        ioTask(int32_t ev, uint32_t ver, task && task)
            : ev_(ev), version_(ver)
            , task_(std::move(task)) {}
        ioTask(ioTask && other) {
            this->ev_      = other.ev_;
            this->version_ = other.version_;
            this->task_    = std::move(other.task_);
        }
    };

    using AsyncQueue = std::list<task>;
    using AsyncArray = std::array<AsyncQueue, 8>;
    using EventArray = std::array<pollfd, 32>;
    using TimerMap   = std::multimap<uint64_t, timerTask>;
    using SignalMap  = std::unordered_map<uint8_t, task>;
    using EventMap   = std::unordered_map<int32_t, ioTask>;
    using SignalSet  = std::bitset<NSIG>;
    using PollfdPtr  = std::unique_ptr<pollfd, void(*)(pollfd *)>;

    struct PollEv {
        PollfdPtr kev_;
        int32_t   size_{0};
        int32_t   nr_{0};
        PollEv() : kev_(nullptr, [](pollfd*){}) { }
        PollEv(PollEv && ) = default;
        PollEv & operator = (PollEv &&) = default;
        PollEv(const PollEv & ) = delete;
        PollEv & operator = (const PollEv &) = delete;
    };

    std::atomic<uint8_t>  nr_nty_{0};
    std::atomic<uint32_t> nr_async_{0};

    /*统计信息*/
    status      status_;
    uint8_t     polling_{0};
    uint8_t     calling_{0};
    int32_t     fd_[2];
    uint32_t    id_{0};

    std::mutex  mutex_;
    std::thread::id tid_;

    SignalSet   asignal_;
    sigset_t    sigmask_; /**< 正在监控的信号*/

    AsyncArray  asyncs_;
    EventArray  kev_;
    EventMap    events_;
    TimerMap    timers_;
    SignalMap   signals_;
    AsyncQueue  waiters_;

    void flush(int32_t);
    /*对于正常的设计，在U32_MAX回绕之前，定时器肯定超时*/
    uint32_t genId(void) { if (unlikely(id_ == 0)) { id_ += 1; } return id_++; }
    void notify(uint8_t v) noexcept;
    int32_t eatSignal(unique_lock &) noexcept;
    int32_t eatTimer(unique_lock &) noexcept;
    void    prepareEvent(PollEv &);
    int32_t eatEvent(unique_lock &, PollEv &) noexcept;
    int32_t eatAsync(unique_lock &, uint32_t nr_max = 32) noexcept;
    reactor(const reactor &) = delete;
    reactor & operator = (const reactor &) = delete;
};

/*
 * Reactor 参数可以是代理，只要提供定时器操作函数即可
 */

template <typename Reactor>
struct restartTimer {
    restartTimer(Reactor loop = nullptr) : loop_(loop) { }
    restartTimer(restartTimer && other)
        : loop_(std::move(other.loop_))
        , interval_(other.interval_)
        , id_(other.id_)
        , task_(std::move(other.task_))
    { other.loop_ = nullptr; other.task_ = nullptr; }
    virtual ~restartTimer(void) { stop(); } /* 不变类型，可以是虚类 */
    bool stop(void) {
        return loop_ && loop_->removeTimer(id_);
    }
    bool alived(void) const { return !!id_.first; }
    bool reset(int32_t interval) {
        return loop_ && loop_->resetTimer(id_, interval, [=](uint32_t) {
            interval_ = interval;
        });
    }
    void launch(int32_t interval, reactor::task && func) {
        assert(loop_ && !id_.first);
        interval_ = interval;
        task_  = std::move(func);
        id_    = loop_->addTimer(interval_,
                    std::bind(&restartTimer<Reactor>::execute, this,
                    std::placeholders::_1));
    }
    void launch(int32_t interval, reactor::task & func) {
        launch(interval, std::forward<reactor::task>(func));
    }
protected:
    Reactor          loop_{nullptr};
    uint32_t         interval_{0};
    reactor::timerId id_{0,0};
    reactor::task    task_{nullptr};

    void execute(uint32_t ev) {
        if (likely(task_)) { task_(ev); }
        id_ = loop_->addTimer(interval_,
                    std::bind(&restartTimer<Reactor>::execute, this,
                    std::placeholders::_1));
    }
    restartTimer(const restartTimer &other) = delete;
    restartTimer & operator=(const restartTimer &other) = delete;
};

template <typename Reactor>
struct intervalTimer : public restartTimer<Reactor> {
    using restartTimer<Reactor>::restartTimer;
    intervalTimer(intervalTimer && other)
        : restartTimer<Reactor>(other)
        , start_(other.start_)
        , count_(other.count_)
    { }
    virtual ~intervalTimer(void) { } /* 不变类型，可以是虚类 */

    void launch(int32_t interval, reactor::task && func) {
        auto & loop = restartTimer<Reactor>::loop_;
        assert(loop && !restartTimer<Reactor>::id_.first);
        start_ = sys_clock();
        restartTimer<Reactor>::interval_ = interval;
        restartTimer<Reactor>::task_  = std::move(func);
        restartTimer<Reactor>::id_    = loop->addTimer((uint32_t)interval,
                    std::bind(&intervalTimer<Reactor>::execute, this,
                    std::placeholders::_1));
    }

    bool reset(int32_t interval) {
        auto & loop = restartTimer<Reactor>::loop_;
        return loop && loop->resetTimer(restartTimer<Reactor>::id_, interval,
                [=](uint32_t) {
            count_ = 1;
            start_ = sys_clock();
            restartTimer<Reactor>::interval_ = interval;
        });
    }
private:
    uint64_t start_;
    uint32_t count_{1};

    void execute(uint32_t ev) {
        if (restartTimer<Reactor>::task_) { restartTimer<Reactor>::task_(ev); }
        /*不论任务延迟如何，间隔定时器必须按预定时间触发*/
        auto expire = (uint64_t)restartTimer<Reactor>::interval_;
        expire = expire * (++count_) * 1000000 + start_;
        restartTimer<Reactor>::id_ = restartTimer<Reactor>::loop_->doAddTimer(
                expire, std::bind(&intervalTimer<Reactor>::execute,
                            this, std::placeholders::_1));
    }
    intervalTimer(const intervalTimer &other) = delete;
    intervalTimer & operator=(const intervalTimer &other) = delete;
};

inline reactor::reactor(void)
{
    if (unlikely(utils::signal_depth_)) {
        throw std::system_error(std::error_code(EBUSY,
            std::generic_category()), "Cannot build reactor in signal handle");
    }
    if (utils::pipe2(fd_, O_CLOEXEC|O_NONBLOCK)) {
        throw std::system_error(std::error_code(errno,
            std::generic_category()), "Cannot create pipe of reactor");
    }
    sigemptyset(&sigmask_);
    addEvent(fd_[0], EV_READ, std::bind(&reactor::flush, this,
            std::placeholders::_1));
    lock_guard guard(utils::all_mutex_);
    utils::all_reactors_.insert(this);
}

inline reactor::~reactor(void) noexcept
{
    assert(!utils::signal_depth_);
    {
        lock_guard _(utils::all_mutex_);
        utils::all_reactors_.erase(this);
    }
    reset();
    ::close(fd_[0]); ::close(fd_[1]);
}

inline void reactor::reset(bool exec) noexcept
{
    unique_lock mutex(mutex_);
    /*执行一些残留的任务*/
    if (exec) {
        eatTimer(mutex);
        eatAsync(mutex, UINT32_MAX);
    }
    for (auto & queue : asyncs_) { queue.clear(); }
    for (auto & kv : signals_) {
        auto signo = kv.first;
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, signo);
        sigdelset(&sigmask_, signo);
        utils::setupSignal(signo, SIG_DFL);
        sigprocmask(SIG_UNBLOCK, &mask, nullptr);
    }
    timers_.clear(); events_.clear(); signals_.clear();
    eatAsync(mutex, 0);
    status_ = status();
    /*重新添加内部侦听*/
    events_.emplace(fd_[0], ioTask(EV_READ, genId(),
        std::bind(&reactor::flush, this, std::placeholders::_1)));
}

template<typename Task>
inline bool reactor::addEvent(int32_t fd, int32_t ev, Task && func)
{
    if (fd < 0) { return false; }
    lock_guard guard(mutex_);
    auto res = events_.emplace(fd, ioTask(ev, genId(), std::forward<Task>(func)));
    if (res.second && polling_) { notify(0); }
    return res.second;
}

inline int32_t reactor::modEvent(int32_t fd, std::function<int32_t(int32_t)> &&func) noexcept
{
    lock_guard guard(mutex_);
    auto it = events_.find(fd);
    if (it == events_.end()) { return -1; }
    int32_t v = it->second.ev_;
    try { it->second.ev_ = func(v);} catch(...) { return -1; }
    if (polling_ && v != it->second.ev_) { notify(0); }
    return v;
}

inline bool reactor::removeEvent(int32_t fd) noexcept
{
    lock_guard guard(mutex_);
    auto rc = events_.erase(fd);
    if (rc && polling_) { notify(0); }
    return !!rc;
}

template<typename Task>
inline reactor::timerId reactor::doAddTimer(uint64_t expire, Task && func)
{
    timerId id;
    lock_guard guard(mutex_);
    if (timers_.size()) {
        auto next = timers_.begin()->first;
        if (next/1000000 == expire/1000000) {
            expire += 1000000;
        }
    }
    id.first  = genId();
    id.second = expire;
    timers_.emplace(id.second, timerTask(id, std::forward<Task>(func)));
    if (polling_) { notify(0); }
    return id;
}

template<typename Task>
inline reactor::timerId reactor::addTimer(uint32_t expire, Task && func)
{
    return doAddTimer(sys_clock() + (uint64_t)expire * 1000000,
                    std::forward<Task>(func));
}

inline bool reactor::resetTimer(timerId & id, uint32_t expire, task && todo) noexcept
{
    if (!id.first) { return false; }
    lock_guard guard(mutex_);
    auto frange = timers_.equal_range(id.second);
    auto it = std::find_if(frange.first, frange.second,
        [&](std::pair<const uint64_t, timerTask> & ref) {
            return (ref.second.id_ == id);
    });
    if (it == frange.second) { return false; }
    auto func = std::move(it->second.task_);
    timers_.erase(it);
    auto start = id.second;
    auto now   = sys_clock();
    id.first  = genId();
    id.second = now + (uint64_t)expire * 1000000;
    timers_.emplace(id.second, timerTask(id, std::move(func)));
    if (todo) {
        expire = 0;
        if (utils::time_before64(now, start)) {
            expire = (uint32_t)(((int64_t)start - (int64_t)now)/1000000);
        }
        try { todo(expire); } catch(...) {}
    }
    if (polling_) { notify(0); }
    return true;
}

inline bool reactor::removeTimer(timerId & id) noexcept
{
    if (!id.first) { return false; }
    lock_guard guard(mutex_);
    auto frange = timers_.equal_range(id.second);
    auto it = std::find_if(frange.first, frange.second,
        [&](std::pair<const uint64_t, timerTask> & ref) {
            return (ref.second.id_ == id);
    });
    if (it != frange.second) { timers_.erase(it); id.first = 0; return true; }
    return false;
}

template<typename Task>
inline bool reactor::addAsync(Task && func, bool wakeup, uint8_t prior)
{
    if (unlikely(prior >= asyncs_.size()))
        prior = static_cast<uint8_t>(asyncs_.size() - 1);
    lock_guard guard(mutex_);
    nr_async_++;
    asyncs_[prior].emplace_back(std::forward<Task>(func));
    /*TODO:需要验证，极限优化（解锁判断？）*/
    if (wakeup && polling_) { notify(0); }
    return true;
}

template<typename Task>
inline void reactor::addSignal(int32_t signo, Task && func)
{
    lock_guard guard(mutex_);
    /*添加信号句柄并阻塞*/
    auto p = signals_.insert({signo, std::forward<Task>(func)});
    if (!p.second) { return; }
    sigset_t mask;
    /*必须全进程阻塞信号*/
    sigemptyset(&mask);
    sigaddset(&mask, signo);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    sigaddset(&sigmask_, signo);
    utils::setupSignal(signo, utils::signalHandle);
    if (polling_) { notify(0); }
}

inline void reactor::removeSignal(int32_t signo) noexcept
{
    lock_guard guard(mutex_);
    if (!signals_.erase(static_cast<uint8_t>(signo))) { return; }
    utils::setupSignal(signo, SIG_DFL);
    sigdelset(&sigmask_, signo);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, signo);
    sigprocmask(SIG_UNBLOCK, &mask, nullptr);
    if (polling_) { notify(0); }
}

inline void reactor::stop(void) noexcept
{
    lock_guard guard(mutex_);
    if (polling_) { notify(0); }
}

inline void reactor::notify(uint8_t v) noexcept
{
    if (nr_nty_.fetch_add(1) && !v) { return; }
    status_.nr_write_++;
    (void)::write(fd_[1], (void*)&v, sizeof(v));
}

inline bool reactor::sync(bool force, uint32_t ms)
{
    unique_lock locker(mutex_);
    if (!force && !calling_) { return true; }
    /*事件回调内，递归等待*/
    if (unlikely(isTaskCtx())) {throw std::invalid_argument("Recursive call");}
    auto bar = std::make_shared<barrier>();
    waiters_.emplace_back([=](uint32_t) {
        lock_guard lg(mutex_);
        bar->notify();
    });
    nr_async_++;
    return bar->wait(locker, ms);
}

inline void reactor::flush(int32_t)
{
    /*
     * 一定要在读之后清零，否则将出现如下错误时序
     *     thread1       |    thread2
     * flush:exchange()  |
     *                   | notify:fetch_add(), nr_nty -> 1
     *                   |        (此后不会再被写，notify() 失去作用)
     *                   | notify:write()
     * flush:read()      |
     *                   |
     * run:poll()        |
     * 阻塞，不能被 notify() 唤醒
     */
    status_.nr_read_++;
    uint8_t buff[16];
    auto rc = ::read(fd_[0], buff, sizeof(buff));
    nr_nty_.exchange(0);
    for (ssize_t i = 0; likely(rc > 0) && i < rc; i++) {
        auto v = buff[i];
        if (unlikely(v) && likely(v < asignal_.size())) { asignal_.set(v); }
    }
}

inline int32_t reactor::nextTimeout(uint32_t timeout, uint64_t now) const noexcept
{
    /*异步事件堆积，则不休眠*/
    if (nr_async_ || asignal_.any()) { return 0; }
    if (timers_.empty() || !timeout) { return timeout; }
    int64_t next = 0;
    auto first = timers_.begin()->first;
    if (utils::time_before64(now, first)) {
        next = (first - now)/1000000;
        if (unlikely(next > INT32_MAX)) { next = INT32_MAX; }
        /*NOTE:不能返回0，导致过多的超时计算*/
        if (!next) next = 1;
    }
    return std::min(static_cast<uint32_t>(next), timeout);
}

inline int32_t reactor::eatTimer(unique_lock & mutex) noexcept
{
    if (timers_.empty()) { return 0; }
    int32_t nr = 0;
    uint64_t now = sys_clock();
    do {
        auto it = timers_.begin();
        if (utils::time_before64(now, it->first)) { break; }
        auto timer = std::move(it->second);
        assert(it->first == timer.id_.second);
        timers_.erase(it);
        status_.nr_done_++;
        calling_ = 1; mutex.unlock();
        auto escape = static_cast<int32_t>(
            (static_cast<int64_t>(now) - timer.id_.second)/1000000);
        try { timer.task_(escape); } catch(...) {}
        if (unlikely(!((++nr)&7))) { now = sys_clock(); }
        mutex.lock(); calling_ = 0;
        if (timers_.empty()) { break; }
    } while (unlikely(nr < 32));
    return nr;
}

inline void reactor::prepareEvent(PollEv & pev)
{
    int32_t max = static_cast<int32_t>(events_.size());
    pev.kev_ = PollfdPtr(kev_.data(), [](pollfd *){});
    if (unlikely(static_cast<size_t>(max) > kev_.size())) {
        pev.kev_ = PollfdPtr(new pollfd[static_cast<size_t>(max)],
                        [](pollfd *ptr){ delete [] ptr; });
    }
    int32_t i = 0;
    for (const auto &it : events_) {
        auto ev              = it.second.ev_;
        if (unlikely(!ev)) { continue; }
        pev.kev_.get()[i].fd      = it.first;
        pev.kev_.get()[i].revents = 0;
        pev.kev_.get()[i].events  = likely(ev)?static_cast<short>(ev):EV_PRI;
        i += 1;
    }
    assert(i < (max + 1));
    pev.size_ = max;
}

/*使用版本号保证解锁期间的 ABA 问题*/
inline int32_t reactor::eatEvent(unique_lock & mutex, PollEv & pev) noexcept
{
    for (int32_t i = 0; (i < pev.size_) && pev.nr_ > 0; i++) {
        auto & pl = pev.kev_.get()[i];
        if (!pl.revents) { continue; }
        pev.nr_ -= 1;
        auto io = events_.find(pl.fd);
        if (unlikely(io == events_.end())) { continue; }
        auto & evTask = io->second;
        auto ev = pl.revents & evTask.ev_;
        /*休眠期间关闭了事件，则不触发？*/
        if (unlikely(!ev && !(pl.revents & (EV_HUP|EV_ERROR)))) { continue; }
        /*写事件时一次性的*/
        if (ev & EV_WRITE) { evTask.ev_ &= (~EV_WRITE); }
        auto version = ++evTask.version_;
        auto task = std::move(evTask.task_);
        status_.nr_done_++;
        calling_ = 1; mutex.unlock();
        try { task(ev); } catch(...) {}
        mutex.lock(); calling_ = 0;
        auto in = events_.find(pl.fd);
        if (unlikely(in == events_.end())) { continue; }
        if (unlikely(in->second.version_ != version)) { continue; }
        in->second.task_ = std::move(task);
    }
    return 0;
}

inline int32_t reactor::eatAsync(unique_lock & mutex, uint32_t nr_max) noexcept
{
    if (!nr_async_) { return 0; }
    uint32_t nr_do = 0;
    auto doAsync = [&](AsyncQueue & source, bool waiter) {
        AsyncQueue asyncs;
        /*移动到本地，减少锁操作*/
        asyncs.splice(asyncs.cend(), source);
        if (!waiter) { calling_ = 1; }
        mutex.unlock();
        while (!asyncs.empty() && (waiter || nr_do < nr_max)) {
            nr_async_--;
            auto task = std::move(asyncs.front());
            asyncs.pop_front();
            nr_do++;
            try { task(0); } catch(...) {}
        }
        mutex.lock();
        if (!waiter) { calling_ = 0; }
        source.splice(source.cbegin(), asyncs);
    };
    if (nr_max) {
        for (auto & queue : asyncs_) {
            if (queue.size()) {
                if (unlikely(nr_do >= nr_max)) { break; }
                doAsync(queue, false);
                if (!nr_async_ || nr_async_ == waiters_.size()) { break; }
            }
        }
    }
    if (waiters_.size()) { doAsync(waiters_, true); }
    status_.nr_done_+= nr_do;
    return 0;
}

inline int32_t reactor::eatSignal(unique_lock & mutex) noexcept
{
    for (uint8_t i = 0; asignal_.any() && i < asignal_.size(); i++) {
        if (asignal_.test(i)) {
            asignal_.reset(i);
            if (signals_.count(i)) {
                auto & task = signals_[i];
                calling_ = 1; mutex.unlock();
                try { task(i); } catch(...) {}
                mutex.lock(); calling_ = 0;
                status_.nr_done_++;
            }
        }
    }
    return 0;
}

inline int32_t reactor::run(int32_t timeout) noexcept
{
    PollEv pev;
    sigset_t sigset;
    auto now = sys_clock();
    unique_lock mutex(mutex_);
    prepareEvent(pev);
    sigprocmask(SIG_UNBLOCK, &sigmask_, &sigset);
    polling_  = 1;
    status_.version_ += 1;
    timeout   = nextTimeout(timeout, now);
    mutex.unlock();
    /*TODO:休眠超时可能大于真实的流逝单调时间，导致定时器延迟*/
    pev.nr_ = ::poll(pev.kev_.get(), static_cast<nfds_t>(pev.size_), timeout);
    mutex.lock();
    polling_ = 0;
    status_.nr_poll_++;
    sigprocmask(SIG_SETMASK, &sigset, nullptr);
    volatile bool recur = isTaskCtx();
    if (!recur) { tid_ = std::this_thread::get_id(); }
    /*优先级信号最高，异步事件其次，定时器最低*/
    eatSignal(mutex);
    eatAsync(mutex);
    eatEvent(mutex, pev);
    /*定时任务优先级较低*/
    eatTimer(mutex);
    /*同步任务优先级最低*/
    eatAsync(mutex, 0);
    if (!recur) { tid_ = std::thread::id(); }
    if (unlikely(pev.nr_ < 0)) {
        auto rc = errno;
        if (unlikely(rc && rc != EINTR && rc != EAGAIN)) {
            fprintf(stderr, "Can't poll events : %s", strerror(rc));
        }
    }
    auto escape = (sys_clock() - now)/1000000;
    return unlikely(escape > INT32_MAX)?INT32_MAX:static_cast<int32_t>(escape);
}

inline void utils::signalHandle(int32_t signo)
{
    /*必须原子，否则可能死锁*/
    fprintf(stderr, "interrupt by : %d\n", signo);
    if (unlikely(!signo || signo >= NSIG)) { return; }
    if (!(signal_depth_++)) { all_mutex_.lock(); }
    utils::for_each([=](reactor *reactor) {
        reactor->notify(static_cast<uint8_t>(signo));
    });
    if (!(--signal_depth_)) { all_mutex_.unlock(); }
}

inline int32_t utils::pipe2(int32_t fd[2], int32_t flags)
{
    int32_t rc = ::pipe(fd);
    if (unlikely(rc)) { return -1; }
    auto enable_fd_flag = [&](int32_t fd, int32_t flags)
    {
        int32_t v = ::fcntl(fd, F_GETFL, nullptr);
        if (unlikely(v < 0)) { rc = -1; return; }
        v = ::fcntl(fd, F_SETFL,
        static_cast<uint32_t>(v) | static_cast<uint32_t>(flags));
        if (unlikely(v < 0)) rc = -1;
    };
    if (flags & O_NONBLOCK) {
        enable_fd_flag(fd[0], O_NONBLOCK);
        enable_fd_flag(fd[1], O_NONBLOCK);
    }
    if (flags & O_CLOEXEC) {
        enable_fd_flag(fd[0], O_CLOEXEC);
        enable_fd_flag(fd[1], O_CLOEXEC);
    }
    if (unlikely(rc)) { ::close(fd[0]); ::close(fd[1]); }
    return rc;
}

inline utils::signalFunc utils::setupSignal(int32_t signo, signalFunc cb)
{
    int32_t flags = 0;
    if (signo == SIGALRM) {
#ifdef SA_INTERRUPT
        flags = SA_INTERRUPT;
#endif
    } else {
#ifdef SA_RESTART
        flags = SA_RESTART;
#endif
    }
    struct sigaction old, n;
    assert(signo != SIGKILL && signo != SIGSTOP);
    n.sa_handler = cb;
    n.sa_flags = flags;
    sigemptyset(&n.sa_mask);
    if (::sigaction(signo, &n, &old)) { return SIG_ERR; }
    return old.sa_handler;
}

static inline uint64_t sys_clock(struct timespec *ts)
{
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    if (ts) {
        ts->tv_sec  = duration / 1000000000;
        ts->tv_nsec = duration % 1000000000;
    }
    return static_cast<uint64_t>(duration);
}

}

#endif /* reactor_h */
