/* Door Bell Demo - C++ Version (Fully Fixed)

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// C++ includes
#include <cstdio>
#include <cstring>
#include <string>

#include "at_modem.h"

// C includes wrapped for C++
extern "C" {
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_webrtc.h"
#include "media_lib_adapter.h"
#include "media_lib_os.h"
#include "esp_timer.h"
#include "webrtc_utils_time.h"
#include "esp_cpu.h"
#include "settings.h"
#include "common.h"
#include "esp_capture.h"
}

static const char *TAG = "Webrtc_Test";

namespace DoorbellApp {

static struct {
    struct arg_str *room_id;
    struct arg_end *end;
} room_args;

static char room_url[128];
char server_url[64] = "https://webrtc.espressif.com";

// ------------------ Async Task Functions (Must be at namespace scope) ------------------

static void run_async_leave(void *arg) {
    stop_webrtc();
    media_lib_thread_destroy(nullptr);
}

static char* gen_room_id_use_mac() {
    static char room_mac[16];
    uint8_t mac[6];
    network_get_mac(mac);
    std::snprintf(room_mac, sizeof(room_mac) - 1, "esp_%02x%02x%02x", mac[3], mac[4], mac[5]);
    return room_mac;
}

static void run_async_start(void *arg) {
    char *room = gen_room_id_use_mac();
    std::snprintf(room_url, sizeof(room_url), "%s/join/%s", server_url, room);
    ESP_LOGI(TAG, "Start to join in room %s", room);
    if (start_webrtc(room_url) == 0) {
        ESP_LOGW(TAG, "Please use browser to join in %s on %s/doorbell", room, server_url);
    }
    media_lib_thread_destroy(nullptr);
}

// ------------------ Command Handlers ------------------

static int join_room(int argc, char **argv) {
    int nerrors = arg_parse(argc, argv, (void **)&room_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, room_args.end, argv[0]);
        return 1;
    }

    static bool sntp_synced = false;
    if (!sntp_synced) {
        if (webrtc_utils_time_sync_init() == 0) {
            sntp_synced = true;
        }
    }

    const char *room_id = room_args.room_id->sval[0];
    std::snprintf(room_url, sizeof(room_url), "%s/join/%s", server_url, room_id);
    ESP_LOGI(TAG, "Start to join in room %s", room_id);
    start_webrtc(room_url);
    return 0;
}

static int leave_room(int argc, char **argv) {
    media_lib_thread_create_from_scheduler(nullptr, "leave", run_async_leave, nullptr);
    return 0;
}

static int cmd_cli(int argc, char **argv) {
    // NOTE: "ring" is a string literal (const), but send_cmd expects char*.
    // If send_cmd does NOT modify the string, this is safe.
    // Better: change common.h to `void send_cmd(const char *cmd);`
    send_cmd(const_cast<char*>((argc > 1) ? argv[1] : "ring"));
    return 0;
}

[[noreturn]] static int assert_cli(int argc, char **argv) {
    *(volatile int *)0 = 0;
    while (1);
}

static int sys_cli(int argc, char **argv) {
    sys_state_show();
    return 0;
}

static int wifi_cli(int argc, char **argv) {
    if (argc < 2) return -1;
    char *ssid = argv[1];
    char *password = (argc > 2) ? argv[2] : nullptr;
    return network_connect_wifi(ssid, password);
}

static int server_cli(int argc, char **argv) {
    int server_sel = (argc > 1) ? std::atoi(argv[1]) : 0;
    if (server_sel == 0) {
        std::strcpy(server_url, "https://webrtc.espressif.com");
    } else {
        std::strcpy(server_url, "https://webrtc.espressif.cn");
    }
    ESP_LOGI(TAG, "Select server %s", server_url);
    return 0;
}

static int capture_to_player_cli(int argc, char **argv) {
    return test_capture_to_player();
}

 extern "C" int measure_cli(int argc, char **argv) {
    extern void measure_enable(bool enable);
    extern void show_measure(void);
    measure_enable(true);
    media_lib_thread_sleep(1500);
    measure_enable(false);
    return 0;
}

// ------------------ Console Initialization ------------------

 extern "C" int init_console()
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "esp>";
    repl_config.task_stack_size = 15 * 1024;
    repl_config.task_priority = 22;
    repl_config.max_cmdline_length = 1024;
    // install console REPL environment
#if CONFIG_ESP_CONSOLE_UART
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t cdc_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&cdc_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t usbjtag_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usbjtag_config, &repl_config, &repl));
#endif

    room_args.room_id = arg_str1(NULL, NULL, "<w123456>", "room name");
    room_args.end = arg_end(2);
    esp_console_cmd_t cmds[] = {
        {
            .command = "join",
            .help = "Please enter a room name.\r\n",
            .func = join_room,
            .argtable = &room_args,
        },
        {
            .command = "leave",
            .help = "Leave from room\n",
            .func = leave_room,
        },
        {
            .command = "cmd",
            .help = "Send command (ring etc)\n",
            .func = cmd_cli,
        },
        {
            .command = "i",
            .help = "Show system status\r\n",
            .func = sys_cli,
        },
        {
            .command = "assert",
            .help = "Assert system\r\n",
            .func = assert_cli,
        },
        {
            .command = "rec2play",
            .help = "Play capture content\n",
            .func = capture_to_player_cli,
        },
        {
            .command = "wifi",
            .help = "wifi ssid psw\r\n",
            .func = wifi_cli,
        },
        {
            .command = "m",
            .help = "measure system loading\r\n",
            .func = measure_cli,
        },
         {
            .command = "server",
            .help = "Select server\r\n",
            .func = server_cli,
        },
    };
    for (int i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    return 0;
}

// ------------------ Thread Schedulers ------------------

static void thread_scheduler(const char *thread_name, media_lib_thread_cfg_t *schedule_cfg) {
    // Set defaults
    schedule_cfg->stack_size = 8 * 1024;
    schedule_cfg->priority = 5;
    schedule_cfg->core_id = 0; // warning unavoidable, but safe

    if (std::strcmp(thread_name, "buffer_in") == 0) {
        schedule_cfg->stack_size = 6 * 1024;
        schedule_cfg->priority = 10;
        schedule_cfg->core_id = 0;
    }
    else if (std::strcmp(thread_name, "venc_0") == 0) {
#if CONFIG_IDF_TARGET_ESP32S3
        schedule_cfg->stack_size = 20 * 1024;
#endif
        schedule_cfg->priority = 10;
    }
#ifdef WEBRTC_SUPPORT_OPUS
    else if (std::strcmp(thread_name, "aenc_0") == 0) {
        schedule_cfg->stack_size = 40 * 1024;
        schedule_cfg->priority = 10;
        schedule_cfg->core_id = 1;
    }
    else if (std::strcmp(thread_name, "Adec") == 0 || std::strcmp(thread_name, "adec_0") == 0) {
        schedule_cfg->stack_size = 40 * 1024;
        schedule_cfg->priority = 10;
        schedule_cfg->core_id = 1;
    }
#endif
    else if (std::strcmp(thread_name, "AUD_SRC") == 0) {
#ifdef WEBRTC_SUPPORT_OPUS
        schedule_cfg->stack_size = 40 * 1024;
#endif
        schedule_cfg->priority = 15;
    }
    else if (std::strcmp(thread_name, "pc_task") == 0) {
        schedule_cfg->stack_size = 25 * 1024;
        schedule_cfg->priority = 18;
        schedule_cfg->core_id = 1;
    }
    else if (std::strcmp(thread_name, "start") == 0) {
        schedule_cfg->stack_size = 6 * 1024;
    }
    else if (std::strcmp(thread_name, "leave") == 0) {
        schedule_cfg->stack_size = 20 * 1024;
        schedule_cfg->priority = 10;
        schedule_cfg->core_id = 0;
    }
}

static void capture_scheduler(const char *name, esp_capture_thread_schedule_cfg_t *schedule_cfg) {
    // Initialize cfg with current values (avoid partial init warning)
    media_lib_thread_cfg_t cfg = {0};
    cfg.stack_size = schedule_cfg->stack_size;
    cfg.priority = schedule_cfg->priority;
    cfg.core_id = schedule_cfg->core_id;

    schedule_cfg->stack_in_ext = true;
    thread_scheduler(name, &cfg);

    schedule_cfg->stack_size = cfg.stack_size;
    schedule_cfg->priority = cfg.priority;
    schedule_cfg->core_id = cfg.core_id;
}

// ------------------ Network Event Handler ------------------

extern "C" int network_event_handler(bool connected) {
    if (connected) {
        media_lib_thread_create_from_scheduler(nullptr, "start", run_async_start, nullptr);
    } else {
        stop_webrtc();
    }
    return 0;
}

void new_4g(){
        ESP_LOGI(TAG, "开始检测4G网络");
    // 自动检测并初始化模组
    auto modem = AtModem::Detect(GPIO_NUM_17, GPIO_NUM_18, GPIO_NUM_4, 921600);
    
    if (!modem) {
        ESP_LOGE(TAG, "模组检测失败");
        return;
    }
    
    // 设置网络状态回调
    modem->OnNetworkStateChanged([](bool ready) {
        ESP_LOGI(TAG, "网络状态: %s", ready ? "已连接" : "已断开");
    });
    
    // 等待网络就绪
    NetworkStatus status = modem->WaitForNetworkReady(30000);
    if (status != NetworkStatus::Ready) {
        ESP_LOGE(TAG, "网络连接失败");
        return;
    }
    
    // 打印模组信息
    ESP_LOGI(TAG, "模组版本: %s", modem->GetModuleRevision().c_str());
    ESP_LOGI(TAG, "IMEI: %s", modem->GetImei().c_str());
    ESP_LOGI(TAG, "ICCID: %s", modem->GetIccid().c_str());
    ESP_LOGI(TAG, "运营商: %s", modem->GetCarrierName().c_str());
    ESP_LOGI(TAG, "信号强度: %d", modem->GetCsq());
    
    
    // 创建 HTTP 客户端
    auto http = modem->CreateHttp(0);
    
    // 设置请求头
    http->SetHeader("User-Agent", "Xiaozhi/3.0.0");
    http->SetTimeout(10000);
    
    // 发送 GET 请求
    if (http->Open("GET", "https://httpbin.org/json")) {
        ESP_LOGI(TAG, "HTTP 状态码: %d", http->GetStatusCode());
        ESP_LOGI(TAG, "响应内容长度: %zu bytes", http->GetBodyLength());
        
        // 读取响应内容
        std::string response = http->ReadAll();
        ESP_LOGI(TAG, "响应内容: %s", response.c_str());
        
        http->Close();
    } else {
        ESP_LOGE(TAG, "HTTP 请求失败");
    }
    
}

// ------------------ Main Entry ------------------

void app_main() {
    esp_log_level_set("*", ESP_LOG_INFO);
    media_lib_add_default_adapter();
    esp_capture_set_thread_scheduler(capture_scheduler);
    media_lib_thread_set_schedule_cb(thread_scheduler);
    init_board();

	new_4g();



    // Log I2S config
    ESP_LOGI("I2S_CHECK", "INMP441 I2S0配置: bclk=%d, ws=%d, din=%d", 14, 13, 12);
    ESP_LOGI("I2S_CHECK", "MAX98357 I2S1配置: bclk=%d, ws=%d, dout=%d", 20, 19, 21);

    media_sys_buildup();
    init_console();
    network_init(WIFI_SSID, WIFI_PASSWORD, network_event_handler);

    while (true) {
        media_lib_thread_sleep(2000);
        query_webrtc();
    }
}

} // namespace DoorbellApp

// Global C-compatible entry point
extern "C" void app_main(void) {
    DoorbellApp::app_main();
}
