#include "lst_timer.h"
#include "../http/http_conn.h"

//解析函数
sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}


//析构函数
sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}


//添加定时器 
void sort_timer_lst::add_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    if (!head)
    {
        head = tail = timer;
        return;
    }

    //如果timer的超时时间比head的时间小，将timer和head互换
    if (timer->expire < head->expire)
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }

    //否则调用私有成员（名字也叫add_timer不过参数不同），调整内部结点
    add_timer(timer, head);
}


//调整定时器位置
void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    util_timer *tmp = timer->next;

    //被调整的定时器在链表尾部 || 超时值仍然小于下一个定时器超时值，则不调整
    if (!tmp || (timer->expire < tmp->expire))
    {
        return;
    }

    //被调整定时器是链表头结点，将定时器取出，重新插入
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    }

    //被调整定时器在内部，将定时器取出，重新插入
    else
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}


//删除定时器
void sort_timer_lst::del_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    if ((timer == head) && (timer == tail))
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    if (timer == tail)
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}


//定时任务处理函数（由SIGALRM信号来驱动），处理链表中到期的定时器
void sort_timer_lst::tick()
{
    if (!head)
    {
        return;
    }
    
    //获取当前时间
    time_t cur = time(NULL);

    //定位到链表的头节点
    util_timer *tmp = head;

    //遍历定时器链表
    while (tmp)
    {
        //一个也没有到期
        if (cur < tmp->expire)
        {
            break;
        }

        //若当前定时器到期，则调用回调函数，执行相应操作
        //删除epoll注册事件；关闭文件描述符；连接数减1
        tmp->cb_func(tmp->user_data);

        //将处理后的定时器从链表容器中删除，并重置头结点
        head = tmp->next;
        if (head)
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}


//timer超时时间比head大的情况下，遍历查询直至timer插入
void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)
{
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;
    while (tmp)
    {
        if (timer->expire < tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    if (!tmp)
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}


//初始化:TIMESLOT = timeslot
void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}


//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}


//信号处理函数：将信号的具体值通过管道发送给主进程
void Utils::sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno：保护现场
    int save_errno = errno;

    //sig是系统产生的信号
    int msg = sig;

    //将信号值从管道写端写入，传输字符类型，而非整型
    send(u_pipefd[1], (char *)&msg, 1, 0);

    //还原现场
    errno = save_errno;
}


//设置信号函数，项目中仅关注SIGPIPE、SIGTERM和SIGALRM两个信号
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    //创建sigaction结构体变量
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));

    //信号处理函数中仅仅发送信号值，不做对应逻辑处理
    sa.sa_handler = handler;

    //SA_RESTART，使被信号打断的系统调用自动重新发起
    if (restart)
        sa.sa_flags |= SA_RESTART;

    //用来将参数set信号集初始化，然后把所有的信号加入到此信号集里;sa_mask指要屏蔽的信号
    sigfillset(&sa.sa_mask);
    
    //sigaction()返回值0表示成功，-1表示有错误发生
    assert(sigaction(sig, &sa, NULL) != -1);
}


//定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler()
{
    //定时任务处理函数
    m_timer_lst.tick();

    //设置信号SIGALRM在经过参数m_TIMESLOT秒数后发送给目前的进程
    alarm(m_TIMESLOT);
}


void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}


int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;
class Utils;

//定时器回调函数
void cb_func(client_data *user_data)
{
    //删除非活动连接在socket上的注册事件
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);

    //关闭文件描述符
    close(user_data->sockfd);

    //连接数减1
    http_conn::m_user_count--;
}
