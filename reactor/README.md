# 自定义Reactor实现：事件驱动编程的实践指南

今天我们来深入探讨一个基于C++的自定义Reactor实现。这份源码来自于提供的`reactor.hpp`、`test_benchmark.cpp`和`test_perf.cpp`文件。它是一个轻量级的事件循环库，支持I/O多路复用、定时器、信号处理和异步任务，特别适合嵌入式系统或高性能网络应用。我们将采用教学风格，从理论基础入手，逐步展开到代码实践，最后通过基准测试和性能案例分析其利弊。

这份Reactor使用`poll`作为底层多路复用机制（而非`epoll`），这在连接数较少（如嵌入式场景下不超过32个socket）时性能与`epoll`相当，但实现更简单跨平台。源码中融入了信号安全处理、定时器补偿等高级特性，让我们一步步拆解。

### 一、理论基础：Reactor模式与事件驱动

#### 1. Reactor模式概述
Reactor模式是一种事件驱动架构，用于处理多路I/O、定时器和信号等事件。它源于网络编程（如Nginx、Redis），核心是**单一线程/进程监听多个事件源**，当事件就绪时触发回调，避免了多线程的锁竞争和上下文切换。

- **关键组件**：
  - **事件循环（Event Loop）**：不断轮询事件（如`poll`、`epoll_wait`），处理就绪事件。
  - **I/O事件**：读/写/异常，使用文件描述符（fd）注册。
  - **定时器**：基于绝对时间，支持重置和移除。
  - **信号处理**：安全捕获信号（如SIGINT），转化为事件。
  - **异步任务**：从其他线程推送任务到主循环执行，确保线程安全。

- **与多路复用关系**：Reactor本质上是I/O多路复用的应用层封装。相比原始`select/poll/epoll`，它添加了定时器和异步机制，提高了开发效率。

#### 2. 为什么需要自定义Reactor？
标准库（如libevent、libuv）强大但庞大，自定义实现可以：
- 优化特定场景（如嵌入式，连接少）。
- 学习事件驱动的核心：锁管理、ABA问题防范、信号原子性。
- 避免依赖外部库。

**利弊预览**：
- **利**：轻量、高效、易调试。
- **弊**：单线程模型下，CPU密集任务会阻塞循环；不支持水平触发（LT）模式，仅边缘触发（ET）类似。

### 二、实现原理：剖析reactor.hpp

Reactor类是核心，内部使用`std::mutex`保护数据结构，确保线程安全。让我们循序渐进分析关键部分。

#### 1. 核心数据结构
- **EventMap**：`std::unordered_map<int32_t, ioTask>`，存储fd到事件的映射。ioTask包含事件掩码（ev_如POLLIN）、版本号（version_防ABA）和回调任务。
- **TimerMap**：`std::multimap<uint64_t, timerTask>`，多键映射，按绝对时间（纳秒）排序定时器。支持O(log N)插入/删除。
- **SignalMap**：`std::unordered_map<uint8_t, task>`，信号号到回调。
- **AsyncArray**：优先级异步队列数组（0最高），支持多优先级任务推送。
- **内部PIPE**：fd_[2]，用于notify唤醒poll（类似eventfd）。

#### 2. 初始化与销毁
```cpp
explicit reactor(void);  // 创建PIPE，注册读事件用于flush通知
~reactor(void) noexcept; // 清理所有事件，关闭PIPE
```
- 初始化时创建非阻塞PIPE，注册fd_[0]的POLLIN事件到flush函数（读取通知）。
- 全局信号深度（signal_depth_）确保不在信号处理中创建Reactor，避免死锁。

#### 3. I/O事件操作
```cpp
bool addEvent(int32_t fd, int32_t ev, task && func);  // 添加fd事件
int32_t modEvent(int32_t fd, std::function<int32_t(int32_t)> &&);  // 修改事件掩码
bool removeEvent(int32_t fd) noexcept;  // 移除fd
```
- 使用mutex_锁保护EventMap。
- 修改后若在polling_中，notify唤醒以更新poll数组。
- **原理**：prepareEvent构建pollfd数组，eatEvent处理就绪事件。写事件（POLLOUT）单次触发，适合“先试写，失败再监听”模式。

#### 4. 定时器操作
```cpp
timerId addTimer(uint32_t expire, task && func);  // 相对时间添加
bool resetTimer(timerId & id, uint32_t expire, task && func = nullptr);  // 重置
bool removeTimer(timerId &) noexcept;  // 移除
```
- 使用绝对时间（sys_clock基于steady_clock，避免系统时间跳变）。
- resetTimer支持回调调整其他数据（如interval_）。
- eatTimer在循环中处理到期定时器，支持批量（最多32个/批）以防饿死其他事件。

扩展：restartTimer和intervalTimer模板类，提供重启和间隔补偿。
- restartTimer：简单重启，无补偿（负载高时漂移）。
- intervalTimer：补偿机制，确保固定间隔（如计算平均速度时必须）。

#### 5. 信号与异步
```cpp
void addSignal(int32_t signo, task && func);  // 安装信号，全进程阻塞
void addAsync(task && func, bool wakeup = true, uint8_t prior = 0);  // 推送异步任务
```
- 信号：sigprocmask阻塞，setupSignal安装handler，转为notify写入PIPE。
- 异步：多优先级队列，eatAsync按优先级执行（最高先）。

#### 6. 事件循环
```cpp
int32_t run(int32_t timeout = -1) noexcept;  // 主循环
```
- prepareEvent构建poll数组。
- sigprocmask临时解锁信号。
- poll等待，处理顺序：信号 > 异步 > I/O > 定时器 > 同步等待。
- 返回流逝时间（ms），用于补偿。

**实现亮点**：
- notify使用原子nr_nty_防重复写。
- 版本号防ABA：eatEvent中++version_，执行后检查。
- 信号安全：全局锁all_mutex_保护多Reactor。

**潜在弊端**：poll的O(N)扫描在fd多时低效（源码限32优化）；无水平触发，需手动重置事件。

### 三、实践：基准测试与代码示例

让我们通过`test_benchmark.cpp`运行基准测试，验证功能。假设在Release模式编译（g++ -O3），以下是典型输出和解释。

#### 1. I/O基准测试
```cpp
// 测试PIPE的可读/写事件
int32_t pfd[2];
qevent::utils::pipe2(pfd, O_NONBLOCK);
loop.addEvent(pfd[1], POLLOUT, [&](uint32_t){ wa = true; });
loop.run(10);  // 立即触发写事件
```
- **输出示例**：
  ```
  IO基准测试
  可写性测试...
  可写事件单次触发...
  可读性测试...
  写满PIPE : .... (直到EAGAIN)
  不可写但可读...
  读空PIPE : .... (读取数据)
  ```
- **说明**：验证写事件单次（利：避免无限循环；弊：需手动重启监听）。读事件在数据可用时触发，模拟网络socket。

#### 2. 定时器基准测试
```cpp
tid = loop.addTimer(1, [&](int32_t diff){ /*...*/ });
loop.resetTimer(tid, 10);
loop.run(5);  // 不触发
loop.run(5);  // 触发
```
- **输出示例**：
  ```
  定时器基准测试
  第一次循环不触发 [5]...
  循环流逝时间 [5]...
  [*] 精准不足 [10]  (期望触发和实际触发的单调时间有较大的差值)
  ```
- **说明**：重置后精确触发（利：O(log N)操作高效；弊：高负载下poll精度不足，可能需多次run）。

#### 3. 间隔定时器测试
```cpp
qevent::intervalTimer<qevent::reactor*> timer2(&loop);
timer2.launch(10, [&](int32_t){ count++; });
std::this_thread::sleep_for(std::chrono::milliseconds(50));  // 模拟负载
loop.addTimer(100, /*破循环*/);
do { loop.run(); } while (!broken);
```
- **输出示例**：
  ```
  间隔定时器基准测试
  间隔定时器受CPU负载影响 ...
  [*] 定时器触发 [1] 到 [5]  (无补偿，触发少)
  补偿间隔定时器不受CPU负载影响 ...
  [*] 定时器触发 [1] 到 [10]  (补偿后固定10次)
  ```
- **利弊**：补偿版适合实时计算（利：准确）；但计算开销稍高（弊：在极低端嵌入式可能多余）。

#### 4. 信号与异步测试
- 信号：addSignal(SIGINT)，kill自身，run捕获。
- 异步：多线程addAsync，run执行。
- **输出**：信号需2次run触发（PIPE机制）；异步确保顺序（利：线程安全；弊：高并发下锁竞争）。

### 四、真实世界案例：性能测试分析

从`test_perf.cpp`看性能案例。

#### 1. 异步任务性能
- 模拟2线程推送1<<20任务，每个任务计算Fibonacci。
- **结果示例**：总时间~几秒，平均ns级（利：单循环高效处理海量任务；弊：多线程推送需锁，极限下瓶颈）。

#### 2. 定时器性能
- 插入1<<16随机定时器。
- **结果**：平均误差<1ms（利：multimap高效；弊：插入多时O(log N)累积）。

#### 3. I/O性能（TCP服务器）
- 监听8000端口，iperf压测（e.g., iperf -c localhost -p 8000 -P 10）。
- **结果**：处理多连接读事件（利：poll在小fd集高效；弊：fd>32需动态分配，O(N)扫描在万级连接低效）。

**案例利弊**：
- **利**：在嵌入式（如IoT设备）中，资源占用低，支持10k+ QPS。
- **弊**：CPU密集（如Fib计算）阻塞循环；建议offload到worker线程。相比Nginx的epoll，poll在高fd时慢，但源码简单易改。

### 五、总结与建议

这个自定义Reactor展示了事件驱动的精髓：从理论的多路复用到实践的线程安全实现。通过基准和性能测试，我们看到其在小规模场景下的高效（如精准定时器补偿），但在大连接数下需优化（如换epoll）。

**学习建议**：
1. 编译运行`test_benchmark.cpp`，观察输出理解基本功能。
2. 修改`test_perf.cpp`添加你的TCP逻辑，iperf压测比较。
3. 扩展：加epoll支持，提升高并发。
4. 阅读源码的signalHandle和eatEvent，体会安全设计。
