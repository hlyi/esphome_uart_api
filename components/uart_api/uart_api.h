#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/socket/socket.h"

#include <cstring>
#include <memory>

namespace esphome {
namespace uart_api {

class UARTAPIBridge : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
  void dump_config() override;

  void set_api_port(uint16_t port) { this->api_port_ = port; }
  void set_rx_buffer_size(uint16_t size) { this->rx_buffer_size_ = size; }
  void set_status_pin(InternalGPIOPin *pin) { this->status_pin_ = pin; }
  void set_debug_echo(bool enable) { this->debug_echo_ = enable; }

 protected:
  void connect_to_api_();
  void disconnect_();
  void forward_uart_to_tcp_();
  void forward_tcp_to_uart_();

  std::unique_ptr<socket::Socket> socket_;
  InternalGPIOPin *status_pin_{nullptr};
  uint16_t api_port_{6053};
  uint16_t rx_buffer_size_{256};
  bool connected_{false};
  bool debug_echo_{false};
  uint32_t last_connect_attempt_{0};
  uint32_t reconnect_interval_{1000};
};

}  // namespace uart_api
}  // namespace esphome
