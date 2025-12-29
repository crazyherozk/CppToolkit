#include "xprt.hpp"

EZLOG_HOOKER_VAR();
QEVENT_REACTOR_VAR();

int main(void)
{

    if (1)
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

    fprintf(stderr, "\t  [-] %s:%s - 0x%08x.0x%08x.0x%08x.0x%08x:0x%04x\n",
        ipv6.first.c_str(), ipv6.second.c_str(),
        sin6->sin6_addr.__u6_addr.__u6_addr32[0],
        sin6->sin6_addr.__u6_addr.__u6_addr32[1],
        sin6->sin6_addr.__u6_addr.__u6_addr32[2],
        sin6->sin6_addr.__u6_addr.__u6_addr32[3],
        sin6->sin6_port
    );

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
        {"www.baidu.com", "80"}, network::INET_IPV4, SOCK_STREAM
    );
    fprintf(stderr, "\t  [-] ipv4 连接 : %s\n", *fd < 0?"失败":"成功");

    fprintf(stderr, "\t[*] ipv6 连接\n");
    fd = network::utils::connect(
        {"ipv6.baidu.com", "443"}, network::INET_IPV6, SOCK_STREAM
    );
    fprintf(stderr, "\t  [-] ipv6 连接 : %s\n", *fd < 0?"失败":"成功");
}

    if (1)
{
    fprintf(stderr, "\t[*] ipv4 侦听\n");
    network::fd_guard fd = network::utils::listen(
        {"localhost", "10000"}, network::INET_IPV4, 
            [](int32_t sfd, const network::InAddr & addr)
    {
        auto rc = network::utils::keepAlive(sfd, 10, 5);
        fprintf(stderr, "\t  [!] 设置选项 : %s\n", rc?"失败":"成功");
        return rc;
    });
    fprintf(stderr, "\t  [-] ipv4 侦听 : %s\n", *fd < 0?"失败":"成功");
}

}

    return EXIT_SUCCESS;
}