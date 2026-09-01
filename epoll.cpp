#include <iostream>
#include <sys/epoll.h>
#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <errno.h>
#include <sys/timerfd.h>

#define MAXEVENTS 10
#define PORT 8848

int set_nonblock(int fd){
    int flags = fcntl(fd,F_GETFL,0);
    return fcntl(fd,F_SETFL,flags | O_NONBLOCK);
}

int main()
{
    int listen_fd = socket(AF_INET,SOCK_STREAM,0);
    set_nonblock(listen_fd);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if(bind(listen_fd,(struct sockaddr*)&addr,sizeof(addr))==-1){
        perror("bind");
        close(listen_fd);
        return -1;
    }
    if(listen(listen_fd,0)==-1){
        perror("listen");
        close(listen_fd);
        return -1;
    }

    int epfd = epoll_create1(0);
    if(epfd==-1){
        perror("epoll_create1");
        close(listen_fd);
        return -1;
    }

    int timer_fd = timerfd_create(CLOCK_MONOTONIC,TFD_NONBLOCK);
    struct itimerspec ts;
    ts.it_value.tv_sec = 3;//3s后第一次触发;
    ts.it_interval.tv_sec = 3; //之后每隔3s触发一次
    timerfd_settime(timer_fd,0,&ts,NULL);

    struct epoll_event ev,events[10];
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    if(epoll_ctl(epfd,EPOLL_CTL_ADD,listen_fd,&ev)==-1){
        perror("epoll_ctl");
        close(listen_fd);
        return -1;
    }

    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = timer_fd;
    if(epoll_ctl(epfd,EPOLL_CTL_ADD,timer_fd,&ev) == -1){
        perror("epoll_ctr_timer_fd");
        close(listen_fd);
        close(timer_fd);
    }

    std::string buffer;
    char temp[1024];
    while(1){
        int epsize = epoll_wait(epfd,events,MAXEVENTS,10000);
        if(epsize==0) {//超时
            continue;
        }
        else if (epsize==-1) return -1;//错误

        for(int i=0;i<epsize;i++){
            int getfd = events[i].data.fd;
            if(getfd==listen_fd){//如果是监听的描述符
                while(1){
                    int client_fd = accept(listen_fd,NULL,NULL);
                    if(client_fd == -1){
                        if(errno == EAGAIN || errno == EWOULDBLOCK) break;
                        else{
                            perror("accept");
                            break;
                        }
                    }
                    set_nonblock(client_fd);

                    ev.data.fd = client_fd;
                    ev.events = EPOLLIN | EPOLLET;
                    if(epoll_ctl(epfd,EPOLL_CTL_ADD,client_fd,&ev)==-1){
                        perror("epoll_ctl_client_fd");
                        close(client_fd);
                    }
                }
            }else if(getfd == timer_fd){
                uint64_t exp;
                read(timer_fd,&exp,sizeof(uint64_t));
                printf("定时器触发了 %llu 次\n",exp);
            }else{
                while(1){
                    ssize_t size_n = recv(getfd,temp,sizeof(temp),0);
                    if(size_n>0){
                        buffer.append(temp,size_n);
                    }else if(size_n==0){//连接关闭
                        close(getfd);
                        break;
                    }else{
                        if(errno == EAGAIN || errno == EWOULDBLOCK) break;//读取万册
                        else {close(getfd);break;}//出错了
                    }
                }
                if(getfd) {send(getfd,buffer.c_str(),buffer.size(),0);buffer.clear();}
            }
        }
    }
    close(listen_fd);
    close(epfd);
    close(timer_fd);
    return 0;
}
