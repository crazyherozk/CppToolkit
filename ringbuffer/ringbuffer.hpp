#ifndef ring_buffer_hpp
#define ring_buffer_hpp

#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <cassert>
#include <cstring>

#include <thread>
#include <memory>
#include <chrono>
#include <array>
#include <atomic>
#include <stdexcept>
#include <exception>
#include <functional>

#include <fcntl.h>
#include <sys/stat.h>
#include <semaphore.h>

#ifndef L1_CACHE_SHIFT
#if defined(__APPLE__) && defined(__aarch64__)
# define L1_CACHE_SHIFT 7
#else
# define L1_CACHE_SHIFT 6
#endif
#endif

#ifndef __cacheline_aligned
# define __cacheline_aligned __attribute__((aligned(1<<L1_CACHE_SHIFT)))
#endif

#ifndef likely
# define likely(x)   __builtin_expect(!!(x), 1)
#endif

#ifndef unlikely
# define unlikely(x) __builtin_expect(!!(x), 0)
#endif

namespace ring {

template<typename T>
void assert_atomic() {
    using AtomicT = std::atomic<T>;
    static_assert(std::is_trivially_copyable<AtomicT>::value,   "Atomic<T> is not trivially copyable");
    static_assert(std::is_standard_layout<AtomicT>::value,      "Atomic<T> is not standard layout");
    static_assert(sizeof(AtomicT) == sizeof(T),                 "Atomic<T> size is not equal to T");
    static_assert(!std::has_virtual_destructor<AtomicT>::value, "Atomic<T> has virtual destructor (implies vtable)");
}

/*一些辅助函数*/
static inline void cpu_relax(void) {
#if (__i386__ || __i386 || __amd64__ || __amd64)
    asm volatile("pause" ::: "memory");
#elif __aarch64__ || __arm__
    asm volatile("yield" ::: "memory");
#else
    std::atomic_thread_fence(std::memory_order_release);
#endif
}

enum {
    ENQUEUE_FIXED = 0x01U, /*入队列的元素个数是否固定*/
    DEQUEUE_FIXED = 0x02U, /*出队列的元素个数是否固定*/
    SINGLE_CONS   = 0x04U, /*单消费者模式*/
    SINGLE_PROD   = 0x08U, /*单生产者模式*/
};

struct headtail {
    std::atomic<uint32_t> head{0};
    std::atomic<uint32_t> tail{0};
};

struct index {
    explicit inline index(uint8_t size_shift = 9) {
        static_assert(sizeof(index)==2*(1UL<<L1_CACHE_SHIFT),
            "sizeof(index) is not equal to 2*L1_CACHE");
        assert_atomic<uint32_t>();
        if (unlikely(size_shift < 1 || size_shift > 31))
            throw std::invalid_argument("Invalid capicity shift of ring index");
        size_ = 1U << size_shift;
        mask_ = size_ - 1;
    }
    ~index(void) {}

    void     mode(uint8_t mode)   { mode_ |= (0x0F & mode); }
    uint8_t  mode(void) const     { return mode_; }
    uint32_t size(void) const     { return size_; }
    uint32_t capacity(void) const { return mask_; }
    uint32_t mask(void) const     { return mask_; }

    void reset(void) {
        prod_.head = prod_.tail = cons_.head = cons_.tail = 0;
    }

    /*
     *
     *     C.t       C.h        P.t       P.h
     * -----+---------+----------+---------+-------
     *      | pending |  stable  | pending |
     * -----+---------+----------+---------+-------
     *    ->|<- nr_count_stable->|         |<- nr_free_stable
     *
     */

    bool stabled(void) {
        return (prod_.head == prod_.tail && cons_.head == cons_.tail);
    }

    /**
     * @brief 包括出队列的未决，但不包括入队列的未决
     */
    uint32_t count_stable(void) const {
        return (prod_.tail - cons_.head) & mask();
    }

    uint32_t free_stable(void) const {
        return cons_.tail + mask() - prod_.head;
    }

    bool full_stable(void) const { return !free_stable(); }
    bool empty_stable(void) const { return !count_stable(); }
    uint32_t locate(uint32_t idx) const { return idx & mask(); }

    /*tail 消费者索引*/
    uint32_t prepare_enqueue(uint32_t n, uint32_t &prev, uint32_t &next, uint32_t &nr_free);
    /*tail 生产者索引*/
    uint32_t prepare_dequeue(uint32_t n, uint32_t &prev, uint32_t &next, uint32_t &nr_count);
    /*更新尾部索引*/
    void update_tail(uint32_t prev, uint32_t next, bool enqueue = true);

    void finish_enqueue(uint32_t prev, uint32_t next) { update_tail(prev, next, true); }
    void finish_dequeue(uint32_t prev, uint32_t next) { update_tail(prev, next, false); }

    template<class Func>
    uint32_t enqueue(uint32_t n, const Func &func) {
        uint32_t prev, next, nr_free;
        uint32_t max = prepare_enqueue(n, prev, next, nr_free);
        if (max) {
            func(locate(prev), max);
            finish_enqueue(prev, next);
        }
        return max;
    }

    template<class Func>
    uint32_t peek(uint32_t n, const Func &func) {
        assert(mode_ & SINGLE_CONS);
        uint32_t prev, next, nr_count;
        prepare_dequeue(0, prev, next, nr_count);
        if (nr_count >= n) {
            func(locate(prev), nr_count);
        }
        return nr_count;
    }

    template<class Func>
    uint32_t dequeue(uint32_t n, const Func &func) {
        uint32_t prev, next, nr_count;
        uint32_t max = prepare_dequeue(n, prev, next, nr_count);
        if (max) {
            func(locate(prev), max);
            finish_dequeue(prev, next);
        }
        return max;
    }

private:
    /** 生产者状态. */
    headtail prod_ __cacheline_aligned;
    /** 消费者状态. */
    headtail cons_ __cacheline_aligned;
    uint32_t size_;
    uint32_t mask_;
    /*可以自行初始化，默认是出入队列是多线程安全且固定数目*/
    uint8_t mode_{ENQUEUE_FIXED|DEQUEUE_FIXED};
};

inline void index::update_tail(uint32_t prev, uint32_t next, bool enqueue) {
    auto & tail = (enqueue ? prod_.tail : cons_.tail);
    /*等待上一个排队更新*/
    if (!(enqueue?(mode_ & SINGLE_PROD):(mode_ & SINGLE_CONS))) {
        /*并发模式需要一个短暂休眠，因为拷贝数据的花销不定*/
        for (uint32_t seq = 1;;seq++) {
            auto val = tail.load(std::memory_order_relaxed);
            if (val == prev) { break; }
            if (likely(seq&7)) { cpu_relax(); } else { std::this_thread::yield(); }
#ifdef DEBUG
            if (unlikely(!(seq & 1023)))
                fprintf(stderr, "cond_load_acquire timeout\n");
#endif
        }
    }
    tail.store(next, std::memory_order_release);
}

/*prev 当前索引，next 自身保存索引*/
inline uint32_t index::prepare_enqueue(uint32_t n, uint32_t &prev, uint32_t &next, uint32_t &nr_free)
{
    uint32_t max, mask = this->mask();
    const bool single = (mode_ & SINGLE_PROD);
    const bool fixed  = (mode_ & ENQUEUE_FIXED);
    prev = prod_.head.load(std::memory_order_relaxed);
    do {
        max = n;
        auto tail = cons_.tail.load(std::memory_order_acquire);
        nr_free = mask + tail - prev;
        if (max > nr_free) { max = fixed?0:nr_free; }
        if (max == 0) { break; }
        next = prev + max;
        if (single) {
            /*没有竞争，直接普通写值*/
            prod_.head.store(next, std::memory_order_relaxed);
            break;
        }
        if (likely(prod_.head.compare_exchange_weak(prev, next,
                std::memory_order_relaxed))) {
            /*成功更新，直接返回*/
            break;
        }
    } while (1);

    return max;
}

inline uint32_t index::prepare_dequeue(uint32_t n, uint32_t &prev, uint32_t &next, uint32_t &nr_count)
{
    uint32_t max;
    const bool single = (mode_ & SINGLE_CONS);
    const bool fixed  = (mode_ & DEQUEUE_FIXED);
    prev = cons_.head.load(std::memory_order_relaxed);
    do {
        max = n;
        auto tail = prod_.tail.load(std::memory_order_acquire);
        nr_count = tail - prev;
        if (max > nr_count) { max = fixed?0:nr_count; }
        if (max == 0) { break; }
        next = prev + max;
        if (single) {
            /*没有竞争，直接普通写值*/
            cons_.head.store(next, std::memory_order_relaxed);
            break;
        }
        if (likely(cons_.head.compare_exchange_weak(prev, next,
                std::memory_order_relaxed))) {
            /*成功更新，直接返回*/
            break;
        }
    } while (1);

    return max;
}

template <typename T, std::size_t N=9>
struct buffer {
public:
    buffer(void) : index_(N) { }
    virtual ~buffer(void) { clear(); }

    uint32_t capacity(void) const { return (1U << N) - 1; }
    uint32_t size(void) const { return index_.count_stable(); }

    /*设置多个或单个生产者*/
    void multi_pusher(bool v) {
        uint8_t mode = index_.mode() & ~SINGLE_PROD;
        index_.mode(v?mode:(mode|SINGLE_PROD));
    }

    /*设置多个或单个消费者*/
    void multi_popper(bool v) {
        uint8_t mode = index_.mode() & ~SINGLE_CONS;
        index_.mode(v?mode:(mode|SINGLE_CONS));
    }

    /*设置单次推入的数量固定或可变*/
    void fixed_pusher(bool v) {
        uint8_t mode = index_.mode() & ~ENQUEUE_FIXED;
        index_.mode(v?(mode|ENQUEUE_FIXED):mode);
    }

    /*设置单次弹出的数量固定或可变*/
    void fixed_popper(bool v) {
        uint8_t mode = index_.mode() & ~DEQUEUE_FIXED;
        index_.mode(v?(mode|DEQUEUE_FIXED):mode);
    }

    /*失败，表示不稳定，需要重试*/
    bool clear(void) {
        if (!index_.stabled()) { return false; }
        T item; while (pop(item)) { destroy(item); }
        index_.reset();
        return true;
    }

    bool push(T && item) { return doPush(std::forward<T>(item)); }
    bool push(const T & item) { return doPush(item); }

    template<typename U = T>
    std::pair<bool, const U &> peek(void) {
        uint32_t prev, next, nr_count;
        assert(index_.mode() & SINGLE_CONS);
        index_.prepare_dequeue(0, prev, next, nr_count);
        if (nr_count > 0) {
            return { true, queue_.at(index_.locate(prev)) };
        }
        return { false, queue_.at(0) };
    }

    bool pop(T & item) {
        uint32_t prev, next, nr_count;
        auto max = index_.prepare_dequeue(1, prev, next, nr_count);
        if (max) {
            item = std::move(queue_.at(index_.locate(prev)));
            index_.finish_dequeue(prev, next);
        }
        return !!max;
    }

protected:
    index                   index_;
    std::array<T, (1UL<<N)> queue_;

private:
    template <typename U>
    static void destroy(U &item) { item = U(); }
    template <typename U>
    static void destroy(U* &item) {delete item; item = nullptr;}

    template <typename U> bool doPush(U &&item) {
        uint32_t prev, next, nr_free;
        auto max = index_.prepare_enqueue(1, prev, next, nr_free);
        if (max) {
            queue_.at(index_.locate(prev)) = std::forward<decltype(item)>(item);
            index_.finish_enqueue(prev, next);
        }
        return !!max;
    }
};

#ifdef __APPLE__
static inline int32_t sem_timedwait(sem_t* sem, const struct timespec* ts) {
    using namespace std::chrono;
    if (unlikely(!sem || !ts)) {
        errno = EINVAL;
        return -1;
    }
    const auto deadline = system_clock::from_time_t(ts->tv_sec) +
                      nanoseconds(ts->tv_nsec);
    const microseconds min_step(100);
    const microseconds max_step(5000);
    microseconds step = min_step;
    do {
        if (sem_trywait(sem) == 0) { return 0; }
        if (errno != EAGAIN && errno != EINTR) { return -1; }
        const auto now = system_clock::now();
        if (unlikely(now >= deadline)) { errno = ETIMEDOUT; return -1; }
        const auto remaining = duration_cast<microseconds>(deadline - now);
        const auto tiny = std::min(max_step, remaining);
        std::this_thread::sleep_for(std::min(min_step, tiny));
        step += min_step;
        if (unlikely(step > max_step)) { step = min_step; }
    } while (true);
}
#endif

static inline bool sem_timedwait(sem_t * sem, int32_t ms) {
    int32_t res;
    if (likely(ms == -1)) {
        res = ::sem_wait(sem);
    } else {
        struct timespec ts;
        ::clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (static_cast<uint32_t>(ms)%1000)*1000*1000;
        if (ms >= 1000) {
            ts.tv_sec  += (static_cast<uint32_t>(ms)/1000);
        }
        if (unlikely(ts.tv_nsec >= 1000*1000*1000)) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000*1000*1000;
        }
        res = sem_timedwait(sem, &ts);
    }
    if (unlikely(res == -1)) {
        if (unlikely(errno != ETIMEDOUT && errno != EINTR)) {
            fprintf(stderr, "sem wait failed : %s\n", std::strerror(errno));
        }
        return false;
    }
    return true;
}

/*等待模式只支持单消费者，多生产者模式*/
template <typename T, std::size_t N=9>
struct blocking_buffer {
public:
    explicit blocking_buffer(const std::string & name = "") {
        buffer_.multi_pusher(true);
        buffer_.multi_popper(false);
#ifdef __APPLE__
        if (name.empty()) {
            throw std::invalid_argument("empty name of queue");
        }
        ::sem_unlink(name.c_str());

        sem_t * sem = sem_open(name.c_str(), O_CREAT | O_EXCL, 0666, 0);
        if (sem == SEM_FAILED) {
            fprintf(stderr, "create a sem failed : %s\n", std::strerror(errno));
            throw std::runtime_error(
                std::string("create a sem failed: ") + std::strerror(errno)
            );
        }

        cond_ = std::move(SemPtr(sem, [=](sem_t *ptr){
            ::sem_close(ptr);
            ::sem_unlink(name.c_str());
        }));
#else
        auto rc = ::sem_init(&sem_, 0, 0);
        if (rc != 0) {
            fprintf(stderr, "init a sem failed : %s\n", std::strerror(errno));
            throw std::runtime_error(
                std::string("init a sem failed: ") + std::strerror(errno)
            );
        }
        cond_ = std::move(SemPtr(&sem_, [](sem_t *ptr){
            ::sem_destroy(ptr);
        }));
#endif
    }

    template<typename U>
    bool push(U && item) {
        bool rc = buffer_.push(std::forward<U>(item));
        if (likely(rc)) {
            wakeup();
        }
        return rc;
    }

    template<typename U = T>
    std::pair<bool, const U &> peek(void) { return buffer_.template peek<U>(); }

    bool pop(T & item, int32_t ms = -1) {
        auto rc = buffer_.pop(item);
        if (unlikely(!rc && ms)) {
            /*自旋锁模式，大多临界区仅是数据拷贝，很短，所以可以尝试几次再休眠等待*/
            for (int8_t i = 0; i < 7; i++) {
                std::this_thread::yield();
                rc = buffer_.pop(item);
                if (rc) {
                    return rc;
                }
            }
            do {
                waitPrepare();
                rc = buffer_.pop(item);
                if (rc) {
                    break;
                }
            } while (waitTimedout(ms));
            waitFinish();
        }
        return rc;
    }

    size_t capacity(void) const { return buffer_.capacity(); }
    size_t size(void) const { return buffer_.size(); }
private:
    using SemDeleter = std::function<void(sem_t*)>;
    using SemPtr = std::unique_ptr<sem_t, SemDeleter>;

    buffer<T, N> buffer_;
#ifndef __APPLE__
    sem_t  sem_;
#endif
    SemPtr cond_{nullptr};
    std::atomic_bool waiting_{false};

    void wakeup(void) {
        if (unlikely(waiting_.exchange(false))) {
            int32_t rc = ::sem_post(cond_.get());
            if (unlikely(rc == -1)) {
                fprintf(stderr, "Oops!!!sem post failed : %s\n", strerror(errno));
            }
        }
    }
    void waitPrepare(void) { waiting_.store(true); }
    void waitFinish(void) { waiting_.store(false); }
    bool waitTimedout(int32_t ms) { return sem_timedwait(cond_.get(), ms); }
};

}

#endif
