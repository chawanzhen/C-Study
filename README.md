# 高性能网络编程学习笔记

整理了这段时间写的三个小模块：TCP 粘包处理、epoll 用法、异步双缓冲日志。代码分别在 `frame_parser.cpp`、`epoll.cpp`、`AsyncLogger.h`。

---

## 一、TCP 粘包处理

> 对应代码：`frame_parser.cpp`

### 1.1 问题在哪

TCP 是字节流协议，一次 `send` 不一定对应一次 `recv`。多条消息可能粘在一起发过来（粘包），一条消息也可能被拆成几段收到（拆包）。接收端得自己按协议格式把完整消息切出来。

### 1.2 怎么解决

用了三个办法：

- **状态机逐字节解析**：按 `魔数 → 长度 → 负载` 三个状态推进，每次只消费确定数量的字节，粘包拆包都能处理
- **魔数同步滑窗**：缓冲区头部对不上魔数时，逐字节丢头部字节往后滑，直到对齐
- **单包长度上限**：长度字段超过 10 MB 直接丢弃并复位，防止有人拿超长包打 DoS

### 1.3 帧格式

```
| 魔数 magic (4B) | 包体长度 length (4B) | 负载 payload (length B) |
```

- 魔数：`"BHNP"`，4 字节，标识帧起始，用来同步错位
- 长度：4 字节，网络字节序（大端），接收后用 `ntohl` 转主机序
- 负载：`length` 字节的业务数据（比如 protobuf 序列化后的数据）
- 上限：单包最大 10 MB，超出算非法帧

### 1.4 状态机流转

```
ReadMagic ──(头部4字节 == 魔数)──▶ ReadLength ──(读到4字节长度)──▶ ReadObject
    ▲                                                                 │
    └────────────────────(读完完整负载，返回数据后 Reset)──────────────┘
```

三个状态各管一段：

| 状态 | 做什么 |
| --- | --- |
| `ReadMagic` | 检查缓冲区头部 4 字节是不是魔数；不对就逐字节删头部，滑窗继续对齐 |
| `ReadLength` | 读 4 字节长度，`ntohl` 转主机序；超过 10 MB 直接 `Reset` 丢弃 |
| `ReadObject` | 缓冲区里长度不够就等后续数据；凑齐了取出完整负载返回，然后 `Reset` |

`toParser` 把收到的字节压进内部 `buffer`，循环按状态机消费。数据不够时返回空，等下一包数据补齐再继续——拆包问题就这么绕过去了。

---

## 二、epoll 详解

> 对应代码：`epoll.cpp`

### 2.1 epoll 是什么

epoll 是 Linux 上处理海量并发连接的 I/O 多路复用机制。跟 `select` / `poll` 比，它不用每次调用都遍历全部 fd，而是让内核维护被监听 fd 的状态，效率跟连接数没关系。从内核底层入手理解会比较直观。

### 2.2 底层结构

epoll 分内核态和用户态两部分，用户态通过 3 个系统调用跟内核交互：

```
用户态  ──▶  epoll_create  创建 epoll 实例
        ──▶  epoll_ctl     增 / 删 / 改 监听事件
        ──▶  epoll_wait    等待就绪事件
```

内核里的 `struct event_poll` 对象维护着 3 个核心数据结构：

| 内核结构 | 说明 |
| --- | --- |
| 红黑树 | 存所有被监控的 fd 及其事件，增删改查 O(log n) |
| 就绪队列 | 存已发生但还没被用户态取走的事件项 |
| 等待队列 | 没事件时进程挂在这里睡眠，等内核唤醒 |

### 2.3 三个系统调用

#### ① epoll_create —— 创建实例

```c
#include <sys/epoll.h>
int epfd = epoll_create(0);   // size 参数已基本失效，传 >= 0 就行
```

内核原型：

```c
SYSCALL_DEFINE1(epoll_create, int, size) {
    if (size < 0) return -EINVAL;   // size 小于 0 返回错误
    return do_epoll_create(0);      // size 实际不参与后续逻辑
}
```

传进来的 `size` 基本不起作用。调用后内核创建 `struct event_poll` 对象，向用户态返回对应的文件描述符 `epfd`。

#### ② epoll_ctl —— 管理监听事件

```c
epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
```

函数原型：

```c
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev);
```

| 参数 | 说明 |
| --- | --- |
| `epfd` | `epoll_create` 返回的 epoll 实例文件描述符 |
| `op` | 对目标 fd 执行的操作（见下表） |
| `fd` | 目标文件描述符（socket 等） |
| `ev` | 指向 `struct epoll_event` 的指针，指定监听的事件类型 |

`op` 支持三种操作：

| 操作 | 含义 |
| --- | --- |
| `EPOLL_CTL_ADD` | 添加一个新的文件描述符 |
| `EPOLL_CTL_MOD` | 修改已存在 fd 的事件类型 |
| `EPOLL_CTL_DEL` | 删除一个文件描述符 |

成功返回 0，失败返回 -1 并设置 `errno`。

`struct epoll_event` 结构体：

```c
struct epoll_event {
    uint32_t     events;   // 监听的事件类型
    epoll_data_t data;     // 用户自定义数据（通常存 fd 或上下文指针）
};
```

`events` 常用取值：

| 事件 | 含义 |
| --- | --- |
| `EPOLLIN` | socket 可读 |
| `EPOLLOUT` | socket 可写 |
| `EPOLLERR` | socket 发生错误 |
| `EPOLLRDHUP` | 对方关闭连接或半关闭 |
| `EPOLLET` | 设为边缘触发（默认是水平触发） |

`struct epoll_data` 联合体：

```c
typedef union epoll_data {
    void    *ptr;
    int      fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;
```

调用 `epoll_ctl` 添加 fd（比如标准输入 0）后，内核在红黑树里插入一个键值对：`key = fd`，`value = {events, data}`，同时挂入等待队列。

#### ③ epoll_wait —— 等待就绪事件

```c
epoll_wait(epfd, events, 10, 1000);
```

函数原型：

```c
int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
```

| 参数 | 说明 |
| --- | --- |
| `epfd` | epoll 实例文件描述符 |
| `events` | 就绪事件数组，用来接收返回的事件 |
| `maxevents` | `events` 数组大小，一次最多返回多少个事件 |
| `timeout` | 超时时间：`-1` 无限等，`0` 立即返回，单位毫秒 |

返回值：

| 返回值 | 含义 |
| --- | --- |
| 小于 0 | 出错 |
| 等于 0 | 超时，没有就绪事件 |
| 大于 0 | 就绪事件个数 |

有就绪事件就直接返回；没有的话，`epoll_wait` 所在线程会睡眠（挂到等待队列），不占 CPU，等内核唤醒。

### 2.4 等待队列 vs 就绪队列

| 对比项 | 等待队列 | 就绪队列 |
| --- | --- | --- |
| 属于谁 | 被监控的文件描述符 | epoll 实例 |
| 作用 | 进程调 `epoll_wait` 且无就绪事件时睡眠，把 `task_struct` 挂到该 fd 的等待队列上，等内核唤醒 | 存已发生但还没被用户态取走的事件项 |

### 2.5 实例：标准输入 + 边缘触发

```c
#include <iostream>
#include <sys/epoll.h>
#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

int main()
{
    int epfd = epoll_create1(0);
    struct epoll_event ev, events[10];

    ev.events = EPOLLIN | EPOLLET;   // 边缘触发（水平触发只写 EPOLLIN）
    ev.data.fd = 0;                  // 标准输入
    epoll_ctl(epfd, EPOLL_CTL_ADD, 0, &ev);

    char buf[10];
    while (1) {
        int nfds = epoll_wait(epfd, events, 10, -1);
        if (nfds == -1) return -1;

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            if (fd == 0) {                       // 标准输入触发
                ssize_t n = read(0, buf, 1);
                printf("触发，读取了 %d 字节 : '%c'\n", n, buf[0]);
            }
        }
    }
    return 0;
}
```

运行后输入 `hello` 并回车，边缘触发（EPOLLET）只触发一次：

```text
hello
触发，读取了 1 字节 : 'h'
```

改成水平触发（EPOLLIN）后，内核会持续上报直到数据读完：

```text
hello
触发，读取了 1 字节 : 'h'
触发，读取了 1 字节 : 'e'
触发，读取了 1 字节 : 'l'
触发，读取了 1 字节 : 'l'
触发，读取了 1 字节 : 'o'
触发，读取了 1 字节 : '
'
```

水平触发（LT）：只要 fd 上有数据就持续上报。边缘触发（ET）：只在状态变化（从无到有）时上报一次，所以 ET 模式下得一次性把数据读完——通常配合非阻塞 fd + while 循环 `recv` 直到 `EAGAIN`。

### 2.6 综合实例：echo 服务器 + 定时器

`epoll.cpp` 是个综合 demo：非阻塞监听 socket + `timerfd` 定时器 + 多客户端 echo 回显，全部由同一个 epoll 事件循环驱动。

```c
// 关键流程
int epfd = epoll_create1(0);                    // 1. 创建 epoll 实例
int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);  // 2. 创建 3s 周期定时器

struct epoll_event ev;
ev.events = EPOLLIN | EPOLLET;                  // 3. 监听 socket：边缘触发
ev.data.fd = listen_fd;
epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

ev.events = EPOLLIN | EPOLLET;                  // 4. 定时器 fd 一并注册
ev.data.fd = timer_fd;
epoll_ctl(epfd, EPOLL_CTL_ADD, timer_fd, &ev);

while (1) {
    int epsize = epoll_wait(epfd, events, MAXEVENTS, 10000);  // 5. 10s 超时等待
    if (epsize == 0) continue;                  // 超时则继续
    if (epsize == -1) return -1;                // 出错

    for (int i = 0; i < epsize; i++) {
        int getfd = events[i].data.fd;

        if (getfd == listen_fd) {               // 6. 新连接：accept 循环（ET 需读满）
            while (1) {
                int client_fd = accept(listen_fd, NULL, NULL);
                if (client_fd == -1) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // 读空
                    else { perror("accept"); break; }
                }
                set_nonblock(client_fd);        // 7. 客户端 fd 设为非阻塞并注册
                ev.data.fd = client_fd;
                ev.events = EPOLLIN | EPOLLET;
                epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);
            }
        } else if (getfd == timer_fd) {         // 8. 定时器触发
            uint64_t exp;
            read(timer_fd, &exp, sizeof(uint64_t));
            printf("定时器触发了 %llu 次\n", exp);
        } else {                                 // 9. 客户端数据：recv 循环 + echo 回显
            while (1) {
                ssize_t size_n = recv(getfd, temp, sizeof(temp), 0);
                if (size_n > 0)        buffer.append(temp, size_n);
                else if (size_n == 0) { close(getfd); break; }          // 连接关闭
                else {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break; // 读空
                    else { close(getfd); break; }                        // 出错
                }
            }
            if (getfd) { send(getfd, buffer.c_str(), buffer.size(), 0); buffer.clear(); }
        }
    }
}
```

几个需要注意的点：

- **ET + 非阻塞**：边缘触发下，`accept` / `recv` 都要用 while 循环读到 `EAGAIN / EWOULDBLOCK` 才算读空，否则会漏事件
- **`timerfd` 跟网络 fd 统一调度**：定时器也能像 socket 一样注册进 epoll，用同一套事件循环驱动
- **非阻塞 fd**：用 `fcntl` 设置 `O_NONBLOCK`，配合 ET 循环读取

---

## 三、异步双缓冲日志系统

> 对应代码：`AsyncLogger.h`

### 3.1 思路

日志写磁盘不能阻塞业务线程。用异步 + 双缓冲的模式：

- 业务线程只把日志消息追加到内存缓冲区，不碰磁盘 IO
- 后台线程专门负责把缓冲区里的日志落盘（写文件 + flush）
- 两个 1 MB 缓冲区轮流用，避免单缓冲在写入和落盘之间互相覆盖

```
业务线程 ──append──▶ current_buffer ──swap──▶ write_buffer ──▶ log.log
                        │                             后台线程
                    next_buffer ◀────────备用────────┘
```

### 3.2 双缓冲结构

| 成员 | 作用 |
| --- | --- |
| `current_buffer` | 业务线程当前写入的缓冲区，预留 1 MB |
| `next_buffer` | 备用缓冲区，预留 1 MB，`current` 写满或快满时承接新数据 |
| `write_buffer` | 后台线程待落盘的中转缓冲 |
| `_mutex` | 保护缓冲区的互斥锁 |
| `cv` | 条件变量，唤醒后台线程落盘 |

### 3.3 写入流程（`append`）

```cpp
void append(const std::string& msg) {
    std::unique_lock<std::mutex> lock(_mutex);

    // ① current 已接近写满（>= 900KB），先通知后台线程准备落盘
    if (current_buffer.size() >= 1024 * 900) {
        cv.notify_one();
    }

    // ② 当前缓冲 + 新消息将超过 1MB：新消息暂存到 next_buffer 并通知
    if (current_buffer.size() + msg.size() >= 1024 * 1024) {
        next_buffer.insert(next_buffer.end(), msg.begin(), msg.end());
        next_buffer.push_back('\n');
        cv.notify_one();
        return;
    }

    // ③ 正常情况：直接写入 current_buffer
    current_buffer.insert(current_buffer.end(), msg.begin(), msg.end());
    current_buffer.push_back('\n');
}
```

策略很简单：

- `current_buffer` 用到 900 KB 就通知后台线程落盘，别等写满了再喊
- 如果 `current` 放不下新消息（`current + msg >= 1MB`），就把消息塞进 `next_buffer`，不丢日志也不阻塞
- 每次追加自动补 `'\n'`

### 3.4 落盘流程（`work_thread`）

```cpp
void work_thread() {
    std::fstream file("log.log", std::ios::app);   // 追加模式打开日志文件
    if (!file) { perror("文件打开失败\n"); return; }

    std::vector<char> write_buffer;                // 待写盘的缓冲
    while (_running) {
        {
            std::unique_lock<std::mutex> lock(_mutex);

            // ① current 为空：等待新日志，最多等 3 秒（超时兜底）
            if (current_buffer.empty()) {
                cv.wait_for(lock, std::chrono::seconds(3), [this]() {
                    return !_running || !current_buffer.empty();
                });
            }

            // ② 退出条件：停止运行且所有缓冲已清空
            if (!_running && current_buffer.empty() && next_buffer.empty()) break;

            // ③ 交换缓冲：current 与 next 内容都归入 write_buffer
            if (next_buffer.empty()) {             // 常规情况：直接交换 current
                next_buffer.swap(current_buffer);
                write_buffer.swap(next_buffer);
            } else {                                // 双缓冲都有数据：合并
                write_buffer.swap(current_buffer);
                write_buffer.insert(write_buffer.end(), next_buffer.begin(), next_buffer.end());
                next_buffer.clear();
            }
        }

        // ④ 锁外写盘：避免持锁做 IO
        if (!write_buffer.empty()) {
            file.write(write_buffer.data(), write_buffer.size());
            file.flush();
            write_buffer.clear();
        }
    }
    file.close();
}
```

### 3.5 线程同步与生命周期

| 场景 | 怎么处理 |
| --- | --- |
| 唤醒写盘 | `append` 端写入后调 `cv.notify_one()` |
| 实时性兜底 | 后台线程 `cv.wait_for(..., 3s)`，即使没人通知也每 3 秒检查一次落盘 |
| 优雅退出 | 析构时置 `_running = false` 并 `notify_one`，唤醒后台线程把剩余缓冲写完再 `join` |
| 锁外 IO | 缓冲交换在锁内完成，文件写入在锁外执行，减少持锁时间 |

### 3.6 几个实际好处

- 业务线程只做内存拷贝，磁盘 IO 全丢给后台线程，不阻塞业务
- 双缓冲加锁保护，写入和落盘之间不会互相覆盖，日志不丢
- 条件变量即时唤醒加 3 秒超时兜底，吞吐和延迟都能兼顾
- 析构先 `notify` 再 `join`，进程退出前日志能全部落盘

---
