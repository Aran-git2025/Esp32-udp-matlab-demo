#include <string.h>
#include <sys/socket.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "lwip/err.h"
#include "lwip/sys.h"

static const char *TAG = "RealTime_Ctrl";

/* --- 配置区 --- */
#define WIFI_SSID "Aran"
#define WIFI_PASS "19999999"

#define ESP_UDP_PORT 12345       // ESP32 监听端口
#define PC_IP_ADDR "172.20.10.3" // 电脑 IP
#define PC_UDP_PORT 25001        // 电脑接收端口

#define PWM_CH0_GPIO 18       // PWM 通道 0 引脚
#define PWM_CH1_GPIO 19       // PWM 通道 1 引脚
#define ADC_CH0 ADC_CHANNEL_4 // GPIO 32 (ADC1)
#define ADC_CH1 ADC_CHANNEL_5 // GPIO 33 (ADC1)

/* --- 全局变量 --- */
typedef struct
{
    float pwm_cmd[2];  // 当前 PWM 指令百分比 (0.0 - 1.0)
    float adc_val[2];  // 当前采集的电压值
    int64_t timestamp; // 微秒级时间戳
} system_state_t;

static system_state_t g_state = {};
static int g_sock = -1;
static struct sockaddr_in g_pc_addr;
static adc_oneshot_unit_handle_t g_adc_handle;

/* --- 外设初始化 --- */
static void init_hw_peripherals()
{
    // 1. PWM (LEDC) 配置
    ledc_timer_config_t ledc_timer = {}; // 先全部清零
    ledc_timer.speed_mode = LEDC_LOW_SPEED_MODE;
    ledc_timer.duty_resolution = LEDC_TIMER_12_BIT;
    ledc_timer.timer_num = LEDC_TIMER_0;
    ledc_timer.freq_hz = 1000;
    ledc_timer.clk_cfg = LEDC_AUTO_CLK;

    ledc_timer_config(&ledc_timer);

    // Channel 配置同理，建议先清零再赋值，最兼容 C++
    ledc_channel_config_t pwm_ch0 = {};
    pwm_ch0.gpio_num = PWM_CH0_GPIO;
    pwm_ch0.speed_mode = LEDC_LOW_SPEED_MODE;
    pwm_ch0.channel = LEDC_CHANNEL_0;
    pwm_ch0.intr_type = LEDC_INTR_DISABLE;
    pwm_ch0.timer_sel = LEDC_TIMER_0;
    pwm_ch0.duty = 0;
    pwm_ch0.hpoint = 0;
    ledc_channel_config(&pwm_ch0);

    ledc_channel_config_t pwm_ch1 = {};
    pwm_ch1.gpio_num = PWM_CH1_GPIO;
    pwm_ch1.speed_mode = LEDC_LOW_SPEED_MODE;
    pwm_ch1.channel = LEDC_CHANNEL_1;
    pwm_ch1.intr_type = LEDC_INTR_DISABLE;
    pwm_ch1.timer_sel = LEDC_TIMER_0;
    pwm_ch1.duty = 0;
    pwm_ch1.hpoint = 0;
    ledc_channel_config(&pwm_ch1);

    // ADC 初始化修复
    adc_oneshot_unit_init_cfg_t adc_init_cfg = {};
    adc_init_cfg.unit_id = ADC_UNIT_1;
    adc_oneshot_new_unit(&adc_init_cfg, &g_adc_handle);

    adc_oneshot_chan_cfg_t adc_chan_cfg = {};
    adc_chan_cfg.atten = ADC_ATTEN_DB_12;
    adc_chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    adc_oneshot_config_channel(g_adc_handle, ADC_CH0, &adc_chan_cfg);
    adc_oneshot_config_channel(g_adc_handle, ADC_CH1, &adc_chan_cfg);
}

/* --- WiFi 事件处理 --- */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGW(TAG, "WiFi 断开，正在重连...");
        esp_wifi_connect();
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "连接成功，IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

/* --- 任务 1: 控制任务 (20ms 周期) --- */
static void control_task(void *pv)
{
    uint8_t rx_buffer[64];
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(20);

    while (1)
    {
        if (g_sock != -1)
        {
            struct sockaddr_storage source_addr;
            socklen_t socklen = sizeof(source_addr);

            // 使用 MSG_DONTWAIT 非阻塞接收，避免拖慢 20ms 循环
            int len = recvfrom(g_sock, rx_buffer, sizeof(rx_buffer), MSG_DONTWAIT, (struct sockaddr *)&source_addr, &socklen);

            if (len >= 8)
            { // 假设收到 2 个 float (8字节)
                float cmds[2];
                memcpy(cmds, rx_buffer, 8);

                for (int i = 0; i < 2; i++)
                {
                    g_state.pwm_cmd[i] = cmds[i];
                    // 映射到 12位占空比 (0.0~1.0 -> 0~4095)
                    uint32_t duty = (uint32_t)(cmds[i] * 4095.0f);
                    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i, duty);
                    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i);
                }
            }
        }
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

/* --- 任务 2: 采样与数据回传任务 (10ms 周期) --- */
static void telemetry_task(void *pv)
{
    float tx_packet[5]; // [Time, PWM0, PWM1, ADC0, ADC1]
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(10);

    while (1)
    {
        // 1. 获取当前高精度时间戳 (秒)
        g_state.timestamp = esp_timer_get_time();
        tx_packet[0] = (float)(g_state.timestamp / 1000000.0);

        // 2. 采集数据
        int r0, r1;
        adc_oneshot_read(g_adc_handle, ADC_CH0, &r0);
        adc_oneshot_read(g_adc_handle, ADC_CH1, &r1);
        g_state.adc_val[0] = r0 * 3.3f / 4095.0f;
        g_state.adc_val[1] = r1 * 3.3f / 4095.0f;

        // 3. 打包当前指令和采样值
        tx_packet[1] = g_state.pwm_cmd[0];
        tx_packet[2] = g_state.pwm_cmd[1];
        tx_packet[3] = g_state.adc_val[0];
        tx_packet[4] = g_state.adc_val[1];

        // 4. 发送 UDP
        if (g_sock != -1)
        {
            sendto(g_sock, tx_packet, sizeof(tx_packet), 0, (struct sockaddr *)&g_pc_addr, sizeof(g_pc_addr));
           
        }

        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

extern "C" void app_main(void)
{
    // 1. 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. 初始化硬件
    init_hw_peripherals();

    // 3. 网络配置
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {};
    strcpy((char *)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char *)wifi_config.sta.password, WIFI_PASS);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // --- 实时性优化：关闭 WiFi 低功耗模式 ---
    esp_wifi_set_ps(WIFI_PS_NONE);

    // 4. 创建 UDP Socket
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr)); // 先清零
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(ESP_UDP_PORT);

    bind(g_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr));

    // 预设电脑地址
    memset(&g_pc_addr, 0, sizeof(g_pc_addr));
    g_pc_addr.sin_family = AF_INET;
    g_pc_addr.sin_port = htons(PC_UDP_PORT);
    g_pc_addr.sin_addr.s_addr = inet_addr(PC_IP_ADDR);

    // 5. 启动任务
    // 采样回传任务 10ms，优先级设高 (6)
    xTaskCreate(telemetry_task, "tele_task", 4096, NULL, 6, NULL);
    // 控制更新任务 20ms，优先级稍低 (5)
    xTaskCreate(control_task, "ctrl_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "系统启动完成");
}