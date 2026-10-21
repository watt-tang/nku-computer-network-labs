# Lab 1 验收测试清单

请在 Windows 实际编译、运行后逐项测试。不要把未执行的测试写成“通过”。

| 编号 | 测试目标     | 操作                              | 预期现象                                  | 实测结果 |
| ---- | ------------ | --------------------------------- | ----------------------------------------- | -------- |
| T01  | 服务器启动   | `bin\server.exe 8888`             | 监听 `0.0.0.0:8888`，无错误退出           | 待实测   |
| T02  | 单客户端注册 | 启动 Alice                        | 显示 WELCOME 与连接信息                   | 待实测   |
| T03  | 多客户端注册 | 再启动 Bob、Charlie               | 三个客户端在线，在线列表更新              | 待实测   |
| T04  | 中英文消息   | 发送“大家好！”和“Hello!”          | 其他在线客户端显示完整文本及时间戳        | 待实测   |
| T05  | 多人广播     | Alice 发消息，Bob 与 Charlie 接收 | 所有在线客户端均收到同一条消息            | 待实测   |
| T06  | 在线列表     | 输入 `/list`                      | 显示当前在线用户名                        | 待实测   |
| T07  | 退出清理     | Bob 输入 `/quit`                  | 其他客户端收到离开通知，在线列表更新      | 待实测   |
| T08  | 重复用户名   | 两个客户端都用 Alice 登录         | 第二个登录收到 ERROR，不进入在线列表      | 待实测   |
| T09  | 连续消息     | Alice 输入 `/burst 100`           | 观察消息序号连续性和服务器日志            | 待实测   |
| T10  | 中文长消息   | 发送一条接近长度限制的中文消息    | 合法消息正确传输，超长消息被拒绝          | 待实测   |
| T11  | 异常断开     | 直接关闭一个客户端窗口            | 服务器清理该会话，其他客户端在线列表更新  | 待实测   |
| T12  | TCP 抓包     | Wireshark 过滤 `tcp.port == 8888` | 能检查 TCP 连接、数据段、ACK 和可能的重传 | 待实测   |

## 流程图建议

报告中可以使用以下逻辑绘制服务器流程图：

1. 初始化 Winsock。
2. 创建 TCP Socket。
3. `bind()` → `listen()`。
4. `accept()` 获取新连接。
5. 创建客户端会话和工作线程。
6. 工作线程读取 HELLO 并验证用户名。
7. 进入消息循环：CHAT / LIST_REQUEST / GOODBYE / 异常。
8. 对 CHAT 生成时间戳并广播；对 LIST_REQUEST 返回用户列表。
9. 连接结束后清理会话、广播离开通知、更新在线列表。
10. 服务器停止时关闭会话并回收工作线程。

客户端流程图建议：

1. 初始化 Winsock 并输入连接参数。
2. 创建 Socket，连接服务器。
3. 发送 HELLO 并等待 WELCOME。
4. 启动接收线程。
5. 主线程读取输入：普通消息发送 CHAT；`/list` 发送 LIST_REQUEST；`/quit` 发送 GOODBYE。
6. 接收线程解析 CHAT_DELIVERY、SYSTEM、USER_LIST、ERROR 并显示。
7. 断开连接，等待线程结束，释放 Socket 和 Winsock。

## Wireshark 观察记录模板

- 日期/时间：【填写实际时间】
- 测试接口：【填写网卡或 Npcap Loopback Adapter】
- 过滤条件：`tcp.port == 8888`
- 测试操作：【例如 Alice 执行 `/burst 100`】
- 应用层结果：【记录各客户端实际接收的消息数量及是否出现序号提示】
- TCP 观察：【记录是否观察到重传、重复 ACK 或其他异常】
- 结论：【基于真实抓包证据填写，不要将“未观察到”写成“绝对不存在”】
- 截图文件名：【填写实际截图名】

  cd D:\dasanshangbaogao\jiwang\lab1
  .\build.bat
  .\bin\server.exe 8888

  cd D:\dasanshangbaogao\jiwang\lab1
  .\bin\client.exe 127.0.0.1 8888 Alice

cd D:\dasanshangbaogao\jiwang\lab1
.\bin\client.exe 127.0.0.1 8888 Bob

cd D:\dasanshangbaogao\jiwang\lab1
.\bin\client.exe 127.0.0.1 8888 Charlie

捕获过滤器使用 `tcp port 8888`，显示过滤器使用 `tcp.port == 8888`，两者不能混用。


| 操作            | 输入内容                                         | 预期现象                |
| --------------- | ------------------------------------------------ | ----------------------- |
| Alice发送中文   | `<span>大家好，我是 </span><span>Alice！</span>` | Bob、Charlie收到消息    |
| Bob发送英文     | `<span>Hello </span><span>everyone!</span>`      | Alice、Charlie收到消息  |
| Charlie查询用户 | `<span>/list</span>`                             | 显示Alice、Bob、Charlie |
| Alice查看帮助   | `<span>/help</span>`                             | 显示支持的命令          |
| Alice连续发送   | `<span>/burst </span><span>100</span>`           | 连续发送100条测试消息   |
