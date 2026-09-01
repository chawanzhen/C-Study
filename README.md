本仓库记录学习总结



一：有关对Tcp粘包的处理：

&#x09;1.使用状态机，并实现逐字解析，有效应对粘包/拆包

&#x09;2.魔数同步滑窗，可以有效应对错位情况

&#x09;3.单包长度设限，防止恶意Dos攻击



二：有关对epoll的详解：

\##epoll介绍



epoll是为了解决海量的并发请求问题而发明的，从内核底层来理解更好；



\##epoll底层分析拆解



epoll分为内核态和用户态两个部分，而用户态用3个系统调用来与内核实现交互：

epoll\_create,epoll\_ctl,epoll\_wait

接下来分别讲解这三个系统调用：



首先是epoll\_create：

```c

\#include <sys/epoll.h>

int epfd = epoll\_create(0);

```



而它的原型函数是：

```c

SYSCALL\_DEFINE1(epoll\_create, int , size){

&#x09;if(size<0) return -EINVAL; //size小于0返回错误

&#x09;return do\_epoll\_create(0);

}

```

我们可以发现，传入的参数size基本上没有起到作用，我们在创建时输入大于等于0的数就好了；

而当我们调用这个函数后，内核会创建一个struct event\_poll对象，并返回文件描述符给用户态

而这个struct event\_poll储存着3个重要的内容：红黑树，就绪队列，等待队列；在接下来会介绍到；





接下来是epoll\_ctl：

```c

epoll\_ctl(epfd,EPOLL\_CTL\_ADD,0,\&ev);

```



而它的函数原型是:

```c

int epoll\_ctl(int epfd,int op,int fd,struct epoll\_event\* ev);

```

参数：

epfd:我们使用epoll\_crete创建的epoll实例文件描述符

op: 表示我们对目标文件描述符进行的操作，常用的有：

&#x09;EPOLL\_CTL\_ADD: 像epoll实例中添加一个新的文件描述符

&#x09;EPOLL\_CTL\_MOD: 修改已经存在文件描述符的事件类型

&#x09;EPOLL\_CTL\_DEL: 向epoll实例中删除一个文件描述符

fd: socket,我们想要操作的目标文件描述符，这里输入0是表示标准输入

event: 指向struct epoll\_event结构的指针，指定了需要监听的事件类型

成功返回0，失败返回-1并设置errno



其中，struct epoll\_event的结构体如下：

```c

struct epoll\_event{

&#x09;uint32\_t events;

&#x09;epoll\_data\_t data;

};

```

events: 指定要监听的事件类型，常用的有：

EPOLLIN: socket可读

EPOLLOUT: socket可写

EPOLLERR: socket发送错误

EPOLLRDHUP: 对方关闭连接或者半关闭连接

EPOLLET: 将socket设置成边缘触发



data: 用户自定义的数据，通常用于储存于文件描述符相关的上下文信息，获取就绪事件成功后，事件数组或记录data数据。

struct epoll\_data结构如下:

```c

typedefunion epoll\_data{

&#x09;void \*ptr;

&#x09;int fd;

&#x09;uint32\_t u32;

&#x09;uint64\_t u64;

}epoll\_data\_t;

```



而当我们调用这个函数后，比如我们向其中添加了一个标准输入文件描述符socket，内核会在红黑树中添加一个键值对：key: socket(比如标准输出0) value:{event, data} 其中event就储存了是边缘触发还是水平触发等等，data储存了我们的fd等等,并在等待队列中插入



最后是epoll\_wait:

```c

epoll\_wait(epfd,event\[10],10,1000);

```

它的函数原型是：

```c

int epoll\_ctl(int epfd, struct epoll\_event\* event ,int maxevents,int time\_out);

```

参数：

epfd: epoll文件描述符

events: epoll事件数组

maxevents: 指定events数组大小，即可以储存的最大事件数

timeout: 超过时间(-1: 表示无限等待, 0: 表示立刻返回,单位是毫秒)



返回值： 小于-表示出错；等于0表示超时；大于0表示获得的就绪事件个数



而当我们调用这个函数后，如果有就绪事件，则会获得就绪事件，如果没有，epoll线程就会陷入休眠，不占用cpu



接下来介绍一下等待队列和就绪队列：



等待队列：

储存位置： 属于被监控的文件描述符

作用：当进程调用epoll\_wait且当前没有就绪事件的时候，进程会主动“睡眠”，把自己(task\_struct)挂载到这个fd的等待队列上，等待内核唤醒



就绪队列：

储存位置：属于epoll实例

作用：储存已经发生且未被用户态取走的事件项



\##实例：

了解了基本的原理与底层构造，接下来用一个简单的实例来进一步理解：

```c

\#include <iostream>

\#include <sys/epoll.h>

\#include <string>

\#include <stdio.h>

\#include <stdlib.h>

\#include <fcntl.h>

\#include <unistd.h>



int main()

{

&#x20;   int epfd = epoll\_create1(0);

&#x20;   struct epoll\_event ev,event\[10];



&#x20;   ev.events = EPOLLIN | EPOLLET;//边缘触发模式(水平触发模式是 EPOLLIN）

&#x20;   ev.data.fd = 0;//标准输入

&#x20;   epoll\_ctl(epfd,EPOLL\_CTL\_ADD,0,\&ev);



&#x20;   char buf\[10];

&#x20;   while(1){

&#x20;       int nfds = epoll\_wait(epfd,event,10,-1);

&#x20;       if(nfds == -1) return -1;

&#x20;       for(int i=0;i<nfds;i++){

&#x20;           int fd = event\[i].data.fd;

&#x20;           if(fd==0){//如果等于接听的标准输入

&#x20;               ssize\_t n = read(0,buf,1);

&#x20;               printf("触发，读取了 %d 字节 : '%c'\\n",n,buf\[0]);

&#x20;           }

&#x20;       }

&#x20;   }

&#x20;   return 0;

}

```



当我们使用如上所示运行后输入hello并回车：

```输出结果

hello

触发，读取了 1 字节 : 'h'

```



而如果我们调为水平模式：

```输出结果

hello

触发，读取了 1 字节 : 'h'

触发，读取了 1 字节 : 'e'

触发，读取了 1 字节 : 'l'

触发，读取了 1 字节 : 'l'

触发，读取了 1 字节 : 'o'

触发，读取了 1 字节 : '

'

```



最后，我们简单的介绍一下水平触发(LT)与边缘触发的区别(ET):

水平模式：如果一次没有读完就绪队列中的内容，那下次epoll\_wait仍然能检测到就绪事件

编译模式：需要一次性读完就绪队列的内容，因为一次没有读完，那下次epoll\_wait无法检测到就绪事件，需要等待新的状态更新（即有新的输入或就绪队列加入）











