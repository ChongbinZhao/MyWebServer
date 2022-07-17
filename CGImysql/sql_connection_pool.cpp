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

//构造函数
connection_pool::connection_pool()
{
	m_CurConn = 0;
	m_FreeConn = 0;
}


//单例懒汉模式返回连接池实例
connection_pool *connection_pool::GetInstance()
{
	static connection_pool connPool;
	return &connPool;
}


//构造初始化
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn, int close_log)
{
	//初始化数据库信息
	m_url = url;
	m_Port = Port;
	m_User = User;
	m_PassWord = PassWord;
	m_DatabaseName = DBName;
	m_close_log = close_log;

	//创建MaxConn条数据库连接
	for (int i = 0; i < MaxConn; i++)
	{
		MYSQL *con = NULL;
		con = mysql_init(con);

		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}

		//创建一条数据库连接
		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);

		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}
		connList.push_back(con);
		++m_FreeConn;
	}

	//将信号量初始化为最大连接次数，sem是locker.h里的信号量类
	reserve = sem(m_FreeConn);

	m_MaxConn = m_FreeConn;
}


//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection()
{
	MYSQL *con = NULL;

	if (0 == connList.size()) return NULL;

	//取出连接，信号量原子减1，为0则等待
	reserve.wait();
	
	//对连接池进行操作前先上锁
	lock.lock();

	con = connList.front();
	connList.pop_front();

	//这两个变量并没有用到，仅是记录连接池当前状态
	--m_FreeConn;
	++m_CurConn;

	//连接池操作完毕，解锁
	lock.unlock();
	return con;
}


//释放当前使用的连接
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


//销毁数据库连接池，使用mysql_close()逐个关闭mysql连接
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


//当前空闲的连接数
int connection_pool::GetFreeConn()
{
	return this->m_FreeConn;
}


//RAII机制调用DestroyPool()销毁连接池：在构造函数中申请分配资源，在析构函数中释放资源
connection_pool::~connection_pool()
{
	DestroyPool();
}


//上述所有connection_pool类成员函数都被封装在connectionRAII类里面了
connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool){
	
	//取出一个可用mysql连接
	*SQL = connPool->GetConnection();

	//将取出的mysql连接放入conRAII供外界调用
	conRAII = *SQL;

	//数据库连接池
	poolRAII = connPool;
}


//connectionRAII析构函数
connectionRAII::~connectionRAII(){
	poolRAII->ReleaseConnection(conRAII);
}