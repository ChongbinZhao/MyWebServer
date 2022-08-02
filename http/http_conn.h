#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
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
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"


class http_conn
{
public:
    //���ö�ȡ�ļ�������m_real_file��С
    static const int FILENAME_LEN = 200;
    //���ö�������m_read_buf��С
    static const int READ_BUFFER_SIZE = 2048;
    //����д������m_write_buf��С
    static const int WRITE_BUFFER_SIZE = 1024;
    //���ĵ����󷽷�������Ŀֻ�õ�GET��POST���ֱ��Ӧ0��1
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    //��״̬����״̬
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
     //���Ľ����Ľ��
    enum HTTP_CODE
    {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };
    //��״̬����״̬
    enum LINE_STATUS
    {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    //��ʼ��socket��ַ�Լ����ݿ���Ϣ
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    //�ر�http����
    void close_conn(bool real_close = true);
    //ִ������
    void process();
    //��ȡ������˷�����ȫ������
    bool read_once();
    //��Ӧ����д�뺯��
    bool write();
    //ͬ���̳߳�ʼ�����ݿ��ȡ��
    void initmysql_result(connection_pool *connPool);

    sockaddr_in *get_address()
    {
        return &m_address;
    }
    //reactorģʽ������֪ͨ�������Ƿ�����˶�д������������û�гɹ������Ǿ���Ϊ1
    int improv;
    //reactorģʽ�е���дʧ��ʱ��Ϊ1
    int timer_flag;


private:
    //��ʼ���´���������
    void init();
    //��m_read_buf��ȡ��������������
    HTTP_CODE process_read();
    //��m_write_bufд����Ӧ��������
    bool process_write(HTTP_CODE ret);
    //��״̬�����������е�����������
    HTTP_CODE parse_request_line(char *text);
    //��״̬�����������е�����ͷ����
    HTTP_CODE parse_headers(char *text);
    //��״̬�����������е���������
    HTTP_CODE parse_content(char *text);
    //������Ӧ����
    HTTP_CODE do_request();
    //get_line���ڽ�ָ�����ƫ�ƣ�ָ��δ������ַ���m_start_line���Ѿ��������ַ�
    //��ʱ��״̬������ǰ��һ�е�ĩβ�ַ�\r\n��Ϊ\0\0������text����ֱ��ȡ���������н��н���
    char *get_line() { return m_read_buf + m_start_line; };
    //��״̬����ȡһ�У������������ĵ���һ����
    LINE_STATUS parse_line();

    void unmap();

    //������Ӧ���ĸ�ʽ�����ɶ�Ӧ8�����֣����º�������do_request����
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;
    static int m_user_count;
    MYSQL *mysql;
    int m_state;  //��Ϊ0, дΪ1

private:
    int m_sockfd;
    sockaddr_in m_address;

    //�洢��ȡ������������
    char m_read_buf[READ_BUFFER_SIZE];
    //��������m_read_buf�����ݵ����һ���ֽڵ���һ��λ��
    int m_read_idx;
    //m_read_buf��ȡ��λ��m_checked_idx
    int m_checked_idx;
    //m_read_buf���Ѿ��������ַ�����
    int m_start_line;
    //��Ӧ�������ݻ��������洢Ҫ���͵���Ӧ����
    char m_write_buf[WRITE_BUFFER_SIZE];
    //ָʾbuffer�еĳ���
    int m_write_idx;

    //��״̬����״̬
    CHECK_STATE m_check_state;
    //���󷽷�
    METHOD m_method;

    /*����Ϊ�����������ж�Ӧ��6������*/
    //Ҫ��ȡ�ļ�������
    char m_real_file[FILENAME_LEN];
    //Ҫ���ʵ���Դ��ַ������/xxx.jpg
    char *m_url;
    //httpЭ��汾��Ϣ������http1.1
    char *m_version;
    //����������
    char *m_host;
    //http��Ϣ����
    int m_content_length;
    //�Ƿ����ŶϿ�����ִ��close()��ȵ����ݷ�����ͷ���Դ
    bool m_linger;

    //��ȡ�������ϵ��ļ���ַ
    char *m_file_address;
    
    struct stat m_file_stat;
    
    struct iovec m_iv[2];
    int m_iv_count;

    //�Ƿ����õ�POST
    int cgi;
    //�洢����ͷ����        
    char *m_string; 
    //ʣ�෢���ֽ���
    int bytes_to_send;
    //�ѷ����ֽ���
    int bytes_have_send;
    
    char *doc_root;

    //�����ݿ���û������������뵽�������е�map
    map<string, string> m_users;

    int m_TRIGMode;
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
