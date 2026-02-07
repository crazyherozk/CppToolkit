#ifndef EZLOG_MODULE_NAME
#define EZLOG_MODULE_NAME "[ShmTopic]"
#endif

#include "../log/log.hpp"
#include "../reactor/reactor.hpp"
#include "../shmQueue/shareMemoryQueue.hpp"
#include <map>
#include <thread>
#include <string>
#include <cstddef>
#include <fstream>
#include <type_traits>
#include <stdexcept>
#include <unordered_set>
#include <unordered_map>

#ifndef MAKE_SHARED_LIB
# ifndef INLINE
#  define INLINE inline
#endif
#else
# define INLINE
#endif

#if __has_include(<shared_mutex>) && defined(__cpp_lib_shared_mutex)
# include <shared_mutex>
# define HAS_SHARED_MUTEX 1
#endif

#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#ifndef offsetof
# ifdef __compiler_offsetof
#  define offsetof(TYPE, MEMBER)    __compiler_offsetof(TYPE, MEMBER)
# else
#  define offsetof(TYPE, MEMBER)    ((size_t)&((TYPE *)0)->MEMBER)
# endif
#endif

#define RPCTOPIC_RUNTIME_VAR() \
    rpc_topic::RuntimePtr rpc_topic::Runtime::instance_ = nullptr

namespace rpc_topic {

struct Fnv1a64Hasher {
    std::size_t operator()(uint64_t key) const noexcept {
        return static_cast<std::size_t>(key);
    }
};

constexpr uint32_t PageSize     = 4096;
constexpr uint32_t NameSize     = 128;
constexpr uint32_t RecvBuffSize = 192; /*接收静态缓存大小*/
constexpr uint32_t MaxTryAgain  = 256; /*最大重试次数*/
constexpr uint32_t MaxReqCount  = 1024;
constexpr uint32_t GenTimeouts  = 500; /*通用定时间隔*/
constexpr const char * FLKPath  = "/tmp/rpcTopic.os.lock";

class Proxy;
class ShadowProxy;
class Service;
class Runtime;
class Publisher;
class Subscriber;
class RpcServer;
class RpcClient;

using ProxyPtr       = std::shared_ptr<Proxy>;
using ShadowProxyPtr = std::shared_ptr<ShadowProxy>;
using ServicePtr     = std::shared_ptr<Service>;
using RuntimePtr     = std::shared_ptr<Runtime>;
using ReactorPtr     = std::shared_ptr<qevent::reactor>;
using PublisherPtr   = std::shared_ptr<Publisher>;
using SubscriberPtr  = std::shared_ptr<Subscriber>;
using RpcServerPtr   = std::shared_ptr<RpcServer>;
using RpcClientPtr   = std::shared_ptr<RpcClient>;

using ProxyWPtr       = std::weak_ptr<Proxy>;
using ShadowProxyWPtr = std::weak_ptr<ShadowProxy>;
using ServiceWPtr     = std::weak_ptr<Service>;

using TimerId = qevent::reactor::timerId;

template <class T>
using StrWeakMap   = std::unordered_map<std::string, std::weak_ptr<T>>;
template <class T>
using IntWeakMap   = std::unordered_map<uint64_t, std::weak_ptr<T>, Fnv1a64Hasher>;
template <class T>
using IntStrongMap = std::unordered_map<uint64_t, std::shared_ptr<T>, Fnv1a64Hasher>;

template<typename T_>
using IsProxy      = std::enable_if_t<std::is_base_of<Proxy, T_>::value>;
template<typename T_>
using IsService    = std::enable_if_t<std::is_base_of<Service, T_>::value>;
template<typename T_>
using IsPublisher  = std::enable_if_t<std::is_base_of<Publisher, T_>::value>;
template<typename T_>
using IsSubscriber = std::enable_if_t<std::is_base_of<Subscriber, T_>::value>;
template<typename T_>
using IsRpcServer  = std::enable_if_t<std::is_base_of<RpcServer, T_>::value>;

template<typename T_>
using IsPodLike = std::enable_if_t<
    std::is_void<T_>::value || (std::is_standard_layout<T_>::value &&
    !std::has_virtual_destructor<T_>::value)
>;

////////////////////////////////////////////////////////////////////////////////
// 读写锁
#ifdef HAS_SHARED_MUTEX
using SharedMutex = std::shared_mutex;
using RLock = std::shared_lock<SharedMutex>;
#else
class shared_mutex {
public:
    shared_mutex(void) {
        if (::pthread_rwlock_init(&lock_, nullptr) != 0) {
            throw std::runtime_error("shared_mutex init failed");
        }
    }
    ~shared_mutex(void) noexcept { ::pthread_rwlock_destroy(&lock_); }
    shared_mutex(const shared_mutex&) = delete;
    shared_mutex& operator=(const shared_mutex&) = delete;
    // 写锁（独占）
    void lock(void) { ::pthread_rwlock_wrlock(&lock_); }
    bool try_lock(void) { return ::pthread_rwlock_trywrlock(&lock_) == 0; }
    void unlock(void) { ::pthread_rwlock_unlock(&lock_); }
    // 读锁（共享）
    void lock_shared(void) { ::pthread_rwlock_rdlock(&lock_); }
    bool try_lock_shared(void) { return ::pthread_rwlock_tryrdlock(&lock_) == 0; }
    void unlock_shared(void) { ::pthread_rwlock_unlock(&lock_); }
private:
    pthread_rwlock_t lock_;
};

template <typename _Mutex>
class shared_lock {
public:
    explicit shared_lock(_Mutex& l) : lock_(l) { lock_.lock_shared(); }
    ~shared_lock() noexcept { lock_.unlock_shared(); }
    shared_lock(const shared_lock&) = delete;
    shared_lock& operator=(const shared_lock&) = delete;
private:
    _Mutex& lock_;
};
using SharedMutex = shared_mutex;
using RLock = shared_lock<SharedMutex>;
#endif
using WLock = std::lock_guard<SharedMutex>;
using GLock = std::lock_guard<std::mutex>;
////////////////////////////////////////////////////////////////////////////////
// 打包辅助

template <typename... Args> struct pack_size;
template <typename First, typename... Rest>
struct pack_size<First, Rest...> {
    static constexpr size_t value = sizeof(First) + pack_size<Rest...>::value;
};
template <> struct pack_size<> { static constexpr size_t value = 0; };

class PackBuffer {
public:
    explicit PackBuffer(size_t cap) { reserve(cap); }
    PackBuffer(void) noexcept {
        static_assert(RecvBuffSize > 32, "RecvBuffSize is too small.");
    }
    virtual ~PackBuffer(void) { }

    PackBuffer(const PackBuffer&) = delete;
    PackBuffer& operator=(const PackBuffer&) = delete;

    PackBuffer(PackBuffer&& other) noexcept {
        move(std::move(other));
    }

    PackBuffer& operator=(PackBuffer&&other) noexcept {
        if (&other != this) { move(std::move(other)); }
        return *this;
    }

    template <typename... Args>
    size_t pack(Args&&... args) {
        constexpr size_t total_size = pack_size<std::decay_t<Args>...>::value;
        reserve(tail_ + total_size);
        auto old = tail_;
        auto packer = [&](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            static_assert(std::is_trivially_copyable<T>::value,
                    "All types must be trivially copyable for raw memory copy");
            std::memcpy(buffer_.get() + tail_, &arg, sizeof(T));
            tail_ += sizeof(T);
        };
        int32_t dummy[] = { 0, (packer(std::forward<Args>(args)), 0)... };
        static_cast<void>(dummy);
        return tail_ - old;
    }

    template <typename... Args>
    size_t unpack(Args&... args) {
        constexpr size_t total_size = pack_size<std::decay_t<Args>...>::value;
        if (unlikely(head_ + total_size > tail_)) { return 0; }
        auto old = head_;
        auto unpacker = [&](auto& arg) {
            using T = std::decay_t<decltype(arg)>;
            static_assert(std::is_trivially_copyable<T>::value,
                    "All types must be trivially copyable for raw memory copy");
            std::memcpy(&arg, buffer_.get() + head_, sizeof(T));
            head_ += sizeof(T);
        };
        int32_t dummy[] = { 0, (unpacker(args), 0)... };
        static_cast<void>(dummy);
        return head_ - old;
    }

    const void *data(void) const noexcept {
        return reinterpret_cast<const void*>(&buffer_.get()[head_]);
    }
    void *data(void) noexcept {
        return reinterpret_cast<void*>(&buffer_.get()[head_]);
    }

    template <typename T,
              typename std::enable_if<!std::is_void<T>::value, int>::type = 0>
    T* data() noexcept {
        if (size() < sizeof(T)) { return nullptr; }
        return reinterpret_cast<T*>(&buffer_.get()[head_]);
    }

    template <typename T,
              typename std::enable_if<!std::is_void<T>::value, int>::type = 0>
    const T* data() const noexcept {
        if (size() < sizeof(T)) { return nullptr; }
        return reinterpret_cast<const T*>(&buffer_.get()[head_]);
    }

    size_t size() const noexcept { return tail_ - head_; }
    size_t capacity() const noexcept { return capacity_; }
    bool empty() const noexcept { return size() == 0; }
    void clear() noexcept { head_ = tail_ = 0; }

    void resize(size_t bytes) {
        if (bytes > capacity_) { throw std::out_of_range("resize > capacity"); }
        tail_ = head_ + bytes;
    }

    void reset(void) {
        head_     = tail_ = 0;
        capacity_ = sizeof(data_);
        buffer_   = BuffPtr(data_, [](uint8_t*){});
    }
private:
    void reserve(std::size_t bytes) {
        if (bytes <= capacity_) return;
        size_t cap = capacity_ << 1;
        if (cap < bytes) cap = (bytes + 63UL) & (~63UL);
        auto tmp = BuffPtr(new uint8_t[cap], [](uint8_t*ptr){ delete[] ptr;});
        if (tail_ > head_) {
            std::memcpy(tmp.get() + head_, buffer_.get() + head_, tail_ - head_);
        }
        buffer_   = std::move(tmp);
        capacity_ = cap;
    }
    void move(PackBuffer&& other) {
        head_     = other.head_;
        tail_     = other.tail_;
        capacity_ = other.capacity_;
        if (other.buffer_.get() == other.data_) {
            /*仅拷贝有效部分*/
            std::memcpy(data_, other.data_, tail_);
            /*静态的则重置*/
            buffer_ = BuffPtr(data_, [](uint8_t *){});
        } else {
            buffer_ = std::move(other.buffer_);
        }
        other.reset();
    }

    using BuffPtr = std::unique_ptr<std::uint8_t, void(*)(uint8_t*)>;

    /*减少小数据分配*/
    alignas(std::max_align_t)
    uint8_t data_[RecvBuffSize];
    size_t  capacity_{RecvBuffSize};
    size_t  head_{0};
    size_t  tail_{0};
    BuffPtr buffer_{data_, [](uint8_t*){}};
};

////////////////////////////////////////////////////////////////////////////////
// 提取 function 的参数类型
template <typename T>
struct function_traits;

template <typename R, typename... Args>
struct function_traits<R(Args...)> {
    using return_type = R;
    using function_type = R(Args...);
    static constexpr size_t arity = sizeof...(Args);
    template <size_t I>
    struct arg {
        static_assert(I < arity, "function_traits: argument index out of range");
        using type = typename std::tuple_element<I, std::tuple<Args...>>::type;
    };
    template <size_t I>
    using arg_type = typename arg<I>::type;
};

template <typename R, typename... Args>
struct function_traits<R(*)(Args...)> : function_traits<R(Args...)>
{};

template <typename R, typename... Args>
struct function_traits<std::function<R(Args...)>> : function_traits<R(Args...)>
{};

template <typename T>
struct function_traits : function_traits<decltype(&T::operator())>
{};

template <typename C, typename R, typename... Args>
struct function_traits<R(C::*)(Args...) const> : function_traits<R(Args...)>
{};

template <typename C, typename R, typename... Args>
struct function_traits<R(C::*)(Args...)> : function_traits<R(Args...)>
{};

////////////////////////////////////////////////////////////////////////////////
// 固定长度字符串
template <std::size_t N>
class CString {
public:
    CString(void) = default;
    CString(const CString &) = default;
    CString(const char * str) { this->operator=(str); }
    CString(const std::string & str) { this->operator=(str); }

    CString & operator=(const std::string & str) {
        if (unlikely(str.size() >= N)) {
            throw std::invalid_argument("FixedString is too short");
        }
        size_ = str.size();
        if (size_) { std::memcpy(data_, str.data(), size_); }
        data_[size_] = '\0';
        return *this;
    }

    CString & operator=(const char* str) {
        std::size_t len = str?std::strlen(str):0;
        if (unlikely(len >= N)) {
            throw std::invalid_argument("FixedString is too short");
        }
        size_ = len;
        if (size_) { std::memcpy(data_, str, size_); }
        data_[size_] = '\0';
        return *this;
    }

    template <std::size_t C>
    bool operator == (const CString<C> & other) const {
        if (other.size_ == size_) {
            return std::memcmp(other.data_, data_, size_) == 0;
        }
        return false;
    }

    template <std::size_t C>
    bool operator!=(const CString<C> & other) const {
        return !(*this == other);
    }

    operator std::string() const { return std::string(data_); }
    operator const char*() const { return data_; }
    const char* c_str() const { return data_; }
    char* data(void) { return data_; }
    const char * data(void) const { return data_; }
    size_t size(void) const { return size_; }
    static constexpr std::size_t capacity() { return N; }
private:
    size_t size_{0};
    char   data_[N];
};

////////////////////////////////////////////////////////////////////////////////
// hash 计算
constexpr uint64_t
fnv1a_64(const char* str, uint64_t basis = 0xcbf29ce484222325ULL) {
    return (*str == '\0') ? basis :
        fnv1a_64(str + 1, (basis ^ uint64_t(*str)) * 0x100000001b3ULL);
}

static inline const char * toXBytes(uint32_t size) {
    static thread_local char xbytes[16];
    if (size >= 1U << 30) {
        snprintf(xbytes, sizeof(xbytes), "%.3f Gb",
                    static_cast<float>(size)/(1U<<30));
    } else if (size >= 1U << 20) {
        snprintf(xbytes, sizeof(xbytes), "%.3f Mb",
                    static_cast<float>(size)/(1U<<20));
    } else if (size >= 1U << 10) {
        snprintf(xbytes, sizeof(xbytes), "%.3f Kb",
                    static_cast<float>(size)/(1U<<10));
    } else {
        snprintf(xbytes, sizeof(xbytes), "%d bytes", size);
    }
    return xbytes;
}

////////////////////////////////////////////////////////////////////////////////
/// Runtime 定义

using NameString = CString<NameSize>;

enum class StatusValue : uint8_t {
    APP_OFFLINE,
    APP_ONLINE,
    INVALID_VALUE = 0xff,
};

struct RuntimeConfig {
    uint32_t rpcQueueSize{PageSize}; /*RPC的共享队列的大小*/
    uint32_t topicQueueSize{PageSize*16}; /*订阅发布的共享队列大小*/
    uint16_t keepAliveInterval{100}; /*保活心跳间隔时间*/
    uint16_t keepAliveCount{5}; /*保活心跳判定失败次数*/
    int16_t  taskPriority{20}; /*内部接收线程的优先级*/
    uint16_t maxReqCount{1024}; /*最大未响应的RPC请求数量*/
    bool valid(void) const;
};

using PortId = uint64_t;
using IpcKey = uint64_t;
using IpcVer = uint32_t;

enum class MsgType : uint8_t {
    REQ_REG       = 0x81, /*注册代理*/
    RSP_REG,
    REQ_SUB, /*订阅*/
    RSP_SUB,
    REQ_RPC, /*远程过程调用*/
    RSP_RPC,
    TICK_PXY, /*心跳*/
    TICK_SRV, /*心跳*/
    TOPIC_DATA, /*主题数据*/
    RPC_DATA, /*过程调用数据*/
    INTR_TASK, /*中断任务*/
    INVALID_TYPE  = 0xff,
};

enum class RunningCode : uint32_t {
    SUCCESS             = 0x80U,
    SERVICE_NOEXIST,
    PROXY_NOEXIST,
    TOPIC_NOEXIST,
    RPC_NOEXIST,
    INVALID_VALUE       = 0xffffffffU,
};

/*协议消息*/
struct MsgHdr {
    /*TODO:请求 id？*/
    MsgType  type{MsgType::INVALID_TYPE};
    uint8_t  magic[7]; /*复用？*/
    IpcVer   localVersion{0};
    IpcVer   peerVersion{0};
    IpcKey   localIpcId{0};
    IpcKey   peerIpcId{0};
    uint64_t msgId{0};
    union {
    uint64_t tick;
    PortId   topic;
    PortId   rpcId;
    };
    MsgHdr() = default;
    MsgHdr(const MsgType t) noexcept
        : type(t), localVersion(::getpid()), tick(0)
    {
        if (unlikely(t != MsgType::TOPIC_DATA && t != MsgType::RPC_DATA)) {
            tick = qevent::sys_clock();
        }
        *reinterpret_cast<uint32_t*>(&magic[0]) = 0xdeadbeefU;
    }
    bool valid(void) const {
        return *reinterpret_cast<const uint32_t*>(&magic[0]) == 0xdeadbeefU;
    }
    bool expired(uint32_t val) const {
        if (unlikely(type != MsgType::TOPIC_DATA && type != MsgType::RPC_DATA)) {
            auto diff = (qevent::sys_clock() - tick)/(1000*1000);
            return diff > val;
        }
        return false;
    }
    bool isTopic(void) const { return type == MsgType::TOPIC_DATA; }
    bool isRpc(void) const   { return type == MsgType::RPC_DATA;   }
    bool isIntr(void) const  { return type == MsgType::INTR_TASK;  }
};

using IpcHdr = MsgHdr;

struct TopicHdr : public IpcHdr {
    TopicHdr(void) : IpcHdr() {}
    TopicHdr(PortId topic) : IpcHdr() {
        this->type  = MsgType::TOPIC_DATA;
        this->topic = topic;
        *reinterpret_cast<uint32_t*>(&this->magic[0]) = 0xdeadbeefU;
    }
    bool valid(void) const {
        if (IpcHdr::valid()) {
            return this->type == MsgType::TOPIC_DATA;
        }
        return false;
    }
};

struct RpcHdr : public IpcHdr {
    RpcHdr(void) : IpcHdr() {}
    RpcHdr(PortId rpcId) : IpcHdr() {
        this->type  = MsgType::RPC_DATA;
        this->rpcId = rpcId;
        *reinterpret_cast<uint32_t*>(&this->magic[0]) = 0xdeadbeefU;
    }
    bool valid(void) const {
        if (IpcHdr::valid()) {
            return this->type == MsgType::RPC_DATA;
        }
        return false;
    }
};

struct ProtoMsg {
    MsgHdr     common;
    NameString localName;
    NameString peerName;
    union {
    uint8_t data_[0];
    struct {
        uint32_t   ipcSize;
        uint32_t   _pad;
        NameString ipcName; /*proxy 的 ipc*/
        NameString serviceName;
    } reqRegProxy;
    struct {
        RunningCode code;
        uint32_t    ipcSize;
        NameString  ipcName; /*service 的 ipc*/
    } rspRegProxy;
    struct {
        PortId     topic;
        uint8_t    cancel; /*=1 则取消订阅，不需要响应*/
        uint8_t    _pad[7];
        NameString topicName;
    } reqSubTopic;
    struct {
        RunningCode code;
        PortId      topic;
    } rspSubTopic;
    struct {
        PortId      rpcId;
        NameString  rpcName;
    } reqRegRpc;
    struct {
        RunningCode code;
        PortId      rpcId;
    } rspRegRpc;
    };

    ProtoMsg(void) : common(), localName(), peerName() {}
    ProtoMsg(MsgType t);
};

class Runtime {
public:
    using RawFd = shm::ShareMemoryQueueBase::RawFd;

    virtual ~Runtime(void) {
        app_log_info("Destroy Runtime : %s.", appName_.c_str());
    }

    /*获取运行时实例*/
    static RuntimePtr & get(void) { return instance_; }
    /*
     * 运行时初始化，必须在 所有操作 前调用
     * @appName 进程实例名，OS内唯一
     */
    static void init(const std::string &appName,
            const RuntimeConfig & cfg = RuntimeConfig())
    {
        assert(!instance_);
        instance_ = RuntimePtr(new Runtime(appName, cfg));
    }

    /*
     * 创建代理，以弱引用的方式被 运行时 管理，服务上线时可以被通知
     * @ServiceName 服务端名 @see buildService()
     * @ProxyName 代理实例名
     */
    template <
        class Proxy_ = Proxy, typename = IsProxy<Proxy_>,
        class... Args
    >
    std::shared_ptr<Proxy_> buildProxy(const std::string & AppName,
            const std::string & ServiceName, Args && ... args)
    {
        auto proxy = std::shared_ptr<Proxy_>(
            new Proxy_(AppName, ServiceName, std::forward<Args>(args)...)
        );
        return insertProxy(proxy)?proxy:nullptr;
    }

    /*创建服务，以弱引用的方式被 运行时 管理，代理上线时可以被通知*/
    template <
        class Service_ = Service, typename = IsService<Service_>,
        class... Args
    >
    std::shared_ptr<Service_> buildService(const std::string & ServiceName,
            Args && ... args)
    {
        auto service = std::shared_ptr<Service_>(
            new Service_(ServiceName, std::forward<Args>(args)...)
        );
        return insertService(service)?service:nullptr;
    }

    /*获取reactor事件循环对象*/
    ReactorPtr & loop(void) { return loop_; }
    /*配置和名称*/
    const NameString & appName(void) const { return appName_; }
    const RuntimeConfig & config(void) const { return cfg_; }
    void config(const RuntimeConfig & cfg) { GLock g(mutex_); cfg_ = cfg; }

    /*服务和代理查找*/
    ServicePtr findService(const std::string &);
    ServicePtr findService(IpcKey);
    ProxyPtr   findProxy(IpcKey);

    /*优先级*/
    void setPriority(void) const;
    /*发送协议消息*/
    void sendProtoMsg(const ProtoMsg &);
protected:
    /*管理对象辅助函数*/
    bool insertProxy(ProxyPtr);
    bool insertService(ServicePtr);
    bool insertShadowProxy(ShadowProxyPtr);
    bool createUnixSocket(void);
    /*TODO:不应该开放？*/
    ShadowProxyPtr findShadowProxy(IpcKey);
    void removeShadowProxy(IpcKey, IpcVer);
private:
    Runtime(const std::string &, const RuntimeConfig &);

    mutable std::mutex  mutex_;
    RawFd               rfd_;
    RawFd               sfd_;
    ReactorPtr          loop_;
    RuntimeConfig       cfg_;
    NameString          appName_;
    /*主动创建*/
    IntWeakMap<Proxy>   proxyIdx_;
    IntWeakMap<Service> serviceIdx_;
    StrWeakMap<Service> serviceTab_;
    /*被动创建*/
    IntStrongMap<ShadowProxy> shadowProxyIdx_;

    template<class T>
    bool insertIpcEntry(IntWeakMap<T> &, std::shared_ptr<T> &);

    template<class T>
    bool insertIpcEntry(IntStrongMap<T> &, std::shared_ptr<T> &);

    template<class T>
    std::shared_ptr<T> removeIpcEntry(IntStrongMap<T> &, uint64_t, uint32_t);

    /*接收、处理协议消息*/
    void recvProtoMsg(uint32_t);
    bool procProtoMsg(const ProtoMsg &, ssize_t);
    bool procReqRegProxy(const ProtoMsg &);
    bool procRspRegProxy(const ProtoMsg &);
    bool procReqSubTopic(const ProtoMsg &);
    bool procRspSubTopic(const ProtoMsg &);
    bool procReqRegRpc(const ProtoMsg &);
    bool procRspRegRpc(const ProtoMsg &);
    bool procSrvTick(const ProtoMsg &);
    bool procPxyTick(const ProtoMsg &);

    /*一些静态数据*/
    static RuntimePtr instance_;
public:
    /*构造Unix套接字辅助函数*/
    static RawFd createUnixSocketFd(void);
    static socklen_t genSockAddr(sockaddr_un &, const char *);
    static INLINE bool write(const std::string& filename, int value) {
        std::ofstream ofs(filename, std::ios::trunc);
        if (!ofs) return false;
        ofs << value;
        return ofs.good();
    }

    static INLINE bool read(const std::string& filename, int& value) {
        std::ifstream ifs(filename);
        if (!ifs) return false;
        return static_cast<bool>(ifs >> value);
    }
};

template<class Base, class Value = StatusValue>
class Status {
public:
    using ChangedFunc = std::function<void(Value)>;
    Status(Base & base) : host_(base) {}
    virtual ~Status(void) {
        Runtime::get()->loop()->removeTimer(timerId_);
    }
    Value status(void) const {
        return status_.load(std::memory_order::memory_order_relaxed);
    }
protected:
    Base &      host_;
    TimerId     timerId_{0,0};
    uint32_t    tick_{0};
    ChangedFunc onStatusChanged_;
    std::atomic<Value> status_{Value::INVALID_VALUE};
};

class IpcEntry {
    friend class Runtime;
    using ShmQueue = shm::ShareMemoryQueue;
public:
    virtual ~IpcEntry(void);
    IpcEntry(uint32_t size) : shmQueue_(size) {}

    ShmQueue & shmQueue(void) { return shmQueue_; }
    /*未打开之前还可以设置大小*/
    void setSize(uint32_t size) { shmQueue_.size(size); }
    uint32_t getSize(void) const { return shmQueue_.size(); }
    IpcKey ipcId(void) const { return id_; }
    IpcVer ipcVersion(void) const { return version_; }
    const NameString & ipcName(void) const { return ipcName_; }
    /*打开*/
    bool open(IpcKey id, const NameString & name, IpcVer version);
    /*关闭*/
    void close(void) { shmQueue_.close(); version_ = 0; }
    /*初始化*/
    bool init(bool creat);
    void startRecv(void);
    void stopRecv(void);
    bool compare(const IpcEntry & src) const;
protected:
    virtual void procIpcMsg(const IpcHdr &, PackBuffer &) {}
    void doTask();

    IpcKey      id_;
    IpcVer      version_{0}; /*创建此内存的版本号*/
    NameString  ipcName_;
    ShmQueue    shmQueue_;
    std::thread recvWorker_;
    std::atomic_bool recvRunning_{false};
};

class Service : public IpcEntry {
public:
    friend class Runtime;
    using ReqId    = std::pair<IpcKey, uint64_t>;

    virtual ~Service();

    template <
        class Publisher_ = Publisher, typename = IsPublisher<Publisher_>,
        class... Args
    >
    PortId buildPublisher(const std::string & topic, Args &&... args) {
        auto pubPtr = std::shared_ptr<Publisher_>(
                new Publisher_(this, topic, std::forward<Args>(args)...)
        );
        return insertPublisher(pubPtr)?pubPtr->id_:0;
    }

    template <
        class RpcServer_ = RpcServer, typename = IsRpcServer<RpcServer_>,
        class... Args
    >
    PortId buildRpcServer(const std::string & name, Args &&... args) {
        auto rpcPtr = std::shared_ptr<RpcServer_>(
                new RpcServer_(this, name, std::forward<Args>(args)...)
        );
        return insertRpcServer(rpcPtr)?rpcPtr->id_:0;
    }

    /*没有参数默认移除所有相应对象*/

    bool removePublisher(PortId topic = 0);
    bool removeRpcServer(PortId rpc = 0);

    /*快速启动指定参数类型的RPC服务*/
    template<class Callback>
    PortId launchRpcServer(const std::string & name, Callback && func);

    /*使用ID获取相应对象*/

    PublisherPtr topicPublisher(PortId topic) {
        RLock r(mutex_);
        auto it = publisherIdx_.find(topic);
        return likely(it == publisherIdx_.end()) ? nullptr : it->second;
    }

    RpcServerPtr rpcServer(PortId rpc) {
        RLock r(mutex_);
        auto it = rpcServerIdx_.find(rpc);
        return likely(it == rpcServerIdx_.end()) ? nullptr : it->second;
    }

    /*发布的参数是POD的变参列表*/

    /*广播发布*/
    template<const char* Topic_, class... Args>
    bool publish(Args &&...args) {
        constexpr PortId topicId = fnv1a_64(Topic_); /*编译时计算*/
        return publishImpl(topicId, std::forward<Args>(args)...);
    }

    template<const PortId TopicId_, class... Args>
    bool publish(Args &&...args) {
        return publishImpl(TopicId_, std::forward<Args>(args)...);
    }

    /*定向发布*/
    template<const char* Topic_, class... Args>
    bool publishTo(IpcKey key, Args &&...args) {
        constexpr PortId topicId = fnv1a_64(Topic_); /*编译时计算*/
        return publishToImpl(topicId, key, std::forward<Args>(args)...);
    }

    template<const PortId TopicId_, class... Args>
    bool publishTo(IpcKey key, Args &&...args) {
        return publishToImpl(TopicId_, key, std::forward<Args>(args)...);
    }

    /*响应，第一个参数一定是 ReqId ，由 RpcServer 的回调给出*/

    template<const char *Rpc_, class... Args>
    bool response(ReqId id, Args &&...args) {
        constexpr PortId rpcId = fnv1a_64(Rpc_); /*编译时计算*/
        return responseImpl(rpcId, id, std::forward<Args>(args)...);
    }

    template<const PortId RpcId_, class... Args>
    bool response(ReqId id, Args &&...args) {
        return responseImpl(RpcId_, id, std::forward<Args>(args)...);
    }

    uint64_t genMsgId(void) const {
        return msgId_.fetch_add(1, std::memory_order_relaxed);
    }
    const NameString & serviceName(void) const { return serviceName_; }
protected:
    Service(const std::string &);

    /*发布的包装函数*/
    template<class... Args>
    bool publishImpl(PortId topic , Args &&...args);

    template<class... Args>
    bool publishToImpl(PortId topic , IpcKey key, Args &&...args);

    /*响应的包装函数*/
    template<class... Args>
    bool responseImpl(PortId rpcId, ReqId id, Args &&...args);

    /*对象管理函数*/

    template<class T>
    bool insertIpcEntry(const char *, IntStrongMap<T> &, std::shared_ptr<T> &);

    template<class T, class Id>
    bool removeIpcEntry(const char *, IntStrongMap<T> &, Id id);

    bool insertPublisher(PublisherPtr);
    bool insertRpcServer(RpcServerPtr);
    /*处理Rpc响应的入口函数和包装函数*/
    void onRpcEvent(const RpcHdr &, PackBuffer &);
    void procIpcMsg(const IpcHdr &, PackBuffer &) override;

    mutable SharedMutex     mutex_;
    NameString              serviceName_;
    IntStrongMap<Publisher> publisherIdx_;
    IntStrongMap<RpcServer> rpcServerIdx_;
    mutable std::atomic_uint64_t msgId_{0};
};

class Proxy : public IpcEntry {
public:
    class ProxyStatus : public Status<Proxy> {
    public:
        friend class Proxy;
        friend class Runtime;
        using Status<Proxy>::Status;
        uint32_t subscribe(Status<Proxy>::ChangedFunc &&);
        void     unsubscribe(uint32_t v = 0);
    private:
        void sendRegReq(int32_t);
        void sendTick(int32_t);
        bool procTick(const ProtoMsg &);
        bool procRegRsp(const ProtoMsg &);
        bool procRpcRsp(const ProtoMsg &);
        bool procSubRsp(const ProtoMsg &);
    };

    friend class Runtime;
    friend class ProxyStatus;

    virtual ~Proxy(void) {
        stopRecv();
        app_log_info("Destroy proxy : [%s].", ipcName_.c_str());
    }

    /*创建订阅*/
    template <
        class Subscriber_ = Subscriber, typename = IsSubscriber<Subscriber_>,
        class... Args
    >
    PortId buildSubscriber(const std::string & topic, Args &&... args) {
        auto subPtr = std::shared_ptr<Subscriber_>(
                new Subscriber_(this, topic, std::forward<Args>(args)...)
            );
        return insertSubscriber(subPtr)?subPtr->id_:0;
    }

    /*创建RPC客户端，默认每个需要响应的请求，500毫秒的超时*/
    PortId buildRpcClient(const std::string & rpcNam, int32_t timeouts = 500);

    /*自动推导版本*/
    template <class Callback>
    PortId launchSubscribe(const std::string& topic, Callback && func);

    /*必须创建RpcClient*/
    template <const char* Rpc_, class Callback, class... Args>
    bool requestAsync(Callback && func, Args &&... args) {
        constexpr PortId rpcId = fnv1a_64(Rpc_); /*编译时计算*/
        return reqAsyncImpl(rpcId,
            std::forward<Callback>(func), std::forward<Args>(args)...);
    }

    template <const PortId RpcId_, class Callback, class... Args>
    bool requestAsync(Callback && func, Args &&... args) {
        return reqAsyncImpl(RpcId_,
            std::forward<Callback>(func), std::forward<Args>(args)...);
    }

    /*只要Service上线即可*/
    template <const char* Rpc_, class... Args>
    bool request(Args &&... args) {
        constexpr PortId rpcId = fnv1a_64(Rpc_); /*编译时计算*/
        return reqImpl(rpcId, std::forward<Args>(args)...);
    }

    template <const PortId RpcId_, class... Args>
    bool request(Args &&... args) {
        return reqImpl(RpcId_, std::forward<Args>(args)...);
    }

    bool removeSubscriber(PortId topic = 0);
    bool removeRpcClient(PortId rpcId = 0);

    ProxyStatus & getProxyStatusEvent(void) { return status_; }

    uint64_t genMsgId(void) const;
    const NameString & peerName(void) const { return peerName_; }
    const NameString & serviceName(void) const { return serviceName_; }

    static inline void dummy(uint64_t) {}
protected:
    Proxy(const std::string &, const std::string &);

    /*响应主题订阅*/
    void onSubscribeEvent(const TopicHdr &, PackBuffer &);
    void onRpcEvent(const RpcHdr &, PackBuffer &);
    bool insertSubscriber(SubscriberPtr);
    bool checkProtoMsg(const MsgHdr &) const;
    void procIpcMsg(const IpcHdr &, PackBuffer &) override;
    void sendSubReq(PortId, const NameString &, bool) const;
    /*发起RPC注册请求*/
    void doReqRpc(PortId, const NameString &) const;
    /*发起订阅注册请求*/
    void doReqSub(PortId id, const NameString & name) const {
        sendSubReq(id, name, false);
    }
    void doCancelSub(PortId id, const NameString & name) const {
        sendSubReq(id, name, true);
    }

    template <class... Args>
    bool reqImpl(PortId, Args &&... args);

    template <class Callback, class... Args>
    bool reqAsyncImpl(PortId, Callback && func, Args &&... args);

    SubscriberPtr topicSubscriber(PortId topic) {
        RLock r(mutex_);
        auto it = subscriberTab_.find(topic);
        return unlikely(it == subscriberTab_.end()) ? nullptr : it->second;
    }

    RpcClientPtr rpcClient(PortId id) {
        RLock r(mutex_);
        auto it = rpcClientTab_.find(id);
        return unlikely(it == rpcClientTab_.end()) ? nullptr : it->second;
    }

    mutable SharedMutex mutex_;
    ProxyStatus status_;
    IpcEntry    peerIpc_; /*Service IPC，建立连接后打开*/
    NameString  peerName_; /*Service App Name*/
    NameString  serviceName_; /*Service Name*/
    IntStrongMap<Subscriber> subscriberTab_;
    IntStrongMap<RpcClient>  rpcClientTab_;
    mutable std::atomic_uint64_t msgId_{1};
};

class ShadowProxy
    : public IpcEntry
    , public std::enable_shared_from_this<ShadowProxy> {
public:
    friend class Runtime;
    friend class ProxyStatus;
    virtual ~ShadowProxy(void) { 
        app_log_info("Destroy shadow proxy : name [%s].", ipcName_.c_str());
    }

    const NameString & peerName(void) const { return peerName_; } 

protected:
    class ProxyStatus : public Status<ShadowProxy> {
    public:
        using Status<ShadowProxy>::Status;
        void subscribe(Status<ShadowProxy>::ChangedFunc &&);
        void sendRsp(uint32_t);
        void sendTick(int32_t);
        bool procTick(const ProtoMsg &);
        bool procSubReq(const ProtoMsg &);
        bool procRpcReq(const ProtoMsg &);
    };

    ShadowProxy(const ProtoMsg &, ServicePtr &);

    /*初始化资源*/
    ProxyStatus & getProxyStatusEvent(void) { return status_; }
    bool checkProtoMsg(const MsgHdr & msg) const;

    ProxyStatus status_;
    NameString  peerName_; /*App Name of Proxy */
    ServiceWPtr service_; /*Service Ptr*/
};

/*服务端口*/
class ServicePort {
public:
    virtual ~ServicePort(void) {}
    bool valid(void) const { return service_ != nullptr; }
protected:
    ServicePort(Service *service, const std::string & name)
        : service_(service), id_(fnv1a_64(name.c_str()))
        , name_(name)
    { }

    bool insertProxy(ShadowProxyPtr);
    void removeProxy(void);
    void removeProxy(IpcKey, IpcVer);
    void reset(void) {
        service_ = nullptr; pxyTab_.clear();
    }

    Service *         service_;
    const PortId      id_;
    const std::string name_;
    /*TODO:添加一个定时器来扫描失效的 Proxy?*/
    TimerId           timer_;
    mutable SharedMutex     mutex_;
    IntWeakMap<ShadowProxy> pxyTab_; /*与该端口通信的Proxy集合*/
};

class Publisher : protected ServicePort {
public:
    friend class Runtime;
    friend class Service;
    friend class ShadowProxy;
    using NotifyFunc = std::function<void(IpcKey)>;
    virtual ~Publisher(void) {
        app_log_info("Destroy publisher : %s.", name_.c_str());
    }

    /*单播，需要版本匹配？*/
    template<class... Args>
    bool publishTo(IpcKey key, Args &&...args);

    /*广播*/
    template<class... Args>
    bool publish(Args &&...args);

protected:
    Publisher(Service *service, const std::string & topic,
                                NotifyFunc && func = nullptr)
        : ServicePort(service, topic)
        , onChanged_(std::move(func))
    {
        if (topic.empty()) {
            throw std::invalid_argument("Invalid name of Publisher.");
        }
    }
    virtual void onSubscriptionChanged(IpcKey ipc) {
        /*通知有订阅请求被接受*/
        //this->publishTo(IpcKey, ...);
        if (onChanged_) { onChanged_(ipc); }
    }

    template<class... Args>
    bool sendTopic(ShadowProxyPtr &&, uint64_t, Args &&...args);

    void reset(void) {
        WLock w(mutex_); ServicePort::reset(); onChanged_ = nullptr;
    }

    NotifyFunc        onChanged_{nullptr};
};

class Subscriber {
public:
    friend class Runtime;
    friend class Proxy;
    using ProcFunc = std::function<void(PackBuffer &)>;
    virtual ~Subscriber(void) {
        app_log_info("Destroy subscriber : %s.", name_.c_str());
    }

protected:
    Subscriber(Proxy * proxy, const std::string & topic,
            ProcFunc && func = nullptr)
        : proxy_(proxy), id_(fnv1a_64(topic.c_str()))
        , name_(topic), onEvent_(std::move(func))
    {
        if (topic.empty()) {
            throw std::invalid_argument("Invalid name of Subscriber.");
        }
    }

    void reset(void) { proxy_ = nullptr; onEvent_ = nullptr; }

    virtual void onSubscribeEvent(PackBuffer & data) {
        /*实现解包，并使用数据*/
        //data.unpack(...);
        /*默认实现*/
        if (onEvent_) { onEvent_(data); }
    }

    Proxy *           proxy_;
    const PortId     id_;
    const std::string name_;
    ProcFunc          onEvent_{nullptr};
};

class RpcServer : protected ServicePort {
public:
    friend class Runtime;
    friend class Service;
    friend class ShadowProxy;
    using ReqId    = std::pair<IpcKey, uint64_t>;
    using ProcFunc = std::function<void(const ReqId, PackBuffer &)>;
    virtual ~RpcServer(void) {
        app_log_info("Destroy RpcServer : %s.", name_.c_str());
    }

    template<class... Args>
    bool response(const ReqId, Args &&...args);

protected:
    RpcServer(Service * service, const std::string & name,
            ProcFunc && func = nullptr)
        : ServicePort(service, name), onEvent_(std::move(func))
    {
        if (name.empty()) {
            throw std::invalid_argument("Invalid name of RpcServer.");
        }
    }

    template<class... Args>
    bool sendRsp(ShadowProxyPtr &&pxy, uint64_t id, Args &&...args);

    virtual void onRpcEvent(const ReqId id, PackBuffer & data) {
        /*实现解包，并使用数据*/
        //data.unpack(...);
        //响应
        //this->response(id, ...);
        /*默认实现*/
        if (onEvent_) { onEvent_(id, data); }
    }

    void reset(void) {
        WLock w(mutex_); ServicePort::reset(); onEvent_ = nullptr;
    }

    ProcFunc          onEvent_{nullptr};
};

class RpcClient {
public:
    friend class Runtime;
    friend class Proxy;
    ~RpcClient(void) { reset(); }

protected:
    using TimerTask = std::function<void()>;
    using TimerMap  = std::map<uint64_t, TimerTask>;
    using ProcFunc  = std::function<void(bool, PackBuffer &)>;
    using ReqInfo   = std::pair<uint64_t, ProcFunc>;
    using RspCbTab  = std::unordered_map<uint64_t, ReqInfo>;
    RpcClient(Proxy * proxy, PortId id) : proxy_(proxy), id_(id) {}
    explicit RpcClient(Proxy *proxy, const std::string &name, int32_t timeouts)
        : proxy_(proxy), id_(fnv1a_64(name.c_str())) , name_(name)
        , timeouts_(static_cast<uint16_t>(timeouts))
        , maxReqCount_(Runtime::get()->config().maxReqCount)
    {
        if (name.empty()) {
            throw std::invalid_argument("Invalid name of RpcClient");
        }
        if (!timeouts_) {
            throw std::invalid_argument("Invalid timeouts of RpcClient");
        }
        if (timeouts_ < 10) {
            throw std::invalid_argument("Timeouts is too small (>=10)");
        }
        /*直接启动定时器*/
        timerId_ = Runtime::get()->loop()->addTimer(timeouts_/2,
            std::bind(&RpcClient::onTimedout, this, std::placeholders::_1));
    }

    void onRpcEvent(uint64_t msgId, PackBuffer & data);

    template <class Callback, class... Args>
    bool request(IpcEntry & srv, Callback && func, Args &&... args);

    template <class... Args>
    bool sendReq(IpcEntry & srv, uint64_t msgId, Args &&... args);

    ReqInfo removeRequest(uint64_t msgId);

    uint64_t insertTimer(TimerTask &&);
    bool removeTimer(uint64_t);
    void onTimedout(int32_t);
    template<class RpcData> void doTimedout(uint64_t);

    void reset(void);

    Proxy *     proxy_;
    std::mutex  mutex_;
    TimerId     timerId_{0,0};
    const PortId id_;
    const std::string name_;
    const uint16_t timeouts_{0};
    const uint16_t maxReqCount_{0};
    /*一个定时器来清理超时的请求*/
    TimerMap  timerTab_;
    RspCbTab  rspCbTab_;
};

INLINE bool RuntimeConfig::valid(void) const {
    if (!topicQueueSize) {
        return false;
    }

    return true;
}

INLINE ProtoMsg::ProtoMsg(MsgType t)
    : common(t), localName(Runtime::get()->appName())
{
}

INLINE Runtime::Runtime(const std::string &appName, const RuntimeConfig & cfg)
    : loop_(std::make_shared<qevent::reactor>())
    , cfg_(cfg)
    , appName_(appName)
{
    if (!appName_.size()) {
        throw std::invalid_argument("Invalid Runtime name.");
    }
    /*TODO:配置参数是否正确?*/
    const std::string filePid = "/tmp/rpcTopic.Runtime." + appName + ".pid";
    auto rc = shm::ShareMemoryQueueBase::atomicExec(true, [=](void) {
        int32_t pid;
        /*判断是否存活*/
        if (read(filePid, pid)) {
            if (::kill(pid, 0) == 0) {
                app_log_error("shmRpcTopic App [%s:%d] is running.",
                    appName.c_str(), pid);
                return false;
            }
        }
        /*创建内部通信使用的UNIX套接字*/
        auto rc = this->createUnixSocket();
        if (rc && !write(filePid, ::getpid())) {
            app_log_error("Can't write pid to file : %s.", filePid.c_str());
            rc = false;
        }
        return rc;
    }, FLKPath);
    if (!rc) {
        throw std::invalid_argument("Can't create unix socket by Runtime name");
    }
    app_log_info("Create Runtime : %s.", appName_.c_str());
}

INLINE socklen_t Runtime::genSockAddr(sockaddr_un & addr, const char * app) {
#ifdef DEBUG //其他平台可能需要清零？
    ::memset(&addr, 0, sizeof(addr));
#endif
    socklen_t l = 0;
    addr.sun_family = AF_UNIX;
    const char *sockFileFmt = "/tmp/rpcTopic.Runtime.%s.sock";
    l = snprintf(addr.sun_path, sizeof(addr.sun_path), sockFileFmt, app);
    if (l >= sizeof(addr.sun_path)) {
        throw std::invalid_argument("The name of socket is too long.");
    }
    l += offsetof(struct sockaddr_un, sun_path) + 1;
    return l;
}

INLINE Runtime::RawFd Runtime::createUnixSocketFd(void) {
    auto rc = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (rc < 0) {
        app_log_error("Can't create unix socket : %s.", ::strerror(errno));
        return RawFd();
    }
    auto fd = RawFd(rc, [](int32_t id) { ::close(id); });
    rc  = ::fcntl(*fd, F_GETFL, nullptr);
    ::fcntl(*fd, F_SETFL, rc | O_NONBLOCK | O_CLOEXEC);
    return fd;
}

INLINE bool Runtime::createUnixSocket(void) {
    auto sfd = createUnixSocketFd();
    if (*sfd < 0) { return false; }
    sfd_ = std::move(sfd);
    auto rfd = createUnixSocketFd();
    if (*rfd < 0) { return false; }
    sockaddr_un addr;
    auto len = genSockAddr(addr, appName_.c_str());
    ::unlink(addr.sun_path);
    auto rc = ::bind(*rfd, reinterpret_cast<sockaddr*>(&addr), len);
    if (rc < 0) {
        app_log_error("Can't bind socket %s : %s.", addr.sun_path,
            ::strerror(errno));
        return false;
    }
    /*监听*/
    rc = loop_->addEvent(*rfd, POLLIN,
        std::bind(&Runtime::recvProtoMsg, this, std::placeholders::_1));
    if (!rc) {
        app_log_error("Can't monitor socket : %s.", addr.sun_path);
        return false;
    }
    rfd_ = std::move(rfd);
    app_log_info("Create one socket : %s.", addr.sun_path);
    return true;
}

INLINE void Runtime::setPriority(void) const {
    if (cfg_.taskPriority == 0) { return; }
    int32_t type = 0;
    struct sched_param params;
    ::pthread_getschedparam(pthread_self(), &type, &params);
    auto min_prio = ::sched_get_priority_min(type);
    auto max_prio = ::sched_get_priority_max(type);
    int32_t prio = params.sched_priority + cfg_.taskPriority;
    prio = std::max(min_prio, std::min(max_prio, prio));
#ifdef _QNX_
    if (prio > 50) { prio = 50; }
#endif
    params.sched_priority = prio;
    if (::pthread_setschedparam(pthread_self(), SCHED_RR, &params)) {
        app_log_warn("Can't set priority of worker to %d", prio);
    } else if (::pthread_setschedparam(pthread_self(), type, &params)) {
        app_log_warn("Can't set priority of worker to %d", prio);
    }
}

INLINE ServicePtr Runtime::findService(const std::string & name) {
    GLock w(mutex_);
    auto it = serviceTab_.find(name);
    if (unlikely(it == serviceTab_.end())) { return nullptr; }
    auto obj = it->second.lock();
    if (unlikely(!obj)) { serviceTab_.erase(it); }
    return obj;
}

INLINE ServicePtr Runtime::findService(IpcKey ipcId) {
    GLock w(mutex_);
    auto it = serviceIdx_.find(ipcId);
    if (unlikely(it == serviceIdx_.end())) { return nullptr; }
    auto obj = it->second.lock();
    if (unlikely(!obj)) { serviceIdx_.erase(it); }
    return obj;
}

INLINE ProxyPtr Runtime::findProxy(IpcKey ipcId) {
    GLock w(mutex_);
    auto it = proxyIdx_.find(ipcId);
    if (unlikely(it == proxyIdx_.end())) { return nullptr; }
    auto obj = it->second.lock();
    if (unlikely(!obj)) { proxyIdx_.erase(it); }
    return obj;
}

INLINE ShadowProxyPtr Runtime::findShadowProxy(IpcKey ipcId) {
    GLock w(mutex_);
    auto it = shadowProxyIdx_.find(ipcId);
    if (unlikely(it == shadowProxyIdx_.end())) { return nullptr; }
    return it->second;
}

INLINE void Runtime::removeShadowProxy(IpcKey key, IpcVer ver) {
    ServicePtr srvPtr;
    ShadowProxyPtr pxyPtr;
{
    GLock w(mutex_);
    pxyPtr = removeIpcEntry(shadowProxyIdx_, key, ver);
    if (pxyPtr) {
        srvPtr = pxyPtr->service_.lock();
    }
}
    if (!srvPtr) { return; }
    RLock r(srvPtr->mutex_);
    for (auto && kv : srvPtr->publisherIdx_) {
        auto & pubPtr = kv.second;
        pubPtr->removeProxy(key, ver);
    }
    for (auto && kv : srvPtr->rpcServerIdx_) {
        auto & rcpPtr = kv.second;
        rcpPtr->removeProxy(key, ver);
    }
}

INLINE void Runtime::recvProtoMsg(uint32_t) {
    ProtoMsg msg;
    for (int32_t i = 0; i < 8; i++) {
        sockaddr_un addr;
        socklen_t len = sizeof(addr);
        auto rc = ::recvfrom(*rfd_, &msg, sizeof(msg), 0,
                        reinterpret_cast<sockaddr*>(&addr), &len);
        /*初步验证消息有效性*/
        if (rc < static_cast<ssize_t>(sizeof(MsgHdr))) {
            if (rc < 0) {
                if (errno != EAGAIN) {
                    app_log_error("Recv message failed : %s.", strerror(errno));
                }
                break;
            } else {
                app_log_warn("Recv invalid length from : %s.", addr.sun_path);
            }
        } else if (!procProtoMsg(msg, rc)) {
#ifdef DEBUG
            app_log_debug("Recv invalid message from : %s", addr.sun_path);
#endif
        }
    }
}

INLINE bool Runtime::procProtoMsg(const ProtoMsg & msg, ssize_t size) {
    constexpr auto offset = static_cast<ssize_t>(offsetof(ProtoMsg, data_));
    if (!msg.common.valid()) {
#ifdef DEBUG
        app_log_warn("Receive one invalid (type) protocol message");
#endif
        return false;
    }
    /*过期数据*/
    if (msg.common.expired(cfg_.keepAliveInterval)) {
#ifdef DEBUG
        app_log_warn("Receive one expired protocol message");
#endif
        return false;
    }
    if (size < offset) {
#ifdef DEBUG
        app_log_warn("Receive one invalid (layout) protocol message");
#endif
        return false;
    }
    size -= offset;
    if (msg.peerName != appName_) {
#ifdef DEBUG
        app_log_warn("Receive one invalid (target) protocol message");
#endif
        return false;
    }
    auto type = msg.common.type;
    switch (type) {
    case MsgType::REQ_REG :
        if (size == sizeof(msg.reqRegProxy)) { return procReqRegProxy(msg); }
        break;
    case MsgType::RSP_REG :
        if (size == sizeof(msg.rspRegProxy)) { return procRspRegProxy(msg); }
        break;
    case MsgType::REQ_SUB :
        if (size == sizeof(msg.reqSubTopic)) { return procReqSubTopic(msg); }
        break;
    case MsgType::RSP_SUB :
        if (size == sizeof(msg.rspSubTopic)) { return procRspSubTopic(msg); }
        break;
    case MsgType::REQ_RPC :
        if (size == sizeof(msg.reqRegRpc))   { return procReqRegRpc(msg);   }
        break;
    case MsgType::RSP_RPC :
        if (size == sizeof(msg.rspRegRpc))   { return procRspRegRpc(msg);   }
        break;
    case MsgType::TICK_SRV :
        if (size == 0) { return procSrvTick(msg); }
        break;
    case MsgType::TICK_PXY :
        if (size == 0) { return procPxyTick(msg); }
        break;
    default:
        app_log_warn("Invalid type of message : 0x%02x.", type);
        break;
    }
    return false;
}

INLINE bool Runtime::procReqRegProxy(const ProtoMsg & msg) {
    /*通过名称查找服务*/
    if (!msg.reqRegProxy.serviceName.size()) { return false; }
    auto srvNam = msg.reqRegProxy.serviceName.c_str();
    auto srvPtr = findService(srvNam);
    if (!srvPtr) {
        app_log_warn("Can't found service to register Proxy : "
            "app [%s], service [%s].", msg.localName.c_str(), srvNam);
        /*TODO:响应service不存在，对端停止探测该服务*/
        return false;
    }
    /*创建Proxy影子，必须验证唯一性*/
    auto pxyPtr = ShadowProxyPtr(new ShadowProxy(msg, srvPtr));
    /*TODO:离散到Service，由响应的Service 来管理？*/
    bool rc = insertShadowProxy(pxyPtr);
    if (!rc) { return false; }
    auto key = pxyPtr->id_;
    auto ver = pxyPtr->version_;
    /*启动状态机*/
    pxyPtr->getProxyStatusEvent().subscribe([=](StatusValue status) {
        /*只处理离线*/
        if (status == StatusValue::APP_OFFLINE) {
            /*
             * 移除失效Proxy订阅表、响应表
             * 定时器回调此函数，但存在 ABA 问题，必须 使用 version 进行匹配
             */
            Runtime::get()->removeShadowProxy(key, ver);
        }
    });
    return true;
}

INLINE bool Runtime::procRspRegProxy(const ProtoMsg & msg) {
    auto pxyPtr = findProxy(msg.common.peerIpcId);
    if (!pxyPtr) {
        app_log_warn("Can't found proxy : id [0x%016llx], version [%d].",
            msg.common.peerIpcId, msg.common.peerVersion);
        return false;
    }
    return pxyPtr->getProxyStatusEvent().procRegRsp(msg);
}

INLINE bool Runtime::procReqSubTopic(const ProtoMsg & msg) {
    auto pxyPtr = findShadowProxy(msg.common.localIpcId);
    if (!pxyPtr) {
        /*离线的 Proxy 不应该发送订阅请求，保留这个信息，以查询问题*/
        app_log_warn("Can't found shadow proxy : id [0x%016llx], version [%d].",
                msg.common.localIpcId, msg.common.localVersion);
        return false;
    }
    return pxyPtr->getProxyStatusEvent().procSubReq(msg);
}

INLINE bool Runtime::procRspSubTopic(const ProtoMsg & msg) {
    auto pxyPtr = findProxy(msg.common.peerIpcId);
    if (!pxyPtr) {
        app_log_warn("Can't found proxy : id [0x%016llx], version [%d].",
                msg.common.peerIpcId, msg.common.peerVersion);
        return false;
    }
    return pxyPtr->getProxyStatusEvent().procSubRsp(msg);
}

INLINE bool Runtime::procReqRegRpc(const ProtoMsg & msg) {
    auto pxyPtr = findShadowProxy(msg.common.localIpcId);
    if (!pxyPtr) {
        /*离线的 Proxy 不应该发送订阅请求，保留这个信息，以查询问题*/
        app_log_warn("Can't found shadow proxy : id [0x%016llx], version [%d] .",
                msg.common.localIpcId, msg.common.localVersion);
        return false;
    }
    return pxyPtr->getProxyStatusEvent().procRpcReq(msg);
}

INLINE bool Runtime::procRspRegRpc(const ProtoMsg & msg) {
    auto pxyPtr = findProxy(msg.common.peerIpcId);
    if (!pxyPtr) {
        app_log_warn("Can't found proxy : id [0x%016llx], version [%d].",
            msg.common.peerIpcId, msg.common.peerVersion);
        return false;
    }
    return pxyPtr->getProxyStatusEvent().procRpcRsp(msg);
}

INLINE bool Runtime::procSrvTick(const ProtoMsg & msg) {
    auto pxyPtr = findProxy(msg.common.peerIpcId);
    if (!pxyPtr) {
        app_log_warn("Can't found proxy : id [0x%016llx], version [%d].",
            msg.common.peerIpcId, msg.common.peerVersion);
        return false;
    }
    return pxyPtr->getProxyStatusEvent().procTick(msg);
}

INLINE bool Runtime::procPxyTick(const ProtoMsg & msg) {
    auto pxyPtr = findShadowProxy(msg.common.localIpcId);
    if (!pxyPtr) {
        app_log_warn("Can't found shadow proxy : id [0x%016llx], version [%d].",
            msg.common.localIpcId, msg.common.localVersion);
        return false;
    }
    return pxyPtr->getProxyStatusEvent().procTick(msg);
}

INLINE void Runtime::sendProtoMsg(const ProtoMsg & msg) {
    if (!msg.common.valid()) {
        app_log_error("Invalid message.");
        return;
    }
    /*计算发送的长度*/
    size_t len = offsetof(ProtoMsg, data_);
    switch (msg.common.type) {
    case MsgType::REQ_REG : len += sizeof(msg.reqRegProxy); break;
    case MsgType::RSP_REG : len += sizeof(msg.rspRegProxy); break;
    case MsgType::REQ_SUB : len += sizeof(msg.reqSubTopic); break;
    case MsgType::RSP_SUB : len += sizeof(msg.rspSubTopic); break;
    case MsgType::REQ_RPC : len += sizeof(msg.reqRegRpc);   break;
    case MsgType::RSP_RPC : len += sizeof(msg.rspRegRpc);   break;
    case MsgType::TICK_SRV : /*心跳没有额外的负载*/
    case MsgType::TICK_PXY : break;
    default:
        app_log_error("Invalid type of message : 0x%02x.", msg.common.type);
        return;
    }
    sockaddr_un addr;
    /*需要对端名来路由消息*/
    socklen_t l = genSockAddr(addr, msg.peerName.c_str());
#ifdef DEBUG
    app_log_debug("Send message [0x%02x] to : %s", msg.common.type,
         addr.sun_path);
#endif
    /* macOS 上必须使用不同的套接字发送，否则将发生内核级死锁 */
    auto rc = ::sendto(*sfd_, &msg, len, 0,
            reinterpret_cast<const sockaddr*>(&addr), l);
#ifdef DEBUG
    if (rc != len) {
        app_log_debug("Send message failed to : %s, %s", addr.sun_path,
            rc < 0?strerror(errno):"Oops!!!Message is too long");
    }
#else
    static_cast<void>(rc);
#endif
}

template<class T>
bool Runtime::insertIpcEntry(IntStrongMap<T> & map, std::shared_ptr<T> & entry) {
    auto res = map.emplace(entry->id_, entry);
    auto & it = res.first;
    if (!res.second) {
        /*已经存在，但是可能失效，检测*/
        auto & origin = it->second;
        /*虚化检查机制？*/
        if (origin->version_ == entry->version_) {
            if (::kill(origin->version_, 0) == 0) {
                app_log_warn("IPC is alive : name [%s], version [%d].",
                    origin->ipcName_.c_str(), origin->version_);
                return false;
            }
        }
        /*移除旧的*/
        app_log_warn("IPC was death : name [%s], version [%d].",
                origin->ipcName_.c_str(), origin->version_);
        /*更新*/
        it->second = entry;
    }
    auto rc = entry->init(false);
    if (!rc) { map.erase(it); }
    return rc;
}

template<class T> std::shared_ptr<T>
Runtime::removeIpcEntry(IntStrongMap<T> & map, uint64_t key, uint32_t ver)
{
    auto it = map.find(key);
    if (it == map.end()) { return nullptr; }
    if (it->second->version_ != ver) { return nullptr; }
    auto val = std::move(it->second);
    map.erase(it);
    return val;
}

template<class T>
bool Runtime::insertIpcEntry(IntWeakMap<T> & map, std::shared_ptr<T> & entry) {
    auto res = map.emplace(entry->id_, entry);
    auto & it = res.first;
    if (!res.second) {
        /*已经存在，但是可能失效*/
        auto origin = it->second.lock();
        if (origin) {
            app_log_warn("IPC already exists : name [%s], version [%d].",
                origin->ipcName_.c_str(), origin->version_);
            return false;
        }
        /*更新*/
        it->second = entry;
    }
    auto rc = entry->init(true);
    if (!rc) { map.erase(it); }
    return rc;
}

INLINE bool Runtime::insertProxy(ProxyPtr proxy) {
    assert(proxy);
    GLock w(mutex_);
    bool rc = insertIpcEntry(proxyIdx_, proxy);
    app_log_info("Create proxy %s : app [%s], service [%s].",
        rc?"success":"failed", proxy->peerName_.c_str(),
        proxy->serviceName_.c_str());
    return rc;
}

INLINE bool Runtime::insertShadowProxy(ShadowProxyPtr proxy) {
    assert(proxy);
    GLock w(mutex_);
    bool rc = insertIpcEntry(shadowProxyIdx_, proxy);
    if (rc) {
        auto srvPtr = proxy->service_.lock();
        const char *name = srvPtr?srvPtr->serviceName_.c_str():"N/A";
        app_log_info("Create shadow proxy, proxy was online : "
            "app [%s], service [%s], version [%d].",
            proxy->peerName_.c_str(), name, proxy->version_);
    }
    return rc;
}

INLINE bool Runtime::insertService(ServicePtr service) {
    assert(service);
    GLock w(mutex_);
    auto & name = service->serviceName_;
    auto rc = insertIpcEntry(serviceIdx_, service);
    if (rc) {
        auto res = serviceTab_.emplace(name, service);
        if (!res.second) { res.first->second = service; }
    } else {
        serviceTab_.erase(name.c_str());
    }
    app_log_info("Create service %s : app [%s], service [%s].",
            rc?"success":"failed", appName_.c_str(), name.c_str());
    return rc;
}

INLINE bool IpcEntry::init(bool creat) {
    return shm::ShareMemoryQueueBase::atomicExec(creat, [=](void) {
        bool rc;
#ifdef __APPLE__
        char name[PSHMNAMLEN];
        auto s = snprintf(name, sizeof(name), "/rpc.%llx.shm", id_);
        if (s >= sizeof(name)) {
            app_log_warn("The length of IPC name is too long : %s.",
                ipcName_.c_str());
            return false;
        }
        rc = shmQueue_.create(name, creat);
#else
        rc = shmQueue_.create(std::string(ipcName_.c_str()) + ".shm", creat);
#endif
        if (rc && creat) {
            /*本地创建的*/
            version_ = ::getpid();
        }
        app_log_info("%s IPC %s : name [%s], id [0x%016llx], version [%d], "
            "size [%s].", creat?"Create":"Open", rc?"success":"failed",
            ipcName_.c_str(), id_, version_, toXBytes(shmQueue_.size()));
        return rc;
    }, FLKPath);
}

INLINE IpcEntry::~IpcEntry(void) {
    app_log_info("Destroy IPC : name [%s], id [0x%016llx], version [%d].",
        ipcName_.c_str(), id_, version_);
    id_      = 0;
    version_ = 0;
}

INLINE bool IpcEntry::open(IpcKey id, const NameString & name, IpcVer version) {
    /*重复打开，自动移除旧的映射*/
    id_      = id;
    ipcName_ = name;
    version_ = version;
    return init(false);
}

INLINE void IpcEntry::startRecv(void) {
    if (unlikely(!recvRunning_) && !recvWorker_.joinable()) {
        recvRunning_ = true;
        recvWorker_  = std::thread(&IpcEntry::doTask, this);
    }
}

INLINE void IpcEntry::stopRecv(void) {
    /*TODO:发送一条无效消息去中断？*/
    if (recvWorker_.joinable()) {
        recvRunning_ = false;
        MsgHdr hdr(MsgType::INTR_TASK);
        shmQueue_.push(&hdr, sizeof(hdr));
        recvWorker_.join();
    }
}

INLINE bool IpcEntry::compare(const IpcEntry & src) const {
    if (&src != this) {
        return id_ == src.id_ && version_ == src.version_;
    }
    return true;
}

INLINE void IpcEntry::doTask(void) {
    static_assert(sizeof(RpcHdr  )==sizeof(IpcHdr), "RpcHdr isn't MsgHdr");
    static_assert(sizeof(TopicHdr)==sizeof(IpcHdr), "TopicHdr isn't MsgHdr");
    app_log_debug("Start receive message : %s", ipcName_.c_str());
    /*尝试提高优先级*/
    Runtime::get()->setPriority();
    while (likely(recvRunning_)) {
        try {
            /*超时接收，外层保证了循环*/
            size_t size;
            /*超时值只影响析构时的线程回收速度*/
            constexpr int32_t timeouts = -1;
            bool rc = shmQueue_.pop(nullptr, size, timeouts);
            if (!rc) { continue; }
            PackBuffer buff(size);
            buff.resize(size);
            rc = shmQueue_.pop(buff.data(), size, 0);
            assert(rc);
            assert(size == buff.size());
            /*消费头部*/
            IpcHdr hdr;
            size = buff.unpack(hdr);
            if (likely(hdr.valid())) {
                if (unlikely(hdr.isIntr())) { break; }
                procIpcMsg(hdr, buff);
            } else {
                app_log_warn("Oops!!!Invalid message from ShmQueue.");
            }
        } catch (const std::exception & e) {
            app_log_error("Oops!!! %s", e.what());
        }
    }
    app_log_debug("Stop receive message : %s", ipcName_.c_str());
}

INLINE Service::Service(const std::string & serviceName)
    : IpcEntry(Runtime::get()->config().rpcQueueSize)
    , serviceName_(serviceName)
{
    if (unlikely(serviceName.empty())) {
        throw std::invalid_argument("Invalid service name");
    }
    /*生成本地名称*/
    std::string ipcName = "/rpcTopic.service.";
    ipcName += Runtime::get()->appName().c_str(); // OS 唯一
    ipcName += "." + serviceName; // APP 内唯一
    //ipcName += "." + std::to_string(::getpid());
    ipcName_ = ipcName;
    id_ = fnv1a_64(ipcName.c_str());
    app_log_info("Gen name of service [0x%016llx] : %s.", id_, ipcName_.c_str());
}

INLINE Service::~Service(void) {
    {
        WLock w(mutex_);
        /*失效Publisher，否则发布会错误引用该销毁对象的引用*/
        for (auto && kv: publisherIdx_) { kv.second->reset(); }
        /*失效RpcServer，否则响应会错误引用该销毁对象*/
        for (auto && kv: rpcServerIdx_) { kv.second->reset(); }
    }
    stopRecv();
    app_log_info("Destroy service : %s.", serviceName_.c_str());
}

template<class Callback>
PortId Service::launchRpcServer(const std::string & name, Callback && func) {
    using traits    = function_traits<std::decay_t<Callback>>;
    using RpcReturn = std::decay_t<typename traits::return_type>;
    using RpcData   = std::decay_t<typename traits::template arg_type<0>>;
    static_assert(traits::arity == 1, "func must take exactly 1 argument");
    static_assert(!std::is_pointer<RpcData>::value, "RpcData cannot be Pointer");
    static_assert(std::is_trivially_copyable<RpcData>::value,
        "RpcData must be trivially copyable for raw buffer access");
    static_assert(std::is_void<RpcReturn>::value, "RpcReturn must be void");
    return buildRpcServer<>(name, [=, proc = std::forward<Callback>(func)]
        (const RpcServer::ReqId reqId, PackBuffer & buff) mutable
    {
        if (buff.size() != sizeof(RpcData)) {
            app_log_error("Oops!!! size of RpcData miss match");
            return;
        }
        if (reqId.second) {
            app_log_warn("Oops!!! This RPC [%s] should not return any data.",
                name.c_str());
        }
        auto data = buff.data<RpcData>();
        proc(*data);
    });
}

template<class T>
bool Service::insertIpcEntry(const char *Name, IntStrongMap<T> & map,
    std::shared_ptr<T> & entry)
{
    assert(entry);
    WLock w(mutex_);
    auto res = map.emplace(entry->id_, entry);
    if (!res.second) {
        app_log_warn("%s already exists : app [%s], id [0x%016llx], name [%s]",
            Name, Runtime::get()->appName().c_str(),
            entry->id_, entry->name_.c_str());
    } else {
        app_log_info("Create %s : app [%s], service [%s], name [%s].",
            Name, Runtime::get()->appName().c_str(),
            serviceName_.c_str(), entry->name_.c_str());
    }
    return res.second;
}

template<class T, class Id>
bool Service::removeIpcEntry(const char *Name, IntStrongMap<T> & map, Id id) {
    std::decay_t<decltype(map)> gc;
    if (id == 0) {
        WLock w(mutex_);
        /*移除所有*/
        map.swap(gc);
    } else {
        gc.reserve(1);
        WLock w(mutex_);
        auto it = map.find(id);
        if (it == map.end()) { return false; }
        auto entry = std::move(it->second);
        gc.emplace(id, std::move(entry));
        map.erase(it);
    }
    /*Service 并没有失效，所以可以在所外重置移除的对象*/
    for (auto && kv : gc) {
        auto & entry = kv.second;
        app_log_info("Remove %s : app [%s], service [%s], name [%s].",
            Name, Runtime::get()->appName().c_str(),
            serviceName_.c_str(), entry->name_.c_str());
        /*移除其中所有的Proxy订阅表*/
        entry->reset();
    }
    return true;
}

INLINE bool Service::insertPublisher(PublisherPtr pubPtr) {
    return insertIpcEntry("Publisher", publisherIdx_, pubPtr);
}

INLINE bool Service::insertRpcServer(RpcServerPtr rpcPtr) {
    return insertIpcEntry("RpcServer", rpcServerIdx_, rpcPtr);
}

INLINE bool Service::removePublisher(PortId topic) {
    return removeIpcEntry("Publisher", publisherIdx_, topic);
}

INLINE bool Service::removeRpcServer(PortId rpc) {
    return removeIpcEntry("RpcServer", rpcServerIdx_, rpc);
}

INLINE void Service::onRpcEvent(const RpcHdr & hdr, PackBuffer & data) {
    auto rpcPtr = rpcServer(hdr.rpcId);
    if (unlikely(!rpcPtr)) {
#ifdef DEBUG
        app_log_warn("Can't found rpcServer of proxy from [%d] : 0x%016llx.",
            hdr.localVersion, hdr.rpcId);
#endif
        return;
    }
#ifdef DEBUG
    app_log_debug("[%s] Receive one rpc request from [%d] : "
        "id [0x%016llx], name [%s], msgId [0x%016llx].",
        serviceName_.c_str(), hdr.localVersion, hdr.rpcId,
        rpcPtr->name_.c_str(), hdr.msgId);
#endif
    rpcPtr->onRpcEvent({hdr.localIpcId, hdr.msgId}, data);
}

INLINE void Service::procIpcMsg(const IpcHdr &hdr, PackBuffer &buff) {
#ifdef DEBUG
    if (unlikely(hdr.peerIpcId != id_)) {
        app_log_warn("Invalid message, miss match local id : [0x%02x].",
            hdr.type);
        return;
    }
    if (unlikely(hdr.peerVersion != version_)) {
        app_log_warn("Expired message, miss match local version : [0x%02x].",
            hdr.type);
        return;
    }
#endif
    /*TODO: 数据有效性，来之匹配的 Proxy*/
    if (likely(hdr.isRpc())) {
        this->onRpcEvent(static_cast<const RpcHdr&>(hdr), buff);
    } else {
        app_log_warn("Invalid type of message");
    }
}

template<class... Args>
bool Service::publishImpl(PortId topic, Args &&...args) {
    auto pubPtr = topicPublisher(topic);
    if (unlikely(!pubPtr)) { return false; }
    return pubPtr->publish(std::forward<Args>(args)...);
}

template<class... Args>
bool Service::publishToImpl(PortId topic, IpcKey key, Args &&...args) {
    auto pubPtr = topicPublisher(topic);
    if (unlikely(!pubPtr)) { return false; }
    return pubPtr->publishTo(key, std::forward<Args>(args)...);
}

template<class... Args>
bool Service::responseImpl(PortId rpcId, ReqId id, Args &&...args) {
    auto rpcPtr = rpcServer(rpcId);
    if (unlikely(!rpcPtr)) { return false; }
    return rpcPtr->response(id, std::forward<Args>(args)...);
}

INLINE Proxy::Proxy(const std::string & peerName, const std::string & serviceName)
    : IpcEntry(Runtime::get()->config().topicQueueSize)
    , status_(*this)
    , peerIpc_(0)
    , peerName_(peerName)
    , serviceName_(serviceName)
{
    if (peerName.empty() || serviceName.empty()) {
        throw std::invalid_argument("Invalid peer name or service name");
    }
    /*生成本地名称，进程号代替*/
    std::string ipcName = "/rpcTopic.proxy.";
    ipcName += Runtime::get()->appName().c_str(); // OS 唯一
    ipcName += "." + peerName; // OS 唯一
    ipcName += "." + serviceName; // APP 内唯一
    /*如果每次都一样，导致内存不会变，一旦在操作内存时崩溃，就破坏了shmQueue的结构?*/
    //ipcName += "." + std::to_string(::getpid());
    ipcName_ = ipcName;
    id_ = fnv1a_64(ipcName.c_str());
    app_log_info("Gen name of proxy [0x%016llx] : %s.", id_, ipcName_.c_str());
}

template <class... Args>
bool Proxy::reqImpl(PortId rpcId, Args &&... args) {
    if (unlikely(status_.status_ != StatusValue::APP_ONLINE)) {
        app_log_info("Service was offline already : %s.", serviceName_.c_str());
        return false;
    }
    /*不需要响应的RPC，构造一个 dummy RpcClient*/
    RpcClient dummy(this, rpcId);
    auto rpcPtr  = RpcClientPtr(&dummy, [](RpcClient*){});
    /*
     * 用读锁保证 peerIpc_ 的有效性
     * TODO: peerIpc_ 使用shared_ptr<> 来保证有效性，从而移除持有读锁?
     */
    RLock r(mutex_);
    if (likely(status_.status_ != StatusValue::APP_ONLINE) ||
            unlikely(peerIpc_.ipcVersion() == 0))
    {
        app_log_warn("Service was offline already : %s.", serviceName_.c_str());
        return false;
    }
    /*发送0值，表示不需要回复*/
    return rpcPtr->sendReq(peerIpc_, 0, std::forward<Args>(args)...);
}

template <class Callback, class... Args>
bool Proxy::reqAsyncImpl(PortId rpcId, Callback && func, Args &&... args) {
    if (unlikely(status_.status_ != StatusValue::APP_ONLINE)) {
        app_log_warn("Service was offline already : %s.", serviceName_.c_str());
        return false;
    }
    auto rpcPtr = rpcClient(rpcId);
    if (unlikely(!rpcPtr)) {
        app_log_warn("Can't found RpcClient : "
            "app [%s], service [%s], id [0x%016llx]",
            peerName_.c_str(), serviceName_.c_str(), rpcId);
        return false;
    }
    RLock r(mutex_);
    if (status_.status_ != StatusValue::APP_ONLINE ||
            unlikely(peerIpc_.ipcVersion() == 0))
    {
        app_log_warn("Service was offline already : %s.", serviceName_.c_str());
        return false;
    }
    return rpcPtr->request(peerIpc_, func, std::forward<Args>(args)...);
}

INLINE void Proxy::onSubscribeEvent(const TopicHdr & hdr, PackBuffer & data) {
    /*TODO:大量的原子操作，使用RCU释放*/
    auto subPtr = topicSubscriber(hdr.topic);
    if (unlikely(!subPtr)) {
#ifdef DEBUG
        /*TODO:异步事件的方式发送反馈到 Servier 移除订阅？*/
        app_log_warn("Can't found topic of proxy from [%d]: 0x%016llx",
            hdr.localVersion, hdr.topic);
#endif
        return;
    }
#ifdef DEBUG
    app_log_debug("Receive one topic from : "
        "service [%s], verison [%d], name [%s], msgId [0x%016llx].",
        serviceName_.c_str(), hdr.localVersion, subPtr->name_.c_str(), hdr.msgId
    );
#endif
    subPtr->onSubscribeEvent(data);
}

INLINE void Proxy::onRpcEvent(const RpcHdr & hdr, PackBuffer & data) {
    auto rpcPtr = rpcClient(hdr.rpcId);
    if (unlikely(!rpcPtr)) {
#ifdef DEBUG
        app_log_warn("Can't found rpcClient of proxy from [%d]: 0x%016llx",
            hdr.localVersion, hdr.rpcId);
#endif
        return;
    }
#ifdef DEBUG
    app_log_debug("[%s] Receive one rpc response from [%d] : "
        "id [0x%016llx], name [%s], msgId [0x%016llx].",
        serviceName_.c_str(), hdr.localVersion, hdr.rpcId,
        rpcPtr->name_.c_str(), hdr.msgId);
#endif
    rpcPtr->onRpcEvent(hdr.msgId, data);
}

INLINE void Proxy::procIpcMsg(const IpcHdr & hdr, PackBuffer & buffer) {
    /*应该全部丢弃？*/
    if (unlikely(status_.status_ != StatusValue::APP_ONLINE)) { return; }
#ifdef DEBUG
    if (unlikely(!checkProtoMsg(hdr))) { return; }
#endif
    if (likely(hdr.isTopic())) {
        this->onSubscribeEvent(static_cast<const TopicHdr&>(hdr), buffer);
    } else if (likely(hdr.isRpc())) {
        this->onRpcEvent(static_cast<const RpcHdr&>(hdr), buffer);
    } else {
        app_log_warn("Invalid type of message");
    }
}

INLINE void Proxy::sendSubReq(PortId topicId, const NameString & name,
        bool cancel) const
{
    if (status_.status_ != StatusValue::APP_ONLINE) { return; }
    ProtoMsg msg(MsgType::REQ_SUB);
    msg.common.localIpcId  = this->ipcId();
    msg.common.peerIpcId   = peerIpc_.ipcId();
    msg.common.peerVersion = peerIpc_.ipcVersion();
    msg.peerName           = peerName_;
    msg.reqSubTopic.cancel    = cancel?1:0;
    msg.reqSubTopic.topic     = topicId;
    msg.reqSubTopic.topicName = name;
    Runtime::get()->sendProtoMsg(msg);
}

INLINE void Proxy::doReqRpc(PortId rpcId, const NameString & name) const {
    if (status_.status_ != StatusValue::APP_ONLINE) { return; }
    ProtoMsg msg(MsgType::REQ_RPC);
    msg.common.localIpcId  = this->ipcId();
    msg.common.peerIpcId   = peerIpc_.ipcId();
    msg.common.peerVersion = peerIpc_.ipcVersion();
    msg.peerName           = peerName_;
    msg.reqRegRpc.rpcId    = rpcId;
    msg.reqRegRpc.rpcName  = name;
    Runtime::get()->sendProtoMsg(msg);
}

INLINE bool Proxy::insertSubscriber(SubscriberPtr subPtr) {
    WLock w(mutex_);
    auto res = subscriberTab_.emplace(subPtr->id_, subPtr);
    if (!res.second) {
        app_log_warn("Subscriber already exists : "
            "app [%s], service [%s], id [0x%016llx], topic [%s].",
            peerName_.c_str(), serviceName_.c_str(),
            subPtr->id_, subPtr->name_.c_str());
        return false;
    }
    app_log_info("Create Subscriber : "
        "app [%s], service [%s], id [0x%016llx], topic [%s].",
        peerName_.c_str(), serviceName_.c_str(),
        subPtr->id_, subPtr->name_.c_str());
    /*发起主题订阅请求*/
    doReqSub(subPtr->id_, subPtr->name_);
    return true;
}

template <class Callback>
PortId Proxy::launchSubscribe(const std::string& topic, Callback && func) {
    using traits      = function_traits<std::decay_t<Callback>>;
    using TopicData   = std::decay_t<typename traits::template arg_type<0>>;
    using TopicReturn = std::decay_t<typename traits::return_type>;
    static_assert(traits::arity == 1, "func must take exactly 1 argument");
    static_assert(!std::is_pointer<TopicData>::value,
        "TopicData cannot be Pointer");
    static_assert(std::is_trivially_copyable<TopicData>::value,
        "TopicData must be trivially copyable for raw buffer access");
    static_assert(std::is_void<TopicReturn>::value,
        "This Subscribe Callback should not return any data.");
    return buildSubscriber<>(topic,
        [proc = std::forward<Callback>(func)](PackBuffer & buff) mutable
    {
        if (buff.size() != sizeof(TopicData)) {
            app_log_error("Oops!!! size of TopicData miss match");
            return;
        }
        auto data = buff.data<TopicData>();
        proc(*data);
    });
}

INLINE PortId Proxy::buildRpcClient(const std::string &name, int32_t timeouts) {
    auto rpcPtr = RpcClientPtr(new RpcClient(this, name, timeouts));
    WLock w(mutex_);
    auto res = rpcClientTab_.emplace(rpcPtr->id_, rpcPtr);
    if (!res.second) {
        app_log_warn("RpcClient already exists : "
            "app [%s], service [%s], id [0x%016llx], name [%s].",
            peerName_.c_str(), serviceName_.c_str(),
            rpcPtr->id_, rpcPtr->name_.c_str());
        return false;
    }
    app_log_info("Create RpcClient : "
        "app [%s], service [%s], id [0x%016llx], name [%s].",
        peerName_.c_str(), serviceName_.c_str(),
        rpcPtr->id_, rpcPtr->name_.c_str());
    /*发送注册，虽然和执行请求不能同步，但是能防止后续的错误*/
    doReqRpc(rpcPtr->id_, rpcPtr->name_);
    return true;
}

INLINE bool Proxy::removeSubscriber(PortId topicId) {
    /*
     * !!!NOTE:
     * Subscriber 有自己的析构，可能有对 Proxy 的操作
     * 从而发生死锁，所以不能简单的加锁移除 Subscriber，
     * 并且由于为了保证一致性，发送取消订阅必须持有锁
     */
    decltype(subscriberTab_) gc;
    if (topicId == 0) {
        WLock w(mutex_);
        /*移除所有的*/
        subscriberTab_.swap(gc);
        for (auto && kv : gc) {
            auto & subPtr = kv.second;
            doCancelSub(subPtr->id_, subPtr->name_);
        }
    } else {
        gc.reserve(1);
        WLock w(mutex_);
        auto it = subscriberTab_.find(topicId);
        if (it == subscriberTab_.end()) { return false; }
        /*发送取消订阅*/
        auto subPtr = std::move(it->second);
        doCancelSub(topicId, subPtr->name_);
        gc.emplace(topicId, std::move(subPtr));
        subscriberTab_.erase(it);
    }
    for (auto && kv : gc) {
        auto & subPtr = kv.second;
        subPtr->reset();
        app_log_info("Destroy Subscriber : "
            "service [%s]. id [0x%016llx], topic [%s].",
            serviceName_.c_str(), subPtr->id_, subPtr->name_.c_str());
    }
    return true;
}

INLINE bool Proxy::removeRpcClient(PortId rpcId) {
    /*
     * 不用发送取消，因为对端是被动响应的
     * !!!NOTE:
     * RpcClient 携带了调用者回调，可能有对 Proxy 的操作
     * 从而发生死锁，所以不能简单的加锁移除 RpcClient
     */
    decltype(rpcClientTab_) gc;
    if (rpcId == 0) {
        /*移除所有的*/
        WLock w(mutex_);
        rpcClientTab_.swap(gc);
    } else {
        gc.reserve(1);
        WLock w(mutex_);
        auto it = rpcClientTab_.find(rpcId);
        if (it == rpcClientTab_.end()) { return false; }
        gc.emplace(rpcId, std::move(it->second));
        rpcClientTab_.erase(it);
    }
    for (auto && kv : gc) {
        auto & rpcPtr = kv.second;
        app_log_info("Destroy RpcClient : "
            "service [%s]. id [0x%016llx], name [%s].",
            serviceName_.c_str(), rpcPtr->id_, rpcPtr->name_.c_str());
        rpcPtr->reset();
    }
    return true;
}

INLINE uint64_t Proxy::genMsgId(void) const {
    auto id = msgId_.fetch_add(1, std::memory_order_relaxed);
    if (unlikely(id == 0)) {
        id = msgId_.fetch_add(1, std::memory_order_relaxed);
    }
    return id;
}

INLINE bool Proxy::checkProtoMsg(const MsgHdr & msg) const {
    if (unlikely(msg.peerIpcId != id_)) {
        app_log_warn("Invalid message, miss match local id : [0x%02x].",
            msg.type);
        return false;
    }
    if (unlikely(msg.peerVersion != version_)) {
        app_log_warn("Expired message, miss match local version : [0x%02x].",
            msg.type);
        return false;
    }
    if (unlikely(msg.localIpcId != peerIpc_.ipcId())) {
        app_log_warn("Invalid message, miss match peer id : [0x%02x].",
            msg.type);
        return false;
    }
    if (unlikely(msg.localVersion != peerIpc_.ipcVersion())) {
        app_log_warn("Expired message, miss match peer version : [0x%02x].",
            msg.type);
        return false;
    }
    return true;
}

INLINE uint32_t Proxy::ProxyStatus::subscribe(Status::ChangedFunc && func) {
    if (onStatusChanged_) { return 0; }
    onStatusChanged_ = std::move(func);
    sendRegReq(0); // 发起请求
    return 1;
}

INLINE void Proxy::ProxyStatus::unsubscribe(uint32_t version) {
    if (!onStatusChanged_) { return; }
    status_ = StatusValue::INVALID_VALUE;
    Runtime::get()->loop()->removeTimer(timerId_);
    onStatusChanged_(status_);
    onStatusChanged_ = nullptr;
}

INLINE void Proxy::ProxyStatus::sendRegReq(int32_t) {
    /*发起注册*/
    ProtoMsg msg(MsgType::REQ_REG);
    msg.common.localIpcId = host_.id_;
    /*需要告诉对端本地IPC的信息，请求注册服务的名称*/
    msg.peerName                = host_.peerName_;
    msg.reqRegProxy.ipcSize     = host_.shmQueue_.size();
    msg.reqRegProxy.ipcName     = host_.ipcName_;
    msg.reqRegProxy.serviceName = host_.serviceName_;
#ifdef DEBUG
    app_log_debug("Try to register request : app [%s], service [%s].",
                host_.peerName_.c_str(), host_.serviceName_.c_str());
#endif
    Runtime::get()->sendProtoMsg(msg);

    tick_ = 0;
    /*
     * 启动定时器，持续发送，直到探测成功
     * this 不会失效，因为析构时会主动移除该定时器
     */
    timerId_ = Runtime::get()->loop()->addTimer(
        Runtime::get()->config().keepAliveInterval,
        std::bind(&ProxyStatus::sendRegReq, this, std::placeholders::_1)
    );
}

INLINE void Proxy::ProxyStatus::sendTick(int32_t) {
    app_log_debug("Check tick from service ...");
    auto & cfg = Runtime::get()->config();
    if (++tick_ > cfg.keepAliveCount) {
        status_ = StatusValue::APP_OFFLINE;
        /*移除服务在本地的IPC信息*/
        {
            WLock w(host_.mutex_);
            host_.peerIpc_.close();
        }
        app_log_info("Service was offline : %s", host_.serviceName_.c_str());
        onStatusChanged_(status_);
        /*喂狗超时，重新发送请求去探测*/
        sendRegReq(0);
        return;
    }
    timerId_ = Runtime::get()->loop()->addTimer(
        Runtime::get()->config().keepAliveInterval,
        std::bind(&ProxyStatus::sendTick, this, std::placeholders::_1)
    );
}

INLINE bool Proxy::ProxyStatus::procRegRsp(const ProtoMsg & msg) {
    if (msg.common.peerVersion != host_.version_) {
        app_log_warn("Expired message, miss match version");
        return false;
    }
    if (host_.peerIpc_.ipcVersion()) {
        app_log_warn("Peer ipc has been opened : app [%s], service [%s].",
            host_.peerName_.c_str(), host_.serviceName_.c_str());
        return true;
    }
    /*打开对端的IPC，用于 RPC 请求，可并发，需要一个读写锁*/
{
    /*必须保护 peerIpc_ 的有效性*/
    WLock w(host_.mutex_);
    assert(!host_.peerIpc_.ipcVersion());/*只能一个线程打开*/
    auto rc = host_.peerIpc_.open(msg.common.localIpcId,
                msg.rspRegProxy.ipcName, msg.common.localVersion);
    if (!rc) {
        return rc;
    }
    /*未上线不能发送其他请求，必须在这里*/
    status_ = StatusValue::APP_ONLINE;
    app_log_info("Service was online : %s", host_.serviceName_.c_str());
}
    /*请求异步到达，需要手动移除定时器*/
    Runtime::get()->loop()->removeTimer(timerId_);
    /*实现常驻订阅和RPC*/
{
    RLock r(host_.mutex_);
    /*收集旧的所有的订阅，然后发送请求？*/
    for (auto && kv : host_.subscriberTab_) {
        auto & subPtr = kv.second;
        host_.doReqSub(subPtr->id_, subPtr->name_);
    }
    /*收集旧的所有的RPC，然后发送请求？*/
    for (auto && kv : host_.rpcClientTab_) {
        auto & rpcPtr = kv.second;
        host_.doReqRpc(rpcPtr->id_, rpcPtr->name_);
    }
}
    /*动态订阅请求可能在这里回调中执行*/
    onStatusChanged_(status_);
    /*启动喂狗定时器*/
    timerId_ = Runtime::get()->loop()->addTimer(
        Runtime::get()->config().keepAliveInterval,
        std::bind(&ProxyStatus::sendTick, this, std::placeholders::_1)
    );
    return true;
}

INLINE bool Proxy::ProxyStatus::procRpcRsp(const ProtoMsg & msg) {
    if (!host_.checkProtoMsg(msg.common)) { return false; }
    auto rpcPtr = host_.rpcClient(msg.rspRegRpc.rpcId);
    if (!rpcPtr) {
        app_log_warn("Can't found rpcClient of proxy : "
            "app [%s], service [%s], id [0x%016llx].",
            host_.peerName_.c_str(), host_.serviceName_.c_str(),
            msg.rspRegRpc.rpcId
        );
        return false;
    }
    if (msg.rspRegRpc.code != RunningCode::SUCCESS) {
        app_log_warn("Can't rpcServer of service : "
            "app [%s], service [%s], name [%s].",
            host_.peerName_.c_str(), host_.serviceName_.c_str(),
            rpcPtr->name_.c_str()
        );
        /*失败则主动移除？*/
        host_.removeRpcClient(msg.rspRegRpc.rpcId);
        return false;
    }
    /*启动接收线程*/
    host_.startRecv();
    return true;
}

INLINE bool Proxy::ProxyStatus::procTick(const ProtoMsg & msg) {
    if (!host_.checkProtoMsg(msg.common)) { return false; }
    //app_log_debug("Response tick from service ...");
    ProtoMsg tick(MsgType::TICK_PXY);
    /*心跳进需要 两端的名称、IPC号和版本号，用于检查存活*/
    tick.common.peerIpcId   = host_.peerIpc_.ipcId();
    tick.common.peerVersion = host_.peerIpc_.ipcVersion();
    tick.common.localIpcId  = host_.id_;
    tick.peerName           = host_.peerName_;
    Runtime::get()->sendProtoMsg(tick);
    /*重置 tick*/
    tick_ = 0;
    return true;
}

INLINE bool Proxy::ProxyStatus::procSubRsp(const ProtoMsg & msg) {
    if (!host_.checkProtoMsg(msg.common)) { return false; }
    auto subPtr = host_.topicSubscriber(msg.rspSubTopic.topic);
    if (!subPtr) {
        app_log_warn("Can't found subscriber of proxy : "
            "app [%s], service [%s], id [0x%016llx].",
            host_.peerName_.c_str(), host_.serviceName_.c_str(),
            msg.rspSubTopic.topic
        );
        return false;
    }
    if (msg.rspSubTopic.code != RunningCode::SUCCESS) {
        app_log_warn("Can't subscribe topic of service : "
            "app [%s], service [%s], topic [%s].",
            host_.peerName_.c_str(), host_.serviceName_.c_str(),
            subPtr->name_.c_str()
        );
        /*失败则主动移除？*/
        host_.removeSubscriber(msg.rspSubTopic.topic);
        return false;
    }
    /*启动接收线程*/
    host_.startRecv();
    return true;
}

INLINE ShadowProxy::ShadowProxy(const ProtoMsg & msg, ServicePtr & service)
    : IpcEntry(msg.reqRegProxy.ipcSize)
    , status_(*this)
    , service_(service)
{
    assert(msg.common.type == MsgType::REQ_REG);
    peerName_ = msg.localName;
    /*需要下面的信息来打开IPC对象*/
    id_       = msg.common.localIpcId;
    version_  = msg.common.localVersion;
    ipcName_  = msg.reqRegProxy.ipcName;
}

INLINE bool ShadowProxy::checkProtoMsg(const MsgHdr & msg) const {
    if (msg.localIpcId != id_) {
        app_log_warn("Invalid message, miss match peer id [0x%02x]",
            msg.type);
        return false;
    }
    if (msg.localVersion != version_) {
        app_log_warn("Expired message, miss match peer version [0x%02x]",
            msg.type);
        return false;
    }
    auto srvPtr = service_.lock();
    if (!srvPtr) {
        app_log_error("Oops!!!Service has been destroy?");
        return false;
    }
    if (msg.peerIpcId != srvPtr->ipcId()) {
        app_log_warn("Invalid message, miss match local id [0x%02x]",
            msg.type);
        return false;
    }
    if (msg.peerVersion != srvPtr->ipcVersion()) {
        app_log_warn("Expired message, miss match local version [0x%02x]",
            msg.type);
        return false;
    }
    return true;
}

INLINE void ShadowProxy::ProxyStatus::subscribe(Status::ChangedFunc && func) {
    if (!onStatusChanged_) { onStatusChanged_ = std::move(func); }
    this->sendRsp(0);
}

INLINE void ShadowProxy::ProxyStatus::sendRsp(uint32_t) {
    auto srvPtr = host_.service_.lock();
    if (!srvPtr) {
        /*可能已经失效了*/
        onStatusChanged_((status_ = StatusValue::APP_OFFLINE));
        return;
    }
    onStatusChanged_((status_ = StatusValue::APP_ONLINE));
    /*响应注册请求*/
    ProtoMsg msg(MsgType::RSP_REG);
    msg.common.peerIpcId    = host_.id_;
    msg.common.peerVersion  = host_.version_;
    msg.common.localIpcId   = srvPtr->ipcId();
    msg.peerName            = host_.peerName_;
    msg.rspRegProxy.code    = RunningCode::SUCCESS;
    msg.rspRegProxy.ipcName = srvPtr->ipcName();
    msg.rspRegProxy.ipcSize = srvPtr->shmQueue().size();
    Runtime::get()->sendProtoMsg(msg);
    /*重置*/
    tick_ = 0;
    /*发送后，启动定时任务，发送 tick 喂狗，并检查本地看门狗*/
    timerId_ = Runtime::get()->loop()->addTimer(
        Runtime::get()->config().keepAliveInterval,
        std::bind(&ProxyStatus::sendTick, this, std::placeholders::_1));
}

INLINE void ShadowProxy::ProxyStatus::sendTick(int32_t) {
    auto srvPtr = host_.service_.lock();
    if (!srvPtr) {
        /*可能已经失效了*/
        onStatusChanged_((status_ = StatusValue::APP_OFFLINE));
        return;
    }
    auto & cfg = Runtime::get()->config();
    if (++tick_ > cfg.keepAliveCount) {
        app_log_warn("Proxy was offline : app [%s], service [%s]",
            host_.peerName_.c_str(), srvPtr->serviceName().c_str());
        onStatusChanged_((status_ = StatusValue::APP_OFFLINE));
        return;
    }
    app_log_debug("Send tick to proxy ...");
    ProtoMsg msg(MsgType::TICK_SRV);
    /*心跳仅需要 两端的名称、IPC的ID和版本号，用于检查存活*/
    msg.common.peerIpcId   = host_.id_;
    msg.common.peerVersion = host_.version_;
    msg.common.localIpcId  = srvPtr->ipcId();
    msg.peerName           = host_.peerName_;
    Runtime::get()->sendProtoMsg(msg);
    timerId_ = Runtime::get()->loop()->addTimer(
        cfg.keepAliveInterval,
        std::bind(&ProxyStatus::sendTick, this, std::placeholders::_1)
    );
}

INLINE bool ShadowProxy::ProxyStatus::procTick(const ProtoMsg & msg) {
    auto srvPtr = host_.service_.lock();
    if (!srvPtr) {
        /*可能已经失效了*/
        onStatusChanged_((status_ = StatusValue::APP_OFFLINE));
        return false;
    }
    /*重置计数*/
    tick_ = 0;
    return true;
}

INLINE bool ShadowProxy::ProxyStatus::procSubReq(const ProtoMsg & msg) {
    auto srvPtr = host_.service_.lock();
    if (!srvPtr) {
        /*Service 已经失效了，那么 Proxy 应该离线被销毁*/
        onStatusChanged_((status_ = StatusValue::APP_OFFLINE));
        return false;
    }
    auto doRspMsg = [&](RunningCode code) {
        ProtoMsg response(MsgType::RSP_SUB);
        response.common.localIpcId  = msg.common.peerIpcId;
        response.common.peerIpcId   = msg.common.localIpcId;
        response.common.peerVersion = msg.common.localVersion;
        response.peerName = msg.localName;
        response.rspSubTopic.code  = code;
        response.rspSubTopic.topic = msg.reqSubTopic.topic;
        Runtime::get()->sendProtoMsg(response);
    };
    if (!host_.checkProtoMsg(msg.common)) {
        if (msg.reqSubTopic.cancel == 0) {
            doRspMsg(RunningCode::SERVICE_NOEXIST);
        }
        return false;
    }
    auto pubPtr = srvPtr->topicPublisher(msg.reqSubTopic.topic);
    if (!pubPtr) {
        app_log_warn("Can't found publisher of service [%s] : 0x%016llx, %s",
            srvPtr->serviceName().c_str(), msg.reqSubTopic.topic,
            msg.reqSubTopic.topicName.c_str());
        if (msg.reqSubTopic.cancel == 0) {
            doRspMsg(RunningCode::TOPIC_NOEXIST);
        }
        return false;
    }
    app_log_info("Topic [%s] subscribe : "
        "app [%s], id [0x%llx], service [%s], topic [%s].",
        msg.reqSubTopic.cancel == 1?"cancel":"request",
        host_.peerName().c_str(), host_.ipcId(),
        srvPtr->serviceName().c_str(), msg.reqSubTopic.topicName.c_str());
    /*取消订阅，不需要响应*/
    if (msg.reqSubTopic.cancel == 1) {
        pubPtr->removeProxy(msg.common.localIpcId, msg.common.localVersion);
        return true;
    }
    /*响应，对端可以启动接收线程*/
    doRspMsg(RunningCode::SUCCESS);
    auto _this = host_.shared_from_this();
    /*通知*/
    if (pubPtr->insertProxy(_this)) {
        /*重复的不通知?*/
        pubPtr->onSubscriptionChanged(msg.common.localIpcId);
    }
    return true;
}

INLINE bool ShadowProxy::ProxyStatus::procRpcReq(const ProtoMsg & msg) {
    auto srvPtr = host_.service_.lock();
    if (!srvPtr) {
        /*Service 已经失效了，那么 Proxy 应该离线被销毁*/
        onStatusChanged_((status_ = StatusValue::APP_OFFLINE));
        return false;
    }
    auto doRspMsg = [&](RunningCode code) {
        ProtoMsg response(MsgType::RSP_RPC);
        response.common.localIpcId  = msg.common.peerIpcId;
        response.common.peerIpcId   = msg.common.localIpcId;
        response.common.peerVersion = msg.common.localVersion;
        response.peerName = msg.localName;
        response.rspRegRpc.code  = code;
        response.rspRegRpc.rpcId = msg.reqRegRpc.rpcId;
        Runtime::get()->sendProtoMsg(response);
    };
    if (!host_.checkProtoMsg(msg.common)) {
        doRspMsg(RunningCode::SERVICE_NOEXIST);
        return false;
    }
    auto rpcPtr = srvPtr->rpcServer(msg.reqRegRpc.rpcId);
    if (!rpcPtr) {
        app_log_warn("Can't found rpcServer of service [%s] : 0x%016llx, %s",
            srvPtr->serviceName().c_str(), msg.reqRegRpc.rpcId,
            msg.reqRegRpc.rpcName.c_str());
        doRspMsg(RunningCode::RPC_NOEXIST);
        return false;
    }
    app_log_info("Rpc request register : "
        "app [%s], id [0x%llx], service [%s], name [%s].",
        host_.peerName().c_str(), host_.ipcId(),
        srvPtr->serviceName().c_str(), msg.reqRegRpc.rpcName.c_str());
    /*响应，对端可以启动接收线程*/
    doRspMsg(RunningCode::SUCCESS);
    auto _this = host_.shared_from_this();
    if (rpcPtr->insertProxy(_this)) {
        srvPtr->startRecv();
    }
    return true;
}

INLINE bool ServicePort::insertProxy(ShadowProxyPtr pxy) {
    WLock w(mutex_);
    auto res = pxyTab_.emplace(pxy->ipcId(), pxy);
    if (!res.second) {
        auto old = res.first->second.lock();
        if (old && old->compare(*pxy)) {
            return false;
        }
        /*TODO:判断对端是否有效？*/
        res.first->second = pxy;
    }
    return true;
}

INLINE void ServicePort::removeProxy(IpcKey key, IpcVer ver) {
    WLock w(mutex_);
    auto it = pxyTab_.find(key);
    if (it != pxyTab_.end()) {
        auto old = it->second.lock();
        if (old && old->ipcVersion() != ver) {
            return;
        }
        pxyTab_.erase(it);
    }
}

INLINE void ServicePort::removeProxy(void) {
    WLock w(mutex_);
    for (auto it = pxyTab_.begin(); it != pxyTab_.end();) {
        if (it->second.lock()) { it++; } else { it = pxyTab_.erase(it); }
    }
}

template<class... Args>
bool Publisher::sendTopic(ShadowProxyPtr &&pxy, uint64_t id, Args &&...args) {
    assert(service_);
    if (unlikely(!pxy)) {
        app_log_warn("Proxy was offline");
        return false;
    }
    TopicHdr hdr(id_);
    hdr.localIpcId   = service_->ipcId();
    hdr.localVersion = service_->ipcVersion();
    hdr.peerIpcId    = pxy->ipcId();
    hdr.peerVersion  = pxy->ipcVersion();
    hdr.msgId        = id;

    /*TODO:重试次数可配置*/
    for (uint32_t i = 0; i < MaxTryAgain; i++) {
        auto rc = pxy->shmQueue().pack(hdr, std::forward<Args>(args)...);
        /*TODO:空间不足，异步判断是否是失效的 proxy?*/
        if (likely(rc)) {
#ifdef DEBUG
            app_log_debug("[%s] Send one topic to [%d] : "
                "id [0x%016llx], topic [%s], msgId [0x%016llx].",
                service_->serviceName().c_str(), hdr.peerVersion, hdr.peerIpcId,
                name_.c_str(), hdr.msgId
            );
#endif
            return true;
        }
        std::this_thread::yield();
    }

    constexpr size_t pkgSize = pack_size<std::decay_t<Args>...>::value;

    /*TODO:状态检查存活检查*/
    app_log_error("Oops!!!Proxy's queue was full [%s] : "
        "app [%s], service [%s], peer [%s], topic [%s], "
        "msgId [0x%016llx], size [%zu]",
        toXBytes(pxy->shmQueue().size()), Runtime::get()->appName().c_str(),
        service_->serviceName().c_str(), pxy->peerName().c_str(),
        name_.c_str(), hdr.msgId, pkgSize
    );

    return false;
}

template<class... Args>
bool Publisher::publishTo(IpcKey key, Args &&...args) {
    bool rc = false;
{
    RLock r(mutex_);
    auto it = pxyTab_.find(key);
    if (unlikely(!service_) || unlikely(it == pxyTab_.end())) {
        return false;
    }
#ifdef DEBUG
    auto msgId = service_->genMsgId();
#else
    uint64_t msgId = 0;
#endif
    rc = sendTopic(it->second.lock(), msgId, std::forward<Args>(args)...);
}
    if (unlikely(!rc)) { removeProxy(); }
    return rc;
}

template<class... Args>
bool Publisher::publish(Args &&...args) {
    uint32_t nr = 0;
{
    RLock r(mutex_);
    if (unlikely(!service_) || pxyTab_.size() == 0) { return false; }
#ifdef DEBUG
    auto msgId = service_->genMsgId();
#else
    uint64_t msgId = 0;
#endif
    for (auto && kv : pxyTab_) {
        auto pxy = kv.second.lock();
        auto rc = sendTopic(std::move(pxy), msgId, std::forward<Args>(args)...);
        nr += rc;
    }
}
    if (unlikely(nr == 0)) { removeProxy(); }
    return !!nr;
}

template<class... Args>
bool RpcServer::sendRsp(ShadowProxyPtr &&pxy, uint64_t msgId, Args &&...args) {
    if (unlikely(!pxy)) {
        app_log_warn("Proxy was offline");
        return false;
    }
    RpcHdr hdr(id_);
    hdr.localIpcId   = service_->ipcId();
    hdr.localVersion = service_->ipcVersion();
    hdr.peerIpcId    = pxy->ipcId();
    hdr.peerVersion  = pxy->ipcVersion();
    hdr.msgId        = msgId;

    for (uint32_t i = 0; i < MaxTryAgain; i++) {
        auto rc = pxy->shmQueue().pack(hdr, std::forward<Args>(args)...);
        /*空间不足，异步判断是否是失效的 proxy?*/
        if (likely(rc)) {
#ifdef DEBUG
            app_log_debug("[%s] Send one rpc response to [%d] : "
                "id [0x%016llx], rpc [%s], msgId [0x%016llx].",
                service_->serviceName().c_str(), hdr.peerVersion, hdr.peerIpcId,
                name_.c_str(), hdr.msgId
            );
#endif
            return true;
        }
        std::this_thread::yield();
    }

    constexpr size_t pkgSize = pack_size<std::decay_t<Args>...>::value;

    app_log_error("Oops!!!Proxy's queue was full [%s] : "
        "app [%s], service [%s], peer [%s], rpc [%s], "
        "msgId [0x%016llx], size [%zu]",
        toXBytes(pxy->shmQueue().size()),
        Runtime::get()->appName().c_str(), service_->serviceName().c_str(),
        pxy->peerName().c_str(), name_.c_str(), hdr.msgId, pkgSize
    );

    return false;
}

template<class... Args>
bool RpcServer::response(ReqId id, Args &&...args) {
    /*0值的消息ID，不需要响应*/
    auto & msgId = id.second;
    if (msgId == 0) { return false; }
{
    RLock r(mutex_);
    if (unlikely(!service_)) { return false; }
    auto it = pxyTab_.find(id.first);
    if (unlikely(it == pxyTab_.end())) { return false; }
    auto rc = sendRsp(it->second.lock(), msgId, std::forward<Args>(args)...);
    if (likely(rc)) { return true; }
}
    removeProxy();
    return false;
}

INLINE void RpcClient::reset(void) {
    if (proxy_) {
        GLock g(mutex_);
        rspCbTab_.clear();
        timerTab_.clear();
        proxy_ = nullptr;
    }
    /*移除回调超时定时器*/
    auto & loop = Runtime::get()->loop();
    loop->removeTimer(timerId_);
    /*非事件循环上下文，同步一次时间循环，否则 onTimedout() 可能访问野指针*/
    if (loop->isTaskCtx()) { return; }
    for (int32_t i = 0; i < 8; i++) {
        if (loop->sync(false, GenTimeouts)) { return; }
        /*主线程的事件循环出现了卡顿*/
        app_log_warn("The main event loop is experiencing blocking.");
    }
    app_log_error("Oops!!!The event loop has a critical design flaw.");
}

INLINE RpcClient::ReqInfo RpcClient::removeRequest(uint64_t msgId) {
    ReqInfo info{0, nullptr};
    if (unlikely(!proxy_)) { return info; }
    auto it = rspCbTab_.find(msgId);
    if (unlikely(it == rspCbTab_.end())) {
        app_log_warn("Can't found callback for rpc response : "
            "app [%s], service [%s], rpc [%s], msgId [0x%016llx].",
            proxy_->peerName().c_str(), proxy_->serviceName().c_str(),
            name_.c_str(), msgId
        );
        return info;
    }
    info = std::move(it->second);
    rspCbTab_.erase(it);
    return info;
}

INLINE void RpcClient::onRpcEvent(uint64_t msgId, PackBuffer & data) {
    if (unlikely(msgId == 0)) { return; }
    ReqInfo info{0, nullptr};
{
    GLock g(mutex_);
    info = removeRequest(msgId);
    /*移除定时器*/
    if (!removeTimer(info.first)) {
        /*已经报告超时，则不回调任务？*/
        app_log_warn("Receive one expired rpc response : "
            "app [%s], service [%s], rpc [%s], msgId [0x%016llx].",
            proxy_->peerName().c_str(), proxy_->serviceName().c_str(),
            name_.c_str(), msgId
        );
        return;
    }
}
    if (likely(info.second)) {
        info.second(true, data);
    }
}

template <class... Args>
bool RpcClient::sendReq(IpcEntry & srv, uint64_t msgId, Args &&... args) {
    assert(proxy_);
    RpcHdr hdr(id_);
    hdr.localIpcId   = proxy_->ipcId();
    hdr.localVersion = proxy_->ipcVersion();
    hdr.peerIpcId    = srv.ipcId();
    hdr.peerVersion  = srv.ipcVersion();
    hdr.msgId        = msgId;

    PackBuffer buff;
    auto size = buff.pack(hdr, std::forward<Args>(args)...);
    assert(size == buff.size());

    for (int32_t i = 0; i < 256; i++) {
        auto rc = srv.shmQueue().push(buff.data(), size);
    /*空间不足，异步判断是否是失效的 proxy?*/
        if (likely(rc)) {
#ifdef DEBUG
            app_log_debug("[%s] Send one rpc request to [%d] : "
                "id [0x%016llx], rpc [%s], msgId [0x%016llx].",
                proxy_->serviceName().c_str(), hdr.peerVersion, hdr.peerIpcId,
                name_.c_str(), hdr.msgId
            );
#endif
            return true;
        }
        std::this_thread::yield();
    }

    constexpr size_t pkgSize = pack_size<std::decay_t<Args>...>::value;

    app_log_error("Oops!!!Service's queue was full [%s] : "
        "app [%s], service [%s], peer [%s], rpc [%s], "
        "msgId [0x%016llx], size [%zu]",
        toXBytes(srv.shmQueue().size()), Runtime::get()->appName().c_str(),
        proxy_->serviceName().c_str(), proxy_->peerName().c_str(),
        name_.c_str(), hdr.msgId, pkgSize
    );

    return false;
}

INLINE uint64_t RpcClient::insertTimer(RpcClient::TimerTask && task) {
    auto expire = qevent::sys_clock();
    expire += static_cast<uint64_t>(timeouts_) * 1000 * 1000;
    do {/*以超时绝对值作为ID，不能重复*/
        auto res = timerTab_.emplace(expire, std::move(task));
        if (res.second) { break; }
        expire += 1;
        if (unlikely(!expire)) { expire += 1000*1000; }
    } while (true);
    return expire;
}

INLINE bool RpcClient::removeTimer(uint64_t id) {
    if (unlikely(!id)) return false;
    return !!timerTab_.erase(id);
}

INLINE void RpcClient::onTimedout(int32_t) {
    uint64_t next = timeouts_/2;
    auto now = qevent::sys_clock();
    std::unique_lock<std::mutex> mutex(mutex_);
    /*循环查看所有超时的回调*/
    for (int32_t i = 0; i < 32 && likely(proxy_) && timerTab_.size(); i++) {
        auto it = timerTab_.begin();
        auto expire = it->first;
        expire -= (expire % 1000000); /*向下对齐，未来1ms内的回调，在本次触发*/
        if (qevent::utils::time_before64(now, expire)) {
            next = it->first - now;
            if (next < 1000000) { next = 1000000; }
            break;
        }
        auto task = std::move(it->second);
        timerTab_.erase(it);
        mutex.unlock();
        try { task(); } catch(...) {}
        if (unlikely(!((i+1)&7))) { now = qevent::sys_clock(); }
        mutex.lock();
    }
    /*再次添加*/
    if (likely(proxy_)) {
        /*精准定时？*/
        timerId_ = Runtime::get()->loop()->doAddTimer(now + next,
            std::bind(&RpcClient::onTimedout, this, std::placeholders::_1));
    }
}

template <class RpcData>
void RpcClient::doTimedout(uint64_t msgId) {
    ReqInfo info{0, nullptr};
{
    GLock g(mutex_);
    info = removeRequest(msgId);
    if (unlikely(!info.second)) { return; }
    app_log_warn("Oops!!!Rpc's request was timeout : "
        "app [%s], service [%s], peer [%s], rpc [%s], msgId [0x%016llx]",
        Runtime::get()->appName().c_str(), proxy_->serviceName().c_str(),
        proxy_->peerName().c_str(), name_.c_str(), msgId
    );
}
    PackBuffer buff;
    buff.pack(RpcData{});
    info.second(false, buff);
}

template <class Callback, class... Args>
bool RpcClient::request(IpcEntry & srv, Callback && func, Args &&... args) {
    using traits  = function_traits<std::decay_t<Callback>>;
    static_assert(traits::arity == 2, "func must take exactly 2 argument");
    using RpsCode = std::decay_t<typename traits::template arg_type<0>>;
    using RpcData = std::decay_t<typename traits::template arg_type<1>>;
    static_assert(std::is_same<RpsCode, bool>::value,
        "first argument must be bool");
    static_assert(std::is_trivially_copyable<RpcData>::value,
        "RpcData must be trivially copyable for raw buffer access");
    /*需要回复，保存回调*/
    uint64_t msgId = 0;
    assert(timeouts_);
{
    GLock g(mutex_);
    if (unlikely(!proxy_)) {
        throw std::invalid_argument("RpcClient has been removed");
    }
    /*异步请求条目数量限制*/
    if (unlikely(rspCbTab_.size() >= maxReqCount_)) {
        app_log_warn("Oops!!! Too many asynchronous callback requests; "
            "maximum of %u allowed.", maxReqCount_);
        return false;
    }
    /*确保ID唯一*/
    do { msgId = proxy_->genMsgId(); } while (unlikely(rspCbTab_.count(msgId)));
    /*回调包装*/
    auto doProc = [proc = std::forward<Callback>(func)]
        (bool code, PackBuffer & buff) mutable
    {
        /*解码数据*/
        if (unlikely(buff.size() != sizeof(RpcData))) {
            app_log_warn("Oops!!! size of RpcData miss match");
            return;
        }
        auto data = buff.data<RpcData>();
        proc(code, *data);
    };
    /*必须携带定时器*/
    auto timer = insertTimer([=](void) mutable {
        doTimedout<RpcData>(msgId);
    });
    /*插入回调表*/
    rspCbTab_.emplace(msgId, std::make_pair(timer, std::move(doProc)));
}
    /*解锁发送*/
    auto rc = sendReq(srv, msgId, std::forward<Args>(args)...);
    if (unlikely(!rc)) {
        /*失败移除*/
        GLock g(mutex_);
        auto info = removeRequest(msgId);
        removeTimer(info.first);
    }

    return rc;
}

}
