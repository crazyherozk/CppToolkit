#include <sys/mman.h>
#include <sys/stat.h>
#ifndef _QNX_
#include <sys/file.h>
#endif
#include <fcntl.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/posix_shm.h>
#endif

#include <type_traits>
#include "../ringbuffer/ringbuffer.hpp"

namespace shm {

template <typename... Args> struct pack_size;
template <typename First, typename... Rest>
struct pack_size<First, Rest...> {
    static constexpr size_t value = sizeof(First) + pack_size<Rest...>::value;
};
template <> struct pack_size<> { static constexpr size_t value = 0; };

struct LockFreeQueue {
private:
    friend struct ShareMemoryQueueBase;
    static constexpr uint32_t HSize = static_cast<uint32_t>(sizeof(long));
    static constexpr uint32_t RSize = HSize - static_cast<uint32_t>(sizeof(int));
    ring::index index_;
#ifndef __APPLE__
    sem_t       sem_;
#endif
    uint32_t    magic_{0xabadbeef};
    std::atomic_bool waiting_{false};
    uint8_t data_[0] __cacheline_aligned;

    LockFreeQueue(LockFreeQueue &&) = delete;
    LockFreeQueue & operator = (const LockFreeQueue &) = delete;
    LockFreeQueue(const LockFreeQueue &) = delete;
    LockFreeQueue & operator = (LockFreeQueue &&) = delete;

    LockFreeQueue(uint8_t shift) : index_(shift) {
        index_.mode(ring::ENQUEUE_FIXED|ring::DEQUEUE_FIXED|ring::SINGLE_CONS);
    }
    ~LockFreeQueue() = default;

    uint32_t size(void) const { return index_.size(); }
    const uint8_t & at(uint32_t idx) const { return data_[idx & index_.mask()]; }
    uint8_t & at(uint32_t idx) { return data_[idx & index_.mask()]; }

    void copyin(uint32_t & idx, const void *ptr, size_t size) {
        idx = index_.locate(idx);
        auto remaining = index_.size() - idx;
        if (likely(size <= remaining)) {
            std::memcpy(&data_[idx], ptr, size);
        } else {
            std::memcpy(&data_[idx], ptr, remaining);
            std::size_t left = size - remaining;
            std::memcpy(&data_[0],
                static_cast<const uint8_t*>(ptr) + remaining, left
            );
        }
        idx += size;
    }

    void copyout(uint32_t & idx, void *ptr, size_t size) {
        idx = index_.locate(idx);
        auto remaining = index_.size() - idx;
        if (likely(size <= remaining)) {
            std::memcpy(ptr, &data_[idx], size);
        } else {
            std::memcpy(ptr, &data_[idx], remaining);
            auto left = size - remaining;
            std::memcpy(static_cast<uint8_t*>(ptr) + remaining,
                &data_[0], left
            );
        }
        idx += size;
    }

    template<typename T>
    uint32_t encodeHSize(uint32_t & idx, T size) {
        /*编码头部长度*/
        at(idx++) = static_cast<uint8_t>(size & 0xffU);
        at(idx++) = static_cast<uint8_t>((size >> 8) & 0xffU);
        at(idx++) = static_cast<uint8_t>((size >> 16) & 0xffU);
        at(idx++) = static_cast<uint8_t>((size >> 24) & 0xffU);
        idx    += RSize; /*TODO:剩余的头空间，可以做CRC32验证？*/
        return idx;
    }

    uint32_t decodeHSize(uint32_t & prev) {
        uint32_t len = 0;
        auto nr = index_.peek(HSize, [&](uint32_t idx, uint32_t count) {
            /*解码长度*/
            prev = idx;
            len |= static_cast<uint32_t>(at(idx++));
            len |= static_cast<uint32_t>(at(idx++)) << 8;
            len |= static_cast<uint32_t>(at(idx++)) << 16;
            len |= static_cast<uint32_t>(at(idx++)) << 24;
        });
        /*固定IO模式，要么为0，否则一定是大于协议长度，原子的写入*/
        assert (nr == 0 || nr >= len);
        return len;
    }

    bool push(const void *ptr, size_t size) {
        auto rc = index_.enqueue(HSize + static_cast<uint32_t>(size),
                [=](uint32_t idx, uint32_t count) {
            encodeHSize(idx, count);
            /*拷贝内容*/
            copyin(idx, ptr, size);
        });
        return rc > 0?true:false;
    }

    template <typename... Args>
    bool pack(Args&&... args) {
        uint32_t prev, next, nr_free;
        constexpr auto size  = HSize + pack_size<std::decay_t<Args>...>::value;
        auto nr = index_.prepare_enqueue(size, prev, next, nr_free);
        if (!nr ) { return false; }
        assert(nr == size);
        auto idx = prev;
        /*编码头部*/
        encodeHSize(idx, size);
        /*拷贝内容*/
        auto packer = [&](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            static_assert(std::is_trivially_copyable<T>::value,
                    "All types must be trivially copyable for raw memory copy");
            copyin(idx, &arg, sizeof(T));
        };
        int32_t dummy[] = { 0, (packer(std::forward<Args>(args)), 0)... };
        static_cast<void>(dummy);
        index_.finish_enqueue(prev, next);
        return true;
    }

    /*TODO:非线程安全，必须单线程调用*/
    bool pop(void *ptr, size_t & size) {
        uint32_t prev;
        auto len = decodeHSize(prev);
        if (!len) { return false; }
        /*ptr == nullptr 则为 peek 长度的模式*/
        if (ptr) {
            /*原子消费*/
            auto nr = index_.dequeue(len, [&](uint32_t idx, uint32_t count) {
                assert(prev == idx);
                /*移除长度*/
                idx += HSize;
                count = len - HSize;
                count = std::min(count, static_cast<uint32_t>(size));
                copyout(idx, ptr, count);
            });
            assert(nr == len);
        }
        size = len - HSize;
        return true;
    }

    template <typename... Args>
    bool unpack(Args&... args) {
        uint32_t idx;
        auto len = decodeHSize(idx);
        if (len == 0) { return false; }
        constexpr auto size = HSize + pack_size<std::decay_t<Args>...>::value;
        if (unlikely(size > len)) {
            throw std::length_error("unpack : Not match sizeof Args");
        }
        uint32_t prev, next, count;
        auto nr = index_.prepare_dequeue(len, prev, next, count);
        assert(nr == len && index_.locate(prev) == idx); /*单消费者模式*/
        /*剔除长度*/
        idx += HSize;
        /*拷贝内容*/
        auto unpacker = [&](auto& arg) {
            using T = std::decay_t<decltype(arg)>;
            static_assert(std::is_trivially_copyable<T>::value,
                    "All types must be trivially copyable for raw memory copy");
            copyout(idx, &arg, sizeof(T));
        };
        int32_t dummy[] = { 0, (unpacker(args), 0)... };
        static_cast<void>(dummy);
        index_.finish_dequeue(prev, next);
        return true;
    }

    void wakeup(sem_t * sem) {
        if (unlikely(waiting_.exchange(false, std::memory_order_acq_rel))) {
            int32_t rc = ::sem_post(sem);
            if (unlikely(rc == -1)) {
                fprintf(stderr, "Oops!!!sem post failed : %p, %s\n", sem,
                    strerror(errno));
            }
        }
    }

    void waitPrepare(void) {
        /*先判断消费端是否稳定*/
        if (!waiting_.load(std::memory_order_relaxed))
            waiting_.store(true, std::memory_order_release);
    }

    void waitFinish(void) {
        waiting_.store(false, std::memory_order_release);
    }

    static bool waitTimedout(sem_t * sem, int32_t ms) {
        return ring::sem_timedwait(sem, ms);
    }
};

struct ShareMemoryQueueBase {
    explicit ShareMemoryQueueBase(uint32_t size = 1024)
        : shift_(log2_ceil(size))
    {
        if (HSize != reinterpret_cast<uintptr_t>(
                &(static_cast<LockFreeQueue*>(0))->data_[0])) {
            throw std::invalid_argument("Invalid memory layout");
        }
    }

    uint32_t size(void) const { return queue_->size(); }

    ShareMemoryQueueBase(const ShareMemoryQueueBase &) = delete;
    ShareMemoryQueueBase & operator = (const ShareMemoryQueueBase &) = delete;

    ShareMemoryQueueBase(ShareMemoryQueueBase &&) = default;
    ShareMemoryQueueBase & operator = (ShareMemoryQueueBase &&) = default;

    template<typename T> std::pair<bool, const T &> peek(int32_t ms = -1);

    bool push(const void *, size_t);
    bool pop(void *, size_t &, int32_t ms = -1);
    bool pop(int32_t ms = -1) { /*直接丢弃，一般peek后使用*/
        char buf[1];
        size_t size = sizeof(buf);
        return pop(buf, size, ms);
    }

    template <typename... Args> bool pack(Args&&... args);
    template <typename... Args> bool unpack(Args&&... args);

    bool valid(void) const { return queue_ != nullptr; }
    virtual void close(void) { queue_.reset(); sem_.reset(); }

    virtual ~ShareMemoryQueueBase() { }

    /*强制解除VFS对象*/
    static void unlink(const std::string &name);

    /*
     * 文件锁，保证多个VFS对象创建的原子性
     * 特性              | flock                  | fcntl (POSIX)
     * 锁关联对象         | 打开文件表项             | 进程 + 索引节点
     * 同进程多 FD 互斥   | 互斥（会阻塞/失败）       | 不互斥（会覆盖/替换）
     * 关闭任一 FD 的影响 | 无影响,直到最后一个副本关闭 | 致命（释放该进程所有的锁）
     */
    template<class Func>
    static bool atomicExec(bool excl, const Func & func,
                                    const std::string & path = "");

    template <class T, class D>
    struct fd_guard {
        fd_guard() = default;
        fd_guard(T fd, D && df) : fd_(fd), df_(std::move(df)) {}
        fd_guard(fd_guard &&other) {
            this->operator=(std::forward<fd_guard>(other));
        }
        ~fd_guard(void) {
            if (fd_ > -1) try { if (df_) { df_(fd_);} } catch (...) {}
        }
        T eat(void) { T fd = fd_; fd_ = -1; return fd; }
        T operator *() { return fd_; }
        fd_guard & operator=(fd_guard && other) {
            if (this != &other) {
                reset();
                df_ = std::move(other.df_); fd_ = other.eat();
            }
            return *this;
        }
    private:
        fd_guard(const fd_guard &) = delete;
        fd_guard & operator=(const fd_guard &) = delete;
        void reset(void) {
            if (fd_ > -1) try { if (df_) { df_(fd_);} } catch (...) {}
        }
        T fd_ = -1;
        D df_ = nullptr;
    };

    using FdDeleter = std::function<void(int32_t)>;
    using RawFd = fd_guard<int32_t, FdDeleter>;
protected:
    using SemDeleter = std::function<void(sem_t*)>;
    using SemPtr = std::unique_ptr<sem_t, SemDeleter>;
    using QueueDeleter = std::function<void(LockFreeQueue*)>;
    using QueuePtr = std::unique_ptr<LockFreeQueue, QueueDeleter>;

    uint8_t     shift_{0}; /*hint值*/
    std::string name_; /*VFS对象标识*/

    /*注意析构顺序*/
    QueuePtr queue_{nullptr};
    SemPtr   sem_{nullptr};

    QueuePtr mmap(int32_t, bool);
    SemPtr   sem_open(QueuePtr&, const std::string &, bool);

    static inline uint8_t log2_ceil(uint32_t size) {
        if (size <= 1) { return 0; }
        uint8_t i = 0;
        uint32_t value = size;
        while ((value >>= 1)) { i++; }
        return i + ((1UL << i) != size?1:0);
    }
    static constexpr size_t HSize = sizeof(LockFreeQueue);
};

void ShareMemoryQueueBase::unlink(const std::string &name) {
    ::shm_unlink(name.c_str());
#ifdef __APPLE__
    /*TODO:名字检查*/
    ::sem_unlink(name.c_str());
#endif
}

template<class Func>
bool ShareMemoryQueueBase::atomicExec(bool excl, const Func & func,
            const std::string & path)
{
    const char * fileName = "/tmp/shmQ.os.flock";
    if (path.size()) { fileName = path.c_str(); }
    RawFd fd;
    SemPtr sem{nullptr, [](sem_t*){}};
    auto fid = ::open(fileName, O_CREAT | O_RDWR, 0666);
    if (fid < 0) {
        fprintf(stderr,
            "Can't create flock file : %s, try use sem to lock\n", fileName
        );
        /* 退化为 sem_open 来加锁 */
        auto obj = ::sem_open(::strrchr(fileName, '/'), O_CREAT, 0666, 1);
        if (obj == SEM_FAILED) {
            fprintf(stderr, "Can't create sem for flock : %s\n", fileName);
            return false;
        }
        sem = SemPtr(obj, [=](sem_t *ptr) {
            ::sem_post(ptr); /*信号、异常导致没有清理？*/
            ::sem_close(ptr);
        });
        if (!ring::sem_timedwait(sem.get(), 5000)) {/*最多等待5秒*/
            fprintf(stderr, "Can't hold flock file : Operation timed out\n");
            return false;
        }
    } else {
        fd = RawFd(fid, [](int32_t id) { ::close(id); });
        if (::flock(*fd, excl?LOCK_EX:LOCK_SH) == -1) {
            fprintf(stderr, "Can't hold flock file : %s\n", strerror(errno));
            return false;
        }
    }
    return func();
}

inline bool ShareMemoryQueueBase::push(const void *ptr, size_t size)
{
    if (unlikely(ptr == nullptr || size == 0)) { return false; }
    if (unlikely(!queue_)) { return false; }
    bool rc = queue_->push(ptr, size);
    if (likely(rc)) {
        queue_->wakeup(sem_.get());
        return true;
    }
    return false;
}

inline bool ShareMemoryQueueBase::pop(void * ptr, size_t & size, int32_t ms)
{
    if (unlikely(!queue_)) { return false; }
    bool rc = queue_->pop(ptr, size);
    if (!rc && ms) {
        /*自旋模式，大多临界区仅是数据拷贝，很短，所以可以尝试几次再休眠等待*/
#if 1
        for (int8_t i = 0; i < 2; i++) {
            ring::cpu_relax();
            rc = queue_->pop(ptr, size);
            if (rc) {
                return rc;
            }
        }
#endif
        for (int8_t i = 0; i < 3; i++) {
            std::this_thread::yield();
            rc = queue_->pop(ptr, size);
            if (rc) {
                return rc;
            }
        }
        /*乐观等待*/
        do {
            queue_->waitPrepare();
            rc = queue_->pop(ptr, size);
            if (rc) {
                break;
            }
        } while (queue_->waitTimedout(sem_.get(), ms));
        queue_->waitFinish();
    }
    return rc;
}

template<typename T>
std::pair<bool, const T &> ShareMemoryQueueBase::peek(int32_t ms) {
    constexpr size_t ESize = sizeof(std::decay_t<T>);
    size_t size;
    if (pop(nullptr, size, ms)) {
        if (unlikely(size < ESize)) {
            throw std::length_error("Peek : Not match sizeof(T)");
        }
    }
    uint32_t idx;
    const size_t len = queue_->decodeHSize(idx);
    if (!len) {
        return std::make_pair(false, reinterpret_cast<const T&>(queue_->at(0)));
    }
    /*TODO:如果遇尾部不连续内存，需要拷贝*/
    return std::make_pair(true, reinterpret_cast<const T&>(
        queue_->at(idx + LockFreeQueue::HSize)
    ));
}

template <typename... Args>
bool ShareMemoryQueueBase::pack(Args&&... args) {
    if (unlikely(!queue_)) { return false; }
    bool rc = queue_->pack(std::forward<Args>(args)...);
    if (likely(rc)) {
        queue_->wakeup(sem_.get());
        return true;
    }
    return false;
}

/*不加入阻塞，一般使用 peek 去等待数据到来，然后解包*/
template <typename... Args>
bool ShareMemoryQueueBase::unpack(Args&&... args) {
    if (unlikely(!queue_)) { return false; }
    return queue_->unpack(std::forward<Args>(args)...);
}

inline ShareMemoryQueueBase::QueuePtr
ShareMemoryQueueBase::mmap(int32_t fd, bool created)
{
    auto QSize = (1U << shift_);
    auto MSize = HSize + QSize;
    if (fd != -1) {
        /*共享内存*/
        if (created) {
            if (::ftruncate(fd, MSize) < 0) { /*截断大小*/
                fprintf(stderr, "set size of shared memory failed : %s\n",
                            strerror(errno));
                return nullptr;
            }
        } else {
            struct stat st;
            if (::fstat(fd, &st) == -1) { /*获取大小*/
                fprintf(stderr, "get size of shared memory failed : %s\n",
                            strerror(errno));
            }
            off_t SSize = st.st_size;
            QSize  = static_cast<uint32_t>(SSize - HSize);
            /*需要调整？*/
            if (SSize <= static_cast<off_t>(HSize)
#ifndef __APPLE__
                || ((1U << log2_ceil(QSize)) != (QSize))
#endif
            ) {
                fprintf(stderr, "invalid size of shared memory\n");
                return nullptr;
            }
            MSize = HSize + QSize;
        }
    }
    auto addr = ::mmap(nullptr, MSize,
                PROT_READ | PROT_WRITE, MAP_SHARED|(fd == -1?MAP_ANONYMOUS:0),
                fd, 0);
    if (addr == MAP_FAILED) {
        fprintf(stderr, "map shared memory failed : %s\n", strerror(errno));
        return nullptr;
    } else if (reinterpret_cast<uintptr_t>(addr) & alignof(LockFreeQueue)) {
        fprintf(stderr, "memory alignment failed\n");
        return nullptr;
    }
    /*TODO:多余部分修改访问属性*/
    /*创建者需要负责初始化*/
    if (created) {
        new (addr) LockFreeQueue(shift_);
    }
    auto queue = QueuePtr(static_cast<LockFreeQueue*>(addr),
        [=](LockFreeQueue *ptr) {
            fprintf(stderr, "unmap a shared memory : %p\n", ptr);
            /*NOTE:如果解除映射不一致，将导致严重的泄露，系统都可能会死机*/
            ::munmap(static_cast<void*>(ptr), MSize);
    });
    /*简单的检查有效性*/
    if (unlikely(queue->magic_ != 0xabadbeef) ||
            unlikely(queue->index_.size() > QSize) ||
            unlikely(queue->index_.size() != (queue->index_.mask() + 1))) {
        fprintf(stderr, "invalid shared memory block\n");
        return nullptr;
    }
    return queue;
}

inline ShareMemoryQueueBase::SemPtr
ShareMemoryQueueBase::sem_open(QueuePtr & queue, const std::string & name,
    bool created)
{
#ifdef __APPLE__
    /*TODO:名字检查*/
    if (created) { ::sem_unlink(name.c_str()); }
    int32_t flags = O_CREAT | (created?O_EXCL:0);
    sem_t * obj = ::sem_open(name.c_str(), flags, 0666, 0);
    if (obj == SEM_FAILED) {
        fprintf(stderr, "create a sem failed : %s\n", strerror(errno));
        return nullptr;
    }

    auto sem = SemPtr(obj, [=](sem_t *ptr){
        fprintf(stderr, "close a semaphore : %p\n", ptr);
        ::sem_close(ptr);
        if (created) {
            fprintf(stderr, "unlink a semaphore : %s\n", name.c_str());
            ::sem_unlink(name.c_str());
        }
    });
#else
    if (created) {
        auto rc = ::sem_init(&queue->sem_, 1, 0);
        if (rc != 0) {
            fprintf(stderr, "create a semaphore failed : %s\n", strerror(errno));
            return nullptr;
        }
    }
    auto sem = SemPtr(&queue->sem_, [=](sem_t *ptr){
        /*TODO:匿名信号量销毁后，可能导致后续调用段错误？*/
        if (created) {
            fprintf(stderr, "destory a semaphore : %p\n", ptr);
            ::sem_destroy(ptr);
        }
    });
#endif

    return sem;
}

struct ShareMemoryQueue : public ShareMemoryQueueBase {
    using ShareMemoryQueueBase::ShareMemoryQueueBase;

    /*分开创建？可以重新打开一个队列？*/
    bool open(const std::string &name);
    bool create(const std::string &name, bool excl = true);
    void close(void) override {
        ShareMemoryQueueBase::close(); fd_ = ShmFd();
    }
private:
    using ShmDeleter = std::function<void(int32_t*)>;
    using ShmFd = std::unique_ptr<int32_t, ShmDeleter>;
    RawFd shm_open(const std::string &, int32_t &);

    ShmFd fd_;
    bool  created_{false}; /*拥有者*/
};

inline ShareMemoryQueueBase::RawFd
ShareMemoryQueue::shm_open(const std::string & name, int32_t & flags)
{
    /*TODO:名字检查*/
    RawFd fd;
    int32_t oflags = O_RDWR | ((flags & O_CREAT)?(O_CREAT|O_EXCL):0); /*排他创建*/
    do {
        int32_t id = ::shm_open(name.c_str(), oflags, 0666);
        if (id < 0) {
            if (unlikely(errno != EEXIST)) {  /*其他错误*/
                fprintf(stderr, "open & create a shared memory failed : %s\n",
                    strerror(errno));
                break;
            }
            if (flags & O_EXCL) { /*排他？删除重试？*/
                fprintf(stderr, "a shared memory exited, unlink and retry\n");
                ::shm_unlink(name.c_str());
            } else { /*转而打开？*/
                oflags = O_RDWR;
                flags = 0;
            }
        }
        fd = RawFd(id, [=](int32_t fid) {
            fprintf(stderr, "close a shared memory : %d\n", fid);
            ::close(fid);
            if (flags & O_CREAT) {
                fprintf(stderr, "unlink a shared memory : %s\n", name.c_str());
                ::shm_unlink(name.c_str());
            }
        });
    } while (*fd < 0);

    return fd;
}

/*sem 和 shm 创建的原子性由调用者保证*/
inline bool ShareMemoryQueue::create(const std::string &name, bool excl)
{
    int32_t flags = (excl?O_EXCL:0)|O_CREAT;
    auto fd = shm_open(name, flags);
    if (*fd < 0) { return false; }
    bool created = flags & O_CREAT;
    /*映射*/
    auto queue = mmap(*fd, created);
    if (!queue) { return false; }
    /*创建同步信号量*/
    auto sem = sem_open(queue, name, created);
    if (!sem) { return false; }
    /*保存信息，每次打开都不一样*/
    created_ = created;
    name_    = name;
    queue_   = std::move(queue);
    sem_     = std::move(sem);
    if (created) {
        /*本次创建的，需要删除*/
        fd_  = ShmFd(reinterpret_cast<int32_t*>(*fd), [=](int32_t*) {
            fprintf(stderr, "unlink a shared memory : %s\n", name.c_str());
            ::shm_unlink(name.c_str());
        });
    }
    /*描述符可以关掉，同时忽略 RawFd 原本的清理函数*/
    ::close(fd.eat());
    return true;
}

inline bool ShareMemoryQueue::open(const std::string &name)
{
    int32_t flags = 0;
    auto fd = shm_open(name, flags);
    if (*fd < 0) { return false; }
    /*映射*/
    auto queue = mmap(*fd, false);
    if (!queue) { return false; }
    /*创建同步信号量*/
    auto sem = sem_open(queue, name, false);
    if (!sem) { return false; }
    /*保存信息，每次打开都不一样*/
    created_ = false;
    name_    = name;
    queue_   = std::move(queue);
    sem_     = std::move(sem);
    return true;
}

struct AnonMemoryQueue : public ShareMemoryQueueBase {
    using ShareMemoryQueueBase::ShareMemoryQueueBase;

    /*macOS 需要一个名称*/
    explicit AnonMemoryQueue(const std::string & name, uint32_t size = 1024)
        : ShareMemoryQueueBase(size)
    {
        /*构造即打开*/
        /*映射*/
        auto queue = mmap(-1, true);
        if (!queue) { throw std::bad_alloc(); }
        /*创建同步信号量*/
        auto sem = sem_open(queue, name, true);
        if (!sem) { throw std::invalid_argument("Can't create sem"); }
        /*保存信息，每次打开都不一样*/
        name_  = name;
        queue_ = std::move(queue);
        sem_   = std::move(sem);
    }
};

}
