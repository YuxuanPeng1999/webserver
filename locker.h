#ifndef LOCKER_H
#define LOCKER_H

// 线程同步机制封装类

#include <pthread.h>
#include <exception>
#include <semaphore.h>

// 互斥锁类
class locker {
public: 
    // 无参构造
    locker() {
        if (pthread_mutex_init(&m_mutex, NULL) != 0) {
            throw std::exception();
        }
    }
    // 析构函数
    ~locker() {
        pthread_mutex_destroy(&m_mutex);
    }
    // 上锁
    bool lock() {
        return pthread_mutex_lock(&m_mutex) == 0;
    }
    // 解锁
    bool unlock() {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }
    // GET方法
    pthread_mutex_t* get() {
        return &m_mutex;
    }
private:
    pthread_mutex_t m_mutex;
};


// 条件变量类
class cond {
public: 
    // 无参构造
    cond() {
        if (pthread_cond_init(&m_cond, NULL) != 0) {
            throw std::exception();
        }
    }
    // 析构函数
    ~cond() {
        pthread_cond_destroy(&m_cond);
    }
    // wait()
    bool wait(pthread_mutex_t* mutex) {
        return pthread_cond_wait(&m_cond, mutex) == 0;
    }
    // timedwait()
    bool timedwait(pthread_mutex_t* mutex, struct timespec t) {
        return pthread_cond_timedwait(&m_cond, mutex, &t) == 0;
    }
    // signal()
    bool signal(pthread_mutex_t* mutex) {
        return pthread_cond_signal(&m_cond) == 0;
    }
    // broadcast
    bool broadcast() {
        return pthread_cond_broadcast(&m_cond) == 0;
    }
private:
    pthread_cond_t m_cond;
};

// 信号量
class sem {
public: 
    // 无参构造
    sem() {
        if (sem_init(&m_sem, 0, 0) != 0) {
            throw std::exception();
        }
    }
    // 有参构造
    sem(int num) {
        if (sem_init(&m_sem, 0, num) != 0) {
            throw std::exception();
        }
    }
    // 析构函数
    ~sem() {
        sem_destroy(&m_sem);
    }
    // wait
    bool wait() {
        return sem_wait(&m_sem) == 0;
    }
    // post: 增加信号量
    bool post() {
        return sem_post(&m_sem) == 0;
    }
private:
    sem_t m_sem;
};

#endif