#include "http_conn.h"
#include <mysql/mysql.h>
#include <fstream>

//����http��Ӧ��һЩ״̬��Ϣ
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


//�������ݿ�������û������������map��
void http_conn::initmysql_result(connection_pool *connPool)
{
    //�ȴ����ӳ���ȡһ������
    MYSQL *mysql = NULL;

    //�����ӳ���ȡ��һ���������Ӳ�����mysql�����ӳ�connPool����connectionRAII���ڵ�poolRAII�ӹ�
    connectionRAII mysqlcon(&mysql, connPool);

    //��user���м���username��passwd���ݣ������������
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }
    
    //�ӱ��м��������Ľ����
    MYSQL_RES *result = mysql_store_result(mysql);

    //���ؽ�����е�����
    int num_fields = mysql_num_fields(result);

    //���������ֶνṹ������
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //�ӽ�����л�ȡ��һ�У�����Ӧ���û��������룬����map��
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}


//���ļ����������÷�����
int setnonblocking(int fd)
{
    //F_GETFL��ʾ��ȡfd���ļ�״̬��־������ֵ���������
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    //��������״̬��־����Ϊnew_option(ʵ���� ���������һ��O_NONBLOCK)
    fcntl(fd, F_SETFL, new_option);
    //����ԭ���ı�־
    return old_option;
}


//���ں��¼���ע����¼���ETģʽ��ѡ����EPOLLONESHOT
//epollfd��epoll���ļ���������fd��socket���ļ�������
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    //ETģʽ
    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;

    //ע������¼�
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    //���÷�������ET
    setnonblocking(fd);
}


//���ں�ʱ���ɾ��������
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}


//���¼�����ΪEPOLLONESHOT��ÿ��ִ����һ��socket�¼���Ҫ�������ã�
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


//�ر����ӣ��ر�һ�����ӣ��ͻ�������һ
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);//���ں�ʱ���ɾ��������
        m_sockfd = -1;
        m_user_count--;
    }
}


//��ʼ������,�ⲿ���ó�ʼ���׽��ֵ�ַ
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    //�������������������ʱ����������վ��Ŀ¼�����http��Ӧ��ʽ������߷��ʵ��ļ���������ȫΪ��
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();//�������أ����вκ��޲�����
}


//��ʼ���½��ܵ�����
//check_stateĬ��Ϊ����������״̬
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


//��״̬�������ڷ�����һ������
//����ֵΪ�еĶ�ȡ״̬����LINE_OK,LINE_BAD,LINE_OPEN
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


//ѭ����ȡ�ͻ����ݣ�ֱ�������ݿɶ���Է��ر�����
//������ET����ģʽ�£���Ҫһ���ԣ�ͨ��whileѭ���������ݶ���
//LI��ET�����𣺴Ӵ����Ͽ�����ET��LT���˸�whileѭ��
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //LT��ȡ����
    if (0 == m_TRIGMode)
    {   
        //bytes_read�Ƕ��������ݵ��ֽ���Ŀ
        //m_read_buf + m_read_idx�Ǵ洢�˴����ݵĵ�ַ
        //READ_BUFFER_SIZE - m_read_idx����Ҫ��ȡ���ݵ��ֽ��ֽ���
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        //����󻺳���bytes_read���ֽڱ�ռ�ã�ָ����ת
        m_read_idx += bytes_read;
        
        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    //ET�����ݣ�һ���Զ���򻺳���ռ��Ϊֹ
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {   
                //EAGAIN��EWOULDBLOCK����ʾ�ٳ���һ�Σ�ԭ���ǻ������Ѿ���ռ��
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)//��ʱ���������ѶϿ�
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}


//����http�����У�������󷽷���Ŀ��url��http�汾��
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    //��HTTP�����У�����������˵����������,Ҫ���ʵ���Դ�Լ���ʹ�õ�HTTP�汾�����и�������֮��ͨ��\t��ո�ָ���
    //�����������Ⱥ��пո��\t��һ�ַ���λ�ò�����
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }

    //��һ��������ǽ���һ���ո��������ַ����ÿգ�����text(method)��ֻʣ�����󷽷���
    //���硰POST / HTTP1.1����ɡ�POST��
    *m_url++ = '\0';
    char *method = text;

    //strcasecmp()�Ƚ������ַ����Ƿ���ͬ����ͬ�ͷ���0�����Դ�Сд��
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    
    //����Ĳ�����m_url�����˵�һ���ո��\t����֪�������Ƿ���
    //������滹�пո��\t��һֱ������ʹ��m_urlָ��������Դ�ĵ�һ���ַ�
    m_url += strspn(m_url, " \t");

    //ʹ�����ж�����ʽ����ͬ�߼����ж�HTTP�汾�ţ�Ȼ��http�汾��֮�������
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");

    //��֧��http1.1
    if (strcasecmp(m_version, "HTTP/1.1") != 0) return BAD_REQUEST;

    //strncasecmp()��������Դǰ7���ַ����бȽϣ���http://
    //������Ҫ����Щ���ĵ�������Դ�л����http://��������Ҫ������������е�������
    //��ʱm_url�洢����������Դ����֮�������
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    
    //��������Դǰ8���ַ������жϣ���https://
    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    
    //��urlΪ/ʱ����ʾ�жϽ���
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    
    //�����д�����ϣ�����״̬��ת�ƴ�������ͷ
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}


//����http�����һ��ͷ����Ϣ
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    //�ж��ǿ��л�������ͷ
    if (text[0] == '\0')
    {
        //�ж���GET����POST����GET�����������ϢΪ��
        if (m_content_length != 0)//��ӦPOST����
        {   
            //�����POST�����Ҫ��ת����Ϣ����״̬
            m_check_state = CHECK_STATE_CONTENT;
            //��ʾ��������
            return NO_REQUEST;
        }
        return GET_REQUEST;//����POST����
    }
    //��������ͷ�������ֶ�
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            //���Źر�
            m_linger = true;
        }
    }
    //��������ͷ�����ݳ����ֶ�
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    //��������ͷ��HOST�ֶ�
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    //����ͷΪ��
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}


//�ж�http�����Ƿ���������
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    //�ж�buffer���Ƿ��ȡ����Ϣ��
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST���������Ϊ������û���������
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}


//�������ģ����������С�����ͷ����������;����ֵ��do_request�����﷨����BAD_REQUEST������ 
http_conn::HTTP_CODE http_conn::process_read()
{
    //��ʼ����״̬��״̬��HTTP����������
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    /*
    1.parse_lineΪ��״̬���ľ���ʵ��
    2.m_check_state���ں���parse_request_line()��parse_headers()��parse_content()�ڷ����ı�
    3.(m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK)��Ե���POST����
    4.(line_status = parse_line()) == LINE_OK��Ե���GET����
    5.��Ϊ��GET��������ÿһ�ж���\r\n��Ϊ��������POST��������Ϣ��ĩβû���κ��ַ���Ҫʹ����״̬��
    */
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        //m_start_line��ÿһ����������m_read_buf�е���ʼλ��
        //m_checked_idx��ʾ��״̬����m_read_buf�ж�ȡ��λ��
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);

        //��״̬��������״̬ת���߼�
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {   
            //����������
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            //��������ͷ
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            //��������GET�������ת��������Ӧ������GET����û���������ݣ�Ҫ�������Դ��Ϣ���������ͷ�ڣ�
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            //������Ϣ��
            ret = parse_content(text);
            //��������POST�������ת��������Ӧ������POST������ͷһ��ֻ��http�汾�ţ�
            if (ret == GET_REQUEST)
                return do_request();
            //��������Ϣ�弴��ɱ��Ľ����������ٴν���ѭ��������line_status
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
    //����ʼ����m_real_file��ֵΪ��վ��Ŀ¼
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);

    //�ҵ�m_url��/����֮����ַ���
    const char *p = strrchr(m_url, '/');

    //����cgi��2�ǵ�¼��3��ע��
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //���ݱ�־�ж��ǵ�¼��⻹��ע����
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //���û�����������ȡ����
        //n_string���ݣ�user=123&passwd=123
        char name[100], password[100];

        //��ȡ�û��� 
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        //��ȡ����
        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        //ע��
        if (*(p + 1) == '3')
        {   
            //����sql��ѯ��䣬��û�������ģ��ͽ�����������
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            //�����û���û���ظ�
            if (users.find(name) == users.end())
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                //����һ����¼���û��������룩
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)//ע��ɹ�
                    strcpy(m_url, "/log.html");
                else//ע��ʧ��
                    strcpy(m_url, "/registerError.html");
            }
            //�û����ظ��˾�ע��ʧ��
            else
                strcpy(m_url, "/registerError.html");
        }

        //��¼�����������������û����������ڱ��п��Բ��ҵ�������1�����򷵻�0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else0
                strcpy(m_url, "/logError.html");
        }
    }

    //���������ԴΪ/0����ʾ��תע��ҳ��
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        //len����m_real_file�ĳ��ȣ�������һ����˼���ǰ�"/register.html"�ӵ�m_real_file����
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    //���������ԴΪ/1����ʾ��ת��¼ҳ��
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    //���������ԴΪ/5����ʾ��תpicture.html����ͼƬ����ҳ��
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    //���������ԴΪ/6����ʾ��תvideo.html������Ƶ����ҳ��
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    //���������ԴΪ/7����ʾ��ת�����Զ���ķ�˿��ע����
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    //������urlʵ��������ļ�
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    //ͨ��stat��ȡ������Դ�ļ���Ϣ���ɹ�����Ϣ���µ�m_file_stat�ṹ��
    //ʧ�ܷ���NO_RESOURCE״̬����ʾ��Դ������
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    //�ж��ļ���Ȩ�ޣ��Ƿ�ɶ������ɶ��򷵻�FORBIDDEN_REQUEST״̬
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    //�ж��ļ����ͣ������Ŀ¼���򷵻�BAD_REQUEST����ʾ����������
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    //��ֻ����ʽ��ȡ�ļ���������ͨ��mmap�����ļ�ӳ�䵽�ڴ���
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    
    //�����ļ����������˷Ѻ�ռ��
    close(fd);

    //��ʾ�����ļ����ڣ��ҿ��Է���
    return FILE_REQUEST;
}


void http_conn::unmap()
{
    if (m_file_address)
    {
        //munmap����ڴ�ӳ��
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}


//���������̵߳���process_write�����Ӧ���ģ����ע��epollout�¼�
//���������̼߳��д�¼���������http_conn::write��������Ӧ���ķ��͸��������
bool http_conn::write()
{
    int temp = 0;

    //��Ҫ���͵����ݳ���Ϊ0����ʾ��Ӧ����Ϊ�գ�һ�㲻������������
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {   
        //writev������һ�κ���������д������������������ֳƾۼ�д��m_ivд������Ӧ����ͷ�����ļ�������
        //����д�����ݵġ������㡱����λ��һ��iovec���м�ĳ��λ�ã������Ҫ�����ٽ�iovec��io_base��io_len
        //writev�������óɹ�ʱ����д�����ֽ���
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            //�жϻ������Ƿ�д����
            if (errno == EAGAIN)
            {
                //����ע��д�¼�
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            //�������ʧ�ܣ������ǻ��������⣬��ȡ��ӳ��
            unmap();
            return false;
        }

        //�����ѷ����ֽ���
        bytes_have_send += temp;
        bytes_to_send -= temp;

        //��һ��iovecͷ����Ϣ�������ѷ����꣬���͵ڶ���iovec����
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            //���ټ�������ͷ����Ϣ
            m_iv[0].iov_len = 0;

            //bytes_have_send - m_write_idx�ļ�iovec[1]��ָ��ƫ����������˵���ܷ�����һ�����ļ������Ѿ����͵Ĳ��־���ƫ����
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        //��һ��iovecͷ����Ϣ�����ݻ�û����
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        //�ж�������������ȫ��������
        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            //��������������Ϊ������
            if (m_linger)
            {
                //���³�ʼ��http����
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


//add_response������������m_write_idxָ��ͻ�����m_write_buf�е����ݣ�������add_��������
bool http_conn::add_response(const char *format, ...)
{
    //���д�����ݳ���m_write_buf��С�򱨴�
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    
    //����ɱ�����б�����������������ȷ����ʱ�����������������������˱�̵� �����
    va_list arg_list;

    //������arg_list��ʼ��Ϊ�������
    va_start(arg_list, format);
    
    //vsnprintf����ʹ�ò����б��͸�ʽ��������ַ���
    //WRITE_BUFFER_SIZE - 1 - m_write_idxӦ��ָ����m_write_buf��ʣ�������
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    
    //������ʣ����������д��ȫ�����ģ��ͷ���false
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;

    //�������Σ�����vsnprintf�������õĹ̶���ʽ��
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}


//���״̬�У�http/1.1 ״̬�� ״̬��Ϣ
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}


//�����Ϣ��ͷ���ڲ�����add_content_length��add_linger��add_blank_line����
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}


//���Content-Length����ʾ��Ӧ���ĵĳ���
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}


//����ı����ͣ�������html
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}


//�������״̬��֪ͨ��������Ǳ������ӻ��ǹر�(�����Ƿ����Źر�)
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}


//��ӿ���
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}


//����ı�content
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}


//���̸߳���ret���ض�Ӧ����Ӧ���ģ���Ӧ����д��process_write��
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    //�������ڲ�����
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    //http���������﷨�����������ԴΪĿ¼
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    //������Դ��ֹ���ʣ�û�ж�ȡȨ��
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    //������Դ������������
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        //����������Դ����
        if (m_file_stat.st_size != 0)
        {
            //����ļ���С��Ϣ
            add_headers(m_file_stat.st_size);

            //��һ��iovecָ��ָ����Ӧ���Ļ�����������ָ��m_write_idx
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;

            //�ڶ���iovecָ��ָ��mmap���ص��ļ�ָ�룬����ָ���ļ���С
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;

            m_iv_count = 2;

            //���͵�ȫ������Ϊ��Ӧ����ͷ����Ϣ���ļ���С
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
    //NO_REQUEST����ʾ������������Ҫ����������������
    if (read_ret == NO_REQUEST)
    {   
        //ע�Ტ�������¼�
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }

    //����process_write��ɱ�����Ӧ
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    //ע�Ტ����EPOLLOUTд�¼�
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}