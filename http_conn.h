#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <string.h>
#include "locker.h"

class http_conn {
public: 
    // 属性
    static int m_epfd;  // 所有文件描述符均被添加到通过一个epoll实例
    static int m_user_count;  // 记录用户的数量
    static const int FILENAME_LEN = 200;  // 文件名的最大长度
    static const int READ_BUFFER_SIZE = 2048;  // 读缓冲区大小
    static const int WRITE_BUFFER_SIZE = 1024;  // 写缓冲区大小

    // HTTP请求方法，这里只支持GET
    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT};
    
    /*
        解析客户端请求时，主状态机的状态
        CHECK_STATE_REQUESTLINE: 当前正在分析请求行
        CHECK_STATE_HEADER: 当前正在分析头部字段
        CHECK_STATE_CONTENT: 当前正在解析请求体
    */
    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };
    
    /*
        服务器处理HTTP请求的可能结果，报文解析的结果
        NO_REQUEST          :   请求不完整，需要继续读取客户数据
        GET_REQUEST         :   表示将所有请求头(首部行)都解析完了, 获得了一个完整的客户请求
                                (不管有没有请求体, 只要将请求头解析完了, 就是GET_REQUEST状态)
        BAD_REQUEST         :   表示客户请求语法错误
        NO_RESOURCE         :   表示服务器没有资源
        FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
        FILE_REQUEST        :   文件请求,获取文件成功
        INTERNAL_ERROR      :   表示服务器内部错误
        CLOSED_CONNECTION   :   表示客户端已经关闭连接了
    */
    enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };
    
    /* 
        从状态机的三种可能状态，即行的读取状态，分别表示
        LINE_OK: 读取到一个完整的行, 待解析
        LINE_BAD: 读取到了一个有语法错误的行
        LINE_OPEN: 目前缓冲区中的行数据尚且不完整
        这个状态, 是本类中parse_line()函数的返回值.
    */
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };
public:
    http_conn() {}  // 构造函数
    ~http_conn() {}  // 析构函数
public:
    void init(int sockfd, const sockaddr_in& addr);  // 初始化连接相关的信息
    void close_conn();  // 关闭这个连接
    bool read();  // 非阻塞地读数据
    void process();  // 处理客户端请求并响应
    bool write();  // 非阻塞地写数据
    
private:
    int m_sockfd;  // 这个HTTP任务对应的socket
    sockaddr_in m_addr;  // 通信的socket地址

    char m_read_buf[READ_BUFFER_SIZE];  // 读缓冲区
    int m_read_index;  // 标识读m_read_buf中已经读入的客户端数据的字节数
    int m_start_line;  // 当前正在解析的行的起始位置
    int m_checked_index;  // 当前正在分析的字符在读缓冲区的位置

    CHECK_STATE m_check_state;  // 主状态机当前所处状态
    METHOD m_method;  // 请求方法

    char m_real_file[FILENAME_LEN];  // 客户所请求的文件的完整路径, 等于doc_root + m_url
    char* m_url;  // 所请求文件的名字
    char* m_version;  // HTTP协议版本，只支持HTTP1.1
    char* m_host;  // 主机名
    bool m_linger;  // 指示HTTP请求是否要保持连接
    int m_content_length;  // HTTP请求体的长度
    
    char m_write_buf[WRITE_BUFFER_SIZE];  // 写缓冲区
    int m_write_index;  // 写缓冲区中待发送的字节数 = 写缓冲区中最后一个字符的下一个位置的索引
    struct stat m_file_stat;  // 目标文件的状态. 通过该变量可判断文件是否存在, 是否为目录, 是否可读, 并获取文件大小等信息
    char* m_file_address;  // 客户所请求的文件被mmap映射到内存中的起始位置
    struct iovec m_iv[2];  // writev的输入参数, 保存两块分散内存的内容, 一个是m_write_buf, 保存应答报文的状态行和首部行, 一个是m_file_address, 保存应答报文的具体返回内容
    int m_iv_count;  // writev的输入参数, m_iv数组的长度

    int bytes_to_send;
    int bytes_have_send;

    void init();  // 初始化其他信息
    HTTP_CODE process_read();  // 解析HTTP请求报文
    bool process_write(HTTP_CODE ret);  // 构造HTTP应答报文

    // 下面这组函数被process_read调用, 用以解析HTTP请求
    LINE_STATUS parse_line();  // 子函数: 从缓冲区中读取一行
    HTTP_CODE parse_request_line(char* text);  // 子函数: 解析请求首行
    HTTP_CODE parse_headers(char* text);  // 子函数: 解析请求头(首部行)
    HTTP_CODE parse_content(char* text);  // 子函数: 解析请求体
    inline char* get_line() { return m_read_buf + m_start_line; }
    HTTP_CODE do_request();  // 子函数: 找到客户端所请求的文件, 将其映射到内存当中

    // 下面这一组函数被process_write调用以填充HTTP应答
    void unmap();
    bool add_response(const char* format, ...);
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_length);
    bool add_content(const char* content);
    bool add_content_length( int content_length );
    bool add_content_type();
    bool add_linger();
    bool add_blank_line();
};

#endif