# webserver
## 项目描述
此项目是基于 Linux 的轻量级多线程 Web 服务器，支持 HTTP 协议，实现了静态资源访问功能。
## 主要工作： 
1. 基于 Socket 通信、有限状态机，开发逻辑单元，处理 HTTP 报文，实现 GET 请求的解析。
2. 基于 I/O 复用技术 Epoll，实现用同步 I/O 模拟 Proactor 模式，开发 I/O 处理单元，进行数据收发，提高服务器的处理请求的速度。
3. 基于基于生产者 - 消费者模型，构建线程池，实现 I/O 处理单元和逻辑单元的通信以及多线程服务，增加并行服务数量。
