//
//  reactor.hpp
//
//
//  Created by zhoukai on 2023/11/3.
//

#ifndef qev_reactor_hpp
#define qev_reactor_hpp

#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <assert.h>
#include <unistd.h>
#include <sys/poll.h>
#include <sys/time.h>

#include <set>
#include <map>
#include <list>
#include <array>

#include <mutex>
#include <atomic>
#include <bitset>
#include <memory>
#include <exception>
#include <algorithm>
#include <functional>
#include <system_error>
#include <unordered_map>

#define _REACTOR_VERSION_ "2.2"

#if !defined(__linux__) && (defined(__LINUX__) || defined(__KERNEL__) \
    || defined(_LINUX) || defined(LINUX) || defined(__linux))
# define  __linux__    (1)
#elif !defined(__apple__) && (defined(__MacOS__) || defined(__APPLE__))
# define  __apple__    (1)
#elif !defined(__qnx__) && (defined(__QNXNTO__) || defined(__QNX__) \
    || defined(_QNX_) || defined(_HQX_))
# define  __qnx__      (1)
#endif

#ifndef likely
# define likely(x)   __builtin_expect(!!(x), 1)
#endif

#ifndef unlikely
# define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(_a) static_cast<int32_t>((sizeof (_a) / sizeof (_a)[0]))
#endif

/*
 * 必须在函数体外，源文件内包含一次，
 * 建议在可执行程序的 main() 函数所在文件中包含
 */
#define HSAE_QEV_REACTOR_VAR() \
    std::mutex hsae::qev::reactor::all_mutex_; \
    std::set<hsae::qev::reactor*> hsae::qev::reactor::all_reactors_; \
    thread_local std::atomic<uint8_t> hsae::qev::reactor::signal_depth_

namespace qev {

typedef void (*signal_fn)(int32_t);
static inline int32_t pipe2(int32_t fd[2], int32_t flags);
static inline uint64_t sys_clock(struct timespec *ts = nullptr);
static inline signal_fn signal_setup(int32_t signo, signal_fn cb);

class reactor {
public:
    explicit inline reactor(void);
    virtual inline ~reactor(void);

    /*移除所有事件*/
    inline void reset(bool exec = false);

    using task = std::function<void(void)>;
    using timerId = std::pair<uint32_t, uint64_t>;

    /*可以指定事件类型，但是 task 不区分，只能靠任务自己判断事件*/
    inline bool addEvent(int32_t fd, int32_t ev, task && func);
    inline bool modEvent(int32_t fd, int32_t ev);
    inline void removeEvent(int32_t fd);

    inline timerId addTimer(uint32_t expire, task && func);
    inline bool removeTimer(const timerId &);

    inline void addSignal(int32_t signo, task && func);
    inline void removeSignal(int32_t signo);

    /*0为最高优先级*/
    inline void addAsync(task && func, uint8_t prior = 0);

    bool addEvent(int32_t fd, int32_t ev, task & func)
    {
        return addEvent(fd, ev, std::forward<task>(func));
    }
    bool addEvent(int32_t fd, task && func)
    {
        return addEvent(fd, POLLPRI, std::forward<task>(func));
    }
    bool addEvent(int32_t fd, task & func)
    {
        return addEvent(fd, std::forward<task>(func));
    }
    /*在即刻起的未来相对时间添加任务*/
    timerId addTimer(uint32_t expire, task & func)
    {
        return addTimer(expire, std::forward<task>(func));
    }
    void addAsync(task & func, uint8_t prior = 0)
    {
        addAsync(std::forward<task>(func), prior);
    }
    void addSignal(int32_t signo, task & func)
    {
        addSignal(signo, std::forward<task>(func));
    }

    inline int32_t run(int32_t timeout = -1);

    inline void stop(void);
    /*外部只能中断循环，而不能发送其他通知信息*/
    inline void notify(void) { notify(0); }

    uint8_t version(void) const { return version_; }

    /*在未来绝对时间处添加任务*/
    inline timerId addTimerImpl(uint64_t expire, task && func);

private:
    struct timerTask {
        timerId     id_;
        task        task_;
        timerTask(timerId id, task && task)
            : id_(id), task_(std::move(task))
        {}
        timerTask(timerTask && other)
        {
            this->id_   = other.id_;
            this->task_ = std::move(other.task_);
        }
    };

    struct eventTask {
        int32_t     ev_;
        task        task_;
        eventTask(int32_t ev, task && task)
            : ev_(ev), task_(std::move(task))
        {}
        eventTask(eventTask && other)
        {
            this->ev_   = other.ev_;
            this->task_ = std::move(other.task_);
        }
    };

    using AsyncArray = std::array<std::list<task>, 8>;
    using TimerMap   = std::multimap<uint64_t, timerTask>;
    using SignalMap  = std::unordered_map<uint8_t, task>;
    using EventMap   = std::unordered_map<int32_t, eventTask>;
    using SignalSet  = std::bitset<NSIG>;

    std::atomic<uint8_t>  nr_nty_{0};
    std::atomic<uint32_t> nr_async_{0};

    uint8_t     polling_{0};
    uint8_t     version_{0}; /**< 可以查看循环是否正常轮转*/
    int32_t     fd_[2];
    uint32_t    id_{0};
    pollfd      kev_[32];
    std::mutex  mutex_;

    SignalSet   asignal_;
    sigset_t    sigmask_; /**< 正在监控的信号*/

    AsyncArray  asyncs_;
    EventMap    events_;
    TimerMap    timers_;
    SignalMap   signals_;

    inline void flush(void);
    inline uint32_t genId(void)
    {
        /*对于正常的设计，在U32_MAX回绕之前，定时器肯定超时*/
        if (id_ == 0) id_ += 1;
        return id_++;
    }

    inline void notify(uint8_t v);

    inline int32_t nextTimeout(uint32_t timeout);
    inline int32_t processSignal(std::unique_lock<std::mutex> &);
    inline int32_t processTimer(std::unique_lock<std::mutex> &);
    inline int32_t prepareEvent(std::shared_ptr<pollfd> &);
    inline int32_t processEvent(std::unique_lock<std::mutex> &,
                int32_t, int32_t, std::shared_ptr<pollfd> &);
    inline int32_t processAsync(std::unique_lock<std::mutex> &,
                uint32_t nr_max = 32);

    reactor(const reactor &) = delete;
    reactor & operator = (const reactor &) = delete;

public:
    static std::mutex                           all_mutex_;
    static std::set<reactor*>                   all_reactors_;
    static thread_local std::atomic<uint8_t>    signal_depth_;

    static inline void for_each(std::function<void(reactor*)> &&);
    static inline void signal_handle(int32_t signo);
};

#ifdef __apple__
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif

/*a绝对时间在b绝对时间之后*/
template<typename A, typename B>
static bool time_after64(A a, B b) { return ((int64_t)(b) - (int64_t)(a)) < 0; }

/*a绝对时间在b绝对时间之前*/
template<typename A, typename B>
static bool time_before64(A a,B b) { return time_after64(b,a); }

/*
 * Reactor 参数可以是代理，只要提供定时器操作函数即可
 */
template <typename Reactor>
struct intervalTimer {
    intervalTimer(Reactor loop = nullptr) : loop_(loop) { }

    intervalTimer(intervalTimer && other)
        : loop_(std::move(other.loop_))
        , id_(other.id_)
        , task_(std::move(other.task_))
    {
        other.loop_ = nullptr;
        other.task_ = nullptr;
    }

    virtual ~intervalTimer(void)
    {
        if (loop_ && id_.first) {
            loop_->removeTimer(id_);
        }
    }

    void stop(void)
    {
        if (loop_ && id_.first) {
            loop_->removeTimer(id_);
            id_ = reactor::timerId();
        }
    }

    void launch(int32_t interval, reactor::task && func)
    {
        assert(loop_);
        /*记录启动绝对时间*/
        start_ = sys_clock();
        task_  = std::move(func);
        id_    = loop_->addTimer(static_cast<uint32_t>(interval), std::bind(
                    &intervalTimer<Reactor>::execute, this, interval));
    }

    void launch(int32_t interval, reactor::task & func)
    {
        launch(interval, std::forward<reactor::task>(func));
    }

private:
    Reactor          loop_;
    uint32_t         count_{1};
    uint64_t         start_;
    reactor::timerId id_{0,0};
    reactor::task    task_{nullptr};

    void execute(int32_t interval) {
        if (task_)
            task_();
        /*不论任务延迟如何，间隔定时器必须按预定时间触发*/
        auto expire = (uint64_t)interval * (++count_) * 1000000 + start_;
        id_ = loop_->addTimerImpl(expire,
            std::bind(&intervalTimer<Reactor>::execute, this, interval));
    }

    intervalTimer(const intervalTimer &other) = delete;
    intervalTimer & operator=(const intervalTimer &other) = delete;
};

reactor::reactor(void)
{
    if (unlikely(signal_depth_)) {
        throw std::system_error(std::error_code(EBUSY,
            std::generic_category()), "Cannot build reactor in signal handle");
    }

    if (pipe2(fd_, O_CLOEXEC|O_NONBLOCK)) {
        throw std::system_error(std::error_code(errno,
            std::generic_category()), "Cannot create pipe of reactor");
    }

    sigemptyset(&sigmask_);
    addEvent(fd_[0], POLLIN, std::bind(&reactor::flush, this));

    std::lock_guard<std::mutex> guard(all_mutex_);
    all_reactors_.insert(this);
}

reactor::~reactor(void)
{
    assert(!signal_depth_);

    {
        std::lock_guard<std::mutex> _(all_mutex_);
        all_reactors_.erase(this);
    }

    reset();

    ::close(fd_[0]);
    ::close(fd_[1]);
}

void reactor::reset(bool exec)
{
    std::unique_lock<std::mutex> guard(mutex_);

    /*执行一些残留的任务*/
    if (exec) {
        processTimer(guard);
        processAsync(guard, UINT32_MAX);
    }

    for (auto & queue : asyncs_) {
        queue.clear();
    }

    for (auto & kv : signals_) {
        auto signo = kv.first;
        signal_setup(signo, SIG_DFL);
        sigdelset(&sigmask_, signo);
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, signo);
        sigprocmask(SIG_UNBLOCK, &mask, nullptr);
    }

    timers_.clear();
    events_.clear();
    signals_.clear();
}

bool reactor::addEvent(int32_t fd, int32_t ev, task && func)
{
    std::lock_guard<std::mutex> guard(mutex_);
    auto res = events_.emplace(fd, eventTask(ev, std::move(func)));
    if (res.second && polling_)
        notify();
    return res.second;
}

bool reactor::modEvent(int32_t fd, int32_t ev)
{
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = events_.find(fd);
    if (it == events_.end())
        return false;
    it->second.ev_ = ev;
    if (polling_)
        notify();
    return true;
}

void reactor::removeEvent(int32_t fd)
{
    std::lock_guard<std::mutex> guard(mutex_);
    events_.erase(fd);
    if (polling_)
        notify();
}

reactor::timerId reactor::addTimerImpl(uint64_t expire, task && func)
{
    timerId id;

    std::lock_guard<std::mutex> guard(mutex_);

    id.first  = genId();
    id.second = expire;

    timers_.emplace(id.second, timerTask(id, std::move(func)));

    if (polling_)
        notify();

    return id;
}

reactor::timerId reactor::addTimer(uint32_t expire, task && func)
{
    return addTimerImpl(sys_clock() + (uint64_t)expire * 1000000,
                    std::forward<task>(func));
}

bool reactor::removeTimer(const timerId & id)
{
    std::lock_guard<std::mutex> guard(mutex_);

    auto frange = timers_.equal_range(id.second);
    auto it = std::find_if(frange.first, frange.second,
        [&](std::pair<const uint64_t, timerTask> & ref) {
            return (ref.second.id_ == id);
    });

    if (it != frange.second) {
        timers_.erase(it);
        return true;
    }

    return false;
}

void reactor::addAsync(task && func, uint8_t prior)
{
    if (unlikely(prior >= asyncs_.size()))
        prior = static_cast<uint8_t>(asyncs_.size() - 1);

    std::lock_guard<std::mutex> guard(mutex_);
    nr_async_++;
    asyncs_[prior].emplace_back(std::move(func));

    /*TODO:需要验证，极限优化（解锁判断？）*/
    if (polling_)
        notify();
}

void reactor::addSignal(int32_t signo, task && func)
{
    std::lock_guard<std::mutex> guard(mutex_);

    /*添加信号句柄并阻塞*/
    auto p = signals_.insert({signo, std::move(func)});
    if (!p.second) {
        /*已存在*/
        return;
    }

    sigset_t mask;

    /*必须全进程阻塞信号*/
    sigemptyset(&mask);
    sigaddset(&mask, signo);
    sigprocmask(SIG_BLOCK, &mask, nullptr);

    sigaddset(&sigmask_, signo);
    signal_setup(signo, reactor::signal_handle);

    if (polling_)
        notify();
}

void reactor::removeSignal(int32_t signo)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (!signals_.erase(static_cast<uint8_t>(signo))) {
        return;
    }

    signal_setup(signo, SIG_DFL);
    sigdelset(&sigmask_, signo);

    sigset_t mask;

    sigemptyset(&mask);
    sigaddset(&mask, signo);
    sigprocmask(SIG_UNBLOCK, &mask, nullptr);

    if (polling_)
        notify();
}

void reactor::stop(void)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (polling_)
        notify();
}

void reactor::notify(uint8_t v)
{
    if (nr_nty_.fetch_add(1) && !v) {
        return;
    }

    (void)::write(fd_[1], (void*)&v, sizeof(v));
}

void reactor::flush(void)
{
    uint8_t buff[16];

    /*
     * 一定要在读之后清零，否则将出现如下错误时序
     *     thread1            thread2
     * flush:exchange()
     *                     notify:fetch_add(), nr_nty -> 1
     *                            (此后不会再被写，notify() 失去作用)
     *                     notify:write()
     * flush:read()
     *
     * run:poll() -> 永远阻塞，而不能被 notify() 唤醒
     */
    auto rc = ::read(fd_[0], buff, sizeof(buff));
    nr_nty_.exchange(0);

    for (ssize_t i = 0; likely(rc > 0) && i < rc; i++) {
        auto v = buff[i];
        if (unlikely(v) && likely(v < asignal_.size())) {
            asignal_.set(v);
        }
    }
}

int32_t reactor::nextTimeout(uint32_t timeout)
{
    /*异步事件堆积，则不休眠*/
    if (nr_async_)
        return 0;

    if (timers_.empty())
        return static_cast<int32_t>(timeout);

    int64_t next = 0;
    auto now = sys_clock();
    auto first = timers_.begin()->first;
    if (time_before64(now, first)) {
        next = ((int64_t)first - (int64_t)now)/1000000;
        if (unlikely(next > INT32_MAX))
            next = INT32_MAX;
    }

    return static_cast<int32_t>(std::min(static_cast<uint32_t>(next), timeout));
}

int32_t reactor::processTimer(std::unique_lock<std::mutex> & guard)
{
    int32_t nr = 0;
    uint64_t now = sys_clock();

    do {
        if (timers_.empty())
            return nr;

        auto it = timers_.begin();
        if (time_before64(now, it->first))
            return nr;

        timerTask timer = std::move(it->second);
        assert(it->first == timer.id_.second);
        timers_.erase(it);
        guard.unlock();

        timer.task_();

        guard.lock();
        if (unlikely(!((++nr)&7)))
            now = sys_clock();
    } while (unlikely(nr < 32));

    return nr;
}

int32_t reactor::prepareEvent(std::shared_ptr<pollfd> &kev)
{
    int32_t max = static_cast<int32_t>(events_.size());
    kev = std::shared_ptr<pollfd>(&kev_[0], [&](pollfd *){});
    if (unlikely(max > ARRAY_SIZE(kev_))) {
        kev = std::shared_ptr<pollfd>(new pollfd[static_cast<size_t>(max)],
                        [&](pollfd *ptr){ delete [] ptr; });
    }

    int32_t i = 0;
    for (const auto &it : events_) {
        auto ev              = it.second.ev_;
        kev.get()[i].fd      = it.first;
        kev.get()[i].revents = 0;
        /*NOTE: GPIO只有 POLLPRI 表示有事件发送，因为它一直是可读的*/
        kev.get()[i].events  = likely(ev)?static_cast<short>(ev):POLLPRI;
        i += 1;
    }

    assert(i == max);

    return max;
}

int32_t reactor::processEvent(std::unique_lock<std::mutex> & guard,
        int32_t max, int32_t nr, std::shared_ptr<pollfd> &kev)
{
    for (int32_t i = 0; (i < max) && nr; i += 1) {
        if (!kev.get()[i].revents) {
            continue;
        }

        auto ir = events_.find(kev.get()[i].fd);

        assert(ir != events_.end());
        /*task的引用不能失效*/
        auto & evTask = ir->second;
        guard.unlock();
        evTask.task_();
        guard.lock();

        nr -= 1;
    }

    return 0;
}

int32_t reactor::processAsync(std::unique_lock<std::mutex> &guard,
        uint32_t nr_max)
{
    if (!nr_async_) {
        return 0;
    }

    uint32_t nr_do = 0;

    auto doAsync = [&](std::list<task> & source) {
        std::list<task> asyncs;
        /*移动到本地，减少锁操作*/
        asyncs.splice(asyncs.cend(), source);
        guard.unlock();

        while (!asyncs.empty() && (nr_do++ < nr_max)) {
            nr_async_--;
            auto task = std::move(asyncs.front());
            asyncs.pop_front();

            task();
        }

        guard.lock();
        source.splice(source.cbegin(), asyncs);
    };

    for (auto & queue : asyncs_) {
        if (queue.size()) {
            if (unlikely(nr_do >= nr_max)) {
                break;
            }
            doAsync(queue);
        }
    }

    return 0;
}

int32_t reactor::processSignal(std::unique_lock<std::mutex> & guard)
{
    for (uint8_t i = 0; i < asignal_.size(); i++) {
        if (asignal_.test(i)) {
            asignal_.reset(i);
            if (signals_.count(i)) {
                auto & func = signals_[i];
                guard.unlock();
                func();
                guard.lock();
            }
        }
    }
    return 0;
}

int32_t reactor::run(int32_t timeout)
{
    int32_t max;
    std::shared_ptr<pollfd> kev;
    std::unique_lock<std::mutex> guard(mutex_);

    polling_ =  1;
    version_ += 1;

    std::atomic_thread_fence(std::memory_order::memory_order_release);

    max     = prepareEvent(kev);
    timeout = nextTimeout(static_cast<uint32_t>(timeout));

    sigset_t sigset;
    sigprocmask(SIG_UNBLOCK, &sigmask_, &sigset);
    guard.unlock();

    int32_t rc = ::poll(kev.get(), static_cast<nfds_t>(max), timeout);

    guard.lock();
    sigprocmask(SIG_SETMASK, &sigset, nullptr);

    polling_ = 0;

    processSignal(guard);
    processTimer(guard);
    processAsync(guard);

    if (unlikely(rc < 1)) {
        rc = rc ? -errno : (timeout?-ETIMEDOUT:-EAGAIN);
        return rc;
    }

    processEvent(guard, max, rc, kev);

    return 0;
}

void reactor::for_each(std::function<void(reactor*)> && func)
{
    for (auto &reactor : all_reactors_) {
        func(reactor);
    }
}

void reactor::signal_handle(int32_t signo)
{
    /*必须原子，否则可能死锁*/

//    fprintf(stderr, "interrupt by : %d\n", signo);

    if (unlikely(!signo || signo >= NSIG))
        return;

    if (!(signal_depth_++)) {
        all_mutex_.lock();
    }

    reactor::for_each([=](reactor *reactor) {
        reactor->notify(static_cast<uint8_t>(signo));
    });

    if (!(--signal_depth_)) {
        all_mutex_.unlock();
    }
}

static inline int32_t pipe2(int32_t fd[2], int32_t flags)
{
    int32_t rc = ::pipe(fd);
    if (unlikely(rc))
        return -1;

    auto enable_fd_flag = [&](int32_t fd, int32_t flags)
    {
        int32_t v = fcntl(fd, F_GETFL, nullptr);
        if (unlikely(v < 0)) { rc = -1; return; }
        v = fcntl(fd, F_SETFL,
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
    if (unlikely(rc)) {
        close(fd[0]);
        close(fd[1]);
    }
    return rc;
}

static inline uint64_t sys_clock(struct timespec *ts)
{
    struct timespec tsv = { 0, 0 };
    if (!ts)
        ts = &tsv;
#ifndef __apple__
    clock_gettime(CLOCK_MONOTONIC, ts);
    return (uint64_t)ts->tv_sec * 1000000000 + ts->tv_nsec;
#else
    uint64_t ticks;
    static __thread mach_timebase_info_data_t tbinfo;
    if (tbinfo.denom == 0)
        mach_timebase_info(&tbinfo);

    ticks = mach_absolute_time();
    ticks = ticks * tbinfo.numer / tbinfo.denom;
    ts->tv_sec = ticks / 1000000000;
    ts->tv_nsec = ticks % 1000000000;
    return ticks;
#endif
}

static inline signal_fn signal_setup(int32_t signo, signal_fn cb)
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

    if (::sigaction(signo, &n, &old))
        return SIG_ERR;

    return old.sa_handler;
}

}

#endif /* reactor_h */