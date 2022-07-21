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

//��ʱ��
class util_timer;

//��ʱ�����ڲ��ľ�����Ϣ
struct client_data
{
    sockaddr_in address;
    int sockfd;
    util_timer *timer;
};


//��ʱ����
class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    //��ʱʱ��
    time_t expire;
    
    //�ص�����������ɾ���ǻsocket�ϵ�ע���¼����ر�
    void (* cb_func)(client_data *);

    //��ʱ��������������Դ��
    client_data *user_data;

    //ǰ��ͺ�̶�ʱ��
    util_timer *prev;
    util_timer *next;
};


//��ʱ���������:��ÿ�����ӵĶ�ʱ�����ճ�ʱʱ����������;ִ�ж�ʱ����ʱ�����ڵĶ�ʱ��������ɾ��
//��Ӷ�ʱ��ʱ�临�Ӷ�ΪO(n)��ɾ����ʱ��ʱ�临�Ӷ�ΪO(1)
class sort_timer_lst
{
public:
    sort_timer_lst();
    ~sort_timer_lst();

    //��Ŀ�궨ʱ����������ӵ�������
    void add_timer(util_timer *timer);
    
    //������ʱ���������е�λ��
    void adjust_timer(util_timer *timer);
    
    //����ʱ�Ķ�ʱ����������ɾ��
    void del_timer(util_timer *timer);
    
    //��ʱ����������SIGALRM����ʱ�͵����������
    void tick();

private:
    //˽�г�Ա�������г�Աadd_timer��adjust_time����
    //��Ҫ���ڵ��������ڲ����
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

    //���ļ����������÷�����
    int setnonblocking(int fd);

    //���ں��¼���ע����¼���ETģʽ��ѡ����EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    //�źŴ�����
    static void sig_handler(int sig);

    //�����źź���
    void addsig(int sig, void(handler)(int), bool restart = true);

    //��ʱ�����������¶�ʱ�Բ��ϴ���SIGALRM�ź�
    void timer_handler();

    //��������Ϣ�����ͻ���
    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd;//�ܵ��׽���
    sort_timer_lst m_timer_lst;//��ʱ���б�
    static int u_epollfd;//epoll������
    int m_TIMESLOT;//��ʱʱ����������ʱ�� - ���ڵ�ʱ�� = ���ɸ�m_TIMESLOT
};


void cb_func(client_data *user_data);

#endif
