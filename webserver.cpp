#include "webserver.h"

WebServer::WebServer()
{
    //http_conn连接数组
    users = new http_conn[MAX_FD];

    //root文件夹路径
    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    //资源连接数组
    users_timer = new client_data[MAX_FD];
}


WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}


void WebServer::init(int port, string user, string passWord, string databaseName, int log_write, 
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
}


void WebServer::trig_mode()
{
    //LT + LT
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    //LT + ET
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    //ET + LT
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    //ET + ET
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}


void WebServer::log_write()
{
    //m_close_log==1表示不关闭日志功能
    if (0 == m_close_log)
    {
        //初始化日志
        if (1 == m_log_write)
            //异步写日志
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else
            //同步写日志
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}


void WebServer::sql_pool()
{
    //初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    //users是一个http类，initmysql_result用来初始化数据库读取表
    users->initmysql_result(m_connPool);
}


void WebServer::thread_pool()
{
    //线程池
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}


//服务器接收http请求（事件监听）
void WebServer::eventListen()
{
    //创建socket基础步骤
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);
    
    //优雅关闭连接；struct linger中包含l_onoff和l_linger两个参数
    if (0 == m_OPT_LINGER)
    {   
        //调用close()后socket立即关闭，不管数据有没有发完
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
        //调用close()后socket会延迟一时间才关闭，直到数据发完
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);
    
    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);
    
    //定时器的内容
    utils.init(TIMESLOT);
    
    //epoll创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    http_conn::m_epollfd = m_epollfd;

    //创建管道套接字（信号处理函数所触发的系统信号将通过管道发送给主循环）
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);

    //设置写端为非阻塞：如果缓冲区满了，则会阻塞，这时候会进一步增加信号处理函数的执行时间，为此，将其修改为非阻塞
    utils.setnonblocking(m_pipefd[1]);

    //设置管道读端为ET非阻塞
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);

    //传递给主循环的信号值，这里只关注SIGPIPE、SIGALRM和SIGTERM
    utils.addsig(SIGPIPE, SIG_IGN);//向一个已单向close的client调用两次write就会触发SIGPIPE
    utils.addsig(SIGALRM, utils.sig_handler, false);//每隔一段时间就发送SIGALRM
    utils.addsig(SIGTERM, utils.sig_handler, false);//SIGTERM是程序终止信号

    //每隔TIMESLOT时间触发SIGALRM信号
    alarm(TIMESLOT);

    //工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}


//此函数用于添加定时器
void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    //初始化http连接
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    //初始化client_data数据，其中users_timer是一个client_data数组
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;

    //初始化一个定时器对象
    util_timer *timer = new util_timer;

    //设置定时器对应的连接资源（client_data）
    timer->user_data = &users_timer[connfd];
    
    //设置回调函数
    timer->cb_func = cb_func;

    //获取当前时间戳
    time_t cur = time(NULL);

    //设置超时时间
    timer->expire = cur + 3 * TIMESLOT;

    //创建该http连接对应的定时器
    users_timer[connfd].timer = timer;

    //将该定时器添加到链表中
    utils.m_timer_lst.add_timer(timer);
}


//若有数据传输，则将定时器的超时时间往后延迟3个单位
//并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}


//移除相应的定时器
void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    //服务器端关闭连接（删除epoll事件、关闭文件描述符、连接数减1）
    timer->cb_func(&users_timer[sockfd]);
    
    //删除定时器
    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}


//处理客户端的数据
bool WebServer::dealclinetdata()
{
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    //LT模式：这个模式下一次最多连接一个客户端
    if (0 == m_LISTENTrigmode)
    {
        //connfd是客户端的文件描述符
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        //添加定时器
        timer(connfd, client_address);
    }

    //ET模式：对于连续的客户端连接请求，必须要一次性给处理完，直到出现eagain（使用非阻塞I/O）
    else
    {
        while (1)
        {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}


//主进程读取信号值
bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];

    //从管道读端读出信号值，成功返回字节数，失败返回-1
    //正常情况下，这里的ret返回值总是1，只有14或15两个ASCII码对应的字符
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1)
    {
        return false;
    }
    else if (ret == 0)
    {
        return false;
    }
    else
    {   
        for (int i = 0; i < ret; ++i)
        {   
            //信号本身是int类型，管道中转递的是ASCLL码表中整型数值对应的字符
            switch (signals[i])
            {
            case SIGALRM:
            {
                timeout = true;
                break;
            }
            case SIGTERM:
            {
                stop_server = true;
                break;
            }
            }
        }
    }
    return true;
}


//处理读事件
void WebServer::dealwithread(int sockfd)
{   
    util_timer *timer = users_timer[sockfd].timer;

    //reactor基于同步I/O
    //reactor可以理解为「事件来了操作系统通知应用进程，让应用进程来处理」
    //只负责监听文件描述符上是否有事件发生，有的话立即通知工作线程
    //读写数据、接受新连接及处理客户请求（比如解析报文和生成响应报文）均在工作线程中完成
    if (1 == m_actormodel)
    {   
        if (timer)
        {
            adjust_timer(timer);
        }
        
        //若监测到读事件，将该事件放入请求队列;0表示读事件
        m_pool->append(users + sockfd, 0);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {   
        //proactor基于异步I/O（但实际中异步I/O不成熟，项目中用同步I/O模拟异步I/O）
        //Proactor可以理解为「来了事件操作系统来处理，处理完再通知应用进程」
        //主线程和内核负责读写数据等I/O操作
        //工作线程仅负责业务逻辑，如处理GET和POST等客户请求
        if (users[sockfd].read_once())
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            //若监测到读事件，将该事件放入请求队列
            m_pool->append_p(users + sockfd);

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}


//处理写事件
void WebServer::dealwithwrite(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;
    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        m_pool->append(users + sockfd, 1);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].write())
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}


//运行
void WebServer::eventLoop()
{   
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server)
    {   
        //epoll_wait等待所监控文件描述符上有事件的产生; events是内核事件表
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }
        
        //对所有就绪事件进行处理（epoll_wait函数会把就绪事件排到events前面）
        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            //处理新到的客户连接（m_listenfd是服务端用来监听客户连接的socket）
            if (sockfd == m_listenfd)
            {
                bool flag = dealclinetdata();
                if (false == flag)
                    continue;
            }

            //处理异常事件：EPOLLRDHUP表示读关闭；EPOLLHUP表示读写都关闭；EPOLLERR文件描述符发生错误
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }

            //通过管道接收定时器信号（读事件）
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                //主线程仅是接收信号值，不包括处理逻辑
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }

            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                dealwithread(sockfd);
            }
            else if (events[i].events & EPOLLOUT)
            {
                dealwithwrite(sockfd);
            }
        }
        if (timeout)
        {
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}