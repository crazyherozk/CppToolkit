//
//  Xprt.hpp
//
//
//  Created by zhoukai on 2025/01/28.
//
#ifndef qevent_xprt_hpp
#define qevent_xprt_hpp

#include <cstring>
#include <cassert>

#include <netdb.h>
#include <net/if.h>
#include <arpa/inet.h>

#include <stddef.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <netinet/tcp.h>
#include <netinet/ip.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>

#include "../log/log.hpp"
#include "../reactor/reactor.hpp"

/*
    auto loop   = std::make_shared<reactor>();
    auto mgr    = std::make_shared<XprtMgr>(loop);
    auto server = std::make_shared<TcpListenerXprt>(mgr);
    auto rc = server->listen({"127.0.0.1", "4096"},
        XPRT_OPT_NONBLOCK|XPRT_OPT_REUSEDADDR|XPRT_RDREADY,
            [](TcpListenerXprtPtr creator) {
        //操作器
        struct TcpReader : public XprtDelegate {
            void onRecv(XprtPtr & x, int32_t e) noexcept override {
                auto rc = ::read(x->fd(), buff_, sizeof(buff_));
                if (rc > 0) {
                    ::write(x->fd(), buff_, rc);
                } else if (!rc || errno != EAGAIN) {
                    Xprt::shutdown(x, XPRT_SHUT_RDWR);
                }
            }
            char buff_[1024];
        };
        auto clnt = std::make_shared<TcpXprt>(creator->mgr());
        clnt->delegate(std::make_shared<TcpReader>());
        return clnt;
    });
    do { loop->run(); } while(1);
 */

namespace network {

struct Xprt;
struct TcpXprt;
struct TcpListenerXprt;
struct XprtMgr;
struct XprtDelegate;

using ReactorPtr         = std::shared_ptr<qevent::reactor>;
using XprtPtr            = std::shared_ptr<Xprt>;
using TcpXprtPtr         = std::shared_ptr<TcpXprt>;
using TcpListenerXprtPtr = std::shared_ptr<TcpListenerXprt>;
using XprtMgrPtr         = std::shared_ptr<XprtMgr>;
using XprtDelegatePtr    = std::shared_ptr<XprtDelegate>;

/*尽量不使用宏*/
constexpr int32_t INET_NONE = AF_UNSPEC;
constexpr int32_t INET_IPV4 = AF_INET;
constexpr int32_t INET_IPV6 = AF_INET6;
constexpr int32_t UNIX_SOCK = AF_UNIX;
constexpr int32_t INET_TCP  = SOCK_STREAM;
constexpr int32_t INET_UDP  = SOCK_DGRAM;

enum {
    XPRT_TCPLSNR = 1, /*TCP侦听套机字*/
    XPRT_TCPCLNT = 2, /*TCP主动连接套接字*/
    XPRT_TCPTEMP = 3, /*TCP被动连接套接字*/
    XPRT_TYPE_MASK = 0x0000000fU, /**< 可以支持7种基础类型的传输对象*/

    /*option for creating Xprt*/
    XPRT_OPT_NONBLOCK     = 0x00000010U, /**< 非阻塞IO（包括connect()与accept()）*/
    XPRT_OPT_REUSEDADDR   = 0x00000020U, /**< 重用绑定地址*/

    /*仅 TCP 有效*/
    XPRT_OPT_NAGLEOFF     = 0x00000040U, /**< 关闭小包延迟算法*/
    XPRT_OPT_KEEPALIVE    = 0x00000080U, /**< 通过特殊分节自动保持活动链接*/

    /*UNIX 域套接字*/
    XPRT_OPT_UNIX         = 0x00001000U,
    XPRT_OPT_MASK         = 0x0000fff0U,

    /* status bit of Xprt
     * 首次接收数据和完全关闭作为独立的事件，用 on_changed() 回调响应
     */

    XPRT_ATTACHED_BIT = 16, /**< 已经附加在管理容器中*/
    XPRT_CONNECTING_BIT,  /**< 正在连接，还被复用为UDP是否是连接模式*/
    XPRT_OPENED_BIT, /**< 第一次触发打开事件，变为可读/可写*/
    XPRT_CLOSED_BIT, /**< 第一次触发关闭事件，完全关闭，此后一定不会再次触发任何事件回调，
                       *  在强制关闭时，可以无 opened 状态下触发 closed */
    XPRT_SHUTWR_BIT, /**< 已经执行写关闭, 或发送了fin分节*/
    XPRT_SHUTRD_BIT, /**< 已经执行读关闭, 或接收了fin分节*/
    XPRT_CONNREFUSED_BIT, /**< 连接拒绝，作为最后退出状态*/
    XPRT_RDREADY_BIT,
    XPRT_WRREADY_BIT,
    XPRT_UNUSED_BIT,

    XPRT_STATS_MASK = (~(XPRT_TYPE_MASK | XPRT_OPT_MASK)),

    XPRT_ATTACHED = 1U << XPRT_ATTACHED_BIT,
    XPRT_CONNECTING = 1U << XPRT_CONNECTING_BIT,
    XPRT_OPENED = 1U << XPRT_OPENED_BIT,
    XPRT_CLOSED = 1U << XPRT_CLOSED_BIT,
    XPRT_SHUTWR = 1U << XPRT_SHUTWR_BIT,
    XPRT_SHUTRD = 1U << XPRT_SHUTRD_BIT,
    XPRT_CONNREFUSED = 1U << XPRT_CONNREFUSED_BIT,

    /*用于 Xprt 的起始行为*/
    XPRT_RDREADY = 1U << XPRT_RDREADY_BIT,
    XPRT_WRREADY = 1U << XPRT_WRREADY_BIT,

    XPRT_SHUT_RDWR = XPRT_SHUTWR | XPRT_SHUTRD,
};

/*必须保证 onXXX() 执行不会抛出异常*/
struct XprtDelegate {
    friend struct Xprt;
    XprtDelegate(void) {}
    virtual ~XprtDelegate(void) {}
protected:
    /*@s 是 XPRT_XXX 的状态集合*/
    virtual void onChanged(XprtPtr &, uint32_t s){}
    /*@e 是 EV_XXX 的事件集合*/
    virtual void onRecv(XprtPtr &, int32_t e)    {}
    /*@e 是 EV_XXX 的事件集合*/
    virtual void onSend(XprtPtr &, int32_t e)    {}
};

/*网络地址*/
struct InAddr {
    struct sockaddr_storage addr;
    socklen_t               len { 0 };
    InAddr() = default;
    InAddr(const addrinfo * info) { this->operator=(info); }
    InAddr & operator=(const InAddr & other) {
        if (&other != this) {
            len = other.len; std::memcpy(&addr, &other.addr, len);
        }
        return *this;
    }
    InAddr & operator=(const addrinfo * info) {
        if (info) {
            len = info->ai_addrlen;
            std::memcpy(&addr, info->ai_addr, len);
        } else {
            len = 0;
        }
        return *this;
    }
    bool operator==(const InAddr &other) const {
        if (&other == this) return true;
        if (other.len != len) return false;
        return !std::memcmp(&other.addr, &addr, len);
    }
    bool operator!=(const InAddr &other) const { return !(other == *this); }
};

using HostAddr = std::pair<std::string, std::string>;

struct exception : public std::system_error {
    explicit exception (int32_t rc) :
        std::system_error(rc>0?rc:-rc, std::generic_category(),
            "libxprt has encountered an exception")
    { }
};

struct fd_guard {
    fd_guard(int32_t fd = -1) : fd_(fd) {}
    fd_guard(const fd_guard &other) { fd_ = ::dup(other.fd_); }
    ~fd_guard(void) { if (fd_ > -1) ::close(fd_); }
    int32_t eat(void) { int32_t fd = fd_; fd_ = -1; return fd; }
    int32_t operator * () { return fd_; }
    fd_guard & operator=(fd_guard && other) {
        if (this != &other) { fd_ = other.eat(); } return *this;
    }
    fd_guard & operator=(fd_guard & other) {
        return this->operator=(std::forward<fd_guard>(other));
    }
    fd_guard & operator=(int32_t fd) {
        if (fd_ > -1) {::close(fd_);} fd_ = fd; return *this;
    }
    int32_t fd_ = -1;
};

////////////////////////////////////////////////////////////////////////////////
// 只要套接字状态没有关闭，则一直存在（被xprtMgr管理），除非手动关闭
struct XprtMgr
    : public qevent::restartTimer<ReactorPtr>
    , public std::enable_shared_from_this<XprtMgr>
{
    friend struct Xprt;
    friend struct TcpXprt;
    friend struct TcpListenerXprt;

    explicit XprtMgr(
        ReactorPtr lp = std::make_shared<qevent::reactor>(),
        int32_t interval = 100
    ) : restartTimer<ReactorPtr>(lp) {
        launch(interval, std::bind(&XprtMgr::task, this, std::placeholders::_1));
    }
    virtual ~XprtMgr(void);
    ReactorPtr & loop(void) { return restartTimer<ReactorPtr>::loop_; }
protected:
    int32_t attachXprt(XprtPtr &&xpt);
    int32_t detachXprt(XprtPtr &&xpt, bool async = true);
    int32_t attachXprt(XprtPtr &xpt) {
        return attachXprt(std::forward<XprtPtr>(xpt));
    }
    int32_t detachXprt(XprtPtr &xpt, bool async = true) {
        return detachXprt(std::forward<XprtPtr>(xpt), async);
    }
protected:
    std::mutex lock_;
    std::list<XprtPtr> flushingXprts_;
    std::unordered_map<int32_t, XprtPtr> xprts_;
protected:
    inline void task(int32_t);
};

////////////////////////////////////////////////////////////////////////////////
struct Xprt : public std::enable_shared_from_this<Xprt> {
    friend struct XprtMgr;
    explicit Xprt(XprtMgrPtr mgr) : mgr_(mgr) { assert(mgr); }
    virtual ~Xprt(void) { showAddrInfo("closed"); close(); delegate_.reset(); }
    int32_t fd(void) const { return fd_; }
    XprtMgrPtr mgr(void) { return mgr_.lock(); }
    XprtDelegatePtr delegate(void) { return delegate_; }
    /*设置代理，在循环外调用时，必须保证事件已停止*/
    void delegate(XprtDelegatePtr && xdgt);
    void showAddrInfo(const std::string & what = "established");
    /*!!!如果同步，则不能在同一个事件循环中调用*/
    static int32_t destroy(XprtPtr, bool sync = true);
    static int32_t shutdown(XprtPtr xpt, uint32_t how) {
        std::lock_guard<std::mutex> guard(xpt->lock_);
        return xpt->doShutdown(how);
    }
    void sync(void) { mgr()->loop()->sync(); }
    void addEvent(int32_t ev) {
        mgr()->loop()->addEvent(fd_, ev,
            std::bind(&Xprt::task, this->shared_from_this(),
                std::placeholders::_1));
    }
    void removeEvent(void) { 
        mgr()->loop()->removeEvent(fd_);
    }
    int32_t enableEvent(int32_t ev) {
        return mgr()->loop()->enableEvent(fd_, ev);
    }
    int32_t disableEvent(int32_t ev) {
        return mgr()->loop()->disableEvent(fd_, ev);
    }
protected:
    /**
     * 特定的 半关闭实现
     */
    virtual int32_t doShutdown(uint32_t how) { shutdownStat(how); return 0; }

protected:
    void close(void);
    int32_t getAddrInfo(void);
    bool shutdownStat(uint32_t, bool sync = true);
    static uint32_t flags(Xprt * xpt, uint32_t v) {
        auto old = xpt->flags_; xpt->flags_ = v;
        return old;
    }
    static inline uint32_t type(uint32_t s) { return s & XPRT_TYPE_MASK; }
    static inline bool hasClosed(uint32_t s) {
        return (s & XPRT_CLOSED)||((s & XPRT_SHUT_RDWR) == XPRT_SHUT_RDWR);
    }
protected:
    int32_t fd_{-1};
    InAddr  localAddr_;
    InAddr  remoteAddr_;
    uint32_t flags_{0};
    std::mutex lock_;
    XprtDelegatePtr delegate_;
    std::weak_ptr<XprtMgr> mgr_;
private:
    void    task(int32_t);
    int32_t eatConnecting(int32_t);
    void    eatOpened(int32_t);
    void    eatRecv(int32_t);
    void    eatSend(int32_t);
    void    eatClosedLocked(int32_t);
    void    eatClosed(int32_t ev) {
        if (!(ev & POLLERR) && ((flags_ & XPRT_SHUT_RDWR) != XPRT_SHUT_RDWR))
            return;
        std::lock_guard<std::mutex> guard(lock_);
        eatClosedLocked(ev);
    }
};

////////////////////////////////////////////////////////////////////////////////

struct TcpListenerXprt :public Xprt {
    using mkClntFunc = std::function<XprtPtr(TcpListenerXprtPtr)>;
    /*只需要读事件*/
    struct TcpAcceptor : public XprtDelegate {
    protected:
        inline void onRecv(XprtPtr &, int32_t) override;
    };
    friend struct TcpListenerXprt::TcpAcceptor;
    explicit TcpListenerXprt(XprtMgrPtr mgr) : Xprt(mgr) {
        Xprt::delegate(std::make_shared<TcpListenerXprt::TcpAcceptor>());
    }
    virtual ~TcpListenerXprt(void) {
        app_log_info("destroy TcpListenerXprt %p : %d", this, fd_);
    }
    /*
     * 开始侦听，并指定一些选项，来创建套接字
     * @makeXprt 当新的连接到达时，用于创建被动的套接字
     *     返回的指针，必须由实现中自己管理这些对象
     * @option 会被被动连接的套接字继承
     *     XPRT_RDREADY/XPRT_WRREADY 组合指示 被动新 Xprt 的起始事件
     *     如果没有指定，则默认开启读事件
     *
     * 一旦侦听就开启了读事件，自动处理被动连接
     */
    int32_t listen(const HostAddr &, uint32_t, mkClntFunc &&);
protected:
    /*反馈接受成功与否*/
    virtual int32_t doAccept(XprtPtr &) noexcept;

private:
    int32_t doShutdown(uint32_t) override;
    mkClntFunc mkClnt_ {nullptr};
};

////////////////////////////////////////////////////////////////////////////////

struct TcpXprt: public Xprt {
    friend TcpListenerXprt;
    explicit TcpXprt(XprtMgrPtr mgr) : Xprt(mgr) {}
    virtual ~TcpXprt(void);
    /*
     * 开始侦听，并指定一些选项，来创建套接字
     * @option 套接字选项和起始事件
     *     XPRT_RDREADY/XPRT_WRREADY 组合指示起始事件
     *     如果没有指定，则默认开启写
     *     注意，如果是非阻塞连接，则会自动开启写，否则将不会完成连接
     *
     * TODO: 超时连接
     */
    int32_t connect(const HostAddr &, uint32_t option);
protected:
    int32_t doShutdown(uint32_t) override;
};

////////////////////////////////////////////////////////////////////////////////
// utils
////////////////////////////////////////////////////////////////////////////////
namespace utils {

constexpr std::size_t bits_per_int = sizeof(uint32_t) * 8;

constexpr uint32_t bit_mask(const int nr) {
    static_assert(XPRT_UNUSED_BIT <= bits_per_int, "XPRT_UNUSED_BIT must be <= 32");
    return 1UL << (nr % bits_per_int);
}

constexpr std::size_t bit_word(const int nr) {
    return static_cast<std::size_t>(nr / bits_per_int);
}

static inline void set_bit(const int nr, volatile uint32_t* addr) {
    uint32_t mask = bit_mask(nr);
    volatile uint32_t* p = addr + bit_word(nr);
    *p |= mask;
}

static inline bool test_and_set_bit(const int nr, volatile uint32_t* addr) {
    uint32_t mask = bit_mask(nr);
    volatile uint32_t* p = addr + bit_word(nr);
    uint32_t old = *p;
    *p = old | mask;
    return (old & mask) != 0;
}

static inline bool test_and_clear_bit(const int nr, volatile uint32_t* addr) {
    uint32_t mask = bit_mask(nr);
    volatile uint32_t* p = addr + bit_word(nr);
    uint32_t old = *p;
    *p = old & ~mask;
    return (old & mask) != 0;
}

static inline bool test_bit(const int nr, const volatile uint32_t* addr) {
    return (addr[bit_word(nr)] >> (nr % bits_per_int)) & 1UL;
}

static inline int32_t ntop(const InAddr &addr, HostAddr &p);
static inline int32_t pton(const HostAddr &p, InAddr &addr);

static inline addrinfo *getAddrInfo(const HostAddr&, int32_t, int32_t, int32_t);
static inline void freeAddrInfo(addrinfo *ai) {
    if (!ai) return;
    if (ai->ai_family == UNIX_SOCK) {::free(ai);} else {::freeaddrinfo(ai); }
}

static inline std::shared_ptr<addrinfo> getAddrInfoPtr(const HostAddr & host,
        int32_t flag, int32_t family, int32_t socktype)
{
    auto ai = getAddrInfo(host, flag, family, socktype);
    if (unlikely(!ai)) return nullptr;
    return std::shared_ptr<addrinfo>(ai, [](addrinfo*ptr){freeAddrInfo(ptr);});
}

using setupSockFunc = std::function<int32_t(int32_t, const InAddr &)>;
static inline int32_t listen(const HostAddr &, int32_t, setupSockFunc &&);
static inline int32_t accept(int32_t fd, InAddr &);
static inline int32_t connect(const HostAddr &, int32_t, int32_t, setupSockFunc &&);
static inline int32_t tcpConnect(const HostAddr &host, int32_t family, setupSockFunc &&setup) {
    return connect(host, family, SOCK_STREAM, std::forward<setupSockFunc>(setup));
}

static inline int enableFdFlag(int fd, int flags);
static inline int disableFdFlag(int fd, int flags);

static inline int nonblock(int32_t fd, bool on) {
    if (on) {return enableFdFlag(fd, O_NONBLOCK); }
    return disableFdFlag(fd, O_NONBLOCK);
}

template <typename VAL>
static inline int32_t setSockOpt(int32_t f, int32_t l, int32_t n, VAL & v);

static inline int32_t keepAlive(int32_t f, int32_t i, int32_t t);
static inline int32_t reusedAddr(int32_t fd) {
    int32_t val = 1;
    return setSockOpt(fd, SOL_SOCKET, SO_REUSEADDR, val);
}
}
////////////////////////////////////////////////////////////////////////////////
inline XprtMgr::~XprtMgr(void) 
{
    this->stop();
    app_log_info("destroy XprtMgr : %p", this);
}

inline int32_t XprtMgr::attachXprt(XprtPtr && xpt)
{
    std::lock_guard<std::mutex> guard(lock_);
    if (unlikely(xpt->fd() < 0)) return -EBADF;
    /*已经 附加到容器了*/
    bool rc = utils::test_and_set_bit(XPRT_ATTACHED_BIT, &xpt->flags_);
    if (unlikely(rc)) {
        app_log_error("Oops, something was wrong"); return -EBUSY;
    }
    auto it = xprts_.find(xpt->fd());
    if (unlikely(it != xprts_.end())) {
        app_log_error("Oops, something was wrong"); return -EINVAL;
    }
    /*增加引用*/
    xprts_[xpt->fd()] = xpt;
    return 0;
}

inline int32_t XprtMgr::detachXprt(XprtPtr && xpt, bool async)
{
    std::lock_guard<std::mutex> guard(lock_);
    if (unlikely(xpt->fd_ < 0)) return -EBADF;
    /*已经 从容器剥离了*/
    bool rc = utils::test_and_clear_bit(XPRT_ATTACHED_BIT, &xpt->flags_);
    if (unlikely(!rc)) { return -EINVAL; }
    auto it = xprts_.find(xpt->fd_);
    if (unlikely(it == xprts_.end())) {
        app_log_error("Oops, something was wrong"); return -EINVAL;
    }
    xprts_.erase(it);
    if (likely(async)) flushingXprts_.push_back(xpt);
    return 0;
}

inline void XprtMgr::task(int32_t)
{
    std::list<XprtPtr> queue;
    {
        std::lock_guard<std::mutex> guard(lock_);
        queue.splice(queue.end(), std::move(flushingXprts_));
    }
    if (queue.size()) app_log_debug("flush Xprt [%zu] ...", queue.size());
}

////////////////////////////////////////////////////////////////////////////////

inline void Xprt::close(void)
{
    /*只能内部调用，外部必须保证互斥*/
    if (fd_ < 0) { return; }
    auto mgr = mgr_.lock();
    if (mgr) {
        /*构造的一个假的，然后同步删除？*/
        auto _this = std::shared_ptr<Xprt>(this, [](Xprt*){});
        mgr->detachXprt(_this, false);
        mgr->loop()->removeEvent(fd_);
    }
    ::close(fd_);
    fd_ = -1; 
}

inline void Xprt::delegate(XprtDelegatePtr && xdgt)
{
    std::unique_lock<std::mutex> guard(lock_);
    if (unlikely(fd_ > -1)) { throw exception(-EBUSY); }
    delegate_ = std::move(xdgt);
}

inline int32_t Xprt::destroy(XprtPtr xpt, bool sync)
{
    if (unlikely(!xpt)) return -EINVAL;
    /*同步停止*/
    std::unique_lock<std::mutex> mutex(xpt->lock_);
    /*从事件循环中移除*/
    xpt->removeEvent();
    /*等待读写完成*/
    if (sync) { xpt->sync(); }
    auto rc = xpt->mgr()->detachXprt(xpt, false);
    if (rc) return -EINVAL;
    /*可能已经被关闭了*/
    rc = utils::test_and_set_bit(XPRT_CLOSED_BIT, &xpt->flags_);
    if (rc) return 0;
    auto v = flags(xpt.get(), xpt->flags_ & (~XPRT_SHUT_RDWR));
    rc = utils::test_and_clear_bit(XPRT_CONNECTING_BIT, &xpt->flags_);
    if (rc) {
        /*查看是否还处于连接状态，如果是，则不调用关闭事件回调*/
    } else if (likely(v & XPRT_OPENED)) {
        /*没有触发打开，则不会触发关闭*/
        if (likely(xpt->delegate_)) {
            mutex.unlock();
            xpt->delegate_->onChanged(xpt, XPRT_CLOSED);
            mutex.lock();
        }
    }
    return 0;
}

inline void Xprt::task(int32_t ev)
{
    uint32_t flags = *(volatile uint32_t*)&flags_;

#ifdef XPRT_VERBOSE
    app_log_debug("trigger event [0x%x] on Xprt : [%d{0x%lx}], %s",
                  ev, fd_, flags, ev & POLLERR ? "Error" : "Success");
#endif

    try {
        /* eatConnecting() 如果没有成功都发起内部关闭 */
        if (unlikely(flags & XPRT_CONNECTING)) {
            auto rc = eatConnecting(ev);
            if (unlikely(rc < 0)) return;
        }
        if (likely(!(ev & POLLERR))) {
            if (unlikely(!(flags & XPRT_OPENED)))
                eatOpened(ev);
            eatRecv(ev);
            eatSend(ev);
        }
        eatClosed(ev);
    } catch (const std::exception & e) {
        app_log_error("process event failed : %s", e.what());
        eatClosed(qevent::EV_ERROR);
    }
    return;
}

inline int32_t Xprt::eatConnecting(int32_t ev)
{
    std::lock_guard<std::mutex> guard(lock_);
    if (unlikely(hasClosed(flags_))) {
        app_log_warn("nonblock connection has been shutdown : %d/0x%08x",
            fd_, flags_);
        eatClosedLocked(POLLERR);
        return -ECONNABORTED;
    }
    int32_t rc = utils::test_and_clear_bit(XPRT_CONNECTING_BIT, &flags_);
    if (unlikely(!rc)) return 0;
#if 0
    if (unlikely(ev & POLLERR)) {
        app_log_warn("nonblock connection encountered an error : %d/%lx",
             fd_, flags_);
        utils::set_bit(XPRT_CONNREFUSED_BIT, &flags_);
        eatClosed(POLLERR);
        return -ECONNREFUSED;
    }
#endif
    rc = ::connect(fd_, (sockaddr*)&remoteAddr_.addr, remoteAddr_.len);
    if (likely(rc)) {
        rc = errno;
        if (unlikely(rc == EALREADY)) {
            utils::set_bit(XPRT_CONNECTING_BIT, &flags_);
            return -EAGAIN;
        }
        if (unlikely(rc != EISCONN)) {
            app_log_warn("nonblocking connection failed : %d/%s", fd_,
                    strerror(rc));
            utils::set_bit(XPRT_CONNREFUSED_BIT, &flags_);
            eatClosedLocked(POLLERR);
            return -rc;
        }
#ifdef __apple__
    } else {
        app_log_error("Oops, something was wrong");
#endif
    }
    rc = getAddrInfo();
    if (likely(!rc)) { return 1; }
    app_log_warn("getaddrinfo of Xprt failed : %d/%s", fd_, strerror(-rc));
    utils::set_bit(XPRT_CONNREFUSED_BIT, &flags_);
    eatClosedLocked(qevent::EV_ERROR);
    return rc;
}

inline void Xprt::eatOpened(int32_t ev)
{
    std::unique_lock<std::mutex> mutex(lock_);
    /*第一次变为可读/可写 ： 没有设置 opened ，且还没有被关闭*/
    if (unlikely(hasClosed(flags_))) return;
    int32_t rc = utils::test_and_set_bit(XPRT_OPENED_BIT, &flags_);
    if (unlikely(rc)) return;
    if (delegate_) {
        auto xpt = this->shared_from_this();
        mutex.unlock();
        delegate_->onChanged(xpt, XPRT_OPENED);
        mutex.lock();
    }
}

inline void Xprt::eatRecv(int32_t ev)
{
    auto flags = *(volatile uint32_t*)&flags_;
    /*除了写事件，其他任何事件都需要通知读回调，因为缓存区内可能还有数据待处理*/
    if (!(ev & (~qevent::EV_WRITE)) || /*读端被关闭了*/
            unlikely(flags & (XPRT_CLOSED|XPRT_SHUTRD)))
        return;
    if (likely((flags & XPRT_ATTACHED) && delegate_)) {
        auto xpt = this->shared_from_this();
        delegate_->onRecv(xpt, ev);
    }
}

inline void Xprt::eatSend(int32_t ev)
{
    /*尽可写才通知写回调*/
    auto flags = *(volatile uint32_t*)&flags_;
    if (!(ev & qevent::EV_WRITE) || /*写端被关闭了*/
            unlikely(flags & (XPRT_CLOSED|XPRT_SHUTWR)))
        return;
    if (likely((flags & XPRT_ATTACHED) && delegate_)) {
        auto xpt = this->shared_from_this();
        delegate_->onSend(xpt, ev);
    }
}

inline void Xprt::eatClosedLocked(int32_t ev)
{
    /*主动关闭*/
    if (unlikely(ev & qevent::EV_ERROR))
        doShutdown(XPRT_SHUT_RDWR);

    uint32_t stat = 0;
    /*已关闭，则一定删除事件*/
    if (likely(!(flags_ & XPRT_CLOSED))) {
        /*半关闭，或没有关闭*/
        if (likely((flags_ & XPRT_SHUT_RDWR) != XPRT_SHUT_RDWR))
            return;
        auto v = flags(this, (flags_ & ~(XPRT_SHUT_RDWR)) | XPRT_CLOSED);
        /*必须触发过打开过才能触发关闭*/
        if (likely(v & (XPRT_OPENED | XPRT_CONNREFUSED)))
            stat = v & XPRT_OPENED? XPRT_CLOSED:XPRT_CONNREFUSED;
    }
    /*剥离对象*/
    auto xpt = this->shared_from_this();
    mgr()->detachXprt(xpt);
    /*异步删除或关闭事件？*/
    this->close();
    /*需要做最后的状态通知*/
    if (stat && likely(delegate_)) {
        lock_.unlock();
        delegate_->onChanged(xpt, static_cast<uint32_t>(stat));
        lock_.lock();
    }
}

inline bool Xprt::shutdownStat(uint32_t mask, bool sync)
{
    int32_t ev = 0;
    if (mask & XPRT_SHUTRD) ev |= qevent::EV_READ|qevent::EV_PRI;
    if (mask & XPRT_SHUTWR) ev |= qevent::EV_WRITE;
    /*已经关闭或已经设置了*/
    if (unlikely((flags_ & XPRT_CLOSED)) || ((flags_ & mask) == mask) ||
            ((mask != XPRT_SHUT_RDWR) && (mask & flags_)))
        return false;
    auto nflags = flags_ | mask;
    flags(this, nflags);
    /*如果完全关闭，则开启一次写事件，以保证套接字被正确关闭*/
    if (hasClosed(nflags)) {
        enableEvent(qevent::EV_WRITE);
    } else {
        disableEvent(ev);
    }
    return true;
}

inline int32_t Xprt::getAddrInfo(void)
{
    if (flags_ & XPRT_CONNECTING) { return -EINVAL; }
    int32_t rc;
    if (!localAddr_.len) {
        localAddr_.len = sizeof(localAddr_.addr);
        rc = ::getsockname(fd_, reinterpret_cast<sockaddr*>(&localAddr_.addr),
                    &localAddr_.len);
        if (unlikely(rc)) {
            localAddr_.len = 0;
            return -errno;
        }
    }
    if (!remoteAddr_.len) {
        remoteAddr_.len = sizeof(remoteAddr_.addr);
        rc = ::getpeername(fd_, reinterpret_cast<sockaddr*>(&remoteAddr_.addr),
                &remoteAddr_.len);
        if (unlikely(rc)) {
            remoteAddr_.len = 0;
            return -errno;
        }
    }
    showAddrInfo();
    return 0;
}

inline void Xprt::showAddrInfo(const std::string & what)
{
#ifdef DEBUG
    HostAddr local, remote;
    if (unlikely(!localAddr_.len)) { return; }
    utils::ntop(localAddr_, local);
    if ((flags_ & XPRT_CONNECTING) || !remoteAddr_.len) {
        app_log_info("one Xprt was %s [%d] : [%s:%s]",
            what.c_str(), fd_, local.first.c_str(), local.second.c_str());
    } else if (likely(remoteAddr_.len)) {
        utils::ntop(remoteAddr_, remote);
        app_log_info("one Xprt was %s [%d] : [%s:%s] ~ [%s:%s]",
            what.c_str(), fd_, local.first.c_str(), local.second.c_str(),
            remote.first.c_str(), remote.second.c_str());
    } else {
        app_log_info("one Xprt was %s [%d] : [%s:%s]",
            what.c_str(), fd_, local.first.c_str(), local.second.c_str());
    }
#endif
}

////////////////////////////////////////////////////////////////////////////////
inline int32_t TcpListenerXprt::listen(const HostAddr &host,
        uint32_t option, mkClntFunc && mkclnt)
{
    if (unlikely(!mkclnt)) return -EINVAL;
    std::lock_guard<std::mutex> guard(lock_);
    if (unlikely(fd_ > -1)) return -EISCONN;
    int32_t flags = option & (XPRT_RDREADY|XPRT_WRREADY|XPRT_OPT_UNIX);
    fd_guard fd = utils::listen(host,
            flags&XPRT_OPT_UNIX?UNIX_SOCK:INET_NONE,
                [&](int32_t fd, const InAddr & sin) {
        int32_t rc = 0;
        if (option & XPRT_OPT_NONBLOCK) {
            rc |= utils::nonblock(fd, true);
            flags |= XPRT_OPT_NONBLOCK;
        }
        if (option & XPRT_OPT_REUSEDADDR) {
            rc |= utils::reusedAddr(fd);
            flags |= XPRT_OPT_REUSEDADDR;
        }
        if (option & XPRT_OPT_KEEPALIVE) {
            rc |= utils::keepAlive(fd, -1, -1);
            flags |= XPRT_OPT_KEEPALIVE;
        }
        localAddr_ = sin;
        return rc ? -EINVAL : 0;
    });
    if (*fd < 0) return *fd;
    fd_ = *fd; flags_ = XPRT_TCPLSNR | XPRT_SHUTWR | flags;
    /*添加到管理器*/
    auto rc = mgr()->attachXprt(this->shared_from_this());
    if (unlikely(rc)) { fd_ = -1; return rc; }
    mkClnt_ = std::move(mkclnt);
    std::atomic_thread_fence(std::memory_order_release);
    /*启动事件*/
    if (option & XPRT_RDREADY) { addEvent(qevent::EV_READ|qevent::EV_PRI); }
    fd.eat();
    showAddrInfo();
    return 0;
}

inline int32_t TcpListenerXprt::doShutdown(uint32_t how)
{
    if (!(how & XPRT_SHUTRD)) { return -EINVAL; }
    if (!shutdownStat(XPRT_SHUTRD, false)) { return -EINVAL; }
    ::shutdown(this->fd(), SHUT_RDWR);
    return 0;
}

inline void TcpListenerXprt::TcpAcceptor::onRecv(XprtPtr & xpt, int32_t)
{
    std::shared_ptr<TcpListenerXprt> _this =
        std::static_pointer_cast<TcpListenerXprt>(xpt);
    for (int32_t i = 0; i < 16; i++) {
        XprtPtr clnt;
        int32_t rc = _this->doAccept(clnt);
        if (rc != -EBUSY) { break; }
    }
}

inline int32_t TcpListenerXprt::doAccept(XprtPtr & clnt) noexcept
{
    InAddr sin;
    fd_guard gfd = utils::accept(fd(), sin);
    if (*gfd < 0) { return *gfd; }
    std::shared_ptr<TcpListenerXprt> _this =
        std::static_pointer_cast<TcpListenerXprt>(this->shared_from_this());
    /*构造*/
    auto clnt_ = mkClnt_(_this);
    if (unlikely(!clnt_)) {
        app_log_warn("can't make one xprt for new connection");
        return -ENOMEM;
    }
    if (unlikely(clnt_->fd() > -1)) {
        app_log_warn("the given connection xprt is in "
                        "a establish state [%d]", clnt_->fd());
        return -EBUSY;
    }
#ifdef __linux__
    if (likely(_this->flags_ & XPRT_OPT_NONBLOCK))
        utils::nonblock(*gfd, true);
    if (likely(_this->flags_ & XPRT_OPT_KEEPALIVE))
        utils::keepAlive(*gfd, -1, -1);
#endif
    auto _clnt = std::static_pointer_cast<TcpXprt>(std::move(clnt_));
    _clnt->fd_    = *gfd;
    _clnt->flags_ = (_this->flags_ & XPRT_OPT_MASK) | XPRT_TCPTEMP;
    _clnt->getAddrInfo();
    /*先增加 xprtMgr 对 xprt 的持有引用，再开启事件*/
    auto rc = _this->mgr()->attachXprt(_clnt);
    if (unlikely(rc)) {
        app_log_warn("Oops: can't attach one xprt for passive tcpXprt");
        return rc;
    }
    int32_t ev = 0;
    if (_this->flags_ & XPRT_RDREADY) ev |= qevent::EV_READ;
    if (_this->flags_ & XPRT_WRREADY) ev |= qevent::EV_WRITE;
    if (ev) { _clnt->addEvent(ev); }
    gfd.eat();
    clnt = std::move(_clnt);
    return 0;
}
////////////////////////////////////////////////////////////////////////////////

inline TcpXprt::~TcpXprt(void)
{
    app_log_info("destroy TcpXprt %p : %d", this, fd_);
}

inline int32_t TcpXprt::connect(const HostAddr & host, uint32_t option)
{
    std::lock_guard<std::mutex> guard(lock_);
    if (unlikely(fd_ > -1)) return -EISCONN;
    uint32_t flags = 0;
    fd_guard fd = utils::tcpConnect(host,
            option&XPRT_OPT_UNIX?UNIX_SOCK:INET_NONE,
                [&](int32_t fd, const InAddr & ss) {
        int32_t rc = 0;
        if (option & XPRT_OPT_NONBLOCK) {
            rc |= utils::nonblock(fd, true);
            flags |= (XPRT_OPT_NONBLOCK | XPRT_CONNECTING);
        }
        if (option & XPRT_OPT_KEEPALIVE) {
            rc |= utils::keepAlive(fd, -1, -1);
            flags |= XPRT_OPT_KEEPALIVE;
        }
        localAddr_.len = 0;
        /*异步连接时需要二次连接*/
        remoteAddr_ = ss;
        return rc ? -EINVAL : 0;
    });
    if (*fd < 0) return *fd;
    /*连接成功*/
    if (errno != EINPROGRESS) flags &= ~XPRT_CONNECTING;
    /*设置描述符，根据选项启动事件*/
    fd_ = *fd; flags_ = XPRT_TCPCLNT | flags;
    int32_t rc = getAddrInfo();
    if (unlikely(rc)) { fd_ = -1; return rc; }
    rc = mgr()->attachXprt(this->shared_from_this());
    if (unlikely(rc)) { fd_ = -1; return rc; }
    std::atomic_thread_fence(std::memory_order_release);
    int32_t ev = flags & XPRT_OPT_NONBLOCK?qevent::EV_WRITE:0;
    if (option & XPRT_RDREADY) ev |= qevent::EV_READ;
    if (option & XPRT_WRREADY) ev |= qevent::EV_WRITE;
    if (ev) addEvent(ev);
    fd.eat();
    return 0;
}

inline int32_t TcpXprt::doShutdown(uint32_t how)
{
    if (!(how & XPRT_SHUT_RDWR)) return -EINVAL;
    if (shutdownStat(how, true)) {
        int32_t _how = 0;
        if (how & XPRT_SHUTRD) _how |= SHUT_RD + 1;
        if (how & XPRT_SHUTWR) _how |= SHUT_WR + 1;
        ::shutdown(fd_, _how - 1);
    }
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
namespace utils {

int32_t ntop(const InAddr &in, HostAddr &p)
{
    if (!in.len) { return -EINVAL; }
    switch (in.addr.ss_family) {
        case INET_IPV4: {
            auto sin = reinterpret_cast<const sockaddr_in*>(&in.addr);
            if (unlikely(in.len != sizeof(*sin))) {
                app_log_error("invalid length of sockaddr[IPv4]");
                return -EINVAL;
            }
            char ipv4[INET_ADDRSTRLEN];
            if (!::inet_ntop(INET_IPV4, &sin->sin_addr, ipv4, INET_ADDRSTRLEN))
                return -EINVAL;
            p = { ipv4, std::to_string(ntohs(sin->sin_port))};
        }
            break;
#ifdef IPPROTO_IPV6
        case INET_IPV6: {
            auto sin = reinterpret_cast<const sockaddr_in6*>(&in.addr);
            if (unlikely(in.len != sizeof(*sin))) {
                app_log_error("invalid length of sockaddr[IPv6]");
                return -EINVAL;
            }
            char ipv6[INET6_ADDRSTRLEN];
            if (!::inet_ntop(INET_IPV6, &sin->sin6_addr, ipv6, INET6_ADDRSTRLEN))
                return -EINVAL;
            p = { ipv6, std::to_string(ntohs(sin->sin6_port))};
        }
            break;
#endif
        case UNIX_SOCK:
        default:
            return -EAFNOSUPPORT;
    }
    return 0;
}

int32_t pton(const HostAddr &p, InAddr &in)
{
    int32_t rc = 0;
    int32_t port = std::atoi(p.second.c_str());
    if (port < 0 || port > 65535) { return -EINVAL; }

    in.len = 0;
    std::memset(&in.addr, 0, sizeof(in.addr));

    auto *sin = reinterpret_cast<sockaddr_in*>(&in.addr);
    rc = ::inet_pton(INET_IPV4, p.first.c_str(), &sin->sin_addr);
    if (rc == 1) {
        sin->sin_family = INET_IPV4;
        sin->sin_port = htons(port);
        in.len = sizeof(*sin);
        return 0;
    }

#ifdef IPPROTO_IPV6
    auto *sin6 = reinterpret_cast<sockaddr_in6*>(&in.addr);
    rc = ::inet_pton(INET_IPV6, p.first.c_str(), &sin6->sin6_addr);
    if (rc == 1) {
        sin6->sin6_family= INET_IPV6;
        sin6->sin6_port = htons(port);
        in.len = sizeof(*sin6);
        return 0;
    }
#endif
    return -EAFNOSUPPORT;
}

inline addrinfo *getAddrInfo(const HostAddr & host, int32_t flag,
        int32_t family, int32_t socktype)
{
    addrinfo *ai = nullptr;

    if (family == UNIX_SOCK) {
        socklen_t l = 0;
        sockaddr_un *un;
        ai = static_cast<addrinfo*>(std::malloc(sizeof(*ai) + sizeof(*un)));
        std::memset(ai, 0, sizeof(*ai));
        un = reinterpret_cast<sockaddr_un*>(&ai[1]);
        l += offsetof(sockaddr_un, sun_path);
        l += snprintf(un->sun_path, sizeof(un->sun_path), "%s", host.first.c_str());
        ai->ai_family   = UNIX_SOCK;
        ai->ai_socktype = socktype;
        ai->ai_addrlen  = l + 1;
        ai->ai_addr     = reinterpret_cast<sockaddr*>(un);
        return ai;
    }
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_flags    = AI_CANONNAME | flag;
    hints.ai_family   = family;
    hints.ai_socktype = socktype;
    do {
        auto n = ::getaddrinfo(host.first.empty()?"0.0.0.0":host.first.c_str(),
                    host.second.c_str(), &hints, &ai);
        if (n == 0) break;
        if (n != EAI_AGAIN) {
            app_log_error("Can't get address information "
                "about socket by [%s : %s]",
                host.first.c_str(), host.second.c_str());
            ai = nullptr;
            break;
        }
    } while (1);
    return ai;
}

inline int32_t listen(const HostAddr & host, int32_t family,
        setupSockFunc && setup = nullptr)
{
    fd_guard sfd;
    int32_t rc = -EINVAL;
    auto ai = getAddrInfoPtr(host, AI_PASSIVE, family, SOCK_STREAM);
    for (addrinfo *tmp = ai.get(); tmp != nullptr; tmp = tmp->ai_next) {
        fd_guard gfd;
        gfd = ::socket(tmp->ai_family, tmp->ai_socktype, tmp->ai_protocol);
        if (*gfd < 0) { rc = -errno; continue; }
        InAddr sin;
        sin.len = tmp->ai_addrlen;
        std::memcpy(&sin.addr, tmp->ai_addr, tmp->ai_addrlen);
        if (setup) { rc = setup(*gfd, sin); if (rc) break; }
        rc = ::bind(*gfd, tmp->ai_addr, tmp->ai_addrlen);
        if (!rc) {
            rc = ::listen(*gfd, 128);
            if (!rc) { sfd = std::move(gfd); break; }
        }
        rc = -errno;
    }
    if (!rc) return sfd.eat();
    app_log_error("listen on %s:%s failed : %s",
        host.first.c_str(), host.second.c_str(), strerror(-rc));
    return rc;
}

inline int32_t accept(int32_t lfd, InAddr & sin)
{
    sin.len = sizeof(sin.addr);
    auto cfd = ::accept(lfd, reinterpret_cast<sockaddr*>(&sin.addr), &sin.len);
    if (likely(cfd > -1)) return cfd;
    auto rc = errno;
    if (likely(rc == EAGAIN || rc == EMFILE || rc == ECONNABORTED)) {
        /*打开的文件太多了，需要关闭读事件，暂停 accept() */
        if (unlikely(rc == EMFILE)) {
            app_log_error("accept filed : %s", ::strerror(rc));
        } else {
            /*均当作重试处理*/
            rc = errno = EAGAIN;
        }
    }
    return -rc;
}

inline int32_t connect(const HostAddr &host, int32_t family, int32_t proto,
        setupSockFunc && setup = nullptr)
{
    fd_guard sfd;
    int32_t rc = -EINVAL;
    auto ai = getAddrInfoPtr(host, 0, family, proto);
    for (addrinfo *tmp = ai.get(); tmp; tmp = tmp->ai_next) {
        fd_guard gfd;
        gfd = ::socket(tmp->ai_family, tmp->ai_socktype, tmp->ai_protocol);
        if (*gfd < 0) { rc = -errno; continue; }
        InAddr sin;
        sin.len = tmp->ai_addrlen;
        std::memcpy(&sin.addr, tmp->ai_addr, tmp->ai_addrlen);
        if (setup) { rc = setup(*gfd, sin); if (rc) break; }
        rc = ::connect(*gfd, tmp->ai_addr, tmp->ai_addrlen);
        if (!rc||(rc = -errno) == -EINPROGRESS) { sfd = std::move(gfd); break; }
    }
    if (!rc || rc == -EINPROGRESS) {
        if (rc) { errno = EINPROGRESS; } return sfd.eat();
    }
    app_log_error("connecting [%s:%s] address failed : %s",
        host.first.c_str(), host.second.c_str(), strerror(-rc));
    return rc;
}

inline int enableFdFlag(int32_t fd, int32_t flags)
{
    if (fd < 0) return -EBADF;
    int32_t oflags = fcntl(fd, F_GETFL, nullptr);
    if (oflags < 0) return -errno;
    int32_t rc = fcntl(fd, F_SETFL, oflags | flags);
    if (rc < 0) return -errno;
    return 0;
}

inline int disableFdFlag(int32_t fd, int32_t flags)
{
    if (fd < 0) return -EBADF;
    int32_t oflags = fcntl(fd, F_GETFL, nullptr);
    if (oflags < 0) return -errno;
    int32_t rc = fcntl(fd, F_SETFL, oflags & ~flags);
    if (rc < 0) return -errno;
    return 0;
}

template <typename VAL>
int32_t setSockOpt(int32_t fd, int32_t level, int32_t name, VAL & val)
{
    int32_t rc = ::setsockopt(fd, level, name, &val, (socklen_t)sizeof(VAL));
    if (unlikely(rc < 0)) {
        app_log_error("set socket option [L : %d, O : %d] failed: %s",
            level, name, ::strerror(errno));
        return -errno;
    }
    return 0;
}

inline int32_t keepAlive(int32_t fd, int32_t intervel, int32_t trys)
{
    int32_t rc, val;
    val = 0;
    if (intervel == 0)
        return setSockOpt(fd, SOL_SOCKET, SO_KEEPALIVE, val);
    val = 1;
    rc = setSockOpt(fd, SOL_SOCKET, SO_KEEPALIVE, val);
    if (unlikely(rc)) return rc;
    if (intervel < 0) intervel = 75;
    if (trys < 0) trys = 9;
#ifdef TCP_KEEPIDLE
    rc = setSockOpt(fd, IPPROTO_TCP, TCP_KEEPIDLE, intervel);
    if (unlikely(rc)) return rc;
#endif
#ifdef TCP_KEEPINTVL
    rc = setSockOpt(fd, IPPROTO_TCP, TCP_KEEPINTVL, intervel);
    if (unlikely(rc)) return rc;
#endif
#ifdef TCP_KEEPCNT
    rc = setSockOpt(fd, IPPROTO_TCP, TCP_KEEPCNT, trys);
    if (unlikely(rc)) return rc;
#endif
    return 0;
}

}
}

#endif
