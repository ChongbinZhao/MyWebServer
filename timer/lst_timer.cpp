#include "lst_timer.h"
#include "../http/http_conn.h"

//��������
sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}


//��������
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


//��Ӷ�ʱ�� 
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

    //���timer�ĳ�ʱʱ���head��ʱ��С����timer��head����
    if (timer->expire < head->expire)
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }

    //�������˽�г�Ա������Ҳ��add_timer����������ͬ���������ڲ����
    add_timer(timer, head);
}


//������ʱ��λ��
void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    util_timer *tmp = timer->next;

    //�������Ķ�ʱ��������β�� || ��ʱֵ��ȻС����һ����ʱ����ʱֵ���򲻵���
    if (!tmp || (timer->expire < tmp->expire))
    {
        return;
    }

    //��������ʱ��������ͷ��㣬����ʱ��ȡ�������²���
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    }

    //��������ʱ�����ڲ�������ʱ��ȡ�������²���
    else
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}


//ɾ����ʱ��
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


//��ʱ������������SIGALRM�ź��������������������е��ڵĶ�ʱ��
void sort_timer_lst::tick()
{
    if (!head)
    {
        return;
    }
    
    //��ȡ��ǰʱ��
    time_t cur = time(NULL);

    //��λ�������ͷ�ڵ�
    util_timer *tmp = head;

    //������ʱ������
    while (tmp)
    {
        //һ��Ҳû�е���
        if (cur < tmp->expire)
        {
            break;
        }

        //����ǰ��ʱ�����ڣ�����ûص�������ִ����Ӧ����
        //ɾ��epollע���¼����ر��ļ�����������������1
        tmp->cb_func(tmp->user_data);

        //�������Ķ�ʱ��������������ɾ����������ͷ���
        head = tmp->next;
        if (head)
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}


//timer��ʱʱ���head�������£�������ѯֱ��timer����
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


//��ʼ��:TIMESLOT = timeslot
void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}


//���ļ����������÷�����
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//���ں��¼���ע����¼���ETģʽ��ѡ����EPOLLONESHOT
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


//�źŴ����������źŵľ���ֵͨ���ܵ����͸�������
void Utils::sig_handler(int sig)
{
    //Ϊ��֤�����Ŀ������ԣ�����ԭ����errno�������ֳ�
    int save_errno = errno;

    //sig��ϵͳ�������ź�
    int msg = sig;

    //���ź�ֵ�ӹܵ�д��д�룬�����ַ����ͣ���������
    send(u_pipefd[1], (char *)&msg, 1, 0);

    //��ԭ�ֳ�
    errno = save_errno;
}


//�����źź�������Ŀ�н���עSIGPIPE��SIGTERM��SIGALRM�����ź�
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    //����sigaction�ṹ�����
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));

    //�źŴ������н��������ź�ֵ��������Ӧ�߼�����
    sa.sa_handler = handler;

    //SA_RESTART��ʹ���źŴ�ϵ�ϵͳ�����Զ����·���
    if (restart)
        sa.sa_flags |= SA_RESTART;

    //����������set�źż���ʼ����Ȼ������е��źż��뵽���źż���;sa_maskָҪ���ε��ź�
    sigfillset(&sa.sa_mask);
    
    //sigaction()����ֵ0��ʾ�ɹ���-1��ʾ�д�����
    assert(sigaction(sig, &sa, NULL) != -1);
}


//��ʱ�����������¶�ʱ�Բ��ϴ���SIGALRM�ź�
void Utils::timer_handler()
{
    //��ʱ��������
    m_timer_lst.tick();

    //�����ź�SIGALRM�ھ�������m_TIMESLOT�������͸�Ŀǰ�Ľ���
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

//��ʱ���ص�����
void cb_func(client_data *user_data)
{
    //ɾ���ǻ������socket�ϵ�ע���¼�
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);

    //�ر��ļ�������
    close(user_data->sockfd);

    //��������1
    http_conn::m_user_count--;
}
