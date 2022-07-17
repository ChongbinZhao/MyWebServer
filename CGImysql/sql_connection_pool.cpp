#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

//���캯��
connection_pool::connection_pool()
{
	m_CurConn = 0;
	m_FreeConn = 0;
}


//��������ģʽ�������ӳ�ʵ��
connection_pool *connection_pool::GetInstance()
{
	static connection_pool connPool;
	return &connPool;
}


//�����ʼ��
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn, int close_log)
{
	//��ʼ�����ݿ���Ϣ
	m_url = url;
	m_Port = Port;
	m_User = User;
	m_PassWord = PassWord;
	m_DatabaseName = DBName;
	m_close_log = close_log;

	//����MaxConn�����ݿ�����
	for (int i = 0; i < MaxConn; i++)
	{
		MYSQL *con = NULL;
		con = mysql_init(con);

		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}

		//����һ�����ݿ�����
		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);

		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}
		connList.push_back(con);
		++m_FreeConn;
	}

	//���ź�����ʼ��Ϊ������Ӵ�����sem��locker.h����ź�����
	reserve = sem(m_FreeConn);

	m_MaxConn = m_FreeConn;
}


//��������ʱ�������ݿ����ӳ��з���һ���������ӣ�����ʹ�úͿ���������
MYSQL *connection_pool::GetConnection()
{
	MYSQL *con = NULL;

	if (0 == connList.size()) return NULL;

	//ȡ�����ӣ��ź���ԭ�Ӽ�1��Ϊ0��ȴ�
	reserve.wait();
	
	//�����ӳؽ��в���ǰ������
	lock.lock();

	con = connList.front();
	connList.pop_front();

	//������������û���õ������Ǽ�¼���ӳص�ǰ״̬
	--m_FreeConn;
	++m_CurConn;

	//���ӳز�����ϣ�����
	lock.unlock();
	return con;
}


//�ͷŵ�ǰʹ�õ�����
bool connection_pool::ReleaseConnection(MYSQL *con)
{
	if (NULL == con)
		return false;

	lock.lock();

	connList.push_back(con);
	++m_FreeConn;
	--m_CurConn;

	lock.unlock();

	reserve.post();
	return true;
}


//�������ݿ����ӳأ�ʹ��mysql_close()����ر�mysql����
void connection_pool::DestroyPool()
{

	lock.lock();
	if (connList.size() > 0)
	{
		list<MYSQL *>::iterator it;
		for (it = connList.begin(); it != connList.end(); ++it)
		{
			MYSQL *con = *it;
			mysql_close(con);
		}
		m_CurConn = 0;
		m_FreeConn = 0;
		connList.clear();
	}

	lock.unlock();
}


//��ǰ���е�������
int connection_pool::GetFreeConn()
{
	return this->m_FreeConn;
}


//RAII���Ƶ���DestroyPool()�������ӳأ��ڹ��캯�������������Դ���������������ͷ���Դ
connection_pool::~connection_pool()
{
	DestroyPool();
}


//��������connection_pool���Ա����������װ��connectionRAII��������
connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool){
	
	//ȡ��һ������mysql����
	*SQL = connPool->GetConnection();

	//��ȡ����mysql���ӷ���conRAII��������
	conRAII = *SQL;

	//���ݿ����ӳ�
	poolRAII = connPool;
}


//connectionRAII��������
connectionRAII::~connectionRAII(){
	poolRAII->ReleaseConnection(conRAII);
}