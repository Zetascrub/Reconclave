#pragma once
#include <cstdint>
using esp_ping_handle_t = void*;
constexpr int ESP_OK = 0;
struct esp_ping_config_t {
  uint32_t count{},timeout_ms{},interval_ms{},data_size{},target_addr{};
};
#define ESP_PING_DEFAULT_CONFIG() esp_ping_config_t{}
#define IP_ADDR4(ptr,a,b,c,d) (*(ptr) = uint32_t(a)<<24 | uint32_t(b)<<16 | uint32_t(c)<<8 | uint32_t(d))
struct esp_ping_callbacks_t {
  void* cb_args{};
  void (*on_ping_success)(void*,void*){};
  void (*on_ping_end)(void*,void*){};
};
int esp_ping_new_session(const esp_ping_config_t*, const esp_ping_callbacks_t*, void**);
int esp_ping_start(void*);
int esp_ping_stop(void*);
int esp_ping_delete_session(void*);
