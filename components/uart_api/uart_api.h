#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/socket/socket.h"

#include <cstring>
#include <deque>
#include <memory>

#if defined(USE_ESP32) && !defined(USE_ESP8266)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace esphome {
namespace uart_api {

class UARTAPIBridge : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
  void dump_config() override;

  void set_api_port(uint16_t port) { this->api_port_ = port; }
  void set_ota_port(uint16_t port) { this->ota_port_ = port; }
  void set_rx_buffer_size(uint16_t size) { this->rx_buffer_size_ = size; }
  void set_status_pin(InternalGPIOPin *pin) { this->status_pin_ = pin; }
  void set_debug_echo(bool enable) { this->debug_echo_ = enable; }

  void set_ota_mode(bool ota);

 protected:
  void connect_to_server_();
  void disconnect_();
  void forward_uart_to_tcp_();
  void forward_tcp_to_uart_();
  void flush_write_buffer_();
  void start_ota_task_();
  void stop_ota_task_();

#if defined(USE_ESP32) && !defined(USE_ESP8266)
  static void ota_forward_task_(void *arg);
#endif

  std::unique_ptr<socket::Socket> socket_;
  InternalGPIOPin *status_pin_{nullptr};
  uint16_t api_port_{6053};
  uint16_t ota_port_{8266};
  uint16_t rx_buffer_size_{256};
  bool connected_{false};
  bool debug_echo_{false};
  bool switched_to_ota_{false};
  uint32_t last_connect_attempt_{0};
  uint32_t reconnect_interval_{1000};

  std::deque<uint8_t> tcp_rx_queue_;
  static constexpr size_t MAX_TX_QUEUE = 16384;

#if defined(USE_ESP32) && !defined(USE_ESP8266)
  volatile bool ota_task_active_{false};
  volatile uint32_t ota_cookie_{0};
  TaskHandle_t ota_task_handle_{nullptr};
#endif
};

}  // namespace uart_api
}  // namespace esphome
