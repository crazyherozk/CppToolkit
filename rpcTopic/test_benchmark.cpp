#include <limits>
#include <iostream>
#include "shmRpcTopic.hpp"

QEVENT_REACTOR_VAR();
EZLOG_HOOKER_VAR();
RPCTOPIC_RUNTIME_VAR();

using namespace rpc_topic;

int main(void)
{
    std::cout << "**** 初始化服务端环境 ****" << std::endl;

    Runtime::init("topicBenchmarkService");

    auto & appName = Runtime::get()->appName();
    assert(appName == NameString("topicBenchmarkService"));

    if (1)
{
    std::cout << "==== 服务端、代理端的创建、删除 ====" << std::endl;

    const std::string srvName = "BaseService";

    /*创建默认基类 service ，环境只管理它的弱引用*/
    auto service = Runtime::get()->buildService<>(srvName);
    assert(service);

    /*无法创造同名服务*/
    auto srvPtr = Runtime::get()->buildService<>(srvName);
    assert(!srvPtr);

    /*创建默认基类 proxy ，环境只管理它的弱引用*/
    auto proxy = Runtime::get()->buildProxy<>(appName, srvName);
    assert(proxy);

    /*无法创造同名代理*/
    auto pxyPtr = Runtime::get()->buildProxy<>(appName, srvName);
    assert(!pxyPtr);

    /*手动销毁 服务和代理*/

    service.reset();
    proxy.reset();

    /*能再次创建了*/
    service = Runtime::get()->buildService<>(srvName);
    assert(service);

    proxy = Runtime::get()->buildProxy<>(appName, srvName);
    assert(proxy);

    /*服务和代理是确定的，很少在退出程序之前销毁，所以不添加主动移除接口*/
}

    if (1)
{
    std::cout << "==== 服务端上线、离线测试 ====" << std::endl;
    const std::string srvName = "BaseService";

    /*创建默认基类 service ，环境只管理它的弱引用*/
    auto service = Runtime::get()->buildService<>(srvName);
    assert(service);

    /*创建默认基类 proxy ，环境只管理它的弱引用*/
    auto proxy = Runtime::get()->buildProxy<>(appName, srvName);
    assert(proxy);

    /*订阅上线回调*/
    uint8_t status = 0;
    proxy->getProxyStatusEvent().subscribe([&](StatusValue stat) {
        if (stat == StatusValue::APP_ONLINE) {
            status = 1;
        } else {
            status = 2;
        }
    });

    auto & loop = Runtime::get()->loop();

    /*默认500毫秒超时*/
    auto timer = loop->addTimer(700, [&](int32_t){
        assert(status == 1);
    });

    while (status != 1) {
        loop->run();
    }

    loop->removeTimer(timer);

    /*强制离线*/
    service.reset();

    timer = loop->addTimer(700, [&](int32_t){
        assert(status == 2);
    });

    while (status != 2) {
        loop->run();
    }

    loop->removeTimer(timer);

    /*客户端离线时可能再次发送请求，等待请求数据过期*/
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

    if (1)
{
    std::cout << "==== 发布者的创建、删除、发布 ====" << std::endl;

    /*需要使用C样式字符串来进行编译时Hash计算*/
    constexpr char topicName[] = "BaseTopic";

    /*创建默认基类 service ，环境只管理它的弱引用*/
    auto service = Runtime::get()->buildService<>("BaseService");
    assert(service);

    /*创建一个不关心订阅请求的发布者*/

    auto id = service->buildPublisher<>(topicName);
    assert(id);

    /*
     * 嵌入式系统中，主题很少，基本上不超过256个
     * 可以直接通过 service 来发布数据，即使内部使用的Hash表存储，理论上查找是O(1)
     */

    bool rc;
    /*使用 C 样式字符串来生成 主题名 的编译时 Hash，加速Publisher的查找*/
    rc = service->publish<fnv1a_64(topicName)>(3.1415f);
    assert(!rc);

    /*
     * PC服务系统中，主题较多，比如好几千条
     * 可以使用ID查找发布者对象，缓存发布者对象，后续的每次发布都减少一次查找
     */
    auto pubPtr = service->topicPublisher(id);
    assert(pubPtr);

    /*没有任何代理链接，发布失败*/
    rc = pubPtr->publish(3.1415f);
    assert(!rc);

    /*
     * 定向发布，实际上Proxy的Key就是它的IPC对象的名字的Hash值
     * 但是Key只能 通过回调被动获取，当然你可以保存一份
     */
#if 0
    pubPtr->publishTo(fnv1a_64("/proxy.xxxx"), 4096U);
#endif

    /*在移除发布者之前，不能创建同名的发布者*/
    {
        auto id = service->buildPublisher<>(topicName);
        assert(!id);
    }

    /*移除发布者，但是 pubPtr 有一份引用，接口会将发布者置为无效的，此后用它发布数据都是失败的*/
    service->removePublisher(id);

    /*可以操作，但是始终失败*/
    rc = pubPtr->publish(3.1415f);
    assert(!rc);

    /*创建一个关心订阅请求的发布者，上线后定向发布最新数据给订阅者*/
    id = service->buildPublisher<>(topicName, [=](IpcKey key) {
        /*
         * 在此处定向响应主题订阅请求，当然也可以广播数据，比如广播上线者的Key
         * service 被 Publisher的回调 引用了，service 又强引用 Publisher
         * 形成了环形引用，所以必须手动 移除 Publisher，这样才能解除，释放双方
         */
        service->publishTo<fnv1a_64(topicName)>(key, 4096U);
    });

    assert(id);

    /*移除所有发布者*/
    service->removePublisher();

    /*创建一个继承基类的发布者*/

    struct MyPublisher : public Publisher {
        MyPublisher(Service *service, const std::string & topic, int args)
            : Publisher(service, topic)
        {
            /*args 是继承类的 特定参数*/
        }

    protected:
        void onSubscriptionChanged(IpcKey ipc) override {
            /*重写订阅响应*/
            publishTo(ipc/*请求订阅的IPC地址*/, 9527U/*首次发布的数据*/);
        }
    };

    id = service->buildPublisher<MyPublisher>(topicName, 1024);
    assert(id);
}

    if (1)
{
    std::cout << "==== 订阅者的创建、删除 ====" << std::endl;

    /*需要使用C样式字符串来进行编译时Hash计算*/
    constexpr char topicName[] = "BaseTopic";

    /*创建默认基类 Proxy ，环境只管理它的弱引用*/
    auto proxy = Runtime::get()->buildProxy<>(appName, "BaseService");
    assert(proxy);

    /*创建一个回调模式接收发布数据的简单订阅者*/

    auto id = proxy->buildSubscriber<>(topicName, [](PackBuffer & buff) {
        /*数据来了，通过此回调通知，PackBuffer & 中是序列号的数据，可以按发布数据类型解包*/
        float a;
        uint32_t b;
        size_t rc = buff.unpack(a, b);
        assert(rc == (sizeof(a) + sizeof(b)));
    });
    assert(id);

    /*移除之前，不能创建同名的*/
    {
        auto id = proxy->buildSubscriber<>(topicName);
        assert(!id);
    }

    /*移除*/
    proxy->removeSubscriber(id);

    /*订阅数据，发布者以结构体发布数据，订阅则直接以结构体来回调获取数据*/
    struct TopicData {
        uint8_t id;
        uint32_t data[4];
    };

    id = proxy->launchSubscribe(topicName, [](const TopicData &data) {
        /*已经将数据解包到 TopicData 中*/
        (void)data.id;
    });

    assert(id);

    /*移除所有订阅者*/
    proxy->removeSubscriber();

    /*创建一个继承基类的订阅者*/
    struct MySubscriber : public Subscriber {
        MySubscriber(Proxy * proxy, const std::string & topic, int args)
            : Subscriber(proxy, topic)
        {
            /*args 的处理*/
        }
    protected:
        void onSubscribeEvent(PackBuffer & buff) override {
            /*自己按需解包*/
        }
    };

    id = proxy->buildSubscriber<MySubscriber>(topicName, 1024U);
    assert(id);
}

    if (1)
{
    std::cout << "==== 订阅与发布 ====" << std::endl;

    struct CarStatus {
        float   speed{0.0f};
        int32_t ign{0};
        int32_t acc{0};
        bool operator==(const CarStatus & other) const noexcept {
            return (speed == other.speed) && 
               (ign == other.ign) && 
               (acc == other.acc);
        }
    };

    /*需要使用C样式字符串来进行编译时Hash计算*/
    constexpr char topicName[] = "BaseTopic";
    constexpr char srvName[]   = "BaseService";

    /*创建默认基类 Service ，环境只管理它的弱引用*/
    auto service = Runtime::get()->buildService<>(srvName);
    assert(service);

    /*创建默认基类 Proxy ，环境只管理它的弱引用*/
    auto proxy = Runtime::get()->buildProxy<>(appName, srvName);
    assert(proxy);

    CarStatus carStat;
    carStat.speed = 120.1f;
    carStat.ign   = 1;
    carStat.acc   = 1;

    /*创建发布者*/

    /*一个简单的发布者*/
    auto id = service->buildPublisher<>(topicName,
            /*一定是弱引用，否则发生循环引用，Service 无法被释放*/
            [=, srvWPtr=ServiceWPtr(service)](IpcKey ipc)
    {
        /*订阅通知，定向发布一条数据, service 一定存在*/
        srvWPtr.lock()->publishTo<fnv1a_64(topicName)>(ipc, carStat);
        /*启动其他的（发布）任务*/
    });
    assert(id);

    bool running = true;

    /*创建一个回调模式接收发布数据的简单订阅者，常驻*/
    id = proxy->launchSubscribe(topicName, [&](const CarStatus & status) {
        running = false;
        assert(status == carStat);
        /*中断循环*/
        Runtime::get()->loop()->notify();
    });

    assert(id);

    /*请求握手*/
    proxy->getProxyStatusEvent().subscribe([](StatusValue stat) {
        /*常驻订阅，不需要上线发起订阅请求*/
        if (stat == StatusValue::APP_ONLINE) {

        }
    });

    auto timer = Runtime::get()->loop()->addTimer(500, [&](int32_t){
        /*500毫秒一定握手成功，并发布接收到了订阅数据*/
        assert(running == false);
    });

    /*开始循环*/
    do {
        Runtime::get()->loop()->run();
    } while (running);

    Runtime::get()->loop()->removeTimer(timer);

    /*未强制离线，ShadowProxy 可能未超时删除，后续再次请求时，发现已存在，将忽略，直到 Broken*/
#if 1
    /*
     * 即使不运行，原来建立的管道也应该在后续循环中 Broken
     * 如果 存在 过期但是尚未 Broken 的 ShadowProxy，也能正常处理
     */
    proxy.reset();
    running = true;
    timer = Runtime::get()->loop()->addTimer(700,
        [&](int32_t){ running = false; }
    );

    do {
        Runtime::get()->loop()->run();
    } while (running);
#endif
}

    if (1)
{
    std::cout << "==== RPC服务 创建、删除 ====" << std::endl;

    /*需要使用C样式字符串来进行编译时Hash计算*/
    constexpr char rpcName[] = "BaseRpc";
    constexpr char srvName[] = "BaseService";

    auto service = Runtime::get()->buildService<>(srvName);
    assert(service);

    /*创建以透明数据回调处理请求的RPC服务*/
    auto id = service->buildRpcServer<>(rpcName,
        [](const RpcServer::ReqId reqId, PackBuffer &)
    {
        /*解包，处理*/
        (void)reqId.first; /*请求的IPC标识*/
        (void)reqId.second; /*请求的RPC的消息ID，如果为0，表示不需响应*/
        // 响应
        // 如果 reqId 不是有效的，或不需要响应，内部会忽略
        // service->response<fnv1a_64(rpcName)>(reqId, ...);
    });

    /*为删除前，不能创造同名服务*/
    {
        auto id = service->buildRpcServer<>(rpcName);
        assert(!id);
    }

    /*移除*/
    service->removeRpcServer(id);

    /*创建一个继承方式使用的RPC服务*/

    struct MyRpcService : public RpcServer {
        MyRpcService(Service * service, const std::string & name, int32_t args)
            : RpcServer(service, name)
        {
            /*处理 args*/
        }
    protected:
        void onRpcEvent(const ReqId reqId, PackBuffer & buff) override {
            /*解包，处理*/
            // 响应
            // 如果 reqId 不是有效的，或不需要响应，内部会忽略
            // service->response<fnv1a_64(rpcName)>(reqId, ...);
        };
    };

    id = service->buildRpcServer<MyRpcService>(rpcName, 1024U);
    assert(id);
}

    if (1)
{
    std::cout << "==== RPC客户端 创建、删除 ====" << std::endl;

    /*需要使用C样式字符串来进行编译时Hash计算*/
    constexpr char rpcName[] = "BaseRpc";
    constexpr char srvName[] = "BaseService";

    /*创建代理*/
    auto proxy = Runtime::get()->buildProxy<>(appName, srvName);
    assert(proxy);

    /*创建一个指定超时请求的RPC客户端*/
    auto id = proxy->buildRpcClient(rpcName, 100);
    assert(id);

    /*移除前不能创造同名RPC客户端*/
    {
        auto id = proxy->buildRpcClient(rpcName);
        assert(!id);
    }

    /*同步发送请求，不需要响应*/

    proxy->request<fnv1a_64(rpcName)>(10U, 120.0f);

    /*异步发送请求，服务端需要响应*/

    proxy->requestAsync<fnv1a_64(rpcName)>(
        [](const uint32_t & result){
            /*响应通过确切的回调参数给出*/
        }, CString<16>("helloworld")
    );

    /*移除*/
    proxy->removeRpcClient(id);
}

    if (1)
{
    std::cout << "==== 请求与响应 ====" << std::endl;

    /*请求参数*/
    constexpr int32_t CarIGN{1};
    constexpr int32_t CarACC{1};
    constexpr float   CarSpeed{134.1f};
    /*响应结果*/
    constexpr uint32_t CarStatus{0xabadbeef};

    /*需要使用C样式字符串来进行编译时Hash计算*/
    constexpr char rpcName[] = "BaseRpc";
    constexpr char srvName[] = "BaseService";

    /*创建服务*/
    auto service = Runtime::get()->buildService<>(srvName);
    assert(service);

    /*创建代理*/
    auto proxy = Runtime::get()->buildProxy<>(appName, srvName);
    assert(proxy);

    /*创建以透明数据回调处理请求的RPC服务*/
    auto id = service->buildRpcServer<>(rpcName,
        [&, srvWPtr = ServiceWPtr(service)] /*必须要弱引用*/
            (const RpcServer::ReqId reqId, PackBuffer & buff)
    {
        /*解包，处理*/
        std::decay_t<decltype(CarIGN)>   carIGN{0};
        std::decay_t<decltype(CarACC)>   carACC{0};
        std::decay_t<decltype(CarSpeed)> carSpeed{0.0f};

        auto size = buff.unpack(carIGN, carACC, carSpeed);
        assert(size == (sizeof(CarACC) + sizeof(carIGN) + sizeof(CarSpeed)));

        assert(carIGN == CarIGN);
        assert(carACC == CarACC);
        assert(carSpeed == CarSpeed);

        /*响应*/
        auto rc = srvWPtr.lock()->response<fnv1a_64(rpcName)>(reqId, CarStatus);
        assert(rc);
    });

    /*创建一个指定超时请求的RPC客户端*/
    id = proxy->buildRpcClient(rpcName, 100);
    assert(id);

    bool running = true;
    proxy->getProxyStatusEvent().subscribe([&](StatusValue status) {
        /*循环引用，但是 可以 unsubscribe() 手动解除*/
        if (status == StatusValue::APP_ONLINE) {
            /*上线发送请求*/
            auto rc = proxy->requestAsync<fnv1a_64(rpcName)> (
                [&](const decltype(CarStatus) & carStatus)
            {
                /*响应结果*/
                running = false;
                assert(carStatus == CarStatus);
            },
                /*请求参数*/
                CarIGN, CarACC, CarSpeed
            );
            assert(rc == true);
        }
    });

    auto timer = Runtime::get()->loop()->addTimer(700, [&](int32_t) {
        assert(running == false);
    });

    do {
        Runtime::get()->loop()->run();
    } while (running);

    Runtime::get()->loop()->removeTimer(timer);

    /*手动解除上线回调中的智能指针的引用*/
    proxy->getProxyStatusEvent().unsubscribe();

    /*离线时可能再次发送请求，等待请求数据过期*/
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

    if (1)
{
    std::cout << "==== 项目实际框架代码展示 ====" << std::endl;

    const std::string srvName = "BaseService";
    /*绝大多数发布者和服务是一体的，基本上创建服务就创建发布者，所以以嵌入的方式来测试*/
    struct CarStatus : public Service {
        CarStatus(const std::string &name) : Service(name) {
            /*构造*/
            auto id = this->buildPublisher<>("Speed", [=](IpcKey key) {
                /*未开始事件循环前初始化，该调用是安全的*/
                this->onSpeedSubscription(key);
            });
            /*单独以成员存储发布者对象，加速操作*/
            SpeedPublisher = this->topicPublisher(id);
            assert(SpeedPublisher);
        }
        /*必须要在Runtime的事件循环停止后才能正确析构*/
        ~CarStatus(void) {

        }

        /*
         * 在全局的类型定义中，还可以定义如下变参发布
        class XXX {
            struct Directed {
                explicit Directed(IpcKey key) : key_(key) {}
                virtual ~Directed() {}
                IpcKey key_;
            };
            template<class... Args>
            bool fireSpeedEvent(Args &&...args) {
                return SpeedPublisher->publish(std::forward<Args>(args)...);
            }
            template<class... Args>
            bool fireSpeedEvent(Directed && target, Args &&...args) {
                return SpeedPublisher->publishTo(target.key_, std::forward<Args>(args)...);
            }
        };
        调用定向回复
        XXX->fireSpeedEvent(Directed{Key}, ...)
         */
        /*广播*/
        bool fireSpeedEvent(float value) {
            return SpeedPublisher->publish(value);
        }
        /*定向*/
        bool fireSpeedEvent(IpcKey key, float value) {
            return SpeedPublisher->publishTo(key, value);
        }
        virtual void onSpeedSubscription(IpcKey key) {
            /*你的响应订阅请求的代码*/
            fireSpeedEvent(key, 119.5f);
        }
        /*速度发布*/
        PublisherPtr SpeedPublisher;

        /*上述一套可以定义为宏，简化构造*/

        /*电源状态发布，参考速度*/
        /*灯状态发布，参考速度*/
        /*其他*/
    };

    /*构造服务*/
    auto carStatus = Runtime::get()->buildService<CarStatus>("CarStatus");

    /*
     * 开启你的状态机，比如定时器、信号、IO事件，来发布数据
     * carStatus->fireSpeedEvent(250.0f);
     */
    
    /*
     * 开始循环，必须的，不然不能与代理握手
    do {
        Runtime::get()->loop()->run();
    } while (1);
     */
}

    return EXIT_SUCCESS;
}