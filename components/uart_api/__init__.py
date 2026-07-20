import os
from esphome import pins
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID
from esphome.core import CORE

DEPENDENCIES = ["api", "uart"]
MULTI_CONF = False

uart_api_ns = cg.esphome_ns.namespace("uart_api")
UARTAPIBridge = uart_api_ns.class_("UARTAPIBridge", cg.Component, uart.UARTDevice)

CONF_API_PORT = "api_port"
CONF_RX_BUFFER_SIZE = "rx_buffer_size"
CONF_DEBUG_ECHO = "debug_echo"
CONF_STATUS_PIN = "status_pin"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(UARTAPIBridge),
            cv.Optional(CONF_API_PORT, default=6053): cv.port,
            cv.Optional(CONF_RX_BUFFER_SIZE, default=256): cv.int_range(min=64, max=2048),
            cv.Optional(CONF_DEBUG_ECHO, default=False): cv.boolean,
            cv.Optional(CONF_STATUS_PIN): pins.gpio_output_pin_schema,
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)

STUB_HEADER = """\
#pragma once
#include "esphome/core/component.h"
#include "esphome/components/network/ip_address.h"
namespace esphome {
namespace ethernet {
class EthernetComponent : public Component {
 public:
  void setup() override {}
  bool is_connected() { return true; }
  const char *get_use_address() { return nullptr; }
  network::IPAddresses get_ip_addresses() { return {}; }
};
extern EthernetComponent *global_eth_component;
}
}
"""

def _patch_copy_src_tree():
    """Monkey-patch writer.copy_src_tree to write our stub after sources are copied."""
    from esphome import writer

    original = writer.copy_src_tree

    def patched():
        original()
        stub_dir = CORE.relative_src_path("esphome", "components", "ethernet")
        stub_path = os.path.join(stub_dir, "ethernet_component.h")
        os.makedirs(stub_dir, exist_ok=True)
        with open(stub_path, "w") as f:
            f.write(STUB_HEADER)

    writer.copy_src_tree = patched


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_api_port(config[CONF_API_PORT]))
    cg.add(var.set_rx_buffer_size(config[CONF_RX_BUFFER_SIZE]))
    cg.add(var.set_debug_echo(config[CONF_DEBUG_ECHO]))
    cg.add_build_flag("-DUSE_ETHERNET")

    _patch_copy_src_tree()

    if CONF_STATUS_PIN in config:
        pin = await cg.gpio_pin_expression(config[CONF_STATUS_PIN])
        cg.add(var.set_status_pin(pin))
