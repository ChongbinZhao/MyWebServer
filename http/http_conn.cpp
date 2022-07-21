#include "http_conn.h"
#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;


//载入数据库表，并将用户名和密码存入map中
void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;

    //从连接池中取出一个空闲连接并赋给mysql，连接池connPool交由connectionRAII类内的poolRAII接管
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }
    
    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}


//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    //F_GETFL表示获取fd的文件状态标志（返回值就是这个）
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    //将描述符状态标志更改为new_option(实际上 就是添加了一个O_NONBLOCK)
    fcntl(fd, F_SETFL, new_option);
    //返回原来的标志
    return old_option;
}


//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
//epollfd是epoll的文件描述符，fd是socket的文件描述符
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    //ET模式
    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;

    //注册监听事件
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    //设置非阻塞的ET
    setnonblocking(fd);
}


//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}


//将事件重置为EPOLLONESHOT（每次执行完一个socket事件就要重新设置）
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}


int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;


//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);//从内核时间表删除描述符
        m_sockfd = -1;
        m_user_count--;
    }
}


//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();//函数重载，分有参和无参两种
}


//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}


//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}


//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性（通过while循环）将数据读完
//LI和ET的区别：从代码上看就是ET比LT多了个while循环
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {   
        //bytes_read是读出来数据的字节数目
        //m_read_buf + m_read_idx是存储此次数据的地址
        //READ_BUFFER_SIZE - m_read_idx是想要读取数据的字节字节数
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        //读完后缓冲区bytes_read个字节被占用，指针跳转
        m_read_idx += bytes_read;
        
        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    //ET读数据，一次性读完或缓冲区占满为止
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {   
                //EAGAIN（EWOULDBLOCK）表示再尝试一次，原因是缓冲区已经被占满
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)//此时表名连接已断开
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}


//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    //在HTTP报文中，请求行用来说明请求类型,要访问的资源以及所使用的HTTP版本，其中各个部分之间通过\t或空格分隔。
    //请求行中最先含有空格和\t任一字符的位置并返回
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }

    //这一句的作用是将第一个空格及其后面的字符都置空，这样text(method)就只剩下请求方法了
    //例如“POST / HTTP1.1”变成“POST”
    *m_url++ = '\0';
    char *method = text;

    //strcasecmp()比较两个字符串是否相同，相同就返回0（忽略大小写）
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    
    //上面的操作中m_url跳过了第一个空格或\t但不知道后面是否还有
    //如果后面还有空格或\t就一直跳过，使得m_url指向请求资源的第一个字符
    m_url += strspn(m_url, " \t");

    //使用与判断请求方式的相同逻辑，判断HTTP版本号，然后http版本号之后的内容
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");

    //仅支持http1.1
    if (strcasecmp(m_version, "HTTP/1.1") != 0) return BAD_REQUEST;

    //strncasecmp()对请求资源前7个字符进行比较，即http://
    //这里主要是有些报文的请求资源中会带有http://，这里需要对这种情况进行单独处理
    //此时m_url存储的是请求资源及其之后的内容
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    
    //对请求资源前8个字符进行判断，即https://
    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    
    //请求行处理完毕，将主状态机转移处理请求头
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}


//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    //判断是空行还是请求头
    if (text[0] == '\0')
    {
        //判断是GET还是POST请求，GET请求的请求消息为空
        if (m_content_length != 0)//对应POST请求
        {   
            //如果是POST请求就要跳转到消息处理状态
            m_check_state = CHECK_STATE_CONTENT;
            //表示请求不完整
            return NO_REQUEST;
        }
        return GET_REQUEST;//返回POST请求
    }
    //解析请求头部连接字段
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            //优雅关闭
            m_linger = true;
        }
    }
    //解析请求头部内容长度字段
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    //解析请求头部HOST字段
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    //请求头为空
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}


//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    //判断buffer中是否读取了消息体
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}


//解析报文，包括请求行、请求头和请求数据;返回值由do_request或者语法错误（BAD_REQUEST）决定 
http_conn::HTTP_CODE http_conn::process_read()
{
    //初始化从状态机状态、HTTP请求解析结果
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    /*
    1.parse_line为从状态机的具体实现
    2.m_check_state会在函数parse_request_line()、parse_headers()和parse_content()内发生改变
    3.(m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK)针对的是POST请求
    4.(line_status = parse_line()) == LINE_OK针对的是GET请求
    5.因为在GET请求报文中每一行都是\r\n作为结束，而POST请求报文消息体末尾没有任何字符，要使用主状态机
    */
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        //m_start_line是每一个数据行在m_read_buf中的起始位置
        //m_checked_idx表示从状态机在m_read_buf中读取的位置
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);

        //从状态机的三种状态转移逻辑
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {   
            //解析请求行
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            //解析请求头
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            //完整解析GET请求后，跳转到报文响应函数（GET请求没有请求数据，要请求的资源信息存放在请求头内）
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            //解析消息体
            ret = parse_content(text);
            //完整解析POST请求后，跳转到报文响应函数（POST的请求头一般只有http版本号）
            if (ret == GET_REQUEST)
                return do_request();
            //解析完消息体即完成报文解析，避免再次进入循环，更新line_status
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}


http_conn::HTTP_CODE http_conn::do_request()
{
    //将初始化的m_real_file赋值为网站根目录
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);

    //找到m_url中/及其之后的字符串
    const char *p = strrchr(m_url, '/');

    //处理cgi；2是登录，3是注册
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //n_string内容：user=123&passwd=123
        char name[100], password[100];

        //提取用户名 
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        //提取密码
        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        //注册
        if (*(p + 1) == '3')
        {   
            //创建sql查询语句，若没有重名的，就进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            //假如用户名没有重复
            if (users.find(name) == users.end())
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                //插入一条记录（用户名和密码）
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)//注册成功
                    strcpy(m_url, "/log.html");
                else//注册失败
                    strcpy(m_url, "/registerError.html");
            }
            //用户名重复了就注册失败
            else
                strcpy(m_url, "/registerError.html");
        }

        //登录，若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else0
                strcpy(m_url, "/logError.html");
        }
    }

    //如果请求资源为/0，表示跳转注册页面
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        //len就是m_real_file的长度，下面这一句意思就是把"/register.html"加到m_real_file后面
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    //如果请求资源为/1，表示跳转登录页面
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    //如果请求资源为/5，表示跳转picture.html，即图片请求页面
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    //如果请求资源为/6，表示跳转video.html，即视频请求页面
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    //如果请求资源为/7，表示跳转博主自定义的粉丝关注界面
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    //否则发送url实际请求的文件
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    //通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
    //失败返回NO_RESOURCE状态，表示资源不存在
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    //判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    //判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    //以只读方式获取文件描述符，通过mmap将该文件映射到内存中
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    
    //避免文件描述符的浪费和占用
    close(fd);

    //表示请求文件存在，且可以访问
    return FILE_REQUEST;
}


void http_conn::unmap()
{
    if (m_file_address)
    {
        //munmap解除内存映射
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}


//服务器子线程调用process_write完成响应报文，随后注册epollout事件
//服务器主线程检测写事件，并调用http_conn::write函数将响应报文发送给浏览器端
bool http_conn::write()
{
    int temp = 0;

    //若要发送的数据长度为0，表示响应报文为空，一般不会出现这种情况
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {   
        //writev函数在一次函数调用中写多个非连续缓冲区，又称聚集写；m_iv写的是响应报文头部和文件两部分
        //另外写入数据的“结束点”可能位于一个iovec的中间某个位置，因此需要调整临界iovec的io_base和io_len
        //writev函数调用成功时返回写的总字节数
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            //判断缓冲区是否写满了
            if (errno == EAGAIN)
            {
                //重新注册写事件
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            //如果发送失败，但不是缓冲区问题，则取消映射
            unmap();
            return false;
        }

        //更新已发送字节数
        bytes_have_send += temp;
        bytes_to_send -= temp;

        //第一个iovec头部信息的数据已发送完，发送第二个iovec数据
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            //不再继续发送头部信息
            m_iv[0].iov_len = 0;

            //bytes_have_send - m_write_idx文件iovec[1]的指针偏移量，就是说可能发送了一部分文件，这已经发送的部分就是偏移量
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        //第一个iovec头部信息的数据还没发完
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        //判断条件，数据已全部发送完
        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            //如果浏览器的请求为长连接
            if (m_linger)
            {
                //重新初始化http对象
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}


//add_response函数用来更新m_write_idx指针和缓冲区m_write_buf中的内容；供其他add_函数调用
bool http_conn::add_response(const char *format, ...)
{
    //如果写入内容超出m_write_buf大小则报错
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    
    //定义可变参数列表：当函数参数个数不确定的时候可以用这个，这样就增加了编程的 灵活性
    va_list arg_list;

    //将变量arg_list初始化为传入参数
    va_start(arg_list, format);
    
    //vsnprintf函数使用参数列表发送格式化输出到字符串
    //WRITE_BUFFER_SIZE - 1 - m_write_idx应该指的是m_write_buf中剩余的区域
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    
    //缓冲区剩余区域不足以写入全部报文，就返回false
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;

    //结束传参（这是vsnprintf函数调用的固定格式）
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}


//添加状态行：http/1.1 状态码 状态消息
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}


//添加消息报头，内部调用add_content_length、add_linger和add_blank_line函数
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}


//添加Content-Length，表示响应报文的长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}


//添加文本类型，这里是html
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}


//添加连接状态，通知浏览器端是保持连接还是关闭(决定是否优雅关闭)
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}


//添加空行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}


//添加文本content
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}


//子线程根据ret返回对应的响应报文，响应报文写在process_write中
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    //服务器内部错误
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    //http请求报文有语法错误或请求资源为目录
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    //请求资源禁止访问，没有读取权限
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    //请求资源可以正常访问
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        //如果请求的资源存在
        if (m_file_stat.st_size != 0)
        {
            //添加文件大小信息
            add_headers(m_file_stat.st_size);

            //第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;

            //第二个iovec指针指向mmap返回的文件指针，长度指向文件大小
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;

            m_iv_count = 2;

            //发送的全部数据为响应报文头部信息和文件大小
            bytes_to_send = m_write_idx + m_file_stat.st_size;

            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}


void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    //NO_REQUEST，表示请求不完整，需要继续接收请求数据
    if (read_ret == NO_REQUEST)
    {   
        //注册并监听读事件
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }

    //调用process_write完成报文响应
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    //注册并监听EPOLLOUT写事件
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}