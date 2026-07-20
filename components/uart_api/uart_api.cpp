#include "uart_api.h"
#include "esphome/core/log.h"

#include <cerrno>

#if defined(USE_ESP32) && !defined(USE_ESP8266)
#include <esp_netif.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>
#include <lwip/ip4_addr.h>
#endif

#ifdef USE_ETHERNET
// Our stub header is injected into the build tree by __init__.py
#include "esphome/components/ethernet/ethernet_component.h"
namespace esphome::ethernet {
EthernetComponent *global_eth_component = nullptr;
}
#endif

namespace esphome {
namespace uart_api {

static const char *const TAG = "uart_api";

#if defined(USE_ESP32) && !defined(USE_ESP8266)
static struct netif s_loop_netif;
static bool s_loop_if_created = false;

static err_t loop_output_(struct netif *netif, struct pbuf *p,
                          const ip4_addr_t *ipaddr) {
  return netif->input(p, netif);
}

static err_t loop_init_(struct netif *netif) {
  netif->name[0] = 'l';
  netif->name[1] = 'o';
  netif->output = loop_output_;
  netif->linkoutput = NULL;
  netif->mtu = 65535;
  netif->hwaddr_len = 0;
  return ERR_OK;
}

static void create_loopback_netif_() {
  if (s_loop_if_created) return;

  esp_err_t netif_err = esp_netif_init();
  if (netif_err != ESP_OK) {
    if (netif_err == ESP_ERR_INVALID_STATE) {
      ESP_LOGV(TAG, "esp_netif already initialized");
    } else {
      ESP_LOGW(TAG, "esp_netif_init failed: %d", netif_err);
      return;
    }
  }

  LOCK_TCPIP_CORE();
  if (netif_find("lo") == NULL) {
    ip4_addr_t ipaddr, netmask, gw;
    IP4_ADDR(&ipaddr, 127, 0, 0, 1);
    IP4_ADDR(&netmask, 255, 0, 0, 0);
    IP4_ADDR(&gw, 0, 0, 0, 0);

    if (netif_add(&s_loop_netif,
                  (const ip4_addr_t *)&ipaddr, (const ip4_addr_t *)&netmask,
                  (const ip4_addr_t *)&gw, NULL, loop_init_, tcpip_input) != NULL) {
      netif_set_up(&s_loop_netif);
      s_loop_if_created = true;
      ESP_LOGI(TAG, "Loopback interface created (lo: 127.0.0.1)");
    } else {
      ESP_LOGW(TAG, "Failed to create loopback interface");
    }
  } else {
    s_loop_if_created = true;
  }
  UNLOCK_TCPIP_CORE();
}
#else
static void create_loopback_netif_() {
  ESP_LOGW(TAG, "Loopback interface not supported on this platform");
}
#endif

void UARTAPIBridge::setup() {
  ESP_LOGCONFIG(TAG, "Setting up UART API Bridge...");
  if (this->status_pin_) {
    this->status_pin_->setup();
    this->status_pin_->digital_write(false);
  }
  create_loopback_netif_();
#ifdef USE_ETHERNET
  if (ethernet::global_eth_component == nullptr) {
    static ethernet::EthernetComponent stub_eth;
    ethernet::global_eth_component = &stub_eth;
  }
#endif
  this->last_connect_attempt_ = millis();
}

void UARTAPIBridge::loop() {
  const uint32_t now = millis();

  if (this->debug_echo_) {
    while (this->available() > 0) {
      uint8_t byte;
      if (this->read_byte(&byte)) {
        this->write_byte(byte);
      }
    }
    return;
  }

  if (this->connected_) {
    this->forward_uart_to_tcp_();
    if (this->connected_) {
      this->forward_tcp_to_uart_();
    }
  }

  if (!this->connected_ && now - this->last_connect_attempt_ >= this->reconnect_interval_) {
    this->last_connect_attempt_ = now;
    this->connect_to_api_();
  }

  if (this->status_pin_) {
    this->status_pin_->digital_write(this->connected_);
  }
}

void UARTAPIBridge::connect_to_api_() {
  auto sock = socket::socket(AF_INET, SOCK_STREAM, 0);
  if (!sock) {
    ESP_LOGV(TAG, "Cannot create TCP socket (lwIP not ready?)");
    return;
  }

  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(this->api_port_);
  addr.sin_addr.s_addr = htonl(0x7F000001);

  int err = sock->connect((struct sockaddr *)&addr, sizeof(addr));
  if (err != 0) {
    ESP_LOGV(TAG, "Cannot connect to API (will retry): %s", strerror(errno));
    return;
  }

  err = sock->setblocking(false);
  if (err != 0) {
    ESP_LOGW(TAG, "Cannot set socket non-blocking: %s", strerror(errno));
    return;
  }

  this->socket_ = std::move(sock);
  this->connected_ = true;
  ESP_LOGI(TAG, "Connected to API server on 127.0.0.1:%d", this->api_port_);
}

void UARTAPIBridge::disconnect_() {
  if (this->socket_) {
    this->socket_->close();
    this->socket_.reset();
  }
  this->connected_ = false;
  this->last_connect_attempt_ = millis();
  ESP_LOGD(TAG, "Disconnected from API server");
}

void UARTAPIBridge::forward_uart_to_tcp_() {
  size_t max_per_loop = 512;
  size_t total_read = 0;
  uint8_t buf[512];

  while (this->available() > 0 && total_read < max_per_loop) {
    size_t avail = this->available();
    size_t to_read = std::min(avail, sizeof(buf));
    to_read = std::min(to_read, max_per_loop - total_read);
    if (to_read == 0)
      break;

    if (!this->read_array(buf, to_read))
      break;

    size_t written = 0;
    while (written < to_read) {
      int sent = this->socket_->write(buf + written, to_read - written);
      if (sent > 0) {
        written += sent;
      } else if (sent == 0) {
        this->disconnect_();
        return;
      } else {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          ESP_LOGW(TAG, "Socket write error: %s", strerror(errno));
          this->disconnect_();
          return;
        }
        break;
      }
    }
    total_read += written;
  }
}

void UARTAPIBridge::forward_tcp_to_uart_() {
  uint8_t buf[512];
  int received = this->socket_->read(buf, sizeof(buf));

  if (received > 0) {
    this->write_array(buf, received);
  } else if (received == 0) {
    ESP_LOGW(TAG, "API server closed the connection");
    this->disconnect_();
  } else {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      ESP_LOGW(TAG, "Socket read error: %s", strerror(errno));
      this->disconnect_();
    }
  }
}

void UARTAPIBridge::dump_config() {
  ESP_LOGCONFIG(TAG, "UART API Bridge:");
  ESP_LOGCONFIG(TAG, "  API Server Port: %d", this->api_port_);
  ESP_LOGCONFIG(TAG, "  UART RX Buffer: %d", this->rx_buffer_size_);
  ESP_LOGCONFIG(TAG, "  Connected: %s", YESNO(this->connected_));
}

}  // namespace uart_api
}  // namespace esphome
