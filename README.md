# ESPHome UART API Bridge

## Objective
Provide the full ESPHome Native API (protobuf-based) over UART instead of TCP port 6053, using a bridge component on ESP32 (ESP-IDF), without WiFi.

## Architecture

```
TCP-UART-Server  ──UART──  ESP32-C3  ──loopback TCP──  ESPHome API Server
  (230400 baud)           (uart_api)                   (port 6053)
```

- `UARTAPIBridge` (external component) reads UART bytes, forwards them to a loopback TCP connection to the ESPHome API server.
- API server responses are read from TCP and written back to UART.
- No WiFi/Ethernet required — loopback interface created programmatically via lwIP.

## Component Files

### `components/uart_api/__init__.py`
- Registers `uart_api` as an ESPHome external component.
- `CONFIG_SCHEMA`: `api_port` (default 6053), `rx_buffer_size`, `debug_echo`, `status_pin`.
- `to_code()`: generates the component, sets build flag `-DUSE_ETHERNET`, monkey-patches `writer.copy_src_tree()` to inject a stub `ethernet_component.h` into the build tree after sources are copied.
- **Why the monkey-patch**: `-DUSE_ETHERNET` activates the ethernet code path in `network/util.h`, which `#include`s `esphome/components/ethernet/ethernet_component.h`. The real header doesn't exist in the component tree. `cg.add_build_flag("-I...")` doesn't work because `get_project_compile_flags()` only passes `-D` and `-W` flags to CMake. Writing the stub header via `copy_src_tree` monkey-patch is the only reliable injection point.

### `components/uart_api/uart_api.h`
- Class `UARTAPIBridge` extends `Component` and `uart::UARTDevice`.
- Members: `socket_` (TCP), `status_pin_`, `api_port_`, `rx_buffer_size_`, `connected_`, `debug_echo_`, `last_connect_attempt_`, `reconnect_interval_`.

### `components/uart_api/uart_api.cpp`
| Function | Description |
|---|---|
| `setup()` | Init status pin, create loopback netif, set stub `global_eth_component` |
| `loop()` | Forward UART→TCP, TCP→UART; reconnect if disconnected; update status pin |
| `connect_to_api_()` | Create TCP socket, connect to `127.0.0.1:6053`, set non-blocking |
| `disconnect_()` | Close socket, clear `connected_` |
| `forward_uart_to_tcp_()` | Read from UART, write to TCP socket (with partial-write retry) |
| `forward_tcp_to_uart_()` | Read from TCP socket, write to UART |
| `dump_config()` | Log configuration |

#### Loopback Network Interface
- lwIP `netif` with `lo` name, address `127.0.0.1/8`.
- `loop_output_()` routes tx packets back via `netif->input()` (loopback).
- Created once in `setup()` via `create_loopback_netif_()`.

#### Stub Ethernet Component
- `network/util.h` uses a cascading `#ifdef` per network type (ethernet, modem, wifi, openthread, host).
- `-DUSE_ETHERNET` selects the ethernet branch, which includes `ethernet_component.h` and uses `ethernet::global_eth_component`.
- The stub header `ethernet_component.h` provides `EthernetComponent` with `is_connected() → true`, `get_use_address() → nullptr`, `get_ip_addresses() → {}`.
- `global_eth_component` is declared extern in the stub, defined in `uart_api.cpp` (initialized to `nullptr`), and set to a static stub instance in `setup()`.
- Without this, `network::is_connected()` returns false and the API server calls `on_fatal_error()` on every connected client every loop iteration.

## Configuration YAML

### `bleproxy_via_uart.yaml` (actual project)
- Board: `esp32-c3-devkitm-1`, framework: `esp-idf`.
- `api.port: 6053`, `network.enable_ipv6: false`.
- UART: TX=GPIO7, RX=GPIO6, 230400 baud, 1024-byte rx buffer.
- `esp32_ble_tracker`, `bluetooth_proxy` enabled.
- `logger.baud_rate: 0` (serial logging disabled — needed to avoid conflict).
- `uart_api` config: port 6053, rx buffer 512.

## TODO
- Support OTA
