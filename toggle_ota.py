#!/usr/bin/env python3
"""Toggle OTA mode on the UART API Bridge using the ESPHome Native API protocol."""

import socket
import sys

import serial

from aioesphomeapi import api_pb2


_USAGE = """Usage: toggle_ota.py <port> [on|off] [--baud BAUDRATE]

Toggle OTA mode on the UART API Bridge.

Arguments:
  <port>     Serial port (e.g. /dev/ttyUSB0, COM3) or TCP address (e.g. 192.168.1.100:6053)
  [on|off]   Desired state (default: on)
  --baud     Baud rate for serial port (default: 230400)

Examples:
  toggle_ota.py /dev/ttyUSB0 on       # enable OTA mode via UART
  toggle_ota.py /dev/ttyUSB0 off      # disable OTA mode via UART
  toggle_ota.py 192.168.1.100:6053 on # enable OTA mode via TCP
  toggle_ota.py --baud 115200 COM3 on
"""


def _varuint_to_bytes(value: int) -> bytes:
    if value <= 0x7F:
        return bytes((value,))
    result = bytearray()
    while value:
        temp = value & 0x7F
        value >>= 7
        if value:
            result.append(temp | 0x80)
        else:
            result.append(temp)
    return bytes(result)


def _read_varuint(data: bytes, pos: int):
    result = 0
    bitpos = 0
    while pos < len(data):
        val = data[pos]
        pos += 1
        result |= (val & 0x7F) << bitpos
        if not (val & 0x80):
            return result, pos
        bitpos += 7
        if bitpos > 28:
            return None, pos
    return None, pos


def make_packet(msg_type: int, data: bytes) -> bytes:
    return b'\0' + _varuint_to_bytes(len(data)) + _varuint_to_bytes(msg_type) + data


class _TcpTransport:
    def __init__(self, host: str, port: int):
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.settimeout(5)
        self._sock.connect((host, port))

    def read(self, n: int) -> bytes:
        buf = bytearray()
        while len(buf) < n:
            chunk = self._sock.recv(n - len(buf))
            if not chunk:
                break
            buf.extend(chunk)
        return bytes(buf)

    @property
    def timeout(self) -> float:
        return self._sock.gettimeout()

    @timeout.setter
    def timeout(self, value: float) -> None:
        self._sock.settimeout(value)

    def write(self, data: bytes) -> int:
        return self._sock.send(data)

    def close(self) -> None:
        self._sock.close()


_format_error = 0
_format_ok = 1
_format_timeout = 2


def _complete_frame(data: bytes):
    pos = 0
    if pos >= len(data):
        return _format_timeout, None, None, data
    if data[pos] != 0x00:
        return _format_error, None, None, data
    pos += 1

    length = None
    while pos < len(data):
        val, new_pos = _read_varuint(data, pos)
        if val is not None:
            length = val
            pos = new_pos
            break
        if len(data) - pos > 4:
            return _format_error, None, None, data
        return _format_timeout, None, None, data

    if length is None:
        return _format_timeout, None, None, data

    msg_type = None
    while pos < len(data):
        val, new_pos = _read_varuint(data, pos)
        if val is not None:
            msg_type = val
            pos = new_pos
            break
        if len(data) - pos > 4:
            return _format_error, None, None, data
        return _format_timeout, None, None, data

    if msg_type is None:
        return _format_timeout, None, None, data

    remaining = len(data) - pos
    if remaining < length:
        return _format_timeout, None, None, data

    payload = data[pos:pos + length]
    return _format_ok, msg_type, payload, data[pos + length:]


def read_frame(transport, timeout: float = 5):
    transport.timeout = timeout
    buf = b''
    while True:
        status, msg_type, payload, rest = _complete_frame(buf)
        if status == _format_ok:
            return msg_type, payload
        if status == _format_error:
            return None, None
        chunk = transport.read(1)
        if not chunk:
            return None, None
        buf += chunk


def _open_transport(addr: str, baud: int):
    if ':' in addr and not addr.startswith('/dev/') and not addr.upper().startswith('COM'):
        host, port_str = addr.rsplit(':', 1)
        port = int(port_str)
        print(f'Connecting to TCP {host}:{port}...')
        return _TcpTransport(host, port)
    print(f'Opening serial port {addr} @ {baud} baud...')
    return serial.Serial(addr, baud, timeout=5)


def main():
    if '-h' in sys.argv or '--help' in sys.argv:
        print(_USAGE, end='')
        return
    port = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyACM0'
    state_str = sys.argv[2] if len(sys.argv) > 2 else 'on'
    state = state_str.lower() in ('on', 'true', '1')
    baud = 230400

    if '--baud' in sys.argv:
        idx = sys.argv.index('--baud')
        if idx + 1 < len(sys.argv):
            baud = int(sys.argv[idx + 1])

    transport = _open_transport(port, baud)

    req = api_pb2.HelloRequest()
    req.client_info = 'toggle_ota.py'
    req.api_version_major = 1
    req.api_version_minor = 9
    transport.write(make_packet(1, req.SerializeToString()))

    msg_type, data = read_frame(transport)
    if msg_type != 2:
        print(f'Error: expected HelloResponse (2), got {msg_type}')
        transport.close()
        sys.exit(1)

    hello = api_pb2.HelloResponse()
    hello.ParseFromString(data)
    print(f'Connected: {hello.name} (api {hello.api_version_major}.{hello.api_version_minor})')

    transport.write(make_packet(11, b''))

    switch_key = None
    while True:
        msg_type, data = read_frame(transport, timeout=3)
        if msg_type is None:
            print('Warning: timed out waiting for list entities')
            break
        if msg_type == 19:
            break
        if msg_type == 17:
            les = api_pb2.ListEntitiesSwitchResponse()
            les.ParseFromString(data)
            if 'ota' in (les.object_id or '').lower() or 'ota' in (les.name or '').lower():
                switch_key = les.key
                print(f'Found: {les.name} (key={les.key})')

    if switch_key is None:
        print('Error: OTA mode switch not found')
        transport.close()
        sys.exit(1)

    sc = api_pb2.SwitchCommandRequest()
    sc.key = switch_key
    sc.state = state
    transport.write(make_packet(33, sc.SerializeToString()))
    print(f'Sent command: switch=ON' if state else f'Sent command: switch=OFF')
    transport.close()


if __name__ == '__main__':
    main()
