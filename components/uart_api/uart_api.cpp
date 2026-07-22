#include "uart_api.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

#include <cerrno>

#if defined(USE_ESP32) && !defined(USE_ESP8266)
#include <netinet/tcp.h>
#include <esp_netif.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>
#include <lwip/ip4_addr.h>
#endif

#ifdef USE_ETHERNET
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

void UARTAPIBridge::set_ota_mode(bool ota) {
  if (ota == this->switched_to_ota_)
    return;
  this->switched_to_ota_ = ota;

  if (ota) {
    ESP_LOGI(TAG, "Switching to OTA server (port %d)", this->ota_port_);
    this->disconnect_();
    this->last_connect_attempt_ = 0;
    this->connect_to_server_();
    if (this->connected_) {
      this->start_ota_task_();
    }
  } else {
    ESP_LOGI(TAG, "Switching back to API server (port %d)", this->api_port_);
    this->stop_ota_task_();
    this->disconnect_();
    this->last_connect_attempt_ = 0;
    this->connect_to_server_();
  }
}

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

#if defined(USE_ESP32) && !defined(USE_ESP8266)
  if (this->ota_task_active_) {
    if (!this->connected_) {
      this->stop_ota_task_();
    }
    if (this->status_pin_) {
      this->status_pin_->digital_write(this->connected_);
    }
    return;
  }
#endif

  if (this->connected_) {
    if (this->switched_to_ota_)
      this->flush_write_buffer_();
    this->forward_uart_to_tcp_();
    if (this->connected_) {
      this->forward_tcp_to_uart_();
    }
    if (this->switched_to_ota_)
      this->flush_write_buffer_();
  }

  if (!this->connected_ && now - this->last_connect_attempt_ >= this->reconnect_interval_) {
    this->last_connect_attempt_ = now;
    this->connect_to_server_();
#if defined(USE_ESP32) && !defined(USE_ESP8266)
    if (this->connected_ && this->switched_to_ota_) {
      this->start_ota_task_();
    }
#endif
  }

  if (this->status_pin_) {
    this->status_pin_->digital_write(this->connected_);
  }
}

void UARTAPIBridge::connect_to_server_() {
  uint16_t port = this->switched_to_ota_ ? this->ota_port_ : this->api_port_;
  const char *label = this->switched_to_ota_ ? "OTA server" : "API server";

  auto sock = socket::socket(AF_INET, SOCK_STREAM, 0);
  if (!sock) {
    ESP_LOGV(TAG, "Cannot create TCP socket for %s (lwIP not ready?)", label);
    return;
  }

  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(0x7F000001);

  int err = sock->connect((struct sockaddr *)&addr, sizeof(addr));
  if (err != 0) {
    ESP_LOGV(TAG, "Cannot connect to %s (will retry): %s", label, strerror(errno));
    return;
  }

  // Enable TCP_NODELAY to prevent Nagle from delaying small writes
  int flag = 1;
  sock->setsockopt(IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

  err = sock->setblocking(false);
  if (err != 0) {
    ESP_LOGW(TAG, "Cannot set non-blocking for %s: %s", label, strerror(errno));
    return;
  }

  this->socket_ = std::move(sock);
  this->connected_ = true;
  ESP_LOGI(TAG, "Connected to %s on 127.0.0.1:%d", label, port);
}

void UARTAPIBridge::disconnect_() {
  if (this->socket_) {
    this->socket_->close();
    this->socket_.reset();
  }
  this->connected_ = false;
  this->last_connect_attempt_ = millis();
  this->tcp_rx_queue_.clear();
  ESP_LOGD(TAG, "Disconnected");
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
    int retry_count = 0;
    while (written < to_read) {
      int sent = this->socket_->write(buf + written, to_read - written);
      if (sent > 0) {
        written += sent;
        retry_count = 0;
      } else if (sent == 0) {
        // Connection closed by peer — reconnect and retry remaining bytes
        ESP_LOGW(TAG, "TCP write returned 0, reconnecting...");
        this->disconnect_();
        this->connect_to_server_();
        if (!this->connected_) {
          return;
        }
        // Retry the remaining bytes on the fresh connection
        retry_count = 0;
      } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (retry_count < 3) {
          retry_count++;
          delay(1);
        } else {
          ESP_LOGV(TAG, "Socket write EAGAIN after %d retries, deferring", retry_count);
          return;
        }
      } else {
        ESP_LOGW(TAG, "Socket write error: %s", strerror(errno));
        this->disconnect_();
        return;
      }
    }
    total_read += written;
  }
}

void UARTAPIBridge::forward_tcp_to_uart_() {
  uint8_t buf[512];
  int received = this->socket_->read(buf, sizeof(buf));

  if (received > 0) {
    if (this->switched_to_ota_) {
      if (this->tcp_rx_queue_.size() < MAX_TX_QUEUE) {
        this->tcp_rx_queue_.insert(this->tcp_rx_queue_.end(), buf, buf + received);
      }
    } else {
      this->write_array(buf, received);
    }
  } else if (received == 0) {
    ESP_LOGW(TAG, "Active server closed the connection");
    this->disconnect_();
  } else {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      ESP_LOGW(TAG, "Socket read error: %s", strerror(errno));
      this->disconnect_();
    }
  }
}

void UARTAPIBridge::flush_write_buffer_() {
  if (this->tcp_rx_queue_.empty())
    return;

  size_t to_send = std::min(this->tcp_rx_queue_.size(), (size_t)512);
  uint8_t chunk[512];

  for (size_t i = 0; i < to_send; i++) {
    chunk[i] = this->tcp_rx_queue_.front();
    this->tcp_rx_queue_.pop_front();
  }

  this->write_array(chunk, to_send);
}

#if defined(USE_ESP32) && !defined(USE_ESP8266)
void UARTAPIBridge::ota_forward_task_(void *arg) {
  UARTAPIBridge *bridge = static_cast<UARTAPIBridge *>(arg);
  uint32_t my_cookie = bridge->ota_cookie_;
  bridge->ota_task_active_ = true;
  ESP_LOGD(TAG, "OTA task running (cookie=%lu)", (unsigned long)my_cookie);

  while (bridge->ota_cookie_ == my_cookie) {
    if (bridge->connected_) {
      bridge->forward_uart_to_tcp_();
      bridge->forward_tcp_to_uart_();
      bridge->flush_write_buffer_();
    } else {
      ESP_LOGD(TAG, "OTA task: disconnected");
      break;
    }
    // delay(1) blocks for 1 tick (~10ms), yielding to the main task.
    // This is critical: on single-core ESP32-C3, when the OTA server's
    // handle_data_() does a blocking recv(), the main task yields CPU.
    // delay(1) gives a proper yield window for lwIP to process loopback
    // data and wake the main task.
    delay(1);
  }

  bridge->ota_task_active_ = false;
  bridge->ota_task_handle_ = nullptr;
  ESP_LOGD(TAG, "OTA task exiting (cookie=%lu)", (unsigned long)my_cookie);
  vTaskDelete(nullptr);
}

void UARTAPIBridge::start_ota_task_() {
  this->ota_cookie_++;
  BaseType_t res = xTaskCreate(ota_forward_task_, "uart_ota_fwd", 8192, this, 1, &this->ota_task_handle_);
  if (res != pdPASS) {
    ESP_LOGW(TAG, "Failed to create OTA forwarding task");
    this->ota_task_handle_ = nullptr;
  } else {
    ESP_LOGI(TAG, "OTA task started (mode=%s)", this->switched_to_ota_ ? "OTA" : "API");
  }
}

void UARTAPIBridge::stop_ota_task_() {
  ESP_LOGD(TAG, "Stopping OTA task (cookie %lu -> %lu)",
           (unsigned long)this->ota_cookie_, (unsigned long)(this->ota_cookie_ + 1));
  this->ota_cookie_++;
  this->ota_task_handle_ = nullptr;
}
#endif

void UARTAPIBridge::dump_config() {
  ESP_LOGCONFIG(TAG, "UART API Bridge:");
  ESP_LOGCONFIG(TAG, "  API Server Port: %d", this->api_port_);
  ESP_LOGCONFIG(TAG, "  OTA Server Port: %d", this->ota_port_);
  ESP_LOGCONFIG(TAG, "  UART RX Buffer: %d", this->rx_buffer_size_);
  ESP_LOGCONFIG(TAG, "  Mode: %s", this->switched_to_ota_ ? "OTA" : "API");
  ESP_LOGCONFIG(TAG, "  Connected: %s", YESNO(this->connected_));
}

}  // namespace uart_api
}  // namespace esphome
