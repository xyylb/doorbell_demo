# ESP32 OTA 升级使用说明

## 功能概述

本项目实现了基于 ML307 4G 模块的 ESP32 OTA 远程升级功能。通过 HTTP 协议下载固件，边下载边写入 OTA 分区，无需占用 ML307 的文件系统空间。

## 实现原理

### 1. 架构设计

```
[HTTP 服务器] ---> [ML307 4G 模块] ---> [ESP32 OTA 分区]
                    (AT 命令)           (边下载边写入)
```

### 2. 工作流程

1. **创建 HTTP 实例**
   - 使用 `AT+MHTTPCREATE` 命令创建 HTTP 客户端
   - 配置为非缓存模式，实时接收数据

2. **初始化 OTA 分区**
   - 获取下一个可用的 OTA 分区
   - 调用 `esp_ota_begin()` 初始化 OTA 句柄

3. **发送 HTTP GET 请求**
   - 使用 `AT+MHTTPREQUEST` 发送 GET 请求
   - ML307 开始下载固件

4. **接收数据并写入**
   - 通过 URC 回调接收 `+MHTTPURC: "content"` 数据
   - 实时调用 `esp_ota_write()` 写入 OTA 分区
   - 无需在 ML307 文件系统中缓存

5. **完成升级**
   - 调用 `esp_ota_end()` 完成写入
   - 调用 `esp_ota_set_boot_partition()` 设置启动分区
   - 重启系统

### 3. 关键技术点

#### URC 回调机制

```cpp
// 注册 URC 回调
auto urc_iterator = at_uart->RegisterUrcCallback(http_urc_callback);

// URC 回调函数
static void http_urc_callback(const std::string& command,
                              const std::vector<AtArgumentValue>& arguments)
{
    // 处理 +MHTTPURC: "header" - HTTP 响应头
    // 处理 +MHTTPURC: "content" - HTTP 内容数据
    // 处理 +MHTTPURC: "err" - HTTP 错误
}
```

#### 边下载边写入

```cpp
// 在 URC 回调中接收数据
const std::string& data = arguments[5].string_value;

// 立即写入 OTA 分区
esp_ota_write(ota_handle, data.data(), data.length());
```

## API 使用

### 启动 OTA 升级

```c
#include "otaupgrade.h"

// 启动 OTA 升级
void ota_start_upgrade(const char* url, const char* md5);

// 获取当前固件版本
int get_firmware_version(void);
```

### 示例代码

```c
// 启动 OTA 升级
const char* firmware_url = "http://example.com/firmware.bin";
const char* firmware_md5 = ""; // 可选，暂未实现 MD5 校验

ota_start_upgrade(firmware_url, firmware_md5);
```

## AT 命令流程

### 1. 创建 HTTP 实例

```
AT+MHTTPCREATE="http://example.com/firmware.bin"
+MHTTPCREATE: 0
OK
```

### 2. 配置 HTTP 参数

```
// 设置为非缓存模式
AT+MHTTPCFG="cached",0,0
OK

// 设置超时时间
AT+MHTTPCFG="timeout",0,60,0
OK
```

### 3. 发送 GET 请求

```
AT+MHTTPREQUEST=0,1,0,"/firmware.bin"
OK
```

### 4. 接收 URC 数据

```
// HTTP 响应头
+MHTTPURC: "header",0,200,<header_len>,<header_data>

// HTTP 内容数据（多次上报）
+MHTTPURC: "content",0,<content_len>,<sum_len>,<cur_len>,<data>
+MHTTPURC: "content",0,<content_len>,<sum_len>,<cur_len>,<data>
...

// 下载完成（cur_len == 0）
+MHTTPURC: "content",0,<content_len>,<sum_len>,0,
```

### 5. 删除 HTTP 实例

```
AT+MHTTPDEL=0
OK
```

## 配置要求

### 1. 分区表配置

确保 `partitions.csv` 包含两个 OTA 分区：

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x6000,
phy_init, data, phy,     0xf000,  0x1000,
factory,  app,  factory, 0x10000, 1M,
ota_0,    app,  ota_0,   ,        1M,
ota_1,    app,  ota_1,   ,        1M,
```

### 2. 内存配置

OTA 任务栈大小：8192 字节

```c
xTaskCreate(ota_task, "ota_task", 8192, NULL, 5, NULL);
```

### 3. 网络要求

- ML307 4G 模块已连接网络
- HTTP 服务器可访问
- 固件文件可通过 HTTP GET 下载

## 优势特点

1. **节省空间**
   - 不占用 ML307 文件系统空间
   - 适合大文件 OTA 升级

2. **实时传输**
   - 边下载边写入
   - 内存占用小

3. **可靠性高**
   - 支持断点续传（可扩展）
   - 错误处理完善

4. **进度监控**
   - 实时显示下载进度
   - 支持百分比显示

## 注意事项

1. **网络稳定性**
   - 确保 4G 网络稳定
   - 建议在信号良好的环境下进行 OTA

2. **电源供应**
   - OTA 过程中不要断电
   - 建议使用稳定的电源

3. **固件兼容性**
   - 确保新固件与硬件兼容
   - 建议先在测试设备上验证

4. **超时设置**
   - HTTP 响应头超时：30 秒
   - 固件下载超时：5 分钟
   - 可根据实际情况调整

## 故障排查

### 1. 创建 HTTP 实例失败

- 检查 URL 格式是否正确
- 检查 ML307 网络连接状态

### 2. HTTP 请求失败

- 检查服务器是否可访问
- 检查固件文件是否存在

### 3. 下载超时

- 检查网络信号强度
- 检查固件文件大小
- 适当增加超时时间

### 4. OTA 写入失败

- 检查 OTA 分区配置
- 检查固件大小是否超过分区大小
- 检查固件格式是否正确

## 扩展功能

### 1. MD5 校验（待实现）

```c
// 下载完成后校验 MD5
if (strcmp(calculated_md5, s_ota_md5) != 0) {
    ESP_LOGE(TAG, "MD5 校验失败");
    return;
}
```

### 2. 断点续传（待实现）

```c
// 使用 HTTP Range 请求
AT+MHTTPCFG="header",0,"Range: bytes=1024-"
```

### 3. HTTPS 支持（待实现）

```c
// 配置 SSL
AT+MHTTPCFG="ssl",0,1,1
```

## 参考文档

- [ML307 HTTP AT 命令手册](ml307%20http文档.txt)
- [ESP-IDF OTA API 文档](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/ota.html)
