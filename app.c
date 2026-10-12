/***************************************************************************/ /**
 * @file
 * @brief Embedded MQTT Client Example Application
 *******************************************************************************
 * # License
 * <b>Copyright 2022 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * SPDX-License-Identifier: Zlib
 *
 * The licensor of this software is Silicon Laboratories Inc.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 ******************************************************************************/

#include "sl_net.h"
#include "secret.h"
#include "sl_net_default_values.h"
#include "sl_utility.h"
#include "cmsis_os2.h"
#include "sl_constants.h"
#include "sl_mqtt_client.h"
#include "cacert.pem.h"
#include "sl_wifi.h"
#include "sl_wifi_callback_framework.h"
#include "string.h"
#include "sl_si91x_si70xx.h"
#include "sl_i2c_instances.h"
#include "sl_si91x_driver_gpio.h"
#include "RTE_Device_917.h"
#include "si70xx_float.h"
#include "sl_si91x_adc.h"
#include "sl_adc_instances.h"
#include "sl_si91x_adc_common_config.h"
#include "stdio.h"
#include <math.h>
#include "sl_wifi_device.h"
#include "sl_si91x_socket_utility.h"
#include "sl_net_ping.h"
#include "sl_net_wifi_types.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdbool.h>

extern sl_i2c_pin_init_t sl_i2c_i2c2_pin_init;

/******************************************************
 *                    Constants
 ******************************************************/

#ifdef SLI_SI91X_ENABLE_IPV6
#define MQTT_BROKER_IP "2401:4901:1290:10de::1000"
#else
#define MQTT_BROKER_IP "192.168.6.1"
#endif

#define MQTT_BROKER_PORT 1883
#define MQTT_BROKER_HOST "yourmqtthost.com"

// Local TCP port, picked fresh from this range for every connect. With a fixed
// port, a reconnect after a Wi-Fi drop reused the 4-tuple of the session the
// broker still held open, and every connect failed until the broker dropped it.
#define CLIENT_PORT_MIN   49152u
#define CLIENT_PORT_RANGE 16384u

#define PUBLISH_TOPIC          "ghanode/sensor"
#define QOS_OF_PUBLISH_MESSAGE 0

// "AABBCCDDEEFF" + NUL
#define SENSOR_ID_MAC_LEN 13
#define MQTT_JSON_MAX_LEN 208

#define IS_DUPLICATE_MESSAGE 0
#define IS_MESSAGE_RETAINED  0
#define IS_CLEAN_SESSION     1

#define ENCRYPT_CONNECTION     0
#define CERTIFICATE_INDEX      1
// Seconds. The broker drops a silent client after 1.5x this, so it also bounds
// how long a session lost with the Wi-Fi lingers on the broker.
#define KEEP_ALIVE_INTERVAL    60
#define MQTT_CONNECT_TIMEOUT   5000
#define MQTT_KEEPALIVE_RETRIES 0

#define SEND_CREDENTIALS 1

#define USERNAME SECRET_MQTT_USERNAME
#define PASSWORD SECRET_MQTT_PASSWORD

// Si7021 datasheet, Electrical Specifications: tPU from VDD >= 1.9 V to ready
// for a conversion is 80 ms max over the full temperature range.
#define SI70XX_POWER_ON_DELAY_MS 80

#define SI70XX_SAMPLE_INTERVAL_MS 20000

#define WIFI_RETRY_DELAY_MIN_MS 5000
#define WIFI_RETRY_DELAY_MAX_MS 60000

#define MQTT_RETRY_DELAY_MIN_MS    5000
#define MQTT_RETRY_DELAY_MAX_MS    60000
// A TCP connect to an unreachable broker took ~30 s to fail on the NWP. Starting
// another connect before the first resolves left the SDK client stuck in
// CONNECTION_FAILED, so the wait must outlast that.
#define MQTT_CONNECT_WAIT_MS       60000
#define MQTT_PUBLISH_WAIT_MS       10000
#define MQTT_PUBLISH_REJECT_LIMIT  3
#define MQTT_PUBLISH_RETRY_MS      1000
#define MQTT_DISCONNECT_TIMEOUT_MS 5000
// Connect failures right after a Wi-Fi restart before the MCU is reset. A
// broker that is really down costs at most one reset this way.
#define MQTT_FAILS_AFTER_RECOVERY  3

// Readings kept while the broker is unreachable: 30 x 20 s = 10 minutes.
#define READING_QUEUE_LENGTH 30

// Stack high-water marks are logged after the first sample, then hourly.
#define STACK_REPORT_EVERY_N_SAMPLES (3600000u / SI70XX_SAMPLE_INTERVAL_MS)

// net_events bits. Set by the sensor task, the MQTT callbacks and the Wi-Fi
// join callback; waited on by the network loop.
#define NET_EVT_READING        (1UL << 0)
#define NET_EVT_MQTT_UP        (1UL << 1)
#define NET_EVT_MQTT_DOWN      (1UL << 2)
#define NET_EVT_PUBLISHED      (1UL << 3)
#define NET_EVT_PUBLISH_FAILED (1UL << 4)
#define NET_EVT_WIFI_DOWN      (1UL << 5)

#define ADC_VREF_VOLTS 3.3f

// GL5528 LDR from GPIO_25 to GND, 4.7 kOhm pull-up to the ADC reference:
// V = VREF * R_ldr / (R_ldr + R_pullup).
#define LDR_PULLUP_OHMS 4700.0f
// GL5528 datasheet: 10-20 kOhm at 10 lux, gamma 0.7 (10 to 100 lux). The reading
// is used against a 2000 lux threshold; part-to-part spread moves that point to
// 1300-3600 lux, so set R10 from a lux meter reading near 2000 lux.
#define LDR_R10_OHMS  15000.0f
#define LDR_GAMMA     0.7f
#define LIGHT_LUX_MAX 100000

/******************************************************
 *               Variable Definitions
 ******************************************************/

// Bring-up, then the network loop. Formatting the JSON with %f pulls in the
// newlib float formatter.
const osThreadAttr_t thread_attributes = {
  .name       = "net",
  .attr_bits  = 0,
  .cb_mem     = 0,
  .cb_size    = 0,
  .stack_mem  = 0,
  .stack_size = 3072,
  .priority   = osPriorityLow,
  .tz_module  = 0,
};

// Above the network loop so a slow reconnect never delays a sample.
const osThreadAttr_t sensor_thread_attributes = {
  .name       = "sensor",
  .attr_bits  = 0,
  .cb_mem     = 0,
  .cb_size    = 0,
  .stack_mem  = 0,
  .stack_size = 2048,
  .priority   = osPriorityBelowNormal,
  .tz_module  = 0,
};

static const sl_wifi_device_configuration_t wifi_mqtt_client_configuration = {
  .boot_option = LOAD_NWP_FW,
  .mac_address = NULL,
  .band        = SL_SI91X_WIFI_BAND_2_4GHZ,
  // US region only scans channels 1–11. EU APs may sit on 12/13.
  .region_code = SL_WIFI_REGION_EU,
  .boot_config = { .oper_mode              = SL_SI91X_CLIENT_MODE,
                   .coex_mode              = SL_SI91X_WLAN_ONLY_MODE,
                   .feature_bit_map        = (SL_WIFI_FEAT_SECURITY_PSK | SL_WIFI_FEAT_AGGREGATION),
                   .tcp_ip_feature_bit_map = (SL_SI91X_TCP_IP_FEAT_DHCPV4_CLIENT | SL_SI91X_TCP_IP_FEAT_DNS_CLIENT
                                              | SL_SI91X_TCP_IP_FEAT_ICMP | SL_SI91X_TCP_IP_FEAT_EXTENSION_VALID
#if ENCRYPT_CONNECTION
                                              | SL_SI91X_TCP_IP_FEAT_SSL
#endif
#ifdef SLI_SI91X_ENABLE_IPV6
                                              | SL_SI91X_TCP_IP_FEAT_DHCPV6_CLIENT | SL_SI91X_TCP_IP_FEAT_IPV6
#endif
                                              ),
                   .custom_feature_bit_map     = (SL_WIFI_SYSTEM_CUSTOM_FEAT_EXTENSION_VALID),
                   // Plain MQTT: drop TLS memory reserves; keep 802.11w — this AP uses PMF (sec=8).
                   .ext_custom_feature_bit_map = (SL_SI91X_EXT_FEAT_XTAL_CLK | SL_SI91X_EXT_FEAT_IEEE_80211W
                                                  | MEMORY_CONFIG
#if ENCRYPT_CONNECTION
                                                  | SL_SI91X_EXT_FEAT_SSL_VERSIONS_SUPPORT
#endif
#ifdef SLI_SI917
                                                  | SL_SI91X_EXT_FEAT_FRONT_END_SWITCH_PINS_ULP_GPIO_4_5_0
#endif
                                                  ),
                   .bt_feature_bit_map = 0,
                   .ext_tcp_ip_feature_bit_map =
                     (SL_SI91X_EXT_TCP_IP_WINDOW_SCALING | SL_SI91X_EXT_TCP_IP_TOTAL_SELECTS(10)
                      | SL_SI91X_EXT_TCP_IP_WAIT_FOR_SOCKET_CLOSE | SL_SI91X_EXT_EMB_MQTT_ENABLE
                      | SL_SI91X_CONFIG_FEAT_EXTENSION_VALID
#if ENCRYPT_CONNECTION
                      | SL_SI91X_EXT_TCP_IP_FEAT_SSL_THREE_SOCKETS | SL_SI91X_EXT_TCP_IP_FEAT_SSL_MEMORY_CLOUD
#endif
                      ),
                   .ble_feature_bit_map     = 0,
                   .ble_ext_feature_bit_map = 0,
                   // Required for NWP timestamp memory + captive-core logging.
                   .config_feature_bit_map  = SL_SI91X_ENABLE_NWP_LOGGING },
  // 0/0/0 = SDK default pool split (stock MQTT example). Non-zero 1/1/1 is unused.
  .ta_pool         = { .tx_ratio_in_buffer_pool = 0, .rx_ratio_in_buffer_pool = 0, .global_ratio_in_buffer_pool = 0 },
  .efuse_data_type = SL_SI91X_EFUSE_MFG_SW_VERSION,
  .nwp_fw_image_number = SL_SI91X_NWP_FW_IMAGE_NUMBER_0
};

sl_mqtt_client_t client = { 0 };

typedef struct {
  float temperature_c;
  float humidity_pct;
  int light_lux;
} sensor_reading_t;

static osMessageQueueId_t reading_queue = NULL;
static osEventFlagsId_t net_events      = NULL;
static osThreadId_t net_thread          = NULL;
static osThreadId_t sensor_thread       = NULL;

// Written by the stack overflow hook for a debugger to read.
const char *volatile stack_overflow_task = NULL;

static char mqtt_json_payload[MQTT_JSON_MAX_LEN];
static char sensor_id_mac[SENSOR_ID_MAC_LEN] = "000000000000";

sl_mqtt_client_credentials_t *client_credentails = NULL;

sl_mqtt_client_configuration_t mqtt_client_configuration = { .is_clean_session = IS_CLEAN_SESSION,
                                                             // The broker drops an existing session when another
                                                             // client connects with the same ID, so each node
                                                             // uses its own STA MAC (filled in before connect).
                                                             .client_id        = (uint8_t *)sensor_id_mac,
                                                             .client_id_length = SENSOR_ID_MAC_LEN - 1,
#if ENCRYPT_CONNECTION
                                                             .tls_flags = SL_MQTT_TLS_ENABLE | SL_MQTT_TLS_TLSV_1_2
                                                                          | SL_MQTT_TLS_CERT_INDEX_1
                                                                          | SL_MQTT_TLS_SNI_ENABLE,
#endif
                                                             .client_port = CLIENT_PORT_MIN };

sl_mqtt_broker_v2_t mqtt_broker_configuration = {
  .port                    = MQTT_BROKER_PORT,
  .is_connection_encrypted = ENCRYPT_CONNECTION,
  .connect_timeout         = MQTT_CONNECT_TIMEOUT,
  .keep_alive_interval     = KEEP_ALIVE_INTERVAL,
  .keep_alive_retries      = MQTT_KEEPALIVE_RETRIES,
  .host_name               = NULL,
#if ENCRYPT_CONNECTION
  .enable_sni    = 1,                           /* enable SNI when using TLS */
  .sni_host_name = (uint8_t *)MQTT_BROKER_HOST, // SNI hostname for TLS/SNI connections
#else
  .enable_sni    = 0,
  .sni_host_name = NULL,
#endif
};

sl_mqtt_client_message_t message_to_be_published = {
  .qos_level            = QOS_OF_PUBLISH_MESSAGE,
  .is_retained          = IS_MESSAGE_RETAINED,
  .is_duplicate_message = IS_DUPLICATE_MESSAGE,
  .topic                = (uint8_t *)PUBLISH_TOPIC,
  .topic_length         = strlen(PUBLISH_TOPIC),
  .content              = (uint8_t *)mqtt_json_payload,
  .content_length       = 0,
};

/******************************************************
 *               Function Declarations
 ******************************************************/
static void application_start(void *argument);
static void sensor_task(void *argument);
static void enqueue_reading(const sensor_reading_t *reading);
static void log_stack_high_water(void);
static uint32_t next_retry_delay(uint32_t delay_ms, uint32_t max_ms);
static void wifi_join(void);
static sl_status_t wifi_join_callback(sl_wifi_event_t event,
                                      sl_status_t status_code,
                                      char *data,
                                      uint32_t data_length,
                                      void *arg);
static void network_loop(void);
static sl_status_t mqtt_setup(void);
static sl_status_t mqtt_connect_and_wait(void);
static sl_status_t publish_until_disconnected(sensor_reading_t *pending, bool *has_pending, bool *published);
static void recover_or_reset(const char *reason, bool *recovered_once);
static sl_status_t si70xx_enable_sensor(void);
static void adc_event_callback(uint8_t channel, uint8_t event);
static sl_status_t adc_ch1_init(void);
static sl_status_t adc_ch1_read_volts(float *volts);
static int ldr_volts_to_lux(float volts);
static sl_status_t publish_reading(const sensor_reading_t *reading);
static sl_status_t wifi_scan_callback(sl_wifi_event_t event,
                                      sl_wifi_scan_result_t *result,
                                      uint32_t result_length,
                                      void *arg);
static void dump_nearby_access_points(void);
static sl_status_t apply_dhcp_client_profile(const sl_mac_address_t *bssid, uint8_t channel);
static sl_status_t network_event_handler(sl_net_event_t event, sl_status_t status, void *data, uint32_t data_length);
static void dump_station_ip_config(void);
static void icmp_probe_gateway(void);
static void network_tcp_prepare(void);
static void mqtt_release_nwp_client(void);
void mqtt_client_event_handler(void *client, sl_mqtt_client_event_t event, void *event_data, void *context);
void mqtt_client_error_event_handler(void *client, sl_mqtt_client_error_status_t *error);
void mqtt_client_cleanup();
void print_char_buffer(char *buffer, uint32_t buffer_length);

/******************************************************
 *               Function Definitions
 ******************************************************/

void app_init(void)
{
  osThreadNew((osThreadFunc_t)application_start, NULL, &thread_attributes);
}

static osSemaphoreId_t wifi_scan_sem = NULL;
static osSemaphoreId_t ping_sem      = NULL;
static volatile sl_status_t ping_status = SL_STATUS_FAIL;
static uint8_t wifi_target_found     = 0;
static uint8_t wifi_target_channel   = 0;
static uint8_t wifi_target_rssi      = 0xFF;
static uint8_t wifi_target_security  = 0;
static sl_mac_address_t wifi_target_bssid;

static sl_status_t wifi_scan_callback(sl_wifi_event_t event,
                                      sl_wifi_scan_result_t *result,
                                      uint32_t result_length,
                                      void *arg)
{
  UNUSED_PARAMETER(result_length);
  UNUSED_PARAMETER(arg);

  const char *wanted = DEFAULT_WIFI_CLIENT_PROFILE_SSID;
  const size_t wanted_len = sizeof(DEFAULT_WIFI_CLIENT_PROFILE_SSID) - 1u;

  if (SL_WIFI_CHECK_IF_EVENT_FAILED(event)) {
    sl_status_t status = SL_STATUS_FAIL;
    if (result != NULL) {
      status = *(sl_status_t *)result;
    }
    printf("Wi-Fi scan failed: 0x%lx\r\n", (unsigned long)status);
  } else if ((result == NULL) || (result->scan_count == 0u)) {
    printf("Wi-Fi scan: 0 APs\r\n");
  } else {
    printf("Wi-Fi scan: %lu AP(s)\r\n", (unsigned long)result->scan_count);
    for (uint32_t i = 0; i < result->scan_count; i++) {
      char ssid[SL_WIFI_MAX_SSID_LENGTH + 1u];
      memset(ssid, 0, sizeof(ssid));
      memcpy(ssid, result->scan_info[i].ssid, SL_WIFI_MAX_SSID_LENGTH);
      const uint8_t *bssid = result->scan_info[i].bssid;
      printf("  ssid=\"%s\" ch=%u rssi=-%u sec=%u bssid=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
             ssid,
             (unsigned)result->scan_info[i].rf_channel,
             (unsigned)result->scan_info[i].rssi_val,
             (unsigned)result->scan_info[i].security_mode,
             bssid[0],
             bssid[1],
             bssid[2],
             bssid[3],
             bssid[4],
             bssid[5]);

      if (strncmp(ssid, wanted, wanted_len) != 0) {
        continue;
      }
      if ((ssid[wanted_len] != '\0') && (wanted_len < SL_WIFI_MAX_SSID_LENGTH)) {
        continue;
      }
      // Prefer stronger RSSI; on a tie keep the first match (do not walk to a later AP).
      if (wifi_target_found && (result->scan_info[i].rssi_val >= wifi_target_rssi)) {
        continue;
      }
      wifi_target_found    = 1;
      wifi_target_rssi     = result->scan_info[i].rssi_val;
      wifi_target_channel  = result->scan_info[i].rf_channel;
      wifi_target_security = result->scan_info[i].security_mode;
      memcpy(wifi_target_bssid.octet, bssid, sizeof(wifi_target_bssid.octet));
    }
  }

  if (wifi_scan_sem != NULL) {
    osSemaphoreRelease(wifi_scan_sem);
  }
  return SL_STATUS_OK;
}

static void dump_nearby_access_points(void)
{
  sl_status_t status;

  wifi_target_found   = 0;
  wifi_target_rssi    = 0xFF;
  wifi_target_channel = 0;
  memset(wifi_target_bssid.octet, 0, sizeof(wifi_target_bssid.octet));

  if (wifi_scan_sem == NULL) {
    wifi_scan_sem = osSemaphoreNew(1, 0, NULL);
  }
  // A scan that timed out on a previous attempt may still have released it.
  while (osSemaphoreAcquire(wifi_scan_sem, 0) == osOK) {
  }

  sl_wifi_set_scan_callback(wifi_scan_callback, NULL);
  printf("Scanning for 2.4 GHz APs (looking for \"%s\")...\r\n", DEFAULT_WIFI_CLIENT_PROFILE_SSID);
  status = sl_wifi_start_scan(SL_WIFI_CLIENT_2_4GHZ_INTERFACE, NULL, &default_wifi_scan_configuration);
  if ((status != SL_STATUS_OK) && (status != SL_STATUS_IN_PROGRESS)) {
    printf("sl_wifi_start_scan failed: 0x%lx\r\n", (unsigned long)status);
    return;
  }

  if (osSemaphoreAcquire(wifi_scan_sem, 30000) != osOK) {
    printf("Wi-Fi scan timed out\r\n");
    return;
  }

  if (wifi_target_found == 0u) {
    printf("No matching AP \"%s\" in scan results\r\n", DEFAULT_WIFI_CLIENT_PROFILE_SSID);
    return;
  }

  status = apply_dhcp_client_profile(&wifi_target_bssid, wifi_target_channel);
  if (status != SL_STATUS_OK) {
    printf("apply_dhcp_client_profile failed: 0x%lx\r\n", (unsigned long)status);
    return;
  }

  printf("Joining \"%s\" ch=%u rssi=-%u sec=%u bssid=%02X:%02X:%02X:%02X:%02X:%02X (DHCP)\r\n",
         DEFAULT_WIFI_CLIENT_PROFILE_SSID,
         (unsigned)wifi_target_channel,
         (unsigned)wifi_target_rssi,
         (unsigned)wifi_target_security,
         wifi_target_bssid.octet[0],
         wifi_target_bssid.octet[1],
         wifi_target_bssid.octet[2],
         wifi_target_bssid.octet[3],
         wifi_target_bssid.octet[4],
         wifi_target_bssid.octet[5]);
}

static sl_status_t apply_dhcp_client_profile(const sl_mac_address_t *bssid, uint8_t channel)
{
  sl_net_wifi_client_profile_t profile = DEFAULT_WIFI_CLIENT_PROFILE;

  if (bssid != NULL) {
    profile.config.bssid           = *bssid;
    profile.config.channel.channel = channel;
    profile.config.channel.band    = SL_WIFI_BAND_2_4GHZ;
  }

  return sl_net_set_profile(SL_NET_WIFI_CLIENT_INTERFACE, SL_NET_DEFAULT_WIFI_CLIENT_PROFILE_ID, &profile);
}

static sl_status_t si70xx_enable_sensor(void)
{
  sl_status_t status;
  uint8_t pin_status;
  uint8_t pin_dir;
  // MCU_RET NPSS_GPIO_CNTRL[n] lives at 0x2404861C + 4*n; same as UULP pad config.
  volatile uint32_t *const sensor_enable_ctrl =
    (volatile uint32_t *)(0x2404861CUL + (4UL * (uint32_t)SENSOR_ENABLE_GPIO_PIN));

  // The Si7021 is on the WSTK mainboard, isolated from radio-board ULP_I2C until
  // SENSOR_ENABLE (UULP_VBAT_GPIO_1) is configured as an M4-owned GPIO output high.
  status = sl_si91x_gpio_driver_enable_clock((sl_si91x_gpio_select_clock_t)ULPCLK_GPIO);
  if (status != SL_STATUS_OK) {
    SL_DEBUG_LOG_V2(ERROR, "Failed to enable GPIO clock for Si70xx: 0x%lx\r\n", status);
    return status;
  }

  uulp_pad_config_t pad_config = {
    .gpio_padnum = SENSOR_ENABLE_GPIO_PIN,
    .mode        = NPSS_GPIO_PIN_MUX_MODE0,
    .receiver    = GPIO_RECEIVER_EN, // needed for STATUS/readback to reflect the pad
    .direction   = GPIO_OUTPUT,      // OEN=0 => output
    .output      = GPIO_PIN_SET,
    .pad_select  = GPIO_PAD_M4, // MCU (not NWP) owns the pad
    .polarity    = GPIO_POLARITY_0,
  };

  status = sl_si91x_gpio_driver_set_uulp_pad_configuration(&pad_config);
  if (status != SL_STATUS_OK) {
    SL_DEBUG_LOG_V2(ERROR, "Failed to configure Si70xx enable pad: 0x%lx\r\n", status);
    return status;
  }

  // Force OUT again after pad config; some paths leave the latch stale.
  status = sl_si91x_gpio_driver_set_uulp_npss_pin_value(SENSOR_ENABLE_GPIO_PIN, GPIO_PIN_SET);
  if (status != SL_STATUS_OK) {
    SL_DEBUG_LOG_V2(ERROR, "Failed to set Si70xx enable pin high: 0x%lx\r\n", status);
    return status;
  }

  pin_dir    = sl_si91x_gpio_driver_get_uulp_npss_direction(SENSOR_ENABLE_GPIO_PIN);
  pin_status = sl_si91x_gpio_driver_get_uulp_npss_pin(SENSOR_ENABLE_GPIO_PIN);
  SL_DEBUG_LOG_V2(INFO,
                  "SENSOR_ENABLE ctrl=0x%08lx dir=%u status=%u out=%u\r\n",
                  (unsigned long)*sensor_enable_ctrl,
                  (unsigned)pin_dir,
                  (unsigned)pin_status,
                  (unsigned)((*sensor_enable_ctrl >> 5) & 0x1u));

  if (((*sensor_enable_ctrl >> 5) & 0x1u) == 0u) {
    SL_DEBUG_LOG_V2(ERROR, "SENSOR_ENABLE OUT latch is still 0\r\n");
    return SL_STATUS_FAIL;
  }

  osDelay(SI70XX_POWER_ON_DELAY_MS);
  return SL_STATUS_OK;
}

static void adc_event_callback(uint8_t channel, uint8_t event)
{
  UNUSED_PARAMETER(channel);
  UNUSED_PARAMETER(event);
}

static sl_status_t adc_ch1_init(void)
{
  sl_status_t status;

  sl_adc_channel_config.channel = SL_ADC_CHANNEL_1;

  status = sl_si91x_adc_init(sl_adc_channel_config, sl_adc_config, ADC_VREF_VOLTS);
  if (status != SL_STATUS_OK) {
    SL_DEBUG_LOG_V2(ERROR, "ADC init failed: 0x%lx\r\n", status);
    return status;
  }

  status = sl_si91x_adc_set_channel_configuration(sl_adc_channel_config, sl_adc_config);
  if (status != SL_STATUS_OK) {
    SL_DEBUG_LOG_V2(ERROR, "ADC channel config failed: 0x%lx\r\n", status);
    return status;
  }

  status = sl_si91x_adc_register_event_callback(adc_event_callback);
  if (status != SL_STATUS_OK) {
    SL_DEBUG_LOG_V2(ERROR, "ADC callback register failed: 0x%lx\r\n", status);
    return status;
  }

  status = sl_si91x_adc_start(sl_adc_config);
  if (status != SL_STATUS_OK) {
    SL_DEBUG_LOG_V2(ERROR, "ADC start failed: 0x%lx\r\n", status);
    return status;
  }

  SL_DEBUG_LOG_V2(INFO, "ADC CH1 GPIO_25 started\r\n");
  return SL_STATUS_OK;
}

static sl_status_t adc_ch1_read_volts(float *volts)
{
  uint16_t adc_value = 0;
  sl_status_t status = sl_si91x_adc_read_data_static(sl_adc_channel_config, sl_adc_config, &adc_value);
  if (status != SL_STATUS_OK) {
    return status;
  }

  // RSI_ADC_GainOffsetCal returns offset-binary (XOR 0x800). Undo that to a
  // 12-bit unipolar count: 0 = 0 V, 4095 = VREF.
  const uint16_t unipolar = (uint16_t)((adc_value & 0x0FFFu) ^ 0x800u);
  *volts                  = ((float)unipolar / 4096.0f) * ADC_VREF_VOLTS;
  printf("ADC CH1 raw=0x%03x count=%u\r\n", (unsigned)(adc_value & 0x0FFFu), (unsigned)unipolar);
  return SL_STATUS_OK;
}

// LDR power law: R = R10 * (lux / 10)^-gamma, inverted for lux.
static int ldr_volts_to_lux(float volts)
{
  // A full-scale reading means an open LDR or total darkness.
  if (volts >= ADC_VREF_VOLTS * 0.999f) {
    return 0;
  }
  if (volts <= 0.0f) {
    return LIGHT_LUX_MAX;
  }

  const float r_ldr = LDR_PULLUP_OHMS * volts / (ADC_VREF_VOLTS - volts);
  const float lux   = 10.0f * powf(LDR_R10_OHMS / r_ldr, 1.0f / LDR_GAMMA);
  if (lux >= (float)LIGHT_LUX_MAX) {
    return LIGHT_LUX_MAX;
  }
  return (int)(lux + 0.5f);
}

// The payload buffer must stay untouched until the PUBLISHED event; the
// network loop waits for it before formatting the next reading.
static sl_status_t publish_reading(const sensor_reading_t *reading)
{
  int n;

  n = snprintf(mqtt_json_payload,
               sizeof(mqtt_json_payload),
               "{\"Sensor ID\": \"%s\", \"temperature\":%.1f, \"humidity\": %d, \"light\": %d}",
               sensor_id_mac,
               reading->temperature_c,
               (int)(reading->humidity_pct + (reading->humidity_pct >= 0.0f ? 0.5f : -0.5f)),
               reading->light_lux);
  if ((n <= 0) || ((size_t)n >= sizeof(mqtt_json_payload))) {
    printf("MQTT JSON format failed\r\n");
    return SL_STATUS_INVALID_PARAMETER;
  }

  message_to_be_published.content        = (uint8_t *)mqtt_json_payload;
  message_to_be_published.content_length = (uint16_t)n;
  sl_status_t status = sl_mqtt_client_publish(&client, &message_to_be_published, 0, &message_to_be_published);
  if (status == SL_STATUS_IN_PROGRESS) {
    printf("MQTT publish: %s\r\n", mqtt_json_payload);
  }
  return status;
}

// Full queue means the broker has been away for a while; keep the newest
// readings. Only this task puts, so the get-then-put cannot be overtaken.
static void enqueue_reading(const sensor_reading_t *reading)
{
  if (osMessageQueuePut(reading_queue, reading, 0, 0) != osOK) {
    sensor_reading_t oldest;
    (void)osMessageQueueGet(reading_queue, &oldest, NULL, 0);
    (void)osMessageQueuePut(reading_queue, reading, 0, 0);
    printf("Reading queue full, dropped the oldest reading\r\n");
  }
  osEventFlagsSet(net_events, NET_EVT_READING);
}

static void log_stack_high_water(void)
{
  printf("Stack unused at high-water: net %lu of %lu B, sensor %lu of %lu B\r\n",
         (unsigned long)osThreadGetStackSpace(net_thread),
         (unsigned long)thread_attributes.stack_size,
         (unsigned long)osThreadGetStackSpace(sensor_thread),
         (unsigned long)sensor_thread_attributes.stack_size);
}

// Producer: samples on a fixed period and never touches the network, so
// readings keep coming while Wi-Fi or the broker is down. stdio is retargeted
// to the VCOM iostream instance.
static void sensor_task(void *argument)
{
  UNUSED_PARAMETER(argument);
  uint32_t next_wake = osKernelGetTickCount();
  uint32_t samples   = 0;

  // This task outranks its creator, so it runs before osThreadNew() returns
  // the handle to application_start.
  sensor_thread = osThreadGetId();

  while (1) {
    sensor_reading_t reading = { 0 };
    float adc_volts          = 0.0f;
    sl_status_t status;

    status = si70xx_float_measure_rh_and_temp(SI70XX_I2C_INSTANCE,
                                              SI70XX_SLAVE_ADDR,
                                              &reading.humidity_pct,
                                              &reading.temperature_c);
    if (status == SL_STATUS_OK) {
      printf("Si7021: %.2f C, %.2f %%RH\r\n", reading.temperature_c, reading.humidity_pct);

      status = adc_ch1_read_volts(&adc_volts);
      if (status == SL_STATUS_OK) {
        reading.light_lux = ldr_volts_to_lux(adc_volts);
        printf("ADC CH1 GPIO_25: %.3f V, %d lux\r\n", adc_volts, reading.light_lux);
      } else {
        printf("ADC CH1: read failed, 0x%lx\r\n", (unsigned long)status);
      }
      enqueue_reading(&reading);
    } else {
      // A failed measurement leaves 0 C / 0 %RH, which would pass the
      // pipeline's numeric check as a real reading.
      printf("Si7021: measurement failed, 0x%lx, reading skipped\r\n", (unsigned long)status);
    }

    if ((samples % STACK_REPORT_EVERY_N_SAMPLES) == 0u) {
      log_stack_high_water();
    }
    samples++;

    // osDelayUntil keeps the period at exactly 20 s regardless of how long the
    // I2C and ADC reads took.
    next_wake += SI70XX_SAMPLE_INTERVAL_MS;
    osDelayUntil(next_wake);
  }
}

// Called by the kernel (configCHECK_FOR_STACK_OVERFLOW 2) at a context switch
// once a task has written past its stack. Memory is already corrupt, so only
// record the task name for a debugger and stop, matching configASSERT.
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
  UNUSED_PARAMETER(xTask);
  stack_overflow_task = pcTaskName;
  taskDISABLE_INTERRUPTS();
  for (;;) {
  }
}

static sl_status_t network_event_handler(sl_net_event_t event, sl_status_t status, void *data, uint32_t data_length)
{
  UNUSED_PARAMETER(data_length);

  switch (event) {
    case SL_NET_PING_RESPONSE_EVENT: {
      sl_net_ping_response_t *response = (sl_net_ping_response_t *)data;
      ping_status                      = status;
      if (status != SL_STATUS_OK) {
        SL_DEBUG_LOG_V2(ERROR, "ICMP: ping failed status=0x%lx\r\n", (unsigned long)status);
      } else if (response != NULL) {
        SL_DEBUG_LOG_V2(INFO,
                        "ICMP: %u bytes from %u.%u.%u.%u\r\n",
                        (unsigned)response->ping_size,
                        response->ping_address.ipv4_address[0],
                        response->ping_address.ipv4_address[1],
                        response->ping_address.ipv4_address[2],
                        response->ping_address.ipv4_address[3]);
      }
      if (ping_sem != NULL) {
        osSemaphoreRelease(ping_sem);
      }
      break;
    }
    case SL_NET_DHCP_NOTIFICATION_EVENT:
      SL_DEBUG_LOG_V2(INFO, "DHCP notification status=0x%lx\r\n", (unsigned long)status);
      break;
    case SL_NET_IP_ADDRESS_CHANGE_EVENT: {
      sl_net_ip_configuration_t *ip_config = (sl_net_ip_configuration_t *)data;
      if (ip_config != NULL) {
        SL_DEBUG_LOG_V2(INFO,
                        "IP change status=0x%lx ip=%u.%u.%u.%u gw=%u.%u.%u.%u mask=%u.%u.%u.%u\r\n",
                        (unsigned long)status,
                        ip_config->ip.v4.ip_address.bytes[0],
                        ip_config->ip.v4.ip_address.bytes[1],
                        ip_config->ip.v4.ip_address.bytes[2],
                        ip_config->ip.v4.ip_address.bytes[3],
                        ip_config->ip.v4.gateway.bytes[0],
                        ip_config->ip.v4.gateway.bytes[1],
                        ip_config->ip.v4.gateway.bytes[2],
                        ip_config->ip.v4.gateway.bytes[3],
                        ip_config->ip.v4.netmask.bytes[0],
                        ip_config->ip.v4.netmask.bytes[1],
                        ip_config->ip.v4.netmask.bytes[2],
                        ip_config->ip.v4.netmask.bytes[3]);
      }
      break;
    }
    default:
      break;
  }
  return SL_STATUS_OK;
}

static void dump_station_ip_config(void)
{
  sl_net_wifi_client_profile_t profile;
  sl_status_t status;

  status = sl_net_get_profile(SL_NET_WIFI_CLIENT_INTERFACE, SL_NET_DEFAULT_WIFI_CLIENT_PROFILE_ID, &profile);
  if (status != SL_STATUS_OK) {
    SL_DEBUG_LOG_V2(ERROR, "IP dump: get_profile failed 0x%lx\r\n", (unsigned long)status);
    return;
  }

  SL_DEBUG_LOG_V2(INFO,
                  "STA ip=%u.%u.%u.%u gw=%u.%u.%u.%u mask=%u.%u.%u.%u host=%s\r\n",
                  profile.ip.ip.v4.ip_address.bytes[0],
                  profile.ip.ip.v4.ip_address.bytes[1],
                  profile.ip.ip.v4.ip_address.bytes[2],
                  profile.ip.ip.v4.ip_address.bytes[3],
                  profile.ip.ip.v4.gateway.bytes[0],
                  profile.ip.ip.v4.gateway.bytes[1],
                  profile.ip.ip.v4.gateway.bytes[2],
                  profile.ip.ip.v4.gateway.bytes[3],
                  profile.ip.ip.v4.netmask.bytes[0],
                  profile.ip.ip.v4.netmask.bytes[1],
                  profile.ip.ip.v4.netmask.bytes[2],
                  profile.ip.ip.v4.netmask.bytes[3],
                  (profile.ip.host_name != NULL) ? profile.ip.host_name : "(null)");
}

// ICMP echo to MQTT_BROKER_IP — proves whether NWP can ARP outside SOCKET_CREATE.
static void icmp_probe_gateway(void)
{
  sl_ip_address_t remote = { 0 };
  sl_status_t status;
  const uint32_t t0 = osKernelGetTickCount();

  if (ping_sem == NULL) {
    ping_sem = osSemaphoreNew(1, 0, NULL);
  }
  while (osSemaphoreAcquire(ping_sem, 0) == osOK) {
  }
  ping_status = SL_STATUS_FAIL;

  status = sl_net_inet_addr(MQTT_BROKER_IP, (uint32_t *)&remote.ip.v4);
  if (status != SL_STATUS_OK) {
    SL_DEBUG_LOG_V2(ERROR, "ICMP: bad target IP\r\n");
    return;
  }
  remote.type = SL_IPV4;

  SL_DEBUG_LOG_V2(INFO, "ICMP: ping %s ...\r\n", MQTT_BROKER_IP);
  status = sl_si91x_send_ping(remote, 64);
  if (status != SL_STATUS_IN_PROGRESS) {
    SL_DEBUG_LOG_V2(ERROR, "ICMP: send_ping status=0x%lx\r\n", (unsigned long)status);
    return;
  }

  if (osSemaphoreAcquire(ping_sem, 10000) != osOK) {
    SL_DEBUG_LOG_V2(ERROR,
                    "ICMP: no reply after %lu ms (watch capture for ARP/ICMP)\r\n",
                    (unsigned long)(osKernelGetTickCount() - t0));
    return;
  }

  SL_DEBUG_LOG_V2(INFO,
                  "ICMP: done status=0x%lx after %lu ms\r\n",
                  (unsigned long)ping_status,
                  (unsigned long)(osKernelGetTickCount() - t0));
}

// Prepare NWP TCP socket pool before MQTT connect.
static void network_tcp_prepare(void)
{
  sl_status_t status;
  const sl_si91x_socket_config_t socket_cfg = {
    .total_sockets                   = 3,
    .total_tcp_sockets               = 2,
    .total_udp_sockets               = 1,
    .tcp_tx_only_sockets             = 1,
    .tcp_rx_only_sockets             = 1,
    .udp_tx_only_sockets             = 1,
    .udp_rx_only_sockets             = 0,
    .tcp_rx_high_performance_sockets = 1,
    .tcp_rx_window_size_cap          = 10,
    .tcp_rx_window_div_factor        = 0,
  };

  status = sl_si91x_config_socket(socket_cfg);
  SL_DEBUG_LOG_V2(INFO, "TCP prepare: config_socket status=0x%lx\r\n", (unsigned long)status);
}

static uint32_t next_retry_delay(uint32_t delay_ms, uint32_t max_ms)
{
  return (delay_ms * 2u > max_ms) ? max_ms : delay_ms * 2u;
}

// Blocks until associated with an IP address. A failed join or DHCP handshake
// (e.g. 0x1FF9C) is often transient. Rescan on every attempt so a node can
// move to another AP if the first one is gone.
static void wifi_join(void)
{
  const sl_wifi_advanced_client_configuration_t advanced_client_config = {
    .max_retry_attempts      = 5,
    .scan_interval           = 4,
    .beacon_missed_count     = 40,
    .first_time_retry_enable = 1,
  };
  uint32_t retry_delay_ms = WIFI_RETRY_DELAY_MIN_MS;

  while (1) {
    dump_nearby_access_points();
    osDelay(500);

    sl_wifi_configure_timeout(SL_WIFI_CLIENT_2_4GHZ_INTERFACE, SL_WIFI_AUTHENTICATION_ASSOCIATION_TIMEOUT, 3000);
    sl_wifi_set_advanced_client_configuration(SL_WIFI_CLIENT_2_4GHZ_INTERFACE, &advanced_client_config);

    SL_DEBUG_LOG_V2(INFO, "Joining Wi-Fi (sl_net_up)...\r\n");
    sl_status_t status = sl_net_up(SL_NET_WIFI_CLIENT_INTERFACE, SL_NET_DEFAULT_WIFI_CLIENT_PROFILE_ID);
    if (status == SL_STATUS_OK) {
      break;
    }
    SL_DEBUG_LOG_V2(ERROR,
                    "Failed to bring Wi-Fi client interface up: 0x%lx, retrying in %lu s\r\n",
                    status,
                    (unsigned long)(retry_delay_ms / 1000u));
    osDelay(retry_delay_ms);
    retry_delay_ms = next_retry_delay(retry_delay_ms, WIFI_RETRY_DELAY_MAX_MS);
  }
  SL_DEBUG_LOG_V2(INFO, "Wi-Fi client connected\r\n");
}

// After beacon loss the NWP rejoins on its own (max_retry_attempts). A failed
// join event means it gave up, and only a fresh sl_net_up() will recover.
static sl_status_t wifi_join_callback(sl_wifi_event_t event,
                                      sl_status_t status_code,
                                      char *data,
                                      uint32_t data_length,
                                      void *arg)
{
  UNUSED_PARAMETER(data);
  UNUSED_PARAMETER(data_length);
  UNUSED_PARAMETER(arg);

  if (SL_WIFI_CHECK_IF_EVENT_FAILED(event) || (status_code != SL_STATUS_OK)) {
    SL_DEBUG_LOG_V2(ERROR, "Wi-Fi join event failed: 0x%lx\r\n", (unsigned long)status_code);
    osEventFlagsSet(net_events, NET_EVT_WIFI_DOWN);
  }
  return SL_STATUS_OK;
}

// The NWP keeps its own MQTT client instance until it gets a deinit. Resetting
// only the host-side struct left the next connect failing forever, so release
// it explicitly. A dead link makes the DISCONNECT command fail, after which the
// SDK sends only the deinit if the state is CONNECTION_FAILED.
static void mqtt_release_nwp_client(void)
{
  sl_status_t status = SL_STATUS_OK;

  if (client.state == SL_MQTT_CLIENT_CONNECTED) {
    status = sl_mqtt_client_disconnect(&client, MQTT_DISCONNECT_TIMEOUT_MS);
  }
  if (client.state != SL_MQTT_CLIENT_DISCONNECTED) {
    client.state = SL_MQTT_CLIENT_CONNECTION_FAILED;
    status       = sl_mqtt_client_disconnect(&client, MQTT_DISCONNECT_TIMEOUT_MS);
  }
  SL_DEBUG_LOG_V2(INFO, "MQTT client released on the NWP: 0x%lx\r\n", (unsigned long)status);
}

// Consumer: owns Wi-Fi and MQTT after bring-up. Reconnects with backoff and
// drains the reading queue only while the broker is connected.
static void network_loop(void)
{
  sensor_reading_t pending;
  bool has_pending        = false;
  bool recovered_once           = false;
  uint32_t fails_after_recovery = 0;
  uint32_t retry_delay_ms       = MQTT_RETRY_DELAY_MIN_MS;

  while (1) {
    if ((osEventFlagsGet(net_events) & NET_EVT_WIFI_DOWN) != 0u) {
      SL_DEBUG_LOG_V2(ERROR, "Wi-Fi down, rejoining\r\n");
      mqtt_release_nwp_client();
      (void)sl_net_down(SL_NET_WIFI_CLIENT_INTERFACE);
      wifi_join();
      osEventFlagsClear(net_events, NET_EVT_WIFI_DOWN);
      dump_station_ip_config();
      (void)sl_mqtt_client_init(&client, mqtt_client_event_handler);
      // A rejoin after a join-failure event is a recovery too, so connects
      // that keep failing afterwards still end in a reset.
      recovered_once       = true;
      fails_after_recovery = 0;
    }

    sl_status_t status = mqtt_connect_and_wait();
    if (status == SL_STATUS_OK) {
      bool published = false;
      retry_delay_ms = MQTT_RETRY_DELAY_MIN_MS;
      status         = publish_until_disconnected(&pending, &has_pending, &published);
      if (published) {
        recovered_once = false;
      }
      if (status != SL_STATUS_OK) {
        recover_or_reset("MQTT session dead without a disconnect event", &recovered_once);
      }
      continue;
    }

    // Stuck client or an attempt that never resolved: retrying the connect
    // cannot help.
    if ((status == SL_STATUS_INVALID_STATE) || (status == SL_STATUS_TIMEOUT)) {
      recover_or_reset("MQTT client stuck", &recovered_once);
      continue;
    }

    // A clean failure normally means the broker is down. Right after a Wi-Fi
    // restart it can also mean the NWP's MQTT client is wedged, which only a
    // reset clears.
    if (recovered_once && (++fails_after_recovery >= MQTT_FAILS_AFTER_RECOVERY)) {
      recover_or_reset("MQTT connect keeps failing", &recovered_once);
    }
    SL_DEBUG_LOG_V2(ERROR, "MQTT not connected, retrying in %lu s\r\n", (unsigned long)(retry_delay_ms / 1000u));
    // Wakes early if Wi-Fi is lost, so the rejoin is not held up by the backoff.
    (void)osEventFlagsWait(net_events, NET_EVT_WIFI_DOWN, osFlagsWaitAny | osFlagsNoClear, retry_delay_ms);
    retry_delay_ms = next_retry_delay(retry_delay_ms, MQTT_RETRY_DELAY_MAX_MS);
  }
}

// Restart Wi-Fi and the MQTT client once. If that does not get a publish
// through, reset the MCU, which is known to recover.
static void recover_or_reset(const char *reason, bool *recovered_once)
{
  if (!*recovered_once) {
    SL_DEBUG_LOG_V2(ERROR, "%s, restarting Wi-Fi and the MQTT client\r\n", reason);
    *recovered_once = true;
    osEventFlagsSet(net_events, NET_EVT_WIFI_DOWN);
    return;
  }
  SL_DEBUG_LOG_V2(ERROR, "%s again after a Wi-Fi restart, resetting the MCU\r\n", reason);
  osDelay(100);
  NVIC_SystemReset();
}

static sl_status_t mqtt_connect_and_wait(void)
{
  static uint32_t connect_count;

  osEventFlagsClear(net_events, NET_EVT_MQTT_UP | NET_EVT_MQTT_DOWN);

  connect_count++;
  mqtt_client_configuration.client_port =
    (uint16_t)(CLIENT_PORT_MIN + ((osKernelGetTickCount() + connect_count * 7919u) % CLIENT_PORT_RANGE));

  sl_status_t status =
    sl_mqtt_client_connect_v2(&client, &mqtt_broker_configuration, NULL, &mqtt_client_configuration, 0);
  if (status == SL_STATUS_INVALID_STATE) {
    // A failed connect whose own cleanup also failed leaves the client outside
    // DISCONNECTED. sl_mqtt_client_disconnect() did not clear it on the board;
    // the network loop escalates instead.
    SL_DEBUG_LOG_V2(ERROR, "MQTT client in wrong state\r\n");
    return status;
  }
  if (status != SL_STATUS_IN_PROGRESS) {
    // TIMEOUT almost always means the sync firmware MQTT INIT (TCP open to the
    // broker) failed, not MQTT AUTH.
    SL_DEBUG_LOG_V2(ERROR, "Failed to connect to mqtt broker: 0x%lx\r\n", status);
    return status;
  }

  uint32_t flags = osEventFlagsWait(net_events,
                                    NET_EVT_MQTT_UP | NET_EVT_MQTT_DOWN | NET_EVT_WIFI_DOWN,
                                    osFlagsWaitAny | osFlagsNoClear,
                                    MQTT_CONNECT_WAIT_MS);
  if ((flags & osFlagsError) != 0u) {
    SL_DEBUG_LOG_V2(ERROR, "MQTT CONNECTED event not received in %u ms\r\n", MQTT_CONNECT_WAIT_MS);
    return SL_STATUS_TIMEOUT;
  }
  if (((flags & NET_EVT_MQTT_UP) == 0u) || ((flags & (NET_EVT_MQTT_DOWN | NET_EVT_WIFI_DOWN)) != 0u)) {
    return SL_STATUS_FAIL;
  }
  return SL_STATUS_OK;
}

// Returns SL_STATUS_OK when the broker or Wi-Fi reports a disconnect, and
// SL_STATUS_FAIL when the session is dead without any disconnect event.
// *published is set once a publish is confirmed. A reading whose publish was
// not confirmed stays in *pending and goes out first after the reconnect.
static sl_status_t publish_until_disconnected(sensor_reading_t *pending, bool *has_pending, bool *published)
{
  const uint32_t down = NET_EVT_MQTT_DOWN | NET_EVT_WIFI_DOWN;
  uint32_t rejections = 0;
  uint32_t flags;

  while (1) {
    if (!*has_pending) {
      if (osMessageQueueGet(reading_queue, pending, NULL, 0) != osOK) {
        flags = osEventFlagsWait(net_events, NET_EVT_READING | down, osFlagsWaitAny | osFlagsNoClear, osWaitForever);
        if (((flags & osFlagsError) != 0u) || ((flags & down) != 0u)) {
          return SL_STATUS_OK;
        }
        // Cleared before draining, so a reading queued from here on sets it again.
        osEventFlagsClear(net_events, NET_EVT_READING);
        continue;
      }
      *has_pending = true;
    }

    osEventFlagsClear(net_events, NET_EVT_PUBLISHED | NET_EVT_PUBLISH_FAILED);
    sl_status_t status = publish_reading(pending);
    if (status == SL_STATUS_INVALID_STATE) {
      return SL_STATUS_OK;
    }
    if (status != SL_STATUS_IN_PROGRESS) {
      printf("MQTT publish failed: 0x%lx, reading dropped\r\n", (unsigned long)status);
      *has_pending = false;
      continue;
    }

    flags = osEventFlagsWait(net_events,
                             NET_EVT_PUBLISHED | NET_EVT_PUBLISH_FAILED | down,
                             osFlagsWaitAny | osFlagsNoClear,
                             MQTT_PUBLISH_WAIT_MS);
    if ((flags & osFlagsError) != 0u) {
      SL_DEBUG_LOG_V2(ERROR, "MQTT publish not confirmed in %u ms\r\n", MQTT_PUBLISH_WAIT_MS);
      return SL_STATUS_FAIL;
    }
    if ((flags & NET_EVT_PUBLISHED) != 0u) {
      *has_pending = false;
      *published   = true;
      rejections   = 0;
    } else if ((flags & down) != 0u) {
      return SL_STATUS_OK;
    } else {
      // After a Wi-Fi drop the NWP rejected every publish while the SDK still
      // reported CONNECTED and no disconnect event ever arrived. The reading
      // is fine; the session is not.
      rejections++;
      printf("MQTT publish rejected by the NWP (%lu in a row)\r\n", (unsigned long)rejections);
      if (rejections >= MQTT_PUBLISH_REJECT_LIMIT) {
        return SL_STATUS_FAIL;
      }
      (void)osEventFlagsWait(net_events, down, osFlagsWaitAny | osFlagsNoClear, MQTT_PUBLISH_RETRY_MS);
    }
  }
}

static void application_start(void *argument)
{
  UNUSED_PARAMETER(argument);
  sl_status_t status;
  sl_i2c_status_t i2c_status;

  // Bring up the NWP before touching ULP I2C / SENSOR_ENABLE. Host WLAN threads
  // are created first, then sli_si91x_bootup_firmware() waits for TA_is_active
  // with no timeout; ULP activity during that window can leave the app stuck
  // after "Thread priority order is correct" with no error return.
  SL_DEBUG_LOG_V2(INFO, "Starting Wi-Fi (sl_net_init)...\r\n");
  status = sl_net_init(SL_NET_WIFI_CLIENT_INTERFACE, &wifi_mqtt_client_configuration, NULL, network_event_handler);
  if (status != SL_STATUS_OK && status != SL_STATUS_ALREADY_INITIALIZED) {
    SL_DEBUG_LOG_V2(ERROR, "Failed to start Wi-Fi client interface: 0x%lx\r\n", status);
    return;
  }
  SL_DEBUG_LOG_V2(INFO, "Wi-Fi client interface up Success\r\n");

  wifi_join();

  {
    sl_mac_address_t sta_mac          = { 0 };
    sl_wifi_firmware_version_t fw_ver = { 0 };
    sl_wifi_performance_profile_v2_t perf = {
      .profile = SL_WIFI_SYSTEM_HIGH_PERFORMANCE,
    };
    if (sl_wifi_get_mac_address(SL_WIFI_CLIENT_INTERFACE, &sta_mac) == SL_STATUS_OK) {
      snprintf(sensor_id_mac,
               sizeof(sensor_id_mac),
               "%02X%02X%02X%02X%02X%02X",
               sta_mac.octet[0],
               sta_mac.octet[1],
               sta_mac.octet[2],
               sta_mac.octet[3],
               sta_mac.octet[4],
               sta_mac.octet[5]);
      SL_DEBUG_LOG_V2(INFO, "STA mac=%s (sensor id)\r\n", sensor_id_mac);
    }
    if (sl_wifi_get_firmware_version(&fw_ver) == SL_STATUS_OK) {
      SL_DEBUG_LOG_V2(INFO,
                      "NWP fw %u.%u.%u.%u.%u.%u build=%u\r\n",
                      (unsigned)fw_ver.chip_id,
                      (unsigned)fw_ver.rom_id,
                      (unsigned)fw_ver.major,
                      (unsigned)fw_ver.minor,
                      (unsigned)fw_ver.security_version,
                      (unsigned)fw_ver.patch_num,
                      (unsigned)fw_ver.build_num);
    }
    sl_status_t perf_status = sl_wifi_set_performance_profile_v2(&perf);
    SL_DEBUG_LOG_V2(INFO, "performance HIGH status=0x%lx\r\n", (unsigned long)perf_status);
  }

  dump_station_ip_config();
  // Socket pool — needed before any TCP open / ARP.
  network_tcp_prepare();
  icmp_probe_gateway();

  status = si70xx_enable_sensor();
  if (status != SL_STATUS_OK) {
    return;
  }
  SL_DEBUG_LOG_V2(INFO, "Si70xx enable pin high v21\r\n");

  i2c_status = sl_si91x_i2c_pin_init(&sl_i2c_i2c2_pin_init);
  if (i2c_status != SL_I2C_SUCCESS) {
    SL_DEBUG_LOG_V2(ERROR, "Failed to pin-init I2C for Si70xx: 0x%lx\r\n", i2c_status);
    return;
  }

  i2c_status = sl_i2c_driver_init(SI70XX_I2C_INSTANCE, &sl_i2c_i2c2_config);
  if (i2c_status != SL_I2C_SUCCESS) {
    SL_DEBUG_LOG_V2(ERROR, "Failed to init I2C for Si70xx: 0x%lx\r\n", i2c_status);
    return;
  }

  // sl_si91x_i2c_init() leaves the block disabled. TX FIFO empty is then
  // always true, so writes return success with no SCL, and the first read
  // times out (0x10). Configuring the FIFO thresholds re-enables IC_ENABLE.
  i2c_status = sl_i2c_driver_configure_fifo_threshold(SI70XX_I2C_INSTANCE, 0, 0);
  if (i2c_status != SL_I2C_SUCCESS) {
    SL_DEBUG_LOG_V2(ERROR, "Failed to enable I2C FIFO for Si70xx: 0x%lx\r\n", i2c_status);
    return;
  }

  SL_DEBUG_LOG_V2(INFO,
                  "I2C init for Si70xx Success en=%u\r\n",
                  (unsigned)((I2C0_Type *)I2C2_BASE)->IC_ENABLE_b.EN);

  // Reset is a single write and must end with STOP. Combined write-then-read
  // (EID / hold-master measure) is handled inside si70xx_float.c.
  status = sl_si91x_si70xx_reset(SI70XX_I2C_INSTANCE, SI70XX_SLAVE_ADDR);
  if (status != SL_STATUS_OK) {
    SL_DEBUG_LOG_V2(ERROR, "Failed to reset Si70xx sensor: 0x%lx\r\n", status);
    return;
  }
  osDelay(SI70XX_POWER_ON_DELAY_MS);

  uint8_t device_id = 0;
  status            = si70xx_float_read_device_id(SI70XX_I2C_INSTANCE, SI70XX_SLAVE_ADDR, &device_id);
  if (status != SL_STATUS_OK) {
    SL_DEBUG_LOG_V2(ERROR, "Failed to read Si70xx device ID: 0x%lx\r\n", status);
    return;
  }
  if (device_id == SI70XX_DEVICE_ID_SI7021) {
    SL_DEBUG_LOG_V2(INFO, "Si7021 detected, device ID 0x%lx\r\n", (unsigned long)device_id);
  } else {
    SL_DEBUG_LOG_V2(ERROR,
                    "Unexpected sensor on the bus: device ID 0x%lx, expected 0x%lx (Si7021)\r\n",
                    (unsigned long)device_id,
                    (unsigned long)SI70XX_DEVICE_ID_SI7021);
  }

  status = adc_ch1_init();
  if (status != SL_STATUS_OK) {
    return;
  }

  // Created before anything that can fire a callback which sets them.
  net_events    = osEventFlagsNew(NULL);
  reading_queue = osMessageQueueNew(READING_QUEUE_LENGTH, sizeof(sensor_reading_t), NULL);
  if ((net_events == NULL) || (reading_queue == NULL)) {
    SL_DEBUG_LOG_V2(ERROR, "Failed to create the reading queue or event flags\r\n");
    return;
  }
  sl_wifi_set_join_callback_v2(wifi_join_callback, NULL);

  if (mqtt_setup() != SL_STATUS_OK) {
    return;
  }

  net_thread    = osThreadGetId();
  sensor_thread = osThreadNew((osThreadFunc_t)sensor_task, NULL, &sensor_thread_attributes);
  if (sensor_thread == NULL) {
    SL_DEBUG_LOG_V2(ERROR, "Failed to create the sensor thread\r\n");
    return;
  }

  network_loop();
}

void mqtt_client_cleanup()
{
  SL_CLEANUP_MALLOC(client_credentails);
}

void print_char_buffer(char *buffer, uint32_t buffer_length)
{
  if (buffer == NULL || buffer_length == 0) {
    SL_DEBUG_LOG_V2(ERROR, "Buffer is NULL or buffer_length is 0");
    return;
  }

  if (buffer_length > SL_MQTT_CLIENT_MAX_RX_PAYLOAD_SIZE) {
    buffer_length = SL_MQTT_CLIENT_MAX_RX_PAYLOAD_SIZE;
  }

  char *line = (char *)malloc(buffer_length + 1);
  if (line == NULL) {
    SL_DEBUG_LOG_V2(ERROR, "print_char_buffer: malloc failed");
    return;
  }

  memcpy(line, buffer, buffer_length);
  line[buffer_length] = '\0';
  SL_DEBUG_LOG_V2(INFO, "%s", (uintptr_t)line);
  free(line);
}

void mqtt_client_error_event_handler(void *client, sl_mqtt_client_error_status_t *error)
{
  UNUSED_PARAMETER(client);

  switch (*error) {
    case SL_MQTT_CLIENT_RECEIVE_FAILED:
      SL_DEBUG_LOG_V2(ERROR, "MQTT Error: Message receive failed.\r\n");
      break;

    case SL_MQTT_CLIENT_RECEIVE_PAYLOAD_TOO_LARGE:
      SL_DEBUG_LOG_V2(ERROR,
                      "MQTT Error: Received payload exceeds max size (%u bytes). "
                      "Increase SL_MQTT_CLIENT_MAX_RX_PAYLOAD_SIZE.",
                      SL_MQTT_CLIENT_MAX_RX_PAYLOAD_SIZE);
      break;

    case SL_MQTT_CLIENT_RECEIVE_MEMORY_ALLOCATION_FAILED:
      SL_DEBUG_LOG_V2(ERROR, "MQTT Error: Failed to allocate memory for message reassembly.");
      break;

    case SL_MQTT_CLIENT_RECEIVE_DATA_CORRUPTED:
      SL_DEBUG_LOG_V2(ERROR, "MQTT Error: Data corruption detected during message reassembly.");
      break;

    case SL_MQTT_CLIENT_CONNECT_FAILED:
      // The SDK has already sent the NWP deinit; the network loop retries.
      SL_DEBUG_LOG_V2(ERROR, "MQTT Error: connect failed.\r\n");
      osEventFlagsSet(net_events, NET_EVT_MQTT_DOWN);
      break;

    case SL_MQTT_CLIENT_PUBLISH_FAILED:
      SL_DEBUG_LOG_V2(ERROR, "MQTT Error: publish failed.\r\n");
      osEventFlagsSet(net_events, NET_EVT_PUBLISH_FAILED);
      break;

    default:
      SL_DEBUG_LOG_V2(ERROR, "MQTT Error: %d\r\n", *error);
      break;
  }
}

// Runs on the SDK's event thread: only signal the network loop, never block.
void mqtt_client_event_handler(void *client, sl_mqtt_client_event_t event, void *event_data, void *context)
{
  switch (event) {
    case SL_MQTT_CLIENT_CONNECTED_EVENT: {
      SL_DEBUG_LOG_V2(INFO, "Connected to MQTT broker\r\n");
      osEventFlagsSet(net_events, NET_EVT_MQTT_UP);
      break;
    }

    case SL_MQTT_CLIENT_MESSAGE_PUBLISHED_EVENT: {
      sl_mqtt_client_message_t *published_message = (sl_mqtt_client_message_t *)context;

      SL_DEBUG_LOG_V2(INFO, "Published message successfully on topic: ");
      print_char_buffer((char *)published_message->topic, published_message->topic_length);
      osEventFlagsSet(net_events, NET_EVT_PUBLISHED);
      break;
    }

    case SL_MQTT_CLIENT_DISCONNECTED_EVENT: {
      const sl_mqtt_client_disconnection_reason_t *reason = (const sl_mqtt_client_disconnection_reason_t *)event_data;
      SL_DEBUG_LOG_V2(INFO,
                      "Disconnected from MQTT broker, reason %d\r\n",
                      (reason != NULL) ? (int)*reason : -1);
      osEventFlagsSet(net_events, NET_EVT_MQTT_DOWN);
      break;
    }

    case SL_MQTT_CLIENT_ERROR_EVENT: {
      mqtt_client_error_event_handler(client, (sl_mqtt_client_error_status_t *)event_data);
      break;
    }
    default:
      break;
  }
}

// One-time setup: credentials, client init and broker address. Connecting is
// left to the network loop so it can be repeated after a disconnect.
static sl_status_t mqtt_setup(void)
{
  sl_status_t status;

  if (ENCRYPT_CONNECTION) {
    // Load SSL CA certificate
    status = sl_net_set_credential(SL_NET_TLS_SERVER_CREDENTIAL_ID(CERTIFICATE_INDEX),
                                   SL_NET_SIGNING_CERTIFICATE,
                                   cacert,
                                   sizeof(cacert) - 1);
    if (status != SL_STATUS_OK) {
      SL_DEBUG_LOG_V2(ERROR, "Loading TLS CA certificate in to FLASH Failed, Error Code : 0x%lX\r\n", status);
      return status;
    }
    SL_DEBUG_LOG_V2(INFO, "Load TLS CA certificate at index %d Success\r\n", CERTIFICATE_INDEX);
  }

  if (SEND_CREDENTIALS) {
    uint16_t username_length, password_length;

    username_length = strlen(USERNAME);
    password_length = strlen(PASSWORD);

    uint32_t malloc_size = sizeof(sl_mqtt_client_credentials_t) + username_length + password_length;

    client_credentails = malloc(malloc_size);
    if (client_credentails == NULL)
      return SL_STATUS_ALLOCATION_FAILED;
    memset(client_credentails, 0, malloc_size);
    client_credentails->username_length = username_length;
    client_credentails->password_length = password_length;

    memcpy(&client_credentails->data[0], USERNAME, username_length);
    memcpy(&client_credentails->data[username_length], PASSWORD, password_length);

    status = sl_net_set_credential(SL_NET_MQTT_CLIENT_CREDENTIAL_ID(0),
                                   SL_NET_MQTT_CLIENT_CREDENTIAL,
                                   client_credentails,
                                   malloc_size);

    if (status != SL_STATUS_OK) {
      mqtt_client_cleanup();
      SL_DEBUG_LOG_V2(ERROR, "Failed to set credentials: 0x%lx ", status);

      return status;
    }
    SL_DEBUG_LOG_V2(INFO, "Set credentials Success\r\n");

    free(client_credentails);
    client_credentails = NULL;
    mqtt_client_configuration.credential_id = SL_NET_MQTT_CLIENT_CREDENTIAL_ID(0);
  }

  status = sl_mqtt_client_init(&client, mqtt_client_event_handler);
  if (status != SL_STATUS_OK) {
    SL_DEBUG_LOG_V2(ERROR, "Failed to init mqtt client: 0x%lx\r\n", status);

    mqtt_client_cleanup();
    return status;
  }
  SL_DEBUG_LOG_V2(INFO, "Init mqtt client Success\r\n");

  // Intentionally skip sl_mqtt_client_set_tcp_tls_advanced_configuration():
  // it is not used by the SDK example and is unrelated to ENCRYPT_CONNECTION.

#ifdef SLI_SI91X_ENABLE_IPV6
  unsigned char hex_addr[SL_IPV6_ADDRESS_LENGTH] = { 0 };
  status                                         = sl_inet_pton6(MQTT_BROKER_IP,
                         MQTT_BROKER_IP + strlen(MQTT_BROKER_IP),
                         hex_addr,
                         (unsigned int *)mqtt_broker_configuration.ip.ip.v6.value);
  if (status != 0x1) {
    SL_DEBUG_LOG_V2(ERROR, "IPv6 conversion failed.\r\n");
    mqtt_client_cleanup();
    return status;
  }
  mqtt_broker_configuration.ip.type = SL_IPV6;
#else
  status = sl_net_inet_addr(MQTT_BROKER_IP, &mqtt_broker_configuration.ip.ip.v4.value);
  if (status != SL_STATUS_OK) {
    SL_DEBUG_LOG_V2(ERROR, "Failed to convert IP address \r\n");

    mqtt_client_cleanup();
    return status;
  }
  mqtt_broker_configuration.ip.type = SL_IPV4;
#endif

  SL_DEBUG_LOG_V2(INFO,
                  "MQTT broker %u.%u.%u.%u:%u encrypt=%u\r\n",
                  mqtt_broker_configuration.ip.ip.v4.bytes[0],
                  mqtt_broker_configuration.ip.ip.v4.bytes[1],
                  mqtt_broker_configuration.ip.ip.v4.bytes[2],
                  mqtt_broker_configuration.ip.ip.v4.bytes[3],
                  (unsigned)mqtt_broker_configuration.port,
                  (unsigned)mqtt_broker_configuration.is_connection_encrypted);

  // Let DHCP/ARP settle; IP-info notify already timed out once after join.
  osDelay(2000);

  return SL_STATUS_OK;
}
