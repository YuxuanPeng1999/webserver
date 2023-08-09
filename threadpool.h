#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <list>
#include "locker.h"
#include <exception>
#include <cstdio>

/*
问题辨析: 
1.在threadpool<T>::threadpool中, 我们在堆区创建了线程池数组, 并
创建出了所有线程. 在创建所有线程的循环中, 使用了pthread_create. 
该函数的第三个参数为线程的回调函数, 该函数的声明为: 
void* (*start_routine) (void*)
也就是说, 该回调函数的输入参数必须为void*类型. 在C++中, 要求这个
该函数必须为static, 这是为什么? 
答: 一种说法是, C++的类成员函数都有一个默认参数this指针, 而该函数
被限制为只能有一个void*类型输入参数, 因此必须将该函数声明为static,
使其没有this参数. 
附注: 
"值得一提的是, 在C++程序中使用pthread_create时, 该函数的第三个参数
必须指向一个静态函数."  (Linux高性能服务器编程, 303页, 游双)

2.接上一个问题, 我们被迫将pthread_create的第三个参数, 即子线程
(工作线程)的回调函数, 设为了static, 这个该函数就没有this指针, 是
无法操作threadpool的成员的, 但是, 实际上该函数还是需要操作
threadpool的成员的, 如何解决该问题呢? 
答: 通过pthread_create的第四个参数将this指针传给该回调函数. 
*/

/*
* 线程池类: 定义为模板类, 利于代码复用, 模板参数T是任务类. 
*/
template<typename T>
class threadpool {
public: 
    // 有参构造
    threadpool(int thread_number = 8, int max_requests = 10000);
    // 析构函数
    ~threadpool();
    /* 向工作队列中添加任务 */
    bool append(T* request);
private: 
    static void* worker(void* arg);
    void run();
private:
    // 线程数量
    int m_thread_number;
    // 线程池数组
    pthread_t* m_threads;
    // 请求队列中允许的待处理请求的最大数量
    int m_max_requests;
    // 请求队列
    std::list<T*> m_workqueue;
    // 互斥锁(请求队列是所有线程共享的, 因此需要互斥锁, 控制各线程对请求队列资源的访问)
    locker m_queuelocker;
    // 信号量(用来判断是否有任务需要处理)
    sem m_queuestat;
    // 是否结束线程的标志
    bool m_stop;
};

// 语法提醒: 这是类成员函数的类外实现, 需要加上模板声明
// 有参构造函数类外实现
template<typename T>
threadpool<T>::threadpool(int thread_number, int max_requests): 
    m_thread_number(thread_number), 
    m_max_requests(max_requests), 
    m_stop(false), 
    m_threads(NULL) 
{
    // 1.错误检查
    if ((thread_number <= 0) || (max_requests <= 0)) {
        throw std::exception();
    }
    // 2.在堆区创建线程池数组
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads) {  // 如果创建未成功, 抛出异常
        throw std::exception();
    }
    // 3.预先创建好m_thread_number个线程, 并设置为线程分离(detached)
    for (int i = 0; i < m_thread_number; i++) {
        printf("创建第 %d 个线程\n", i);
        // 3-1 创建线程, 并处理错误
        if (pthread_create(m_threads + i, NULL, worker, this) != 0) {
            // 如果创建线程过程中出错, 应将m_threads释放掉
            delete[] m_threads;
            throw std::exception();
        }
        // 3-2 设置线程分离, 并处理错误
        if (pthread_detach(m_threads[i])) {
            // 如果设置线程分离过程中出错, 应将m_threads释放掉
            delete[] m_threads;
            throw std::exception();
        }
    }
}

// 析构函数类外实现
template<typename T>
threadpool<T>::~threadpool() {
    delete[] m_threads;
    m_stop = true;
}

/*
append函数类外实现
功能: 向队列中添加任务
*/
template<typename T>
bool threadpool<T>::append(T* request) {
    // 1.要访问任务队列资源, 先获取互斥锁
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests) {
        // 如果队列中的任务数已超出最大允许量, 则不向其中添加任务
        m_queuelocker.unlock();
        return false;
    }
    // 2.向队列中添加任务
    m_workqueue.push_back(request);
    // 3.释放互斥锁
    m_queuelocker.unlock();
    // 4.通过信号量来通知一个线程可以从任务队列中获取并处理任务了
    m_queuestat.post();

    return true;
}

/*
worker的类外实现.
功能: 子线程的回调函数, 工作线程
*/
template<typename T>
void* threadpool<T>::worker(void* arg) {
    // 1.获取this指针, 用以操作threadpool类型对象的成员变量
    threadpool* pool = (threadpool*) arg;
    /* 2.从任务队列中获取任务, 并处理. 这里, 将这些内容定义在成员函数run()中
     * 因为获取任务、处理任务需要大量调用threadpool类的成员. 如果直接在本函数
     * 中调用这些成员, 每次都需要使用this指针指向它们进行调用, 不方便, 因此获
     * 取this指针之后, 后续步骤都放到函数run()中执行. */
    pool->run();

    return pool;  // 这里的返回值没什么用
}

/*
run的类外实现. 
功能: 从任务队列中获取任务, 并且处理任务. 
*/
template<typename T>
void threadpool<T>::run() {
    while(!m_stop) {
        // 1.获取任务
        // 1-1 执行信号量的wait操作, 保证任务队列中有任务供获取, 然后获取锁
        m_queuestat.wait();
        m_queuelocker.lock();
        // 1-2 获取任务
        // 如果没有任务就释放锁(本段代码实无必要, 可删除)
        if (m_workqueue.empty()) {
            m_queuelocker.unlock();
            continue;
        }
        // 如果有任务就获取任务
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        // 1-3 释放锁
        m_queuelocker.unlock();
        // 2.处理任务
        // 如果任务为NULL就不处理
        if (!request) {
            continue;
        }
        // 如果任务不是NULL就处理
        request->process();
    }
}

#endif