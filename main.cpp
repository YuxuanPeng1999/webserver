#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <signal.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65535  // 文件描述符的最大数量
#define MAX_EVENT_NUMBER 10000  // epoll可检测事件的最大数量

// 添加信号捕捉
void addsig(int sig, void(handler)(int)) {
    struct sigaction sa;  // 首先定义sigaction类型的结构体
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;  // 该参数定义如何处理该信号, 用SIG_IGN信号, 表示忽略该信号, 不作任何处理
    sigemptyset(&sa.sa_mask);  // 清空临时阻塞信号集
    sigaction(sig, &sa, NULL);  // 注册sig信号的处理方法, 第三个参数一般传递NULL
}

// 向epoll实例添加文件描述符
extern void addfd(int epfd, int fd, bool oneshot, bool edge);
// 从epoll实例删除文件描述符
extern void removefd(int epfd, int fd);
// 修改epoll实例中要检测的文件描述符的属性, 将原属性修改为event, 
// 注意: 修改时无需添加EPOLLRDHUP, EPOLLONESHOT. 此函数会自动添加.
extern void modfd(int epfd, int fd, int event);

int main(int argc, char* argv[]) {
    // 1.从终端接收参数
    // 1-1 参数正确性判断
    if (argc <= 1) {
        printf("按照如下格式运行: %s port_number\n", basename(argv[0]));
        exit(-1);
    }
    // 1-2 参数接收
    // argv[0]: 程序名
    // argv[1]: 端口号
    int port = atoi(argv[1]);

    // 2.对SIGPIE信号进行处理
    addsig(SIGPIPE, SIG_IGN);

    // 3.创建并初始化线程池
    threadpool<http_conn>* pool = NULL;
    try{
        pool = new threadpool<http_conn>;
    } catch(...) {
        exit(-1);
    }

    // 4.创建监听套接字
    int lfd = socket(PF_INET, SOCK_STREAM, 0);
    if (lfd == -1) {
        perror("socket");
        exit(-1);
    }

    // 5.设置端口复用
    int reuse = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    // 6.绑定
    struct sockaddr_in saddr;
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(port);
    saddr.sin_addr.s_addr = INADDR_ANY;
    int ret = bind(lfd, (struct sockaddr*)&saddr, sizeof(saddr));
    if (ret == -1) {
        perror("bind");
        exit(-1);
    }

    // 7.设置监听
    ret = listen(lfd, 5);
    if (ret == -1) {
        perror("listen");
        exit(-1);
    } else if (ret == 0) {
        printf("开始监听!\n");
    }

    // 8.创建epoll实例, 并将监听描述符加入epoll实例
    int epfd = epoll_create(1);
    if (epfd == -1) {
        perror("epoll");
        exit(-1);
    }
    addfd(epfd, lfd, false, false);  // lfd无需设为EPOLLONESHOT
    http_conn::m_epfd = epfd;  // 将http_conn的静态属性m_epfd初始化为epfd

    // 9.开始接受连接请求, 并读取数据、创建任务、
    // 创建一个数组, 保存所有客户端的信息
    // 注意, 该数组的元素为类类型, 在定义时即被默认初始化
    http_conn* users = new http_conn[MAX_FD];
    // 创建epoll_wait函数的传出参数
    epoll_event events[MAX_EVENT_NUMBER];
    while(true) {
        // 9-1 调用epoll_wait, 检测文件描述符的属性
        int num = epoll_wait(epfd, events, MAX_EVENT_NUMBER, -1);
        if (num == -1 && errno != EINTR) {
            perror("epoll_wait");
            break;
        }
        // 9-2 遍历检测到属性变化, 需要处理的文件描述符
        for (int i = 0; i < num; i++) {
            int sockfd = events[i].data.fd;
            if (sockfd == lfd) {
                // 9-2-1 如果是有客户端连接进来
                // 接受连接请求
                struct sockaddr_in caddr;
                socklen_t caddr_len = sizeof(caddr_len);
                int cfd = accept(lfd, (struct sockaddr*)&caddr, &caddr_len);
                if (cfd == -1) {
                    perror("accept");
                    exit(-1);
                }
                // 检查是否连接数已达上限, 若是, 则关掉新连接
                if (http_conn::m_user_count >= MAX_FD) {
                    close(cfd);
                    // 待添加改进: 给客户端返回提示信息:"服务器正忙".
                    continue;
                }
                // 将新连接输入存入users
                users[cfd].init(cfd, caddr);
            } else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                // 9-2-2 如果对方异常断开, 或者发生错误
                users[sockfd].close_conn();
            } else if (events[i].events & EPOLLIN) {
                // 9-2-3 如果需要读数据, 则一次性把所有数据都读完, 并向线程池添加新任务
                if (users[sockfd].read()) {  // 如果成功读完, 则向线程池添加新任务
                    pool->append(users + sockfd);  // append要求的输入是T*, 即http_conn*
                } else {  // 如果读出现失败, 则直接关闭当前连接
                    users[sockfd].close_conn();
                }
            } else if (events[i].events & EPOLLOUT) {
                // 9-2-4 如果可以写数据了, 则一次性把所有数据都写完
                if (!users[sockfd].write()) {  // 如果写出现失败, 也是直接关闭当前连接
                    users[sockfd].close_conn();
                }
            }
        }
    }

    close(epfd);
    close(lfd);
    delete[] users;
    delete pool;

    return 0;
}