#include "xprt.hpp"

EZLOG_HOOKER_VAR();
QEVENT_REACTOR_VAR();

struct XprtServer : public network::TcpListenerXprt {
    using network::TcpListenerXprt::TcpListenerXprt;
    static uint32_t nr_server;

    XprtServer(network::XprtMgrPtr mgr) : TcpListenerXprt(mgr) {
        nr_server++;
    }

    ~XprtServer(void) {
        nr_server--;
    }
};

uint32_t XprtServer::nr_server;

int main(void)
{

    if (0)
{
    fprintf(stderr, "==== 辅助函数测试 ====\n");

    if (1)
{
    network::InAddr   inet;
    network::HostAddr ipv4 = { "127.0.0.1", "80" };
    fprintf(stderr, "\t[*] ipv4 地址转换\n");
    auto rc = network::utils::pton(ipv4, inet);
    assert(rc == 0);

    auto sin4 = reinterpret_cast<sockaddr_in*>(&inet.addr);
    assert(inet.len == sizeof(*sin4));
    assert(sin4->sin_family == network::INET_IPV4);

    fprintf(stderr, "\t  [-] %s:%s - 0x%08x:0x%04x\n",
        ipv4.first.c_str(), ipv4.second.c_str(),
        sin4->sin_addr.s_addr, sin4->sin_port
    );
    network::HostAddr tmp;
    rc = network::utils::ntop(inet, tmp);
    assert(rc == 0);
    assert(tmp == ipv4);

    network::HostAddr ipv6 = { "2001:db8:85a3::8a2e:370:7334", "80" };
    fprintf(stderr, "\t[*] ipv6 地址转换\n");
    rc = network::utils::pton(ipv6, inet);
    assert(rc == 0);

    auto sin6 = reinterpret_cast<sockaddr_in6*>(&inet.addr);
    assert(inet.len == sizeof(*sin6));
    assert(sin6->sin6_family == network::INET_IPV6);

#ifdef __APPLE__
    fprintf(stderr, "\t  [-] %s:%s - 0x%08x.0x%08x.0x%08x.0x%08x:0x%04x\n",
        ipv6.first.c_str(), ipv6.second.c_str(),
        sin6->sin6_addr.__u6_addr.__u6_addr32[0],
        sin6->sin6_addr.__u6_addr.__u6_addr32[1],
        sin6->sin6_addr.__u6_addr.__u6_addr32[2],
        sin6->sin6_addr.__u6_addr.__u6_addr32[3],
        sin6->sin6_port
    );
#else
    fprintf(stderr, "\t  [-] %s:%s - 0x%08x.0x%08x.0x%08x.0x%08x:0x%04x\n",
        ipv6.first.c_str(), ipv6.second.c_str(),
        sin6->sin6_addr.__in6_u.__u6_addr32[0],
        sin6->sin6_addr.__in6_u.__u6_addr32[1],
        sin6->sin6_addr.__in6_u.__u6_addr32[2],
        sin6->sin6_addr.__in6_u.__u6_addr32[3],
        sin6->sin6_port
    );
#endif

    rc = network::utils::ntop(inet, tmp);
    assert(rc == 0);
    assert(tmp == ipv6);
}

    if (1)
{
    auto showInfo = [](std::shared_ptr<addrinfo> info) {
        if (info) {
            for (auto next = info.get(); next; next = next->ai_next) {
                network::HostAddr tmp;
                network::InAddr inet = next;
                auto rc = network::utils::ntop(inet, tmp);
                assert(rc == 0);
                fprintf(stderr, "\t  [-] AF : %02x, ADDR : %s, PORT : %s\n",
                    next->ai_family, tmp.first.c_str(), tmp.second.c_str());
            }
        } else {
            fprintf(stderr, "\t[!] 网络地址获取失败\n");
        }
    };
    network::HostAddr host = { "www.baidu.com", "80" };
    fprintf(stderr, "\t[*] inet4 网络地址获取 : %s\n", host.first.c_str());
    auto addrInfo = network::utils::getAddrInfoPtr(host, 0, network::INET_IPV4, SOCK_STREAM);
    showInfo(addrInfo);

    network::HostAddr host6 = { "ipv6.baidu.com", "80" };
    fprintf(stderr, "\t[*] inet6 网络地址获取 : %s\n", host6.first.c_str());
    addrInfo = network::utils::getAddrInfoPtr(host6, 0, network::INET_IPV6, SOCK_STREAM);
    showInfo(addrInfo);
}

    if (1)
{
    fprintf(stderr, "\t[*] ipv4 连接\n");
    network::fd_guard fd = network::utils::connect(
        {"www.baidu.com", "80"}, network::INET_IPV4, network::INET_TCP
    );
    fprintf(stderr, "\t  [-] ipv4 连接 : %s\n", *fd < 0?"失败":"成功");

    fprintf(stderr, "\t[*] ipv6 连接\n");
    fd = network::utils::connect(
        {"ipv6.baidu.com", "443"}, network::INET_IPV6, network::INET_TCP
    );
    fprintf(stderr, "\t  [-] ipv6 连接 : %s\n", *fd < 0?"失败":"成功");
}

    if (1)
{
    fprintf(stderr, "\t[*] ipv4 侦听\n");
    network::fd_guard fd = network::utils::listen(
        {"0.0.0.0", "10000"}, network::INET_IPV4, 
            [](int32_t sfd, const network::InAddr & addr)
    {
        auto rc = network::utils::keepAlive(sfd, 10, 5);
        fprintf(stderr, "\t  [!] 设置选项 : %s\n", rc?"失败":"成功");
        return rc;
    });
    fprintf(stderr, "\t  [-] ipv4 侦听 : %s\n", *fd < 0?"失败":"成功");

    if (*fd > -1) {
        fprintf(stderr, "\t[*] ipv4 侦听(地址重用)\n");
        network::fd_guard dup = network::utils::listen(
            {"localhost", "10000"}, network::INET_IPV4
        );
        assert(*dup < 0);
    }

}

}

    fprintf(stderr, "==== 创建管理器 ====\n");

    auto mgr = std::make_shared<network::XprtMgr>();
    assert(mgr);

    if (1)
{

    fprintf(stderr, "==== 服务端函数测试 ====\n");
    {
        fprintf(stderr, "  1. 侦听创建后，被管理器管理生命周期\n");
        volatile bool oneXprt = false;
        fprintf(stderr, "\t[*] 侦听 [localhost:10000]，但不加入事件循环\n");
        /*客户端已经被管理起来了，离开此作用域并不会销毁*/
        auto server = std::make_shared<XprtServer>(mgr);
        /*不论Server还是被动创建的Client, 它们的事件都由调用者管理*/
        auto rc = server->listen({"localhost", "10000"}, network::XPRT_OPT_NONBLOCK,
            [&](network::TcpListenerXprtPtr srv) 
        {
            oneXprt = true;
            fprintf(stderr, "\t[1] 接受一个连接\n");
            return nullptr;
        });

        if (rc != 0) {
            fprintf(stderr, "侦听失败，检查 10000 端口是否被占用\n");
            exit(EXIT_FAILURE);
        }

        /*并没有加入事件循环，故不会产生被动连接*/
        fprintf(stderr, "\t[*] 连接 [localhost:10000]\n");
        network::fd_guard clnt = network::utils::connect(
            {"localhost", "10000"}, network::INET_NONE, network::INET_TCP
        );

        assert(*clnt > -1);

        fprintf(stderr, "\t[*] 开始循环\n");
        mgr->loop()->run(0);

        assert(oneXprt == false);

        /*手动加入*/
        fprintf(stderr, "\t[*] 加入循环\n");
        server->addEvent(qevent::EV_READ);
        mgr->loop()->run(0);

        assert(oneXprt == true);
    }

    /*上面创建的 Listener 不会被销毁*/
    assert(XprtServer::nr_server == 1);

    {
        fprintf(stderr, "  2. 侦听创建后，发生错误可以被自动销毁\n");
        volatile bool oneXprt = false;
        /*客户端已经被管理起来了，要想在异常时被提前销毁，必须加入到事件循环*/
        fprintf(stderr, "\t[*] 侦听 [localhost:20000]，加入事件循环\n");
        auto server = std::make_shared<XprtServer>(mgr);

        assert(XprtServer::nr_server == 2);

        /*不论Server还是被动创建的Client, 它们的事件都由传入的事件选项来决定*/
        auto rc = server->listen({"localhost", "20000"},
            network::XPRT_OPT_NONBLOCK|network::XPRT_RDREADY,
            [&](network::TcpListenerXprtPtr srv) 
        {
            oneXprt = true;
            fprintf(stderr, "\t[2] 接受一个连接\n");
            return nullptr;
        });

        if (rc != 0) {
            fprintf(stderr, "侦听失败，检查 20000 端口是否被占用\n");
            exit(EXIT_FAILURE);
        }

        fprintf(stderr, "\t[*] 连接 [localhost:20000]\n");
        network::fd_guard clnt = network::utils::connect(
            {"localhost", "20000"}, network::INET_NONE, network::INET_TCP
        );

        assert(*clnt > -1);

        fprintf(stderr, "\t[*] 开始循环\n");
        mgr->loop()->run(0);

        assert(oneXprt == true);

        fprintf(stderr, "\t[!] 手动关闭\n");
        network::Xprt::shutdown(server, network::XPRT_SHUTRD);

        fprintf(stderr, "\t[*] 开始循环\n");
        mgr->loop()->run(0);

        assert(XprtServer::nr_server == 1);
    }
}

    if (1)
{
    fprintf(stderr, "==== 客户端函数测试 ====\n");
}

    return EXIT_SUCCESS;
}