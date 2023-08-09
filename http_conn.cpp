#include "http_conn.h"

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站根目录
const char* doc_root = "/home/peng/webserver/resources";

// 将文件描述符设为非阻塞
void setnonblocking(int fd) {
    int flag = fcntl(fd, F_GETFL);
    flag |= O_NONBLOCK;
    fcntl(fd, F_SETFL, flag);
}

// 向epoll实例添加文件描述符
void addfd(int epfd, int fd, bool oneshot, bool edge) {
    // 创建epoll实例, 并将fd以及要检测的事件添加入epoll实例
    struct epoll_event epev;
    epev.data.fd = fd;
    epev.events = EPOLLIN | EPOLLRDHUP;
    if (edge) {
        epev.events |= EPOLLET;
    }
    if (oneshot) {
        epev.events |= EPOLLONESHOT;
    }
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &epev);
    // 将fd设为非阻塞
    setnonblocking(fd);
}

// 从epoll实例删除文件描述符
void removefd(int epfd, int fd) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
}

// 修改epoll实例中要检测的文件描述符的属性, 将原属性修改为event, 
// 注意: 修改时无需添加EPOLLRDHUP, EPOLLONESHOT. 此函数会自动添加.
void modfd(int epfd, int fd, int event) {
    epoll_event epev;
    epev.data.fd = fd;
    epev.events = event | EPOLLRDHUP | EPOLLONESHOT | EPOLLET;
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &epev);
}

// 静态变量, 类内声明, 类外初始化
int http_conn::m_epfd = -1;
int http_conn::m_user_count = 0;

/* 初始化连接相关信息
 * 注意, init函数不同于有参构造函数. 
 * 有参构造函数是对象实例还不存在, 创建一个;
 * init函数是对象实例已经存在了, 但是通过该函数初始化其中的成员. 
 */
void http_conn::init(int sockfd, const sockaddr_in& addr) {
    m_sockfd = sockfd;
    m_addr = addr;
    // 对m_sockfd设置端口复用
    int reuse = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
    // 将m_sockfd添加到epoll实例当中
    addfd(m_epfd, m_sockfd, true, true);  // 需要检测cfd的EPOLLONESHOT事件; 对cfd使用边沿触发
    // 更新用户数量属性
    m_user_count++;
    // 初始化其他信息(使用私有的那个init)
    init();
}

// 初始化其他信息
void http_conn::init() {
    m_check_state = CHECK_STATE_REQUESTLINE;  // 初始化主状态机状态: 解析请求首行
    m_method = GET;

    bzero(m_read_buf, READ_BUFFER_SIZE);  // 清空读缓冲区
    m_checked_index = 0;
    m_start_line = 0;
    m_read_index = 0;
    
    bzero(m_real_file, FILENAME_LEN);
    m_url = 0;
    m_version = 0;
    m_linger = false;
    m_content_length = 0;
    m_host = 0;
    
    bzero(m_write_buf, WRITE_BUFFER_SIZE);
    m_write_index = 0;

    bytes_to_send = 0;
    bytes_have_send = 0;
}

// 关闭这个连接
void http_conn::close_conn() {
    if (m_sockfd != -1) {  // 如果这个连接还没被关闭
        // 将本连接对应的文件描述符从epoll实例中删除
        removefd(m_epfd, m_sockfd);
        // 更新本对象中的相关成员, 包括m_sockfd和m_user_count
        m_sockfd = -1;
        m_user_count--;
    }
}

/* 非阻塞地读. 由于在addfd中, 文件描述符被设置为非阻塞的, 
 * 因此这里要循环调用read, 直至数据被读完, 或对方关闭连接*/
bool http_conn::read() {
    printf(">>>>> 函数http_conn::read开始执行: \n");
    if (m_read_index >= READ_BUFFER_SIZE) {
        return false;
    }
    // 读取到的字节
    int bytes_read = 0;
    while (true) {
        bytes_read = recv(m_sockfd,m_read_buf + m_read_index,READ_BUFFER_SIZE - m_read_index,0);
        printf("bytes_read = %d\n", bytes_read);
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 读完了
                break;
            } else {
                perror("recv");
                return false;
            }
        } else if (bytes_read == 0) {
            // 对方关闭连接
            return false;
        }
        m_read_index += bytes_read;
    }
    printf("读取到了数据:\n");
    printf("-----------------------------\n");
    printf("%s", m_read_buf);
    printf("-----------------------------\n");
    printf(">>>>> 函数http_conn::read执行完毕!\n");
    return true;
}

// 主状态机: 解析HTTP请求
http_conn::HTTP_CODE http_conn::process_read() {
    printf(">>>>> 函数http_conn::process_read开始执行: \n");
    // 1.创建并初始化需要的变量
    // 创建从状态机状态: line_state, 并将其值初始化为LINE_OK(读取到一个完整的行)
    LINE_STATUS line_state = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;

    // 状态机开始运行
    bool flag = true;  // 用于调试的变量, 标示是否是循环的第一次
    char* text = 0;
    while( ((m_check_state == CHECK_STATE_CONTENT) && (line_state = LINE_OK)) 
        || ((line_state = parse_line()) == LINE_OK) ) {  // 如果// 解析到了一行完整的数据; 或者解析到了请求体, 也是完整的数据

        if (flag) {
            printf("process_read: 状态机开始运行, 进入状态循环.\n");
            flag = false;
        }
        
        text = get_line();  // 获取一行数据

        m_start_line = m_checked_index;
        printf("process_read: 获取到请求报文的一个「未处理行」: %s\n",text);

        switch(m_check_state) {
            case CHECK_STATE_REQUESTLINE: { // 请求行
                printf("process_read: 主状态机进入 CHECK_STATE_REQUESTLINE 状态.\n");
                ret = parse_request_line(text);
                printf("process_read: 处理结果, ret = %d\n", ret);
                if (ret == BAD_REQUEST) {  // 如果客户请求出现语法错误, 只能返回
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: {  // 请求头(首部行)
                printf("process_read: 主状态机进入 CHECK_STATE_HEADER 状态.\n");
                ret = parse_headers(text);
                printf("process_read: 解析结果, ret = %d", ret);
                if (ret == BAD_REQUEST) {  // 如果客户请求出现语法错误, 只能返回
                    return BAD_REQUEST;
                } else if (ret == GET_REQUEST) {  // 如果请求头(首部行)都解析完了
                    return do_request(); 
                }
                break;
            }
            case CHECK_STATE_CONTENT: {  // 请求体
                printf("process_read: 主状态机进入 CHECK_STATE_CONTENT 状态.\n");
                ret = parse_content(text);
                printf("process_read: 解析结果, ret = %d", ret);
                if (ret == GET_REQUEST) {
                    return do_request();
                }
                line_state = LINE_OPEN;
                break;
            }
            default: {
                return INTERNAL_ERROR;
            }
        }
    }

    printf(">>>>> 函数http_conn::process_read执行完毕!\n");
    return NO_REQUEST;
}

// 从状态机, 获取HTTP报文中的一行数据, 重点是如何判断一行终结: \r\n
http_conn::LINE_STATUS http_conn::parse_line() {
    // 逐个检查m_read_buf缓冲区中的字符(缓冲区中字符至m_read_index而止)
    printf(">>>>>>>>>> 函数http_conn::process_line开始运行: \n");
    char temp;
    for (; m_checked_index < m_read_index; ++m_checked_index) {
        temp = m_read_buf[m_checked_index];
        if (temp == '\r') {
            if ((m_checked_index + 1) == m_read_index) {
                printf(">>>>>>>>>> 函数http_conn::process_line运行完毕, 返回值: LINE_OPEN. \n");
                return LINE_OPEN;
            } else if (m_read_buf[m_checked_index + 1] == '\n') {
                m_read_buf[m_checked_index] = '\0';
                m_checked_index++;
                m_read_buf[m_checked_index] = '\0';
                m_checked_index++;
                printf(">>>>>>>>>> 函数http_conn::process_line运行完毕, 返回值: LINE_OK. \n");
                return LINE_OK;
            }
            return LINE_BAD;
        } else if (temp == '\n') {
            if ( (m_checked_index > 1) && (m_read_buf[m_checked_index - 1] == '\r') ) {
                m_read_buf[m_checked_index - 1] = '\0';
                m_read_buf[m_checked_index] = '\0';
                m_checked_index++;
                printf(">>>>>>>>>> 函数http_conn::process_line运行完毕, 返回值: LINE_OK. \n");
                return LINE_OK;
            }
            printf(">>>>>>>>>> 函数http_conn::process_line运行完毕, 返回值: LINE_BAD. \n");
            return LINE_BAD;
        }
    }
    printf(">>>>>>>>>> 函数http_conn::process_line运行完毕, 返回值: LINE_OPEN. \n");
    return LINE_OPEN;
}

// 子函数: 解析请求行，获取：所请求的方法，目标URL，HTTP版本
// 示例请求行：GET / HTTP/1.1
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    printf(">>>>>>>>>> 函数http_conn::parse_request_line开始运行:\n");
    // 获取所请求资源的名称, 即目标url: m_url
    m_url = strpbrk(text, " \t");
    if (! m_url) { 
        printf(">>>>>>>>>> 函数http_conn::parse_request_line运行完毕, 返回值: BAD_REQUEST. \n");
        return BAD_REQUEST;
    }
    *m_url = '\0';
    m_url++;
    // 获取请求方法m_method
    char* method = text;
    if (strcasecmp(method,"GET")==0) {
        m_method = GET;
    } else {  // 当前只处理GET方法
        printf(">>>>>>>>>> 函数http_conn::parse_request_line运行完毕, 返回值: BAD_REQUEST. \n");
        return BAD_REQUEST;
    }
    // 获取HTTP版本m_version
    m_version = strpbrk(m_url, " \t");
    if (!m_version) {  // 如果m_version为NULL
        printf(">>>>>>>>>> 函数http_conn::parse_request_line运行完毕, 返回值: BAD_REQUEST. \n");
        return BAD_REQUEST;
    }
    *m_version = '\0';
    m_version++;
    if (strcasecmp(m_version, "HTTP/1.1") != 0) {  // 当前只接受HTTP/1.1版本
        printf(">>>>>>>>>> 函数http_conn::parse_request_line运行完毕, 返回值: BAD_REQUEST. \n");
        return BAD_REQUEST;
    }

    // 继续解析目标URL
    if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    if (!m_url || m_url[0] != '/') {
        printf(">>>>>>>>>> 函数http_conn::parse_request_line运行完毕, 返回值: BAD_REQUEST. \n");
        return BAD_REQUEST;
    }

    // 更新主状态机的状态
    m_check_state = CHECK_STATE_HEADER;  // 检查完请求行之后, 主状态机状态变为处理首部行

    printf(">>>>>>>>>> 函数http_conn::parse_request_line运行完毕, 返回值: NO_REQUEST. \n");
    return NO_REQUEST;
}

// 子函数: 解析请求头(首部行)
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {
    if( text[0] == '\0' ) {
        // 遇到空行，表示头部字段解析完毕
        // 如果HTTP请求有消息体，则状态机转移到CHECK_STATE_CONTENT状态, 返回NO_REQUEST, 继续解析
        // 否则说明我们已经得到了一个完整的HTTP请求, 返回GET_REQUEST, 继续解析
        if ( m_content_length != 0 ) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    } else if ( strncasecmp( text, "Connection:", 11 ) == 0 ) {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            m_linger = true;
        }
    } else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);
    } else if ( strncasecmp( text, "Host:", 5 ) == 0 ) {
        // 处理Host头部字段
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    } else {
        printf( "parse_headers: 遇到了不在处理范围内的首部行(将忽略): %s\n", text );
    }
    return NO_REQUEST;
}

// 子函数: 解析请求体
// 我们没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content(char* text) {
    if ( m_read_index >= ( m_content_length + m_checked_index ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;  // 解析完成
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request() {
    
    // 构造所请求的资源的路径
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    // 获取所请求文件的相关状态信息, 若失败则返回NO_RESOURCE
    if (stat(m_real_file, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }

    // 检查Others用户对所请求资源的读权限
    if (!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;  // 如果没有权限, 返回此值
    }

    // 判断是否是目录, 如果是, 也不能返回任何内容
    if (S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open(m_real_file, O_RDONLY);
    // 创建内存映射
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

// 对内存映射区执行munmap操作
void http_conn::unmap() {
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// 往写缓冲区m_write_buf中写入待发送的数据
bool http_conn::add_response( const char* format, ... ) {
    // 检查写缓冲区中是否还有空位, 如果没有, 则不往里写数据
    if( m_write_index >= WRITE_BUFFER_SIZE ) {
        return false;
    }
    // 读取输入参数, 并向写缓冲区中按一定格式写数据
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_index, WRITE_BUFFER_SIZE - 1 - m_write_index, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_index ) ) {
        return false;
    }
    // 更新m_write_index
    m_write_index += len;
    va_end( arg_list );
    return true;
}

// 构造应答报文: 添加状态行
bool http_conn::add_status_line(int status, const char* title) {
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

// 构造应答报文: 添加首部行
bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}

// 构造应答报文: 添加应答的具体内容
bool http_conn::add_content(const char* content) {
    return add_response( "%s", content );
}

// add_headers的子函数
bool http_conn::add_content_length( int content_len ) {
    return add_response( "Content-Length: %d\r\n", content_len );
}

// add_headers的子函数
bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

// add_headers的子函数
bool http_conn::add_linger() {
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

// add_headers的子函数
bool http_conn::add_blank_line() {
    return add_response( "%s", "\r\n" );
}

// 根据process_read处理HTTP请求的结果，决定返回客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    printf(">>>>> 函数http_conn::process_write开始执行: \n");
    switch (ret) {
        case INTERNAL_ERROR: 
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form)) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE: 
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST: 
            add_status_line(200, ok_200_title);
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_index;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_index + m_file_stat.st_size;
            return true;
        default:
            return false;
    }

    // 跳到这说明状态码是FILE_REQUEST以外的, 应答体内容已经写入m_write_buf了
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_index;
    m_iv_count = 1;
    bytes_to_send = m_write_index;
    printf(">>>>> 函数http_conn::process_write执行完毕!\n");
    return true;
}

// 由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process() {
    printf(">>>>> 函数http_conn::process开始执行: \n");
    // 解析HTTP请求
    HTTP_CODE read_ret = process_read();
    // 如果此时请求不完整，则继续读取客户数据
    if (read_ret == NO_REQUEST) {
        modfd(m_epfd,m_sockfd,EPOLLIN);
        return;
    }
    // 生成响应
    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        close_conn();
    }
    modfd(m_epfd,m_sockfd,EPOLLOUT);
    printf(">>>>> 函数http_conn::process执行完毕!\n");
}

// 非阻塞地写
bool http_conn::write() {
    printf(">>>>> 函数http_conn::write开始执行: \n");
    printf("写数据, 状态行和首部行: \n");
    printf("----------------------------------------------------\n");
    printf("%s", m_write_buf);
    printf("----------------------------------------------------\n");

    int temp = 0;

    // 如果待发送的字节数为0, 则此次响应结束
    if (bytes_to_send == 0) {
        modfd(m_epfd, m_sockfd, EPOLLIN);  // 重新开始读取客户端发来的数据
        init();  // 重新初始化报文处理相关参数, 为下次处理报文做准备
        printf(">>>>> 函数http_conn::write执行完毕, 返回: true. \n");
        return true;
    }

    while(1) {
        // 将应答报文写入通信文件描述符中
        temp = writev(m_sockfd, m_iv, m_iv_count);
        // 如果报错
        if (temp <= -1) {
            // 如果报EAGAIN错误, 表明TCP写缓冲没有空间, 则等待下一轮EPOLLOUT事件
            // 虽然在此期间，服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if (errno == EAGAIN) {
                modfd(m_epfd, m_sockfd, EPOLLOUT);
                printf(">>>>> 函数http_conn::write执行完毕, 返回: true. \n");
                return true;
            }
            // 如果是其他错误, 那说明写数据出现其他错误, 就不写了, 直接返回
            unmap();
            printf(">>>>> 函数http_conn::write执行完毕, 返回: false. \n");
            return false;
        }
        // 如果不报错, 说明成功写入, 则更新相关变量
        bytes_to_send -= temp;
        bytes_have_send += temp;
        if (bytes_have_send >= m_iv[0].iov_len) {  // 2.如果m_write_buf发完了, m_file_address发了点没发完
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_index);
            m_iv[1].iov_len = bytes_to_send;
        } else {  // 1.如果m_write_buf发了一部分, 还没发完
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
            // 也可以写成: m_iv[0].iov_len = m_write_index - bytes_have_send;
        }

        if (bytes_to_send <= 0) {
            // 3.如果数据都发送完了
            unmap();
            modfd(m_epfd, m_sockfd, EPOLLIN);

            if (m_linger) {
                init();
                printf(">>>>> 函数http_conn::write执行完毕, 返回: true. \n");
                return true;
            } else {
                printf(">>>>> 函数http_conn::write执行完毕, 返回: false. \n");
                return false;
            }
        }
    }
}