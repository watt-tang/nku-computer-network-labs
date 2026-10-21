# NEXUS CHAT 自定义聊天协议设计

## 1. 设计目标

协议运行于 TCP 流式套接字之上，满足课程实验对聊天消息时间戳、多人通信、中文与英文支持，以及对消息语法、语义、时序和处理方式进行完整说明的要求。

设计目标：

1. 解决 TCP 字节流中的消息边界识别问题。
2. 允许客户端先注册用户名，再发送聊天消息。
3. 由服务器统一生成聊天时间戳并转发消息。
4. 允许客户端查询在线用户列表并正常退出。
5. 对畸形帧、非法字段、重复用户名和连接中断作出处理。

## 2. 传输层与编码

- 传输协议：TCP。
- Socket 类型：`SOCK_STREAM`。
- 文本编码：UTF-8。
- 多字节整数：网络字节序（大端序）。
- 时间格式：服务器本地时间 `YYYY-MM-DD HH:MM:SS`。
- 最大协议负载：1 MiB。
- 用户名长度：1–48 个 UTF-8 字节。
- 聊天消息长度：1–4096 个 UTF-8 字节。

## 3. 帧格式（语法）

每一条应用层消息都封装为一个完整帧。

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 字节 | Magic | 固定 ASCII 字节 `NCHT` |
| 4 | 1 字节 | Version | 当前为 `1` |
| 5 | 1 字节 | Type | 消息类型编号 |
| 6 | 2 字节 | Reserved | 当前必须为 `0` |
| 8 | 4 字节 | Payload Length | 负载字节数，32 位大端序 |
| 12 | N 字节 | Payload | 消息负载，长度由 Payload Length 指定 |

接收端必须先读取完整的 12 字节帧头，验证魔数、版本、保留字段、消息类型和长度，再精确读取指定长度的负载。不得假定一次 `recv()` 就能读到完整帧，也不得假定一次 `send()` 就会发送完所有数据。

### 字符串字段格式

协议负载中的每个字符串都编码为：

```text
String := Length(2 bytes, unsigned big-endian) + UTF-8 bytes
```

字符串长度是字节数，不是 Unicode 字符数。空字符串允许用于可选的 GOODBYE 原因；其他业务字段会另行验证是否为空。

## 4. 消息类型及负载语法

| Type | 名称 | Payload 语法 | 方向 |
|---:|---|---|---|
| 1 | `HELLO` | `String(username)` | Client → Server |
| 2 | `WELCOME` | `String(server_time), String(welcome_text)` | Server → Client |
| 3 | `CHAT` | `UInt64(sequence), String(message)` | Client → Server |
| 4 | `CHAT_DELIVERY` | `UInt64(sequence), String(server_time), String(username), String(message)` | Server → All Clients |
| 5 | `SYSTEM` | `String(server_time), String(system_text)` | Server → Clients |
| 6 | `USER_LIST` | `UInt16(count), String(username) × count` | Server → Client(s) |
| 7 | `GOODBYE` | 可选 `String(reason)`；实际客户端发送一个字符串字段 | Client → Server |
| 8 | `ERROR` | `String(error_text)` | Server → Client |
| 9 | `LIST_REQUEST` | 空负载 | Client → Server |

`UInt64(sequence)` 为 8 字节无符号大端序整数；`UInt16(count)` 为 2 字节无符号大端序整数。

## 5. 消息语义

### 5.1 建立连接与注册

1. 服务器创建 TCP Socket，绑定指定端口并调用 `listen()`。
2. 客户端创建 TCP Socket，调用 `connect()`。
3. 服务器 `accept()` 新连接，并为该连接启动一个工作线程。
4. 客户端发送 `HELLO(username)`。
5. 服务器验证用户名长度、控制字符、重复用户名及当前人数上限。
6. 成功时服务器发送 `WELCOME`，随后将客户端加入在线列表；失败时服务器发送 `ERROR` 并关闭连接。
7. 服务器向已有客户端发送加入通知，并广播最新 `USER_LIST`。

用户名在同一时刻必须唯一。服务器最多接纳 256 个已注册客户端。

### 5.2 发送与广播聊天消息

1. 客户端为每条聊天消息分配递增的 64 位序号，从 1 开始。
2. 客户端发送 `CHAT(sequence, message)`。
3. 服务器验证字段完整性、消息长度、控制字符及序号单调递增性。
4. 服务器生成统一时间戳，构造 `CHAT_DELIVERY(sequence, time, username, message)`。
5. 服务器将该帧发送给所有在线客户端，包括发送者。
6. 客户端按帧类型解析并显示时间戳、发送者、序号和消息正文。

序号是应用层完整性观察的辅助信息，不替代 TCP 的可靠传输机制。

### 5.3 查询在线用户

1. 客户端发送无负载的 `LIST_REQUEST`。
2. 服务器从当前在线列表生成 `USER_LIST`。
3. 客户端解析数量及每个用户名并显示。

### 5.4 退出与异常断开

- 正常退出：客户端发送 `GOODBYE`，服务器删除该客户端、关闭连接、广播离开通知并更新在线列表。
- 客户端崩溃或网络断开：服务器的 `recv()` 返回关闭或错误状态，工作线程清理会话并更新在线列表。
- 服务器退出：服务器停止接受新连接，关闭/中止现有会话的阻塞 I/O，等待工作线程结束。
- 畸形帧：帧头无效、版本不支持、长度超过上限、负载字段不完整时，接收函数失败并结束该连接的处理。
- 业务字段错误：服务器发送 `ERROR`；对普通聊天字段错误，服务器不转发该条消息。
- 重复用户名：服务器拒绝注册，不将其加入在线列表。

## 6. 时序示意

```text
Client A                 Server                  Client B
   |                        |                        |
   |------ TCP connect ---->|                        |
   |------ HELLO(Alice) --->|                        |
   |<----- WELCOME ---------|                        |
   |                        |<------ TCP connect ----|
   |                        |<------ HELLO(Bob) -----|
   |                        |------- WELCOME ------->|
   |                        |---- SYSTEM: Bob joined -> A
   |                        |---- USER_LIST -------> A,B
   |------ CHAT(seq=1) ---->|                        |
   |<-- CHAT_DELIVERY ------|------ CHAT_DELIVERY -->|
   |                        |                        |
   |------ GOODBYE -------->|                        |
   |                        |---- SYSTEM: Alice left -> B
   |                        |---- USER_LIST -------> B
```

## 7. 模块划分

### `protocol.h`

- 定义消息类型、帧结构、协议常量。
- 实现 `Writer` / `Reader`，完成整数和 UTF-8 字符串的序列化与反序列化。
- 实现 `send_all()`、`recv_exact()`、`send_frame()`、`recv_frame()`。
- 校验帧头、协议版本、消息类型及负载长度。

### `server.cpp`

- 初始化 Winsock、创建监听 Socket。
- 接收连接并为每个客户端创建一个工作线程。
- 管理在线客户端集合，校验注册与聊天消息。
- 生成时间戳，广播聊天消息、系统通知和在线列表。
- 记录连接、断开、消息序号和发送情况；处理服务器退出。

### `client.cpp`

- 连接服务器并发送注册消息。
- 主线程处理用户输入；接收线程处理服务器推送消息。
- 显示聊天、系统通知、错误、在线用户列表。
- 提供 `/help`、`/list`、`/burst N` 和 `/quit` 命令。
- 根据收到的同一发送者序号观察应用层消息缺口或重复。

## 8. TCP 粘包、半包与丢包分析

TCP 提供有序字节流，而非应用消息边界。一次发送可能被拆成多次接收，多次发送也可能在一次接收中被读取。本协议使用固定长度帧头和负载长度字段确定每一帧的边界；`send_all()` 和 `recv_exact()` 循环直到完成指定字节数或检测到连接失败。

TCP 可能在网络层发生丢包，并通过重传机制恢复。应用层通常不会直接看到 TCP 自动恢复的丢包。实验应结合 Wireshark 检查 TCP 重传、重复 ACK 等迹象，并记录真实观察。`/burst N` 与聊天序号可以辅助确认应用层是否收到了连续的测试消息，但不能独立测出链路层丢包率。
