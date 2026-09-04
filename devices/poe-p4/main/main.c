#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/ip_addr.h"
#include "lwip/etharp.h"
#include "lwip/netdb.h"
#include "lwip/tcpip.h"
#include "ping/ping_sock.h"
#include "generated_trust.h"

#define RC_FIRMWARE_VERSION "0.3.0"
#define RC_PROTOCOL "reconclave/1"
#define RC_HOSTNAME "reconclave-poe-p4"
#define RC_INSTANCE "Reconclave Unit PoE-P4"
#define RC_HTTP_PORT 8765
#define RC_MAX_REQUEST_BYTES 4096

#define POE_P4_PHY_ADDR 1
#define POE_P4_PHY_RESET_GPIO 51
#define POE_P4_MDC_GPIO 31
#define POE_P4_MDIO_GPIO 52
#define POE_P4_LED_GREEN_GPIO 15
#define POE_P4_LED_BLUE_GPIO 16
#define POE_P4_LED_RED_GPIO 17
#define POE_P4_GROVE_TX_GPIO 53
#define POE_P4_GROVE_RX_GPIO 54
#define RC_GROVE_UART UART_NUM_1
#define RC_GROVE_BAUD 115200
#define RC_PAIRING_NAMESPACE "rc_trust"
#define RC_PAIRING_KEY "peer_key"
#define RC_PAIRING_PEER "peer_id"
#define RC_KEY_BYTES 32
#define RC_TAG_BYTES 16
#define RC_SCAN_MAX_HOSTS 254
#define RC_SCAN_MAX_RESULTS 48
#define RC_SCAN_PING_TIMEOUT_MS 120
#define RC_RECUR_MIN_INTERVAL_MS 10000
#define RC_RECUR_MAX_INTERVAL_MS 86400000
#define RC_AUTOMATION_NAMESPACE "rc_auto"
#define RC_AUTOMATION_RULES_KEY "rules"
#define RC_AUTOMATION_OUTBOX_KEY "outbox"
#define RC_MAX_RULES 4
#define RC_MAX_OUTBOX 3
#define RC_LEASE_MIN_MS 1000
#define RC_LEASE_MAX_MS 60000

typedef struct {
    bool started;
    bool link_up;
    bool has_ip;
    char ip[16];
    char gateway[16];
} network_state_t;

static const char *TAG = "reconclave_p4";
static network_state_t s_network;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_eth_handle_t s_eth_handle;
static esp_netif_t *s_eth_netif;
static char s_device_id[32] = "rc-p4-unknown";
static uint32_t s_sequence;
static uint8_t s_peer_key[RC_KEY_BYTES];
static bool s_peer_key_valid;
static char s_peer_id[32];
static uint8_t s_boot_nonce[16];
static char s_boot_nonce_hex[33];
static uint64_t s_recent_nonces[16];
static size_t s_recent_nonce_cursor;
static char s_lease_owner_id[32];
static uint8_t s_lease_priority;
static uint32_t s_lease_expires_ms;

typedef enum {
    RC_SCAN_IDLE,
    RC_SCAN_RUNNING,
    RC_SCAN_COMPLETE,
    RC_SCAN_FAILED,
} scan_status_t;

typedef struct {
    scan_status_t status;
    uint32_t job_id;
    uint16_t checked;
    uint16_t total;
    uint8_t first_host;
    uint8_t last_host;
    uint8_t result_count;
    bool recurring;
    volatile bool cancel_requested;
    uint32_t interval_ms;
    uint32_t run_count;
    char rule_id[32];
    char project_id[64];
    char results[RC_SCAN_MAX_RESULTS][16];
    char error[40];
} scan_state_t;

static scan_state_t s_scan;

typedef enum { RC_CONDITION_DHCP = 1, RC_CONDITION_INTERNET = 2 } rule_condition_t;
typedef enum { RC_PLAYBOOK_SCOUT = 1, RC_PLAYBOOK_SNAPSHOT = 2 } rule_playbook_t;
typedef struct {
    uint8_t version;
    bool enabled;
    uint8_t condition;
    uint8_t playbook;
    uint32_t interval_ms;
    char id[32];
    char project_id[64];
} automation_rule_t;
typedef struct {
    uint8_t version;
    uint32_t sequence;
    uint32_t run_count;
    uint8_t host_count;
    char rule_id[32];
    char project_id[64];
    char kind[24];
    char ip[16];
    char hosts[RC_SCAN_MAX_RESULTS][16];
} outbox_record_t;
static automation_rule_t s_rules[RC_MAX_RULES];
static bool s_rule_matched[RC_MAX_RULES];
static outbox_record_t s_outbox[RC_MAX_OUTBOX];
static uint32_t s_outbox_sequence;

static void hex_encode(char *destination, const uint8_t *source, size_t length)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) {
        destination[i * 2] = digits[source[i] >> 4];
        destination[i * 2 + 1] = digits[source[i] & 0x0f];
    }
    destination[length * 2] = '\0';
}

static int hex_nibble(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool hex_decode(uint8_t *destination, size_t length, const char *source)
{
    if (source == NULL || strlen(source) != length * 2) return false;
    for (size_t i = 0; i < length; ++i) {
        const int high = hex_nibble(source[i * 2]);
        const int low = hex_nibble(source[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        destination[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static bool constant_time_equal(const uint8_t *left, const uint8_t *right, size_t length)
{
    uint8_t difference = 0;
    for (size_t i = 0; i < length; ++i) difference |= left[i] ^ right[i];
    return difference == 0;
}

static uint32_t crc32(const char *data, size_t length)
{
    uint32_t crc = 0xffffffffU;
    for (size_t index = 0; index < length; ++index) {
        crc ^= (uint8_t)data[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}

static bool compute_tag_with_key(const uint8_t key[RC_KEY_BYTES], const char *message,
                                 uint8_t output[RC_TAG_BYTES])
{
    if (key == NULL || message == NULL) return false;
    uint8_t full[32];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL || mbedtls_md_hmac(info, key, RC_KEY_BYTES,
                                       (const uint8_t *)message, strlen(message), full) != 0) {
        return false;
    }
    memcpy(output, full, RC_TAG_BYTES);
    return true;
}

static const rc_provisioned_peer_t *provisioned_peer(const char *peer_id)
{
    if (peer_id == NULL) return NULL;
    for (size_t index = 0; index < sizeof(RC_PROVISIONED_PEERS) / sizeof(RC_PROVISIONED_PEERS[0]); ++index) {
        if (strcmp(peer_id, RC_PROVISIONED_PEERS[index].peer_id) == 0) {
            return &RC_PROVISIONED_PEERS[index];
        }
    }
    return NULL;
}

static bool nonce_seen_or_record(uint64_t nonce)
{
    for (size_t i = 0; i < sizeof(s_recent_nonces) / sizeof(s_recent_nonces[0]); ++i) {
        if (nonce != 0 && s_recent_nonces[i] == nonce) return true;
    }
    s_recent_nonces[s_recent_nonce_cursor++ %
                    (sizeof(s_recent_nonces) / sizeof(s_recent_nonces[0]))] = nonce;
    return nonce == 0;
}

static void load_pairing(void)
{
    nvs_handle_t handle;
    if (nvs_open(RC_PAIRING_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    size_t key_length = sizeof(s_peer_key);
    size_t peer_length = sizeof(s_peer_id);
    if (nvs_get_blob(handle, RC_PAIRING_KEY, s_peer_key, &key_length) == ESP_OK &&
        key_length == sizeof(s_peer_key) &&
        nvs_get_str(handle, RC_PAIRING_PEER, s_peer_id, &peer_length) == ESP_OK &&
        s_peer_id[0] != '\0') {
        s_peer_key_valid = true;
    }
    nvs_close(handle);
}

static bool store_pairing(const char *peer_id, const uint8_t key[RC_KEY_BYTES])
{
    nvs_handle_t handle;
    if (nvs_open(RC_PAIRING_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t result = nvs_set_blob(handle, RC_PAIRING_KEY, key, RC_KEY_BYTES);
    if (result == ESP_OK) result = nvs_set_str(handle, RC_PAIRING_PEER, peer_id);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) return false;
    memcpy(s_peer_key, key, RC_KEY_BYTES);
    const size_t peer_id_length = strnlen(peer_id, sizeof(s_peer_id) - 1);
    memcpy(s_peer_id, peer_id, peer_id_length);
    s_peer_id[peer_id_length] = '\0';
    s_peer_key_valid = true;
    return true;
}

static void load_automation_state(void)
{
    nvs_handle_t handle;
    if (nvs_open(RC_AUTOMATION_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    size_t length = sizeof(s_rules);
    if (nvs_get_blob(handle, RC_AUTOMATION_RULES_KEY, s_rules, &length) != ESP_OK ||
        length != sizeof(s_rules)) memset(s_rules, 0, sizeof(s_rules));
    length = sizeof(s_outbox);
    if (nvs_get_blob(handle, RC_AUTOMATION_OUTBOX_KEY, s_outbox, &length) != ESP_OK ||
        length != sizeof(s_outbox)) memset(s_outbox, 0, sizeof(s_outbox));
    for (size_t index = 0; index < RC_MAX_OUTBOX; ++index) {
        if (s_outbox[index].sequence > s_outbox_sequence) s_outbox_sequence = s_outbox[index].sequence;
    }
    nvs_close(handle);
}

static bool save_automation_blob(const char *key, const void *value, size_t length)
{
    nvs_handle_t handle;
    if (nvs_open(RC_AUTOMATION_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t result = nvs_set_blob(handle, key, value, length);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}

static void queue_evidence(const char *rule_id, const char *project_id, const char *kind,
                           uint32_t run_count, char hosts[][16], uint8_t host_count)
{
    outbox_record_t record = {.version = 1, .sequence = ++s_outbox_sequence,
                              .run_count = run_count,
                              .host_count = host_count > RC_SCAN_MAX_RESULTS ? RC_SCAN_MAX_RESULTS : host_count};
    strlcpy(record.rule_id, rule_id, sizeof(record.rule_id));
    strlcpy(record.project_id, project_id, sizeof(record.project_id));
    strlcpy(record.kind, kind, sizeof(record.kind));
    strlcpy(record.ip, s_network.ip, sizeof(record.ip));
    for (size_t index = 0; index < record.host_count; ++index) {
        strlcpy(record.hosts[index], hosts[index], sizeof(record.hosts[index]));
    }
    memmove(&s_outbox[0], &s_outbox[1], sizeof(s_outbox[0]) * (RC_MAX_OUTBOX - 1));
    s_outbox[RC_MAX_OUTBOX - 1] = record;
    save_automation_blob(RC_AUTOMATION_OUTBOX_KEY, s_outbox, sizeof(s_outbox));
}

static uint64_t timestamp_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000) + 1;
}

static uint32_t next_sequence(void)
{
    uint32_t value;
    portENTER_CRITICAL(&s_lock);
    value = ++s_sequence;
    portEXIT_CRITICAL(&s_lock);
    return value;
}

static void grove_send(const char *payload)
{
    char frame[256];
    const uint32_t checksum = crc32(payload, strlen(payload));
    const int length = snprintf(frame, sizeof(frame), "%s,%08lx\n", payload,
                                (unsigned long)checksum);
    if (length > 0 && length < (int)sizeof(frame)) {
        uart_write_bytes(RC_GROVE_UART, frame, (size_t)length);
    }
}

static void process_pairing_frame(char *line)
{
    char *checksum_separator = strrchr(line, ',');
    if (checksum_separator == NULL) return;
    char *checksum_end = NULL;
    const uint32_t received = strtoul(checksum_separator + 1, &checksum_end, 16);
    if (checksum_end == checksum_separator + 1 || *checksum_end != '\0' ||
        received != crc32(line, (size_t)(checksum_separator - line))) return;
    *checksum_separator = '\0';
    if (strncmp(line, "RC1,P,", 6) != 0) return;
    char *peer_id = line + 6;
    char *target = strchr(peer_id, ',');
    if (target == NULL) return;
    *target++ = '\0';
    char *key_hex = strchr(target, ',');
    if (key_hex == NULL) return;
    *key_hex++ = '\0';
    if (strcmp(target, s_device_id) != 0 || peer_id[0] == '\0' || strlen(peer_id) >= 32) return;
    uint8_t candidate[RC_KEY_BYTES];
    if (!hex_decode(candidate, sizeof(candidate), key_hex)) return;

    bool accepted = false;
    if (!s_peer_key_valid) accepted = store_pairing(peer_id, candidate);
    else accepted = strcmp(peer_id, s_peer_id) == 0 &&
                    constant_time_equal(candidate, s_peer_key, sizeof(candidate));
    char fingerprint[9];
    hex_encode(fingerprint, candidate, 4);
    char response[128];
    snprintf(response, sizeof(response), "RC1,Q,%s,%s,%d", s_device_id,
             fingerprint, accepted ? 1 : 0);
    grove_send(response);
    ESP_LOGI(TAG, "Grove pairing request from %s: %s", peer_id,
             accepted ? "accepted and persisted" : "rejected");
}

static void grove_task(void *argument)
{
    (void)argument;
    const uart_config_t config = {
        .baud_rate = RC_GROVE_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(RC_GROVE_UART, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(RC_GROVE_UART, &config));
    ESP_ERROR_CHECK(uart_set_pin(RC_GROVE_UART, POE_P4_GROVE_TX_GPIO,
                                 POE_P4_GROVE_RX_GPIO, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
    char line[256];
    size_t used = 0;
    int64_t last_heartbeat = 0;
    while (true) {
        uint8_t byte;
        const int count = uart_read_bytes(RC_GROVE_UART, &byte, 1, pdMS_TO_TICKS(20));
        if (count == 1) {
            if (byte == '\n') {
                line[used] = '\0';
                process_pairing_frame(line);
                used = 0;
            } else if (byte != '\r' && used + 1 < sizeof(line)) {
                line[used++] = (char)byte;
            } else if (used + 1 >= sizeof(line)) {
                used = 0;
            }
        }
        const int64_t now = esp_timer_get_time();
        if (now - last_heartbeat >= 1000000) {
            char heartbeat[160];
            snprintf(heartbeat, sizeof(heartbeat), "RC1,H,%s,%d,%s", s_device_id,
                     s_peer_key_valid ? 1 : 0, s_boot_nonce_hex);
            grove_send(heartbeat);
            last_heartbeat = now;
        }
    }
}

static void set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 255 - red);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 255 - green);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, 255 - blue);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
}

static void led_task(void *argument)
{
    (void)argument;
    while (true) {
        network_state_t state;
        portENTER_CRITICAL(&s_lock);
        state = s_network;
        portEXIT_CRITICAL(&s_lock);
        if (!state.started) set_rgb(128, 0, 0);
        else if (!state.link_up) set_rgb(0, 0, 96);
        else if (!state.has_ip) set_rgb(96, 64, 0);
        else set_rgb(0, 96, 32);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static void initialize_led(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    const int pins[] = {POE_P4_LED_RED_GPIO, POE_P4_LED_GREEN_GPIO, POE_P4_LED_BLUE_GPIO};
    for (int channel = 0; channel < 3; ++channel) {
        const ledc_channel_config_t output = {
            .gpio_num = pins[channel],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = (ledc_channel_t)channel,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = 255,
            .hpoint = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&output));
    }
    xTaskCreate(led_task, "status_led", 2048, NULL, 2, NULL);
}

static cJSON *new_envelope(const char *type, const char *destination)
{
    cJSON *root = cJSON_CreateObject();
    char message_id[48];
    const uint32_t sequence = next_sequence();
    snprintf(message_id, sizeof(message_id), "%s-%lu", s_device_id,
             (unsigned long)sequence);
    cJSON_AddStringToObject(root, "proto", RC_PROTOCOL);
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddStringToObject(root, "message_id", message_id);
    cJSON_AddStringToObject(root, "source_node", s_device_id);
    if (destination != NULL && destination[0] != '\0') {
        cJSON_AddStringToObject(root, "destination_node", destination);
    }
    cJSON_AddNumberToObject(root, "timestamp_ms", (double)timestamp_ms());
    cJSON_AddNumberToObject(root, "sequence", sequence);
    return root;
}

typedef struct {
    volatile bool done;
    volatile bool found;
} scan_ping_result_t;

static void scan_ping_success(esp_ping_handle_t handle, void *argument)
{
    (void)handle;
    ((scan_ping_result_t *)argument)->found = true;
}

static void scan_ping_end(esp_ping_handle_t handle, void *argument)
{
    (void)handle;
    ((scan_ping_result_t *)argument)->done = true;
}

static bool ping_host(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    scan_ping_result_t result = {0};
    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.count = 1;
    config.timeout_ms = RC_SCAN_PING_TIMEOUT_MS;
    config.interval_ms = RC_SCAN_PING_TIMEOUT_MS;
    config.data_size = 24;
    IP_ADDR4(&config.target_addr, a, b, c, d);
    esp_ping_callbacks_t callbacks = {
        .cb_args = &result,
        .on_ping_success = scan_ping_success,
        .on_ping_end = scan_ping_end,
    };
    esp_ping_handle_t handle = NULL;
    if (esp_ping_new_session(&config, &callbacks, &handle) != ESP_OK) return false;
    esp_ping_start(handle);
    const int64_t deadline = esp_timer_get_time() +
        (RC_SCAN_PING_TIMEOUT_MS + 250) * 1000LL;
    while (!result.done && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    esp_ping_stop(handle);
    esp_ping_delete_session(handle);
    return result.found;
}

static bool arp_has_host(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    ip4_addr_t target;
    IP4_ADDR(&target, a, b, c, d);
    for (size_t index = 0; index < ARP_TABLE_SIZE; ++index) {
        ip4_addr_t *address = NULL;
        struct netif *interface = NULL;
        struct eth_addr *hardware = NULL;
        if (etharp_get_entry(index, &address, &interface, &hardware) &&
            address != NULL && ip4_addr_cmp(address, &target)) {
            return true;
        }
    }
    return false;
}

typedef struct {
    struct netif *interface;
    ip4_addr_t target;
} arp_request_t;

static void send_arp_request(void *argument)
{
    arp_request_t *request = argument;
    if (request != NULL) {
        etharp_request(request->interface, &request->target);
        free(request);
    }
}

static bool probe_local_host(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    struct netif *interface = (struct netif *)esp_netif_get_netif_impl(s_eth_netif);
    if (interface != NULL) {
        arp_request_t *request = malloc(sizeof(*request));
        if (request != NULL) {
            request->interface = interface;
            IP4_ADDR(&request->target, a, b, c, d);
            if (tcpip_callback(send_arp_request, request) == ERR_OK) {
                vTaskDelay(pdMS_TO_TICKS(80));
                if (arp_has_host(a, b, c, d)) return true;
            } else {
                free(request);
            }
        }
    }
    return ping_host(a, b, c, d);
}

static void discovery_task(void *argument)
{
    (void)argument;
    for (;;) {
    esp_netif_ip_info_t info;
    if (s_eth_netif == NULL || esp_netif_get_ip_info(s_eth_netif, &info) != ESP_OK ||
        info.ip.addr == 0) {
        portENTER_CRITICAL(&s_lock);
        s_scan.status = RC_SCAN_FAILED;
        snprintf(s_scan.error, sizeof(s_scan.error), "Ethernet has no IPv4 address");
        portEXIT_CRITICAL(&s_lock);
        vTaskDelete(NULL);
        return;
    }
    const uint8_t *ip = (const uint8_t *)&info.ip.addr;
    const uint8_t *mask = (const uint8_t *)&info.netmask.addr;
    const uint32_t ip_value = ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
                              ((uint32_t)ip[2] << 8) | ip[3];
    const uint32_t mask_value = ((uint32_t)mask[0] << 24) | ((uint32_t)mask[1] << 16) |
                                ((uint32_t)mask[2] << 8) | mask[3];
    const uint32_t network = ip_value & mask_value;
    const uint32_t broadcast = network | ~mask_value;
    const uint32_t first_host = s_scan.first_host == 0 ? 1 : s_scan.first_host;
    const uint32_t last_host = s_scan.last_host == 0 ? 254 : s_scan.last_host;
    uint32_t total = last_host >= first_host ? last_host - first_host + 1 : 0;
    const uint32_t available = broadcast > network + 1 ? broadcast - network - 1 : 0;
    if (first_host > available) total = 0;
    else if (total > available - first_host + 1) total = available - first_host + 1;
    if (total > RC_SCAN_MAX_HOSTS) total = RC_SCAN_MAX_HOSTS;
    portENTER_CRITICAL(&s_lock);
    s_scan.total = (uint16_t)total;
    portEXIT_CRITICAL(&s_lock);
    for (uint32_t index = 0; index < total; ++index) {
        if (s_scan.cancel_requested) break;
        const uint32_t candidate = network + first_host + index;
        bool found = false;
        if (candidate != ip_value) {
            const uint8_t a = (candidate >> 24) & 0xff;
            const uint8_t b = (candidate >> 16) & 0xff;
            const uint8_t c = (candidate >> 8) & 0xff;
            const uint8_t d = candidate & 0xff;
            found = probe_local_host(a, b, c, d);
        }
        portENTER_CRITICAL(&s_lock);
        s_scan.checked = (uint16_t)(index + 1);
        if (found && s_scan.result_count < RC_SCAN_MAX_RESULTS) {
            snprintf(s_scan.results[s_scan.result_count], 16, "%u.%u.%u.%u",
                     (unsigned)((candidate >> 24) & 0xff),
                     (unsigned)((candidate >> 16) & 0xff),
                     (unsigned)((candidate >> 8) & 0xff),
                     (unsigned)(candidate & 0xff));
            ++s_scan.result_count;
        }
        portEXIT_CRITICAL(&s_lock);
    }
    portENTER_CRITICAL(&s_lock);
    ++s_scan.run_count;
    const bool repeat = s_scan.recurring && !s_scan.cancel_requested;
    s_scan.status = repeat ? RC_SCAN_RUNNING :
        (s_scan.cancel_requested ? RC_SCAN_IDLE : RC_SCAN_COMPLETE);
    const uint32_t interval_ms = s_scan.interval_ms;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Discovery job complete: %u host(s)", (unsigned)s_scan.result_count);
    if (s_scan.rule_id[0] != '\0') {
        queue_evidence(s_scan.rule_id, s_scan.project_id, "network-hosts",
                       s_scan.run_count, s_scan.results, s_scan.result_count);
    }
    if (!repeat) break;
    uint32_t waited = 0;
    while (waited < interval_ms) {
        vTaskDelay(pdMS_TO_TICKS(250));
        waited += 250;
        if (s_scan.cancel_requested) break;
    }
    portENTER_CRITICAL(&s_lock);
    if (s_scan.cancel_requested) {
        s_scan.status = RC_SCAN_IDLE;
        portEXIT_CRITICAL(&s_lock);
        break;
    }
    s_scan.checked = 0;
    s_scan.total = 0;
    s_scan.result_count = 0;
    memset(s_scan.results, 0, sizeof(s_scan.results));
    portEXIT_CRITICAL(&s_lock);
    }
    vTaskDelete(NULL);
}

static const char *scan_status_name(scan_status_t status)
{
    switch (status) {
        case RC_SCAN_RUNNING: return "running";
        case RC_SCAN_COMPLETE: return "complete";
        case RC_SCAN_FAILED: return "failed";
        default: return "idle";
    }
}

static cJSON *scan_response(const char *destination, const char *request_id,
                            bool start, const cJSON *arguments)
{
    if (start) {
        bool launch = false;
        const uint32_t candidate_job_id = next_sequence();
        portENTER_CRITICAL(&s_lock);
        if (s_scan.status != RC_SCAN_RUNNING) {
            memset(&s_scan, 0, sizeof(s_scan));
            s_scan.job_id = candidate_job_id;
            s_scan.status = RC_SCAN_FAILED;
            const cJSON *start_ip = cJSON_GetObjectItemCaseSensitive(arguments, "start_ip");
            const cJSON *end_ip = cJSON_GetObjectItemCaseSensitive(arguments, "end_ip");
            const cJSON *network = cJSON_GetObjectItemCaseSensitive(arguments, "network");
            const cJSON *schedule = cJSON_GetObjectItemCaseSensitive(arguments, "schedule");
            unsigned local_a, local_b, local_c, local_d;
            unsigned start_a, start_b, start_c, first;
            unsigned end_a, end_b, end_c, last;
            char expected_network[24] = {0};
            const bool local_valid = sscanf(s_network.ip, "%u.%u.%u.%u", &local_a, &local_b,
                                            &local_c, &local_d) == 4;
            if (local_valid) snprintf(expected_network, sizeof(expected_network), "%u.%u.%u.0/24",
                                      local_a, local_b, local_c);
            bool schedule_valid = true;
            if (schedule != NULL) {
                const cJSON *interval = cJSON_GetObjectItemCaseSensitive(schedule, "interval_ms");
                const cJSON *after = cJSON_GetObjectItemCaseSensitive(schedule, "after_completion");
                schedule_valid = cJSON_IsObject(schedule) && cJSON_IsNumber(interval) &&
                    interval->valuedouble >= RC_RECUR_MIN_INTERVAL_MS &&
                    interval->valuedouble <= RC_RECUR_MAX_INTERVAL_MS && cJSON_IsTrue(after);
                if (schedule_valid) {
                    s_scan.recurring = true;
                    s_scan.interval_ms = (uint32_t)interval->valuedouble;
                }
            }
            if (cJSON_IsString(start_ip) && cJSON_IsString(end_ip) && schedule_valid &&
                cJSON_IsString(network) && local_valid &&
                strcmp(network->valuestring, expected_network) == 0 &&
                sscanf(start_ip->valuestring, "%u.%u.%u.%u", &start_a, &start_b, &start_c, &first) == 4 &&
                sscanf(end_ip->valuestring, "%u.%u.%u.%u", &end_a, &end_b, &end_c, &last) == 4 &&
                start_a == local_a && start_b == local_b && start_c == local_c &&
                end_a == local_a && end_b == local_b && end_c == local_c &&
                first >= 1 && first <= last && last <= 254) {
                s_scan.first_host = (uint8_t)first;
                s_scan.last_host = (uint8_t)last;
                s_scan.status = RC_SCAN_RUNNING;
                launch = true;
            } else {
                snprintf(s_scan.error, sizeof(s_scan.error), "Scope outside attached /24");
            }
        }
        portEXIT_CRITICAL(&s_lock);
        if (launch && xTaskCreate(discovery_task, "rc_discovery", 4096, NULL, 4, NULL) != pdPASS) {
            portENTER_CRITICAL(&s_lock);
            s_scan.status = RC_SCAN_FAILED;
            snprintf(s_scan.error, sizeof(s_scan.error), "Could not allocate scan task");
            portEXIT_CRITICAL(&s_lock);
        }
    }
    scan_state_t snapshot;
    portENTER_CRITICAL(&s_lock);
    snapshot = s_scan;
    portEXIT_CRITICAL(&s_lock);
    cJSON *root = new_envelope("response", destination);
    cJSON *payload = cJSON_AddObjectToObject(root, "payload");
    cJSON_AddStringToObject(payload, "request_id", request_id);
    cJSON_AddStringToObject(payload, "status", "ok");
    cJSON *result = cJSON_AddObjectToObject(payload, "result");
    cJSON_AddNumberToObject(result, "job_id", snapshot.job_id);
    cJSON_AddStringToObject(result, "job_status", scan_status_name(snapshot.status));
    cJSON_AddNumberToObject(result, "checked", snapshot.checked);
    cJSON_AddNumberToObject(result, "total", snapshot.total);
    cJSON_AddBoolToObject(result, "recurring", snapshot.recurring);
    cJSON_AddNumberToObject(result, "run_count", snapshot.run_count);
    if (snapshot.error[0] != '\0') {
        cJSON_AddStringToObject(result, "error", snapshot.error);
    }
    cJSON *hosts = cJSON_AddArrayToObject(result, "hosts");
    for (size_t index = 0; index < snapshot.result_count; ++index) {
        cJSON_AddItemToArray(hosts, cJSON_CreateString(snapshot.results[index]));
    }
    return root;
}

static cJSON *cancel_response(const char *destination, const char *request_id)
{
    portENTER_CRITICAL(&s_lock);
    s_scan.cancel_requested = true;
    s_scan.recurring = false;
    if (s_scan.status != RC_SCAN_RUNNING) s_scan.status = RC_SCAN_IDLE;
    portEXIT_CRITICAL(&s_lock);
    return scan_response(destination, request_id, false, NULL);
}

static bool check_internet_possible(network_state_t state, bool *gateway_reachable,
                                    bool *dns_resolved)
{
    *gateway_reachable = false;
    unsigned a, b, c, d;
    if (state.has_ip && sscanf(state.gateway, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
        *gateway_reachable = ping_host((uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d);
    }
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *resolved = NULL;
    *dns_resolved = state.has_ip && getaddrinfo("example.com", NULL, &hints, &resolved) == 0;
    if (resolved != NULL) freeaddrinfo(resolved);
    return *gateway_reachable && *dns_resolved;
}

static cJSON *connectivity_response(const char *destination, const char *request_id)
{
    network_state_t state;
    portENTER_CRITICAL(&s_lock);
    state = s_network;
    portEXIT_CRITICAL(&s_lock);
    bool gateway_reachable, dns_resolved;
    const bool internet = check_internet_possible(state, &gateway_reachable, &dns_resolved);
    cJSON *root = new_envelope("response", destination);
    cJSON *payload = cJSON_AddObjectToObject(root, "payload");
    cJSON_AddStringToObject(payload, "request_id", request_id);
    cJSON_AddStringToObject(payload, "status", "ok");
    cJSON *result = cJSON_AddObjectToObject(payload, "result");
    cJSON_AddBoolToObject(result, "link_up", state.link_up);
    cJSON_AddBoolToObject(result, "dhcp_assigned", state.has_ip);
    cJSON_AddStringToObject(result, "ip", state.ip);
    cJSON_AddStringToObject(result, "gateway", state.gateway);
    cJSON_AddBoolToObject(result, "gateway_reachable", gateway_reachable);
    cJSON_AddBoolToObject(result, "dns_resolved", dns_resolved);
    cJSON_AddBoolToObject(result, "internet_possible", internet);
    return root;
}

static cJSON *arp_snapshot_response(const char *destination, const char *request_id)
{
    cJSON *root = new_envelope("response", destination);
    cJSON *payload = cJSON_AddObjectToObject(root, "payload");
    cJSON_AddStringToObject(payload, "request_id", request_id);
    cJSON_AddStringToObject(payload, "status", "ok");
    cJSON *result = cJSON_AddObjectToObject(payload, "result");
    cJSON *entries = cJSON_AddArrayToObject(result, "entries");
    for (size_t index = 0; index < ARP_TABLE_SIZE; ++index) {
        ip4_addr_t *address = NULL;
        struct netif *interface = NULL;
        struct eth_addr *hardware = NULL;
        if (!etharp_get_entry(index, &address, &interface, &hardware) ||
            address == NULL || hardware == NULL) continue;
        char ip[16];
        char mac[18];
        ip4addr_ntoa_r(address, ip, sizeof(ip));
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 hardware->addr[0], hardware->addr[1], hardware->addr[2],
                 hardware->addr[3], hardware->addr[4], hardware->addr[5]);
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "address", ip);
        cJSON_AddStringToObject(entry, "mac", mac);
        cJSON_AddItemToArray(entries, entry);
    }
    return root;
}

static const char *condition_name(uint8_t condition)
{
    return condition == RC_CONDITION_INTERNET ? "internet_possible" : "dhcp_assigned";
}

static const char *playbook_name(uint8_t playbook)
{
    return playbook == RC_PLAYBOOK_SNAPSHOT ? "system_snapshot" : "network_scout";
}

static bool start_rule_scout(const automation_rule_t *rule)
{
    bool launch = false;
    const uint32_t job_id = next_sequence();
    portENTER_CRITICAL(&s_lock);
    if (s_scan.status != RC_SCAN_RUNNING) {
        memset(&s_scan, 0, sizeof(s_scan));
        s_scan.job_id = job_id;
        s_scan.status = RC_SCAN_RUNNING;
        s_scan.first_host = 1;
        s_scan.last_host = 254;
        s_scan.recurring = rule->interval_ms != 0;
        s_scan.interval_ms = rule->interval_ms;
        strlcpy(s_scan.rule_id, rule->id, sizeof(s_scan.rule_id));
        strlcpy(s_scan.project_id, rule->project_id, sizeof(s_scan.project_id));
        launch = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!launch) return false;
    if (xTaskCreate(discovery_task, "rc_auto_scan", 4096, NULL, 4, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_lock);
        s_scan.status = RC_SCAN_FAILED;
        portEXIT_CRITICAL(&s_lock);
        return false;
    }
    return true;
}

static void automation_task(void *argument)
{
    (void)argument;
    for (;;) {
        network_state_t network;
        portENTER_CRITICAL(&s_lock);
        network = s_network;
        portEXIT_CRITICAL(&s_lock);
        bool internet = false;
        bool gateway, dns;
        bool needs_internet = false;
        for (size_t index = 0; index < RC_MAX_RULES; ++index) {
            if (s_rules[index].version == 1 && s_rules[index].enabled &&
                s_rules[index].condition == RC_CONDITION_INTERNET) needs_internet = true;
        }
        if (needs_internet && network.has_ip) internet = check_internet_possible(network, &gateway, &dns);
        for (size_t index = 0; index < RC_MAX_RULES; ++index) {
            automation_rule_t *rule = &s_rules[index];
            if (rule->version != 1 || !rule->enabled) continue;
            const bool matched = rule->condition == RC_CONDITION_INTERNET ? internet : network.has_ip;
            if (!matched) {
                s_rule_matched[index] = false;
                continue;
            }
            if (s_rule_matched[index]) continue;
            if (rule->playbook == RC_PLAYBOOK_SNAPSHOT) {
                char empty_hosts[RC_SCAN_MAX_RESULTS][16] = {{0}};
                queue_evidence(rule->id, rule->project_id, "system-snapshot", 1,
                               empty_hosts, 0);
                s_rule_matched[index] = true;
            } else if (start_rule_scout(rule)) {
                s_rule_matched[index] = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static cJSON *automation_response(const char *destination, const char *request_id,
                                  const char *operation, const cJSON *arguments)
{
    bool valid = true;
    if (strcmp(operation, "put") == 0) {
        const cJSON *rule_json = cJSON_GetObjectItemCaseSensitive(arguments, "rule");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(rule_json, "id");
        const cJSON *project = cJSON_GetObjectItemCaseSensitive(rule_json, "project_id");
        const cJSON *condition = cJSON_GetObjectItemCaseSensitive(rule_json, "condition");
        const cJSON *playbook = cJSON_GetObjectItemCaseSensitive(rule_json, "playbook");
        const cJSON *interval = cJSON_GetObjectItemCaseSensitive(rule_json, "interval_ms");
        const bool names_valid = cJSON_IsString(id) && strlen(id->valuestring) < 32 &&
            cJSON_IsString(project) && project->valuestring[0] != '\0' && strlen(project->valuestring) < 64 &&
            cJSON_IsString(condition) && cJSON_IsString(playbook) && cJSON_IsNumber(interval);
        uint8_t condition_value = 0, playbook_value = 0;
        if (names_valid) {
            if (strcmp(condition->valuestring, "dhcp_assigned") == 0) condition_value = RC_CONDITION_DHCP;
            if (strcmp(condition->valuestring, "internet_possible") == 0) condition_value = RC_CONDITION_INTERNET;
            if (strcmp(playbook->valuestring, "network_scout") == 0) playbook_value = RC_PLAYBOOK_SCOUT;
            if (strcmp(playbook->valuestring, "system_snapshot") == 0) playbook_value = RC_PLAYBOOK_SNAPSHOT;
        }
        const uint32_t interval_value = names_valid ? (uint32_t)interval->valuedouble : 0;
        valid = names_valid && condition_value && playbook_value &&
            (interval_value == 0 || (interval_value >= RC_RECUR_MIN_INTERVAL_MS &&
                                     interval_value <= RC_RECUR_MAX_INTERVAL_MS));
        int slot = -1;
        if (valid) {
            for (size_t index = 0; index < RC_MAX_RULES; ++index) {
                if (strcmp(s_rules[index].id, id->valuestring) == 0) slot = (int)index;
                if (slot < 0 && s_rules[index].version == 0) slot = (int)index;
            }
            valid = slot >= 0;
        }
        if (valid) {
            automation_rule_t rule = {.version = 1, .enabled = true,
                                      .condition = condition_value, .playbook = playbook_value,
                                      .interval_ms = interval_value};
            const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(rule_json, "enabled");
            if (cJSON_IsBool(enabled)) rule.enabled = cJSON_IsTrue(enabled);
            strlcpy(rule.id, id->valuestring, sizeof(rule.id));
            strlcpy(rule.project_id, project->valuestring, sizeof(rule.project_id));
            s_rules[slot] = rule;
            s_rule_matched[slot] = false;
            valid = save_automation_blob(RC_AUTOMATION_RULES_KEY, s_rules, sizeof(s_rules));
        }
    } else if (strcmp(operation, "delete") == 0) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(arguments, "id");
        valid = cJSON_IsString(id);
        bool found = false;
        if (valid) for (size_t index = 0; index < RC_MAX_RULES; ++index) {
            if (strcmp(s_rules[index].id, id->valuestring) == 0) {
                memset(&s_rules[index], 0, sizeof(s_rules[index]));
                s_rule_matched[index] = false;
                found = true;
            }
        }
        valid = valid && found && save_automation_blob(RC_AUTOMATION_RULES_KEY, s_rules, sizeof(s_rules));
    }
    cJSON *root = new_envelope("response", destination);
    cJSON *payload = cJSON_AddObjectToObject(root, "payload");
    cJSON_AddStringToObject(payload, "request_id", request_id);
    cJSON_AddStringToObject(payload, "status", valid ? "ok" : "rejected");
    if (!valid) {
        cJSON *error = cJSON_AddObjectToObject(payload, "error");
        cJSON_AddStringToObject(error, "code", "INVALID_RULE");
        cJSON_AddStringToObject(error, "message", "Rule is invalid, full, missing, or could not be persisted");
    }
    cJSON *result = cJSON_AddObjectToObject(payload, "result");
    cJSON *rules = cJSON_AddArrayToObject(result, "rules");
    for (size_t index = 0; index < RC_MAX_RULES; ++index) {
        const automation_rule_t *rule = &s_rules[index];
        if (rule->version != 1) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", rule->id);
        cJSON_AddStringToObject(item, "project_id", rule->project_id);
        cJSON_AddStringToObject(item, "condition", condition_name(rule->condition));
        cJSON_AddStringToObject(item, "playbook", playbook_name(rule->playbook));
        cJSON_AddNumberToObject(item, "interval_ms", rule->interval_ms);
        cJSON_AddBoolToObject(item, "enabled", rule->enabled);
        cJSON_AddItemToArray(rules, item);
    }
    return root;
}

static cJSON *outbox_response(const char *destination, const char *request_id,
                              bool acknowledge, const cJSON *arguments)
{
    if (acknowledge) {
        const cJSON *sequence = cJSON_GetObjectItemCaseSensitive(arguments, "sequence");
        if (cJSON_IsNumber(sequence)) {
            for (size_t index = 0; index < RC_MAX_OUTBOX; ++index) {
                if (s_outbox[index].sequence == (uint32_t)sequence->valuedouble) {
                    memset(&s_outbox[index], 0, sizeof(s_outbox[index]));
                }
            }
            save_automation_blob(RC_AUTOMATION_OUTBOX_KEY, s_outbox, sizeof(s_outbox));
        }
    }
    cJSON *root = new_envelope("response", destination);
    cJSON *payload = cJSON_AddObjectToObject(root, "payload");
    cJSON_AddStringToObject(payload, "request_id", request_id);
    cJSON_AddStringToObject(payload, "status", "ok");
    cJSON *result = cJSON_AddObjectToObject(payload, "result");
    cJSON *records = cJSON_AddArrayToObject(result, "records");
    for (size_t index = 0; index < RC_MAX_OUTBOX; ++index) {
        const outbox_record_t *record = &s_outbox[index];
        if (record->version != 1) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "sequence", record->sequence);
        cJSON_AddStringToObject(item, "rule_id", record->rule_id);
        cJSON_AddStringToObject(item, "project_id", record->project_id);
        cJSON_AddStringToObject(item, "kind", record->kind);
        cJSON_AddStringToObject(item, "ip", record->ip);
        cJSON_AddNumberToObject(item, "run_count", record->run_count);
        cJSON *hosts = cJSON_AddArrayToObject(item, "hosts");
        for (size_t host = 0; host < record->host_count; ++host) {
            cJSON_AddItemToArray(hosts, cJSON_CreateString(record->hosts[host]));
        }
        cJSON_AddItemToArray(records, item);
    }
    return root;
}

static cJSON *announcement(void)
{
    cJSON *root = new_envelope("announce", NULL);
    cJSON *payload = cJSON_AddObjectToObject(root, "payload");
    cJSON_AddStringToObject(payload, "device_id", s_device_id);
    cJSON_AddStringToObject(payload, "device_type", "poe-p4");
    cJSON_AddStringToObject(payload, "firmware", RC_FIRMWARE_VERSION);
    cJSON *roles = cJSON_AddArrayToObject(payload, "roles");
    cJSON_AddItemToArray(roles, cJSON_CreateString("node"));
    cJSON *capabilities = cJSON_AddArrayToObject(payload, "capabilities");
    cJSON_AddItemToArray(capabilities, cJSON_CreateString("system.info"));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString("net.discovery.scan"));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString("coordination.job.status"));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString("coordination.job.cancel"));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString("net.connectivity.check"));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString("net.arp.snapshot"));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString("automation.rule.put"));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString("automation.rule.list"));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString("automation.rule.delete"));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString("evidence.outbox.read"));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString("evidence.outbox.ack"));
    cJSON *descriptors = cJSON_AddArrayToObject(payload, "capability_descriptors");
    const char *ids[] = {"system.info", "net.discovery.scan", "coordination.job.status",
                         "coordination.job.cancel", "net.connectivity.check",
                         "net.arp.snapshot", "automation.rule.put", "automation.rule.list",
                         "automation.rule.delete", "evidence.outbox.read", "evidence.outbox.ack"};
    for (size_t index = 0; index < sizeof(ids) / sizeof(ids[0]); ++index) {
        cJSON *descriptor = cJSON_CreateObject();
        cJSON_AddStringToObject(descriptor, "id", ids[index]);
        cJSON_AddNumberToObject(descriptor, "version", 1);
        cJSON_AddStringToObject(descriptor, "permission", "trusted");
        cJSON *features = cJSON_AddArrayToObject(descriptor, "features");
        if (index == 1) {
            cJSON_AddItemToArray(features, cJSON_CreateString("ipv4"));
            cJSON_AddItemToArray(features, cJSON_CreateString("range"));
            cJSON_AddItemToArray(features, cJSON_CreateString("recurring"));
            cJSON_AddItemToArray(features, cJSON_CreateString("after-completion"));
        }
        cJSON *limits = cJSON_AddObjectToObject(descriptor, "limits");
        cJSON_AddNumberToObject(limits, "weight", index == 1 ? 2 : 1);
        cJSON_AddNumberToObject(limits, "max_concurrency", 1);
        cJSON_AddItemToArray(descriptors, descriptor);
    }
    cJSON *resources = cJSON_AddObjectToObject(payload, "resources");
    cJSON_AddNumberToObject(resources, "network_mbps", 100);
    cJSON_AddBoolToObject(resources, "persistent_storage", false);
    cJSON_AddNumberToObject(resources, "storage_free_bytes", 0);
    cJSON_AddStringToObject(payload, "status", "ready");
    cJSON *security = cJSON_AddObjectToObject(payload, "security");
    cJSON_AddBoolToObject(security, "paired", true);
    cJSON_AddStringToObject(security, "boot_nonce", s_boot_nonce_hex);
    cJSON_AddStringToObject(security, "mode", "provisioned-hmac-sha256-128");
    cJSON_AddNumberToObject(security, "trusted_coordinators",
                            sizeof(RC_PROVISIONED_PEERS) / sizeof(RC_PROVISIONED_PEERS[0]));
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_lease_owner_id[0] != '\0' && (int32_t)(s_lease_expires_ms - now) > 0) {
        cJSON_AddStringToObject(security, "active_coordinator", s_lease_owner_id);
        cJSON_AddNumberToObject(security, "active_priority", s_lease_priority);
        cJSON_AddNumberToObject(security, "lease_remaining_ms", s_lease_expires_ms - now);
    }
    return root;
}

static esp_err_t send_json(httpd_req_t *request, cJSON *document)
{
    char *text = cJSON_PrintUnformatted(document);
    cJSON_Delete(document);
    if (text == NULL) return httpd_resp_send_500(request);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const esp_err_t result = httpd_resp_sendstr(request, text);
    cJSON_free(text);
    return result;
}

static esp_err_t announce_handler(httpd_req_t *request)
{
    return send_json(request, announcement());
}

static cJSON *error_response(const char *destination, const char *request_id,
                             const char *code, const char *message)
{
    cJSON *root = new_envelope("response", destination);
    cJSON *payload = cJSON_AddObjectToObject(root, "payload");
    cJSON_AddStringToObject(payload, "request_id", request_id);
    cJSON_AddStringToObject(payload, "status", "rejected");
    cJSON *error = cJSON_AddObjectToObject(payload, "error");
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    return root;
}

static cJSON *system_info_response(const char *destination, const char *request_id)
{
    network_state_t state;
    portENTER_CRITICAL(&s_lock);
    state = s_network;
    portEXIT_CRITICAL(&s_lock);
    cJSON *root = new_envelope("response", destination);
    cJSON *payload = cJSON_AddObjectToObject(root, "payload");
    cJSON_AddStringToObject(payload, "request_id", request_id);
    cJSON_AddStringToObject(payload, "status", "ok");
    cJSON *result = cJSON_AddObjectToObject(payload, "result");
    cJSON_AddStringToObject(result, "device_type", "poe-p4");
    cJSON_AddStringToObject(result, "firmware", RC_FIRMWARE_VERSION);
    cJSON_AddNumberToObject(result, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    cJSON_AddNumberToObject(result, "free_memory_bytes", esp_get_free_heap_size());
    cJSON_AddStringToObject(result, "ip", state.ip);
    cJSON_AddBoolToObject(result, "ethernet_link", state.link_up);
    return root;
}

static bool authenticate_request(const cJSON *input, const char *source_id,
                                 const char *request_id, const char *capability,
                                 uint64_t *nonce_out,
                                 const rc_provisioned_peer_t **peer_out,
                                 uint32_t *lease_ms_out)
{
    const rc_provisioned_peer_t *peer = provisioned_peer(source_id);
    if (peer == NULL) return false;
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(input, "payload");
    const cJSON *auth = cJSON_GetObjectItemCaseSensitive(payload, "auth");
    const cJSON *nonce_json = cJSON_GetObjectItemCaseSensitive(auth, "nonce");
    const cJSON *tag_json = cJSON_GetObjectItemCaseSensitive(auth, "tag");
    const cJSON *priority_json = cJSON_GetObjectItemCaseSensitive(auth, "coordinator_priority");
    const cJSON *lease_json = cJSON_GetObjectItemCaseSensitive(auth, "lease_ms");
    if (!cJSON_IsObject(auth) || !cJSON_IsString(nonce_json) ||
        !cJSON_IsString(tag_json) || !cJSON_IsNumber(priority_json) ||
        !cJSON_IsNumber(lease_json) || strlen(nonce_json->valuestring) != 16 ||
        priority_json->valueint != peer->priority ||
        lease_json->valueint < RC_LEASE_MIN_MS || lease_json->valueint > RC_LEASE_MAX_MS) return false;
    char *nonce_end = NULL;
    const uint64_t nonce = strtoull(nonce_json->valuestring, &nonce_end, 16);
    if (nonce_end == nonce_json->valuestring || *nonce_end != '\0') {
        return false;
    }
    char canonical[320];
    snprintf(canonical, sizeof(canonical), "%s|%s|%s|%s|%s|%s|%d|%d", source_id,
             s_device_id, request_id, capability, s_boot_nonce_hex,
             nonce_json->valuestring, priority_json->valueint, lease_json->valueint);
    uint8_t expected[RC_TAG_BYTES];
    uint8_t supplied[RC_TAG_BYTES];
    if (!compute_tag_with_key(peer->key, canonical, expected) ||
        !hex_decode(supplied, sizeof(supplied), tag_json->valuestring) ||
        !constant_time_equal(expected, supplied, sizeof(expected))) return false;
    if (nonce_seen_or_record(nonce)) return false;
    *nonce_out = nonce;
    *peer_out = peer;
    *lease_ms_out = (uint32_t)lease_json->valueint;
    return true;
}

static bool coordinator_lease_accept(const rc_provisioned_peer_t *peer, uint32_t lease_ms)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    const bool active = s_lease_owner_id[0] != '\0' &&
                        (int32_t)(s_lease_expires_ms - now) > 0;
    const bool same_owner = active && strcmp(s_lease_owner_id, peer->peer_id) == 0;
    if (active && !same_owner && peer->priority <= s_lease_priority) return false;
    strlcpy(s_lease_owner_id, peer->peer_id, sizeof(s_lease_owner_id));
    s_lease_priority = peer->priority;
    s_lease_expires_ms = now + lease_ms;
    return true;
}

static void authenticate_response(cJSON *response, const char *destination,
                                  const char *request_id, const char *status,
                                  uint64_t nonce, const uint8_t key[RC_KEY_BYTES])
{
    if (key == NULL || nonce == 0) return;
    char nonce_hex[17];
    snprintf(nonce_hex, sizeof(nonce_hex), "%016llx", (unsigned long long)nonce);
    char canonical[320];
    snprintf(canonical, sizeof(canonical), "%s|%s|%s|%s|%s|%s", s_device_id,
             destination, request_id, status, s_boot_nonce_hex, nonce_hex);
    uint8_t tag[RC_TAG_BYTES];
    if (!compute_tag_with_key(key, canonical, tag)) return;
    char tag_hex[RC_TAG_BYTES * 2 + 1];
    hex_encode(tag_hex, tag, sizeof(tag));
    cJSON *payload = cJSON_GetObjectItemCaseSensitive(response, "payload");
    cJSON *auth = cJSON_AddObjectToObject(payload, "auth");
    cJSON_AddStringToObject(auth, "nonce", nonce_hex);
    cJSON_AddStringToObject(auth, "tag", tag_hex);
}

static bool json_string_equals(const cJSON *value, const char *expected)
{
    return cJSON_IsString(value) && value->valuestring != NULL &&
           strcmp(value->valuestring, expected) == 0;
}

static esp_err_t message_handler(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len > RC_MAX_REQUEST_BYTES) {
        httpd_resp_set_status(request, "413 Payload Too Large");
        return httpd_resp_sendstr(request, "{\"error\":\"invalid_size\"}");
    }
    char *body = malloc((size_t)request->content_len + 1);
    if (body == NULL) return httpd_resp_send_500(request);
    int received = 0;
    while (received < request->content_len) {
        const int count = httpd_req_recv(request, body + received,
                                         (size_t)(request->content_len - received));
        if (count <= 0) {
            free(body);
            return ESP_FAIL;
        }
        received += count;
    }
    body[received] = '\0';
    cJSON *input = cJSON_Parse(body);
    free(body);
    if (input == NULL) {
        httpd_resp_set_status(request, "400 Bad Request");
        return httpd_resp_sendstr(request, "{\"error\":\"invalid_json\"}");
    }

    const cJSON *proto = cJSON_GetObjectItemCaseSensitive(input, "proto");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(input, "type");
    const cJSON *source = cJSON_GetObjectItemCaseSensitive(input, "source_node");
    const cJSON *destination = cJSON_GetObjectItemCaseSensitive(input, "destination_node");
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(input, "payload");
    const cJSON *request_id = cJSON_GetObjectItemCaseSensitive(payload, "request_id");
    const cJSON *capability = cJSON_GetObjectItemCaseSensitive(payload, "capability");
    const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(payload, "arguments");
    const char *source_id = cJSON_IsString(source) ? source->valuestring : "unknown";
    const char *id = cJSON_IsString(request_id) ? request_id->valuestring : "invalid-request";

    cJSON *response;
    uint64_t request_nonce = 0;
    uint32_t requested_lease_ms = 0;
    const rc_provisioned_peer_t *authenticated_peer = NULL;
    const char *response_status = "rejected";
    if (!json_string_equals(proto, RC_PROTOCOL) || !json_string_equals(type, "request") ||
        !cJSON_IsObject(payload) || !cJSON_IsString(source) ||
        !json_string_equals(destination, s_device_id)) {
        response = error_response(source_id, id, "INVALID_REQUEST", "Malformed or misdirected request");
    } else if (!authenticate_request(input, source_id, id,
                                     cJSON_IsString(capability) ? capability->valuestring : "",
                                     &request_nonce, &authenticated_peer, &requested_lease_ms)) {
        response = error_response(source_id, id, "AUTHENTICATION_REQUIRED", "Coordinator is not provisioned");
    } else if (!coordinator_lease_accept(authenticated_peer, requested_lease_ms)) {
        response = error_response(source_id, id, "COORDINATOR_LEASE_HELD",
                                  "A higher-priority coordinator holds the active lease");
    } else if (json_string_equals(capability, "system.info")) {
        response = system_info_response(source_id, id);
        response_status = "ok";
    } else if (json_string_equals(capability, "net.discovery.scan")) {
        response = scan_response(source_id, id, true, arguments);
        response_status = "ok";
    } else if (json_string_equals(capability, "coordination.job.status")) {
        response = scan_response(source_id, id, false, arguments);
        response_status = "ok";
    } else if (json_string_equals(capability, "coordination.job.cancel")) {
        response = cancel_response(source_id, id);
        response_status = "ok";
    } else if (json_string_equals(capability, "net.connectivity.check")) {
        response = connectivity_response(source_id, id);
        response_status = "ok";
    } else if (json_string_equals(capability, "net.arp.snapshot")) {
        response = arp_snapshot_response(source_id, id);
        response_status = "ok";
    } else if (json_string_equals(capability, "automation.rule.put")) {
        response = automation_response(source_id, id, "put", arguments);
    } else if (json_string_equals(capability, "automation.rule.list")) {
        response = automation_response(source_id, id, "list", arguments);
    } else if (json_string_equals(capability, "automation.rule.delete")) {
        response = automation_response(source_id, id, "delete", arguments);
    } else if (json_string_equals(capability, "evidence.outbox.read")) {
        response = outbox_response(source_id, id, false, arguments);
    } else if (json_string_equals(capability, "evidence.outbox.ack")) {
        response = outbox_response(source_id, id, true, arguments);
    } else {
        response = error_response(source_id, id, "CAPABILITY_UNAVAILABLE", "Capability is not available");
    }
    const cJSON *output_payload = cJSON_GetObjectItemCaseSensitive(response, "payload");
    const cJSON *output_status = cJSON_GetObjectItemCaseSensitive(output_payload, "status");
    if (cJSON_IsString(output_status)) response_status = output_status->valuestring;
    authenticate_response(response, source_id, id, response_status, request_nonce,
                          authenticated_peer != NULL ? authenticated_peer->key : NULL);
    cJSON_Delete(input);
    return send_json(request, response);
}

static esp_err_t not_found_handler(httpd_req_t *request, httpd_err_code_t error)
{
    (void)error;
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_status(request, "404 Not Found");
    return httpd_resp_sendstr(request, "{\"error\":\"not_found\"}");
}

static void start_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = RC_HTTP_PORT;
    config.stack_size = 8192;
    config.max_open_sockets = 6;
    config.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    const httpd_uri_t announce_uri = {
        .uri = "/reconclave/v1/announce", .method = HTTP_GET,
        .handler = announce_handler,
    };
    const httpd_uri_t message_uri = {
        .uri = "/reconclave/v1/message", .method = HTTP_POST,
        .handler = message_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &announce_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &message_uri));
    ESP_ERROR_CHECK(httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found_handler));
}

static void start_mdns(void)
{
    static bool started;
    if (started) return;
    mdns_txt_item_t records[] = {
        {"proto", "reconclave/1"}, {"roles", "node"},
        {"device", "poe-p4"}, {"path", "/reconclave/v1/announce"},
    };
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(RC_HOSTNAME));
    ESP_ERROR_CHECK(mdns_instance_name_set(RC_INSTANCE));
    ESP_ERROR_CHECK(mdns_service_add(RC_INSTANCE, "_reconclave", "_tcp",
                                     RC_HTTP_PORT, records,
                                     sizeof(records) / sizeof(records[0])));
    started = true;
}

static void ethernet_event(void *argument, esp_event_base_t base,
                           int32_t event_id, void *event_data)
{
    (void)argument;
    (void)base;
    (void)event_data;
    portENTER_CRITICAL(&s_lock);
    if (event_id == ETHERNET_EVENT_START) s_network.started = true;
    if (event_id == ETHERNET_EVENT_CONNECTED) s_network.link_up = true;
    if (event_id == ETHERNET_EVENT_DISCONNECTED || event_id == ETHERNET_EVENT_STOP) {
        s_network.link_up = false;
        s_network.has_ip = false;
        s_network.ip[0] = '\0';
    }
    if (event_id == ETHERNET_EVENT_STOP) s_network.started = false;
    portEXIT_CRITICAL(&s_lock);
}

static void got_ip_event(void *argument, esp_event_base_t base,
                         int32_t event_id, void *event_data)
{
    (void)argument;
    (void)base;
    (void)event_id;
    const ip_event_got_ip_t *event = event_data;
    portENTER_CRITICAL(&s_lock);
    snprintf(s_network.ip, sizeof(s_network.ip), IPSTR, IP2STR(&event->ip_info.ip));
    snprintf(s_network.gateway, sizeof(s_network.gateway), IPSTR, IP2STR(&event->ip_info.gw));
    s_network.has_ip = true;
    portEXIT_CRITICAL(&s_lock);
    start_mdns();
    ESP_LOGI(TAG, "Ready at http://%s:%d", s_network.ip, RC_HTTP_PORT);
}

static void initialize_device_id(void)
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_ETH));
    snprintf(s_device_id, sizeof(s_device_id), "rc-p4-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void initialize_ethernet(void)
{
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    phy_config.phy_addr = POE_P4_PHY_ADDR;
    phy_config.reset_gpio_num = POE_P4_PHY_RESET_GPIO;
    emac_config.smi_gpio.mdc_num = POE_P4_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = POE_P4_MDIO_GPIO;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
    ESP_ERROR_CHECK(mac == NULL || phy == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &s_eth_handle));
    const esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_config);
    ESP_ERROR_CHECK(s_eth_netif == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(s_eth_handle)));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, ethernet_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_event, NULL));
    ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));
}

void app_main(void)
{
    initialize_led();
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    initialize_device_id();
    esp_fill_random(s_boot_nonce, sizeof(s_boot_nonce));
    hex_encode(s_boot_nonce_hex, s_boot_nonce, sizeof(s_boot_nonce));
    load_pairing();
    load_automation_state();
    xTaskCreate(grove_task, "grove_pairing", 4096, NULL, 5, NULL);
    xTaskCreate(automation_task, "automation", 6144, NULL, 3, NULL);
    start_server();
    initialize_ethernet();
    ESP_LOGI(TAG, "Reconclave %s node %s started", RC_FIRMWARE_VERSION, s_device_id);
}
