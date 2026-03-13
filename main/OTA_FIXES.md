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

## 问题 2: 固件验证时看门狗超时

### 症状
```
E (575069) task_wdt: Task watchdog got triggered
E (575069) task_wdt: CPU 0: ota_task
Backtrace: ... esp_ota_end ... mbedtls_sha256_update ...
```

### 原因
`esp_ota_end()` 需要计算整个固件的 SHA256 哈希值进行验证。对于 2.64MB 的固件，这个过程可能需要 30-60 秒，超过了看门狗的默认超时时间（5 秒）。

### 解决方案

**文件**: `main/otaupgrade.cpp:525-540`

在固件验证期间临时禁用看门狗：

```cpp
// 在 esp_ota_end() 之前
TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
esp_task_wdt_delete(current_task);
ESP_LOGI(TAG, "已临时禁用看门狗");

esp_err_t err = esp_ota_end(s_ota_context->ota_handle);

// 验证完成后重新启用
esp_task_wdt_add(current_task);
ESP_LOGI(TAG, "已重新启用看门狗");
```

## 问题 3: 固件 segment 大小异常

### 症状
```
I (570104) esp_image: segment 1: paddr=00420290 vaddr=1188c041 size=10c08180h (281051520)
```

segment 1 大小显示为 268 MB，这明显不正常（实际固件只有 2.64 MB）。

### 可能原因
1. **固件文件本身损坏**：服务器上的固件文件可能有问题
2. **下载过程中数据损坏**：虽然下载大小匹配，但数据内容可能损坏
3. **编码问题**：HTTP 数据编码设置不正确

### 排查步骤

#### 1. 验证服务器上的固件文件
```bash
# 在服务器上检查固件文件
ls -lh firmware.bin
md5sum firmware.bin

# 使用 esptool 检查固件信息
esptool.py --chip esp32s3 image_info firmware.bin
```

#### 2. 检查 HTTP 编码设置
确保 `AT+MHTTPCFG="encoding"` 设置正确：

```cpp
// 设置为原始数据模式（0,0）
AT+MHTTPCFG="encoding",<httpid>,0,0
```

#### 3. 添加数据完整性验证
在 URC 回调中添加数据验证：

```cpp
// 检查接收到的数据是否合理
if (data.length() != cur_len) {
    ESP_LOGW(TAG, "数据长度不匹配：期望 %d，实际 %d", cur_len, data.length());
}

// 打印前几个字节用于调试
if (packet_count <= 3) {
    ESP_LOGI(TAG, "数据前 16 字节: %02x %02x %02x %02x %02x %02x %02x %02x ...",
            data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
}
```

## 问题 4: 芯片版本不匹配

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

### 待验证
- ⏳ 固件文件完整性
- ⏳ HTTP 数据编码正确性
- ⏳ 完整的 OTA 流程测试

### 下一步
1. 重新编译固件（确保芯片版本设置正确）
2. 验证固件文件格式和大小
3. 上传到服务器并测试下载
4. 执行完整的 OTA 升级流程
5. 观察新的日志输出，特别是数据包参数
