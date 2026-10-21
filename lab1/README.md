# Lab 1 — NEXUS CHAT（TCP 多人聊天程序）

本项目依据课程《计算机网络》实验说明 PPT 的“编程实验 1：利用流式套接字编写聊天程序”要求设计。程序使用 **C++17 + Windows Winsock2 原生 Socket API + TCP + 多线程**，提供命令行客户端与服务器，支持中英文聊天、服务器时间戳、多人在线、在线用户列表、运行日志和消息序号检查。

> 本次已在 Windows 使用 MinGW-w64 实际运行 `build.bat`。构建及自动化测试结果见 `PRIVATE_CHAT_TEST_RESULTS.md`。

## 1. 文件结构

```text
Lab1_Chat_Program/
├── server.cpp             # TCP 服务器；每个客户端由独立线程处理
├── client.cpp             # TCP 客户端；接收线程与输入主线程分离
├── protocol.h             # 自定义帧协议、编码/解码、send-all/recv-exact
├── build.bat              # 自动识别 MSVC 或 MinGW-w64 并编译
├── run_demo.bat           # 启动本机服务器及 Alice、Bob 两个客户端
├── CMakeLists.txt         # 可选 CMake 构建配置
├── PROTOCOL_DESIGN.md     # 协议语法、语义、时序与错误处理
├── TEST_PLAN.md           # 验收测试清单与丢包观察方法
├── docs/
│   ├── server_flowchart.png  # 服务器流程图
│   ├── server_flowchart.dot  # 服务器流程图源文件
│   ├── client_flowchart.png  # 客户端流程图
│   └── client_flowchart.dot  # 客户端流程图源文件
├── 实验报告.md            # 可编辑的报告正文
├── 实验报告.docx          # Word 版报告草稿
└── bin/
    └── README.txt         # EXE 生成位置说明
```

## 2. 编译环境

- 操作系统：Windows 10 / Windows 11
- 编程语言：C++17
- 网络接口：Winsock2 基本 Socket API
- 编译器：Visual Studio MSVC，或 MinGW-w64 GCC
- 终端：Windows Terminal / PowerShell / CMD，建议使用 Windows Terminal
- 端口：默认 TCP `8888`

### 方式 A：MSVC

1. 安装 Visual Studio 的“使用 C++ 的桌面开发”工作负载。
2. 打开 **x64 Native Tools Command Prompt for VS**。
3. 进入本文件夹。
4. 执行：

```bat
build.bat
```

### 方式 B：MinGW-w64

确保 `g++` 已加入 PATH，然后在 CMD 或 PowerShell 中执行：

```bat
build.bat
```

构建成功后应生成：

```text
bin\server.exe
bin\client.exe
```

如果编译失败，请保留完整编译错误信息并修复后再提交；不要把未成功生成的文件改名成 `.exe`。

## 3. 本机运行

### 终端 1：启动服务器

```bat
bin\server.exe 8888
```

服务器绑定 `0.0.0.0:8888`，可接受本机及允许通过防火墙访问的局域网客户端。按 `Ctrl+C` 关闭服务器。

### 终端 2：启动 Alice

```bat
bin\client.exe 127.0.0.1 8888 Alice
```

### 终端 3：启动 Bob

```bat
bin\client.exe 127.0.0.1 8888 Bob
```

### 快速演示

编译成功后，也可以双击 `run_demo.bat`。它会启动服务器以及 Alice、Bob 两个客户端。需要展示三人聊天时，再打开一个终端运行：

```bat
bin\client.exe 127.0.0.1 8888 Charlie
```

## 4. 客户端命令

| 命令 | 功能 |
|---|---|
| `/help` | 显示命令说明 |
| `/list` | 向服务器请求实时在线用户列表 |
| `/msg 用户名 正文` | 定向私聊，保留正文内部空格 |
| `/burst N` | 发送 1–500 条带编号的测试消息，便于检查应用层消息连续性 |
| `/quit` | 正常退出聊天 |
| 其他输入 | 作为聊天消息发送，支持中英文 |

用户名限制为 1–48 个 UTF-8 字节；聊天消息限制为 1–4096 个 UTF-8 字节。服务器为聊天消息统一生成时间戳。服务器日志记录连接、断开、消息序号及成功执行的 Socket 发送次数。

## 5. 多人聊天测试

### 私聊完整示例

在 `lab1` 目录分别打开四个终端执行：

```bat
bin\server.exe 8888
bin\client.exe 127.0.0.1 8888 Alice
bin\client.exe 127.0.0.1 8888 Bob
bin\client.exe 127.0.0.1 8888 Charlie
```

在 Alice 中输入：

```text
/msg Bob Hello Bob
/msg Bob 你好，这条消息只有你和我能看到。 Hello  Bob
/msg Nobody 测试目标不存在
```

Alice 显示带时间戳及私聊序号的 `[PRIVATE OUT] Alice -> Bob`，Bob 显示 `[PRIVATE IN] Alice -> Bob`，Charlie 不收到正文。普通文本仍向全体用户广播。目标匹配区分大小写；不支持向带空格的用户名发私聊。向自己私聊仅显示一次。

`/msg`、`/msg Bob` 和空白正文会提示用法，不发送无效帧。目标不存在、离线或发送失败会向发送者返回清晰错误。正文内部连续空格保留。

协议新增 PRIVATE_CHAT=10（请求）和 PRIVATE_DELIVERY=11（向双方投递），字段详见 `PROTOCOL_DESIGN.md`。私聊使用独立递增序号，群聊及 `/burst` 原有序号检查保持不变。

私聊没有额外应用层送达确认机制；Socket 发送成功或发送者看到回显都不能保证对方已经阅读。功能实现服务器定向转发，不提供端到端加密，服务器可以读取正文。

1. 启动服务器。
2. 依次启动 Alice、Bob、Charlie 三个客户端。
3. 在 Alice 中发送 `大家好！`，检查 Bob 和 Charlie 是否都收到。
4. 在 Bob 中发送 `Hello Alice!`，检查其他客户端是否收到。
5. 输入 `/list`，检查服务器返回的在线用户名列表。
6. 输入 `/burst 100`，检查其他客户端是否收到完整序列；同时观察服务器日志。
7. 让其中一个客户端输入 `/quit`，检查其他客户端是否收到离开通知、在线列表是否更新。
8. 重复测试中文、英文、多人同时发送以及客户端异常关闭的情况。

## 6. 丢包观察

课程要求“观察是否有数据包的丢失”。建议使用 Wireshark 实际抓包，而不是只根据聊天窗口推断：

1. 在本机选择实际承载流量的网络接口开始抓包。若流量仅经回环地址传输，确认 Wireshark 是否能捕获本机回环流量；必要时安装并使用 Npcap Loopback Adapter。
2. 显示过滤器可使用 `tcp.port == 8888`。
3. 查看 TCP 序列号、确认号、重传和重复 ACK 等信息；也可使用 `tcp.analysis.retransmission` 过滤重传报文。
4. 在聊天客户端执行 `/burst 100`，记录抓包时间、观察到的重传现象及消息接收情况。
5. 在报告中写入真实观察结果和截图。

**解释时要准确：** TCP 会通过确认、重传等机制尽量保证应用层收到有序字节流。因此，“应用层消息完整到达”不代表链路从未丢包；“没有观察到重传”也不能证明整个测试过程中绝对没有丢包。`/burst` 的序号检查主要用于发现应用层消息缺口或重复，不能单独测出网卡或链路层丢包率。

## 7. 协议与实现说明

完整设计见 `PROTOCOL_DESIGN.md`。协议采用 12 字节固定帧头，帧头中包含魔数、版本、消息类型和大端序负载长度。所有可变长度字符串均以 16 位大端序字节长度为前缀。接收端先读取完整帧头，再按长度读取完整负载；发送端循环调用 `send()` 直到发送完毕，避免错误地假定一次 `send()` 对应一次 `recv()`。

程序直接调用 `socket()`、`bind()`、`listen()`、`accept()`、`connect()`、`send()`、`recv()` 等 Winsock 基本函数，没有使用封装网络通信的 Socket 类或网络框架。

## 8. 提交前检查

- [ ] `build.bat` 在目标 Windows 环境编译成功。
- [ ] `bin\server.exe` 与 `bin\client.exe` 均由本项目源码实际编译生成。
- [ ] 两个以上客户端可同时聊天；演示时建议使用三个客户端。
- [ ] 中英文均能正确发送、接收和显示。
- [ ] 聊天消息带服务器生成的时间戳。
- [ ] `/list`、`/help`、`/quit`、`/burst N` 均可正常工作。
- [ ] 完成 Wireshark 抓包并记录实际观察结果。
- [ ] 报告中的环境、结果、截图均为本人真实运行后填写。
- [ ] 按课程要求打包源码、可执行文件和报告，命名为 `学号-姓名.zip`。
- [ ] 在 DDL 前完成演示预约。

本项目未实现课程未要求的 GUI、文件传输、数据库或账号密码系统，避免偏离实验范围。

### Windows/MinGW 编译错误说明

Windows SDK 头文件可能定义 `ERROR` 宏，因此源码中的协议枚举使用 `Type::ERROR_MESSAGE` 作为 C++ 标识符；在线协议的消息类型仍是编号 `8`，协议文档中的名称仍为 `ERROR`。如果你此前运行旧版本时在 `protocol.h` 的 `ERROR = 8` 处编译失败，请使用本次修正版源码重新编译。
