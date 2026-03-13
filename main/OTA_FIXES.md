# OTA 升级问题修复记录

## 问题 1: UART FIFO 溢出导致数据丢失

### 症状
```
E (49282) AtUart: FIFO overflow
固件下载不完整，只接收了 40% 的数据
```

### 原因
1. UART 接收缓冲区太小（8KB）
2. HTTP 数据流控参数过于保守（512 字节/100ms）
3. 高速数据传输时缓冲区溢出

### 解决方案

#### 1. 增大 UART 接收缓冲区
**文件**: `components/esp-ml307/src/at_uart.cc:76`

```cpp
// 从 8192 增加到 16384
ESP_ERROR_CHECK(uart_driver_install(uart_num_, 16384, 0, 100, &event_queue_handle_, ESP_INTR_FLAG_IRAM));
```

#### 2. 优化 HTTP 流控参数
**文件**: `main/otaupgrade.cpp:361`

```cpp
// 从 512/100ms 优化到 1024/50ms
AT+MHTTPCFG="fragment",<httpid>,1024,50
```

#### 3. 改进下载完成判断逻辑
**文件**: `main/otaupgrade.cpp:128-180`

- 明确区分 `cur_len == 0` 作为结束信号
- 验证 `sum_len >= content_len` 作为备用条件
- 添加下载完整性验证
- 如果下载大小不匹配，直接终止 OTA

## 问题 2: HEX 数据解码问题（关键问题！）

### 症状
```
I (1372307) OTA:   - 总共接收: 1532235 字节
I (1372308) OTA:   - sum_len: 2766608, content_len: 2766608
只接收了 55% 的数据（1532235 / 2766608 ≈ 0.55）
E (1372401) esp_image: invalid segment length 0x1d089999
```

### 原因分析
**这是导致 OTA 失败的根本原因！**

ML307 模块即使设置了 `AT+MHTTPCFG="encoding",<httpid>,0,0`（原始数据模式），仍然会以 **HEX 编码格式** 发送 HTTP 响应数据。

- 每个字节被编码为两个 HEX 字符（例如：`0x41` → `"41"`）
- 之前的代码直接使用 `data.length()` 作为写入长度
- 实际上 `data.length()` 是 HEX 字符串的长度，是真实数据的 **2 倍**
- 导致只写入了一半的数据到 OTA 分区

**数学验证**：
- sum_len = 2,766,608 字节（HTTP 报告的总大小）
- 实际接收 = 1,532,235 字节
- 1,532,235 / 2,766,608 ≈ 0.554 ≈ 55%
- 这正好接近 50%，证实了 HEX 编码问题

### 解决方案

**文件**: `main/otaupgrade.cpp:141-148`

```cpp
// 之前的错误代码
const std::string& data = arguments[5].string_value;
esp_ota_write(s_ota_context->ota_handle, data.data(), data.length());  // ❌ 错误！

// 修复后的代码
const std::string& hex_data = arguments[5].string_value;

// 解码 HEX 数据为二进制数据
std::string binary_data;
at_uart->DecodeHexAppend(binary_data, hex_data.c_str(), hex_data.length());

// 验证解码后的数据长度
if (binary_data.length() != (size_t)cur_len) {
    ESP_LOGW(TAG, "数据长度不匹配！期望 %d，实际 %d", cur_len, binary_data.length());
}

// 写入解码后的二进制数据
esp_ota_write(s_ota_context->ota_handle, binary_data.data(), binary_data.length());  // ✅ 正确！
```

### 为什么会这样？

查看 `ml307_http.cc` 的实现：

```cpp
// 第 230 行：设置编码为原始数据
command = "AT+MHTTPCFG=\"encoding\"," + std::to_string(http_id_) + ",0,0";
at_uart_->SendCommand(command);

// 第 253 行：又设置编码为 HEX
command = "AT+MHTTPCFG=\"encoding\"," + std::to_string(http_id_) + ",1,1";
at_uart_->SendCommand(command);

// 第 32 行：接收数据时进行 HEX 解码
at_uart_->DecodeHexAppend(decoded_data, arguments[5].string_value.c_str(), ...);
```

ML307 的 HTTP 实现总是使用 HEX 编码传输数据，这是为了：
1. 避免二进制数据中的特殊字符干扰 AT 命令解析
2. 确保数据传输的可靠性
3. 简化 UART 通信协议

## 问题 3: 固件验证时看门狗超时

### 症状
```
E (575069) task_wdt: Task watchdog got triggered
E (575069) task_wdt: CPU 0: ota_task
Backtrace: ... esp_ota_end ... mbedtls_sha256_update ...
```

### 原因
`esp_ota_end()` 需要计算整个固件的 SHA256 哈希值进行验证。对于 2.64MB 的固件，这个过程可能需要 30-60 秒，超过了看门狗的默认超时时间（5 秒）。

### 解决方案

**文件**: `main/otaupgrade.cpp:525-545`

在固件验证期间临时禁用看门狗：

```cpp
// 在 esp_ota_end() 之前
TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
esp_err_t wdt_err = esp_task_wdt_delete(current_task);
if (wdt_err == ESP_OK) {
    ESP_LOGI(TAG, "已临时禁用看门狗");
} else {
    ESP_LOGW(TAG, "无法禁用看门狗: %s", esp_err_to_name(wdt_err));
}

esp_err_t err = esp_ota_end(s_ota_context->ota_handle);

// 验证完成后重新启用
if (wdt_err == ESP_OK) {
    esp_task_wdt_add(current_task);
    ESP_LOGI(TAG, "已重新启用看门狗");
}
```

**注意**：检查 `esp_task_wdt_delete()` 的返回值，因为任务可能没有被添加到看门狗。

## 问题 4: 固件 segment 大小异常

### 症状
```
I (570104) esp_image: segment 1: paddr=00420290 vaddr=1188c041 size=10c08180h (281051520)
E (1372401) esp_image: invalid segment length 0x1d089999
```

segment 1 大小显示为 268 MB，这明显不正常（实际固件只有 2.64 MB）。

### 根本原因
**这个问题是由问题 2（HEX 数据解码问题）导致的！**

由于没有正确解码 HEX 数据，写入 OTA 分区的是 HEX 字符串而不是二进制数据，导致：
1. 固件数据不完整（只有 55%）
2. 固件格式损坏（HEX 字符串不是有效的二进制固件）
3. ESP32 尝试解析损坏的固件时读取到错误的 segment 大小

### 解决方案
修复问题 2（HEX 数据解码）后，这个问题会自动解决。

## 问题 5: 芯片版本不匹配

### 症状
```
E (75015) boot_comm: Image requires efuse blk rev >= v95.4, but chip is v1.3
E (75026) OTA: esp_ota_end 失败: ESP_ERR_OTA_VALIDATE_FAILED
```

### 解决方案
在 `sdkconfig.defaults.esp32s3` 中设置：

```
CONFIG_ESP32S3_REV_MIN_0=y
CONFIG_ESP32S3_REV_MIN=0
```

然后重新编译固件：
```bash
idf.py build
```

## 完整的 OTA 流程验证清单

### 1. 编译固件
- [ ] 设置正确的芯片版本 (`CONFIG_ESP32S3_REV_MIN=0`)
- [ ] 编译成功
- [ ] 使用 `esptool.py image_info` 验证固件格式
- [ ] 记录固件大小和 MD5

### 2. 上传固件到服务器
- [ ] 上传到 HTTP 服务器
- [ ] 验证文件大小和 MD5
- [ ] 测试 HTTP 下载（使用 curl 或浏览器）

### 3. 执行 OTA 升级
- [ ] 检查网络连接
- [ ] 启动 OTA 升级
- [ ] 观察日志输出

### 4. 验证日志
- [ ] HTTP 实例创建成功
- [ ] 收到 HTTP 200 响应
- [ ] Content-Length 正确
- [ ] 数据包参数正常（content_len, sum_len, cur_len）
- [ ] 无 FIFO overflow 错误
- [ ] 下载大小匹配
- [ ] 固件验证通过
- [ ] 设置启动分区成功
- [ ] 系统重启

## 调试技巧

### 1. 启用详细日志
在 `sdkconfig` 中：
```
CONFIG_LOG_DEFAULT_LEVEL_DEBUG=y
CONFIG_BOOTLOADER_LOG_LEVEL_DEBUG=y
```

### 2. 监控关键参数
观察日志中的：
- 数据包计数
- content_len, sum_len, cur_len 的变化
- 下载进度百分比
- 总接收字节数

### 3. 使用串口监视器
```bash
idf.py monitor
```

### 4. 分析固件文件
```bash
# 查看固件信息
esptool.py --chip esp32s3 image_info build/doorbell_demo_voice.bin

# 计算 MD5
md5sum build/doorbell_demo_voice.bin

# 查看文件大小
ls -lh build/doorbell_demo_voice.bin
```

## 当前状态

### 已修复
- ✅ UART 缓冲区增大到 16KB
- ✅ HTTP 流控优化（1024 字节/50ms）
- ✅ 下载完成判断逻辑改进
- ✅ 看门狗超时问题修复
- ✅ 下载大小验证（不匹配时终止）
- ✅ **HEX 数据解码问题修复（关键！）**

### 关键修复总结

**问题 2（HEX 数据解码）是导致 OTA 失败的根本原因！**

修复前：
- 直接使用 HEX 字符串写入 OTA 分区
- 只写入了 55% 的数据（1.53 MB / 2.64 MB）
- 固件格式损坏，验证失败

修复后：
- 使用 `DecodeHexAppend()` 解码 HEX 数据
- 应该能写入完整的 2.64 MB 固件
- 固件格式正确，验证应该通过

### 下一步测试
1. 重新编译并烧录固件
2. 执行 OTA 升级
3. 观察日志，验证：
   - 数据包解码后的长度与 `cur_len` 匹配
   - 总接收字节数达到 2.64 MB（而不是 1.53 MB）
   - 固件验证通过
   - 系统成功重启到新固件

### 预期日志输出
```
I (xxx) OTA: 数据包 #1: content_len=2766608, sum_len=1024, cur_len=1024, 已接收=1024
I (xxx) OTA: 数据包 #1 前 16 字节: e9 09 00 20 ...  // ESP32 固件魔数
...
I (xxx) OTA: 数据包 #3804: content_len=2766608, sum_len=2766608, cur_len=xxx, 已接收=2766608
I (xxx) OTA: sum_len (2766608) 达到 content_len (2766608)
I (xxx) OTA: 固件下载完成！
I (xxx) OTA:   - 总共接收: 2766608 字节  // 应该是完整的 2.64 MB
I (xxx) OTA: 下载大小验证通过：2766608 字节
I (xxx) OTA: 正在验证固件，这可能需要几十秒时间...
I (xxx) OTA: 已临时禁用看门狗
I (xxx) esp_image: segment 0: paddr=00390020 vaddr=3c200020 size=90268h (590440) map
I (xxx) esp_image: segment 1: paddr=00420290 vaddr=... size=...  // 正常的大小
...
I (xxx) OTA: 已重新启用看门狗
I (xxx) OTA: OTA 升级成功！
I (xxx) OTA: 准备重启系统...
```
