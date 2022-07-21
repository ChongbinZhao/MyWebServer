#ifndef LST_TIMER
#define LST_TIMER

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include "../log/log.h"

//定时器
class util_timer;

//定时器类内部的具体信息
struct client_data
{
    sockaddr_in address;
    int sockfd;
    util_timer *timer;
};


//定时器类
class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    //超时时间
    time_t expire;
    
    //回调函数，用来删除非活动socket上的注册事件并关闭
    void (* cb_func)(client_data *);

    //定时器（含有连接资源）
    client_data *user_data;

    //前向和后继定时器
    util_timer *prev;
    util_timer *next;
};


//定时器容器设计:将每个连接的定时器按照超时时间升序排列;执行定时任务时将到期的定时器从链表删除
//添加定时器时间复杂度为O(n)，删除定时器时间复杂度为O(1)
class sort_timer_lst
{
public:
    sort_timer_lst();
    ~sort_timer_lst();

    //将目标定时器按升序添加到链表中
    void add_timer(util_timer *timer);
    
    //调整定时器在链表中的位置
    void adjust_timer(util_timer *timer);
    
    //将超时的定时器从链表中删除
    void del_timer(util_timer *timer);
    
    //定时任务处理函数，SIGALRM触发时就调用这个函数
    void tick();

private:
    //私有成员，被公有成员add_timer和adjust_time调用
    //主要用于调整链表内部结点
    void add_timer(util_timer *timer, util_timer *lst_head);

    util_timer *head;
    util_timer *tail;
};


class Utils
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    //对文件描述符设置非阻塞
    int setnonblocking(int fd);

    //将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    //信号处理函数
    static void sig_handler(int sig);

    //设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    //定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    //将错误信息发给客户端
    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd;//管道套接字
    sort_timer_lst m_timer_lst;//定时器列表
    static int u_epollfd;//epoll描述符
    int m_TIMESLOT;//超时时间间隔：到期时间 - 现在的时间 = 若干个m_TIMESLOT
};


void cb_func(client_data *user_data);

#endif
