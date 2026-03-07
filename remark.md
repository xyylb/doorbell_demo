ESP32 端 WebRTC 语音通信交互流程（控制台指令版）

原项目是门铃项目，需要改成语音通话项目，所以修改后不要说门铃XXX

原生是websocket+wifi+components_esp/esp_peer闭源模块实现的webrtc语音通话(支持视频，本项目不需要，已经屏蔽了)
现在需要改造成4G(AT模式)+MQTT(客户端使用components/esp_ml307)+components_esp/esp_peer闭源模块，实现webrtc语音通话

原有的wifi模式sdp获取关键点总结

  1. SDP由底层自动生成: 调用 esp_peer_new_connection() 后,ESP-PEER库会自动:
    - 收集本地媒体能力(音频编解码器、采样率等)
    - 收集ICE候选地址
    - 生成符合WebRTC标准的SDP字符串
  2. 通过回调获取: SDP生成完成后,通过 pc_on_msg() 回调函数传递给上层
  3. SDP内容: 包含:
    - 媒体描述(m=audio)
    - 编解码器信息(OPUS/G711)
    - ICE候选地址
    - DTLS指纹
    - 其他WebRTC协商参数
  4. 获取时机:
    - 主动呼叫: 调用 esp_webrtc_enable_peer_connection(true) 后自动生成offer
    - 接听来电: 收到对方offer后,调用 esp_webrtc_enable_peer_connection(true) 生成answer


一、流程核心定位
基于控制台指令交互，实现 ESP32 端全手动控制的 WebRTC 语音通话流程：通过输入 offer/answer/reject/close 等指令触发对应信令发送，
接收远端 offer 时触发铃响提示，复用已有 4G 模块和 MQTT 客户端能力。
二、核心前置准备（初始化流程）
在控制台指令交互前，完成基础环境初始化，为后续指令操作提供支撑：
4G 网络初始化：启动 4G 模块，完成网络注册、数据链路建立，输出 “4G 网络就绪”；
MQTT 客户端初始化：
连接指定 MQTT 服务器，完成用户名 / 密码认证；
自动拼接本设备上行主题（/voice/project/{设备ID}/up）和下行主题（/voice/project/{设备ID}/down）；
订阅下行主题，开启信令监听，输出 “MQTT 信令通道就绪”；
ICE 凭证获取：
自动发送 ice_request 信令至 MQTT 上行主题；
接收信令服务器返回的 ice_response 信令，解析并配置 STUN/TURN 服务器，输出 “ICE 凭证配置完成”；
音频设备初始化：启动麦克风 / 扬声器检测，输出 “音频设备就绪（麦克风：可用，扬声器：可用）”；
通话状态初始化：默认进入 IDLE（空闲）状态，控制台输出 “ESP32 语音终端就绪，支持指令：offer/answer/reject/close”。
三、完整控制台指令交互流程（C++ 风格状态机驱动）
（一）场景 1：ESP32 主动发起呼叫（输入 offer 指令）
表格
步骤	模块动作（C++ 逻辑）	控制台输出
1	用户在控制台输入 offer {Web端ID}（如 offer web_001），指令解析模块校验格式（必须包含目标 Web 端 ID）	无（等待指令处理）
2	通话状态机校验当前状态为 IDLE，允许发起呼叫，状态切换为 CALLING（呼叫中）	“当前状态：呼叫中，目标 Web 端：web_001”
3	WebRTC 核心模块生成 Offer SDP 数据	无（底层处理）
4	信令协议模块封装 offer 信令：
- type: "offer"
- timestamp: 当前时间戳
- target: {Web端ID}
- sdp: 生成的SDP字符串	无（底层封装）
5	MQTT 信令模块以 QoS 1 级别将 offer 信令发布至上行主题	“已发送呼叫请求（offer）至 Web 端：web_001”
6	启动 30s 超时定时器，等待 Web 端回复	“等待 Web 端应答（超时时间：30s）”
7-1	若超时未收到 answer 信令：
- 状态机切换为 TIMEOUT（超时）
- 发送 timeout 信令至 Web 端
- 状态机恢复为 IDLE	“呼叫超时，已终止呼叫，当前状态：空闲”
7-2	若收到 Web 端 reject 信令：
- 状态机切换为 REJECTED（被拒接）
- 状态机恢复为 IDLE	“Web 端拒绝接听，当前状态：空闲”
7-3	若收到 Web 端 answer 信令：
- 解析 SDP 并配置到 WebRTC 对等连接
- 开始收集本地 ICE Candidate，每收集一个就封装 candidate 信令发送
- 状态机切换为 IN_CALL（通话中）
- 启动音频采集 / 播放	“已建立通话连接，当前状态：通话中（可输入 close 挂断）”
（二）场景 2：ESP32 接收呼叫（收到远端 offer 信令）
表格
步骤	模块动作（C++ 逻辑）	控制台输出
1	MQTT 信令模块收到 Web 端 / 其他设备发来的 offer 信令，校验 target 为本设备 ID	无（底层校验）
2	信令协议模块解析 offer 信令，提取 SDP 数据和发起方 ID	无（底层解析）
3	通话状态机校验当前为 IDLE，状态切换为 INCOMING（来电中）	“收到来电请求，发起方：{发起方 ID}”
4	音频模块播放铃音（持续循环），控制台提示操作指令	“铃响中，可输入：answer（接听）/reject（拒接）”
5-1	用户输入 reject：
- 状态机切换为 REJECTED
- 封装 reject 信令发送至发起方
- 停止铃音
- 状态机恢复为 IDLE	“已拒绝来电，当前状态：空闲”
5-2	用户输入 answer：
- 停止铃音
- WebRTC 核心模块生成 Answer SDP 数据
- 封装 answer 信令发送至发起方
- 开始收集本地 ICE Candidate 并发送
- 状态机切换为 IN_CALL
- 启动音频采集 / 播放	“已接听来电，当前状态：通话中（可输入 close 挂断）”
5-3	超时未输入指令（15s）：
- 自动发送 timeout 信令
- 停止铃音
- 状态机恢复为 IDLE	“来电超时未应答，当前状态：空闲”
（三）场景 3：通话中挂断（输入 close 指令）
表格
步骤	模块动作（C++ 逻辑）	控制台输出
1	用户在控制台输入 close（通话中任意时刻），指令解析模块校验当前状态为 IN_CALL	无（等待指令处理）
2	音频模块停止采集 / 播放，释放音频资源	无（底层处理）
3	信令协议模块封装 bye 信令（type: "bye"，target: 通话对端ID）	无（底层封装）
4	MQTT 信令模块发送 bye 信令至对端	“已发送挂断请求（bye）至对端：{对端 ID}”
5	WebRTC 核心模块关闭对等连接，清理 ICE Candidate 等数据	无（底层处理）
6	通话状态机切换为 IDLE	“通话已挂断，当前状态：空闲”
（四）场景 4：接收远端挂断信令（被动挂断）
表格
步骤	模块动作（C++ 逻辑）	控制台输出
1	MQTT 信令模块收到对端 bye/reject/timeout 信令	无（底层接收）
2	音频模块立即停止采集 / 播放	无（底层处理）
3	WebRTC 核心模块关闭对等连接	无（底层处理）
4	通话状态机切换为 IDLE	“对端已挂断 / 拒接 / 呼叫超时，当前状态：空闲”
四、C++ 核心模块交互逻辑（无代码，纯流程）
1. 指令解析模块
监听控制台输入，按 “指令 + 参数” 格式拆分输入内容（如 offer web_001 拆分为指令 offer、参数 web_001）；
校验指令合法性：仅允许 offer/answer/reject/close，且在对应状态下可执行（如 answer 仅在 INCOMING 状态有效）；
合法指令转发至通话状态机，非法指令输出提示（如 “当前状态为空闲，无法执行 answer 指令”）。
2. 通话状态机模块（核心驱动）
表格
当前状态	允许执行指令	禁止执行指令	状态切换触发条件
IDLE（空闲）	offer	answer、reject、close	执行 offer → CALLING；收到 offer → INCOMING
CALLING（呼叫中）	close	offer、answer、reject	收到 answer → IN_CALL；收到 reject / 超时 → IDLE；执行 close → IDLE
INCOMING（来电中）	answer、reject、close	offer	执行 answer → IN_CALL；执行 reject / 超时 → IDLE；执行 close → IDLE
IN_CALL（通话中）	close	offer、answer、reject	执行 close → IDLE；收到 bye → IDLE
3. 模块间交互规则
指令解析模块 → 通话状态机：仅传递 “指令 + 上下文（如目标 ID）”，不处理业务逻辑；
通话状态机 → 其他模块：根据指令和状态，下发 “动作指令”（如 “生成 Offer SDP”“发送 bye 信令”）；
底层模块（MQTT / 音频 / WebRTC）→ 通话状态机：仅反馈 “动作结果”（如 “信令发送成功”“音频启动失败”），由状态机决定后续状态切换；
所有模块交互均通过 C++ 类的公共接口完成，无直接耦合。
五、异常处理流程（C++ 风格容错设计）
指令输入错误：如输入 offer 未带目标 ID、输入不存在的指令（如 pause），指令解析模块输出提示，状态机不做任何操作；
MQTT 信令发送失败：4G 网络波动导致发送失败，MQTT 模块自动重试（QoS 1 机制），失败超过 3 次则通知状态机，状态机切换为 IDLE，控制台输出 “信令发送失败，请检查网络”；
音频设备异常：通话时麦克风 / 扬声器故障，音频模块通知状态机，状态机自动发送 bye 信令，切换为 IDLE，输出 “音频设备异常，通话已终止”；
重复指令操作：如 IN_CALL 状态下再次输入 offer，状态机直接拒绝，输出 “当前正在通话中，无法发起新呼叫”。
总结
整个流程以通话状态机为核心驱动，严格遵循 “状态 - 指令 - 动作” 的 C++ 面向对象交互逻辑，所有操作均通过控制台指令触发；
核心交互场景覆盖 “主动呼叫、被动接听 / 拒接、通话挂断、异常处理”，完全匹配 offer/answer/reject/close 指令的业务诉求；
全程复用已有 4G/MQTT 能力，仅通过上层 C++ 模块化设计实现指令解析、状态管理、信令封装，保证流程的完整性和容错性。


只修改main下的代码，其他的模块不修改，如需修改可在components下新建mqtt_webrtc模块来写
如果需要重新实现的，在main下实现

写完之后要做编译测试
编译测试需要先运行根目录中的/IDF_initialization.bat，才能运行idf.py


audio_server目录下是服务端代码
设备已连接到COM13端口，请帮我输入固件，启动服务端
请帮我调通esp32 s3的服务端再到网页段的整个流程，包括信令交换、音频传输等


附录，以下为esp32的mqtt信令格式
1. ICE 凭证请求 (ice_request)
   用途：设备向服务器请求 TURN/STUN ICE 服务器凭证，初始化和定时刷新时发送
   json
   {
   "type": "ice_request",
   "timestamp": 1740000000
   }
2. ICE 凭证响应 (ice_response)
   用途：服务器返回 TURN/STUN 凭证给设备（下行消息）
   json
   {
   "type": "ice_response",
   "ice_servers": [
   {
   "urls": ["turn:turn.example.com:3478"],
   "username": "user123",
   "credential": "pass456"
   }
   ]
   }
3. 呼叫发起 (offer)
   用途：呼出方（发起方）发送 SDP 信息，发起通话请求
   json
   {
   "target": "device_002",
   "timestamp": 1740000100,
   "type": "offer",
   "sdp": "v=0\r\no=- 12345 67890 IN IP4 192.168.1.1\r\ns=-\r\nt=0 0\r\n..."
   }
4. 呼叫应答 (answer)
   用途：呼入方（被叫方）回复 SDP 信息，确认接听通话
   json
   {
   "target": "device_001",
   "timestamp": 1740000200,
   "type": "answer",
   "sdp": "v=0\r\no=- 54321 09876 IN IP4 192.168.1.2\r\ns=-\r\nt=0 0\r\n..."
   }
5. ICE 候选信息 (candidate)
   用途：通话双方交换 ICE 网络候选地址，用于 P2P 连接建立
   json
   {
   "target": "device_002",
   "timestamp": 1740000300,
   "type": "candidate",
   "candidate": "candidate:1 1 UDP 2113937151 192.168.1.1 50048 typ host"
   }
6. 通话挂断 (bye)
   用途：通话任意一方主动挂断通话，结束会话
   json
   {
   "target": "device_001",
   "timestamp": 1740000400,
   "type": "bye"
   }
7. 呼叫拒接 (reject)
   用途：被叫方主动拒接来电
   json
   {
   "target": "device_001",
   "timestamp": 1740000500,
   "type": "reject"
   }
8. 呼叫超时 (timeout)
   用途：设备侧检测到呼叫超时，通知对方
   json
   {
   "target": "device_001",
   "timestamp": 1740000600,
   "type": "timeout"
   }
9. 自定义消息 (customized)
   用途：扩展自定义信令，传输业务相关的自定义数据
   json
   {
   "target": "device_002",
   "timestamp": 1740000700,
   "type": "customized",
   "data": "自定义业务数据内容"
   }
