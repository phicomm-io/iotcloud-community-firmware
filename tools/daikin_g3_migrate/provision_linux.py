#!/usr/bin/env python3
"""Send the stock G3 custom Wi-Fi/gateway message using Linux BlueZ.

Read a private JSON configuration from stdin. Requires python3-dbus and BlueZ.
"""
import ipaddress
import json
import sys
import time

import dbus


def main():
    config = json.load(sys.stdin)
    address = config['address'].upper()
    expected_name = config['device_name'].upper()
    mac = bytes.fromhex(config['gateway_mac'].replace(':', ''))
    if len(mac) != 6 or mac[0] & 1 or mac == bytes(6):
        raise ValueError('Invalid virtual gateway MAC')
    if config.get('operation') == 'gateway':
        body = bytes([0x42, 3, 0, 0, 6]) + mac + bytes([3])
    else:
        ssid = config['ssid'].encode()
        password = config['password'].encode()
        gateway = str(ipaddress.IPv4Address(config['gateway_ip'])).encode()
        if not 1 <= len(ssid) <= 32 or len(password) > 64:
            raise ValueError('Invalid Wi-Fi field length')
        body = bytes([0x42, 1, 0, 0, len(ssid)]) + ssid
        body += bytes([len(password)]) + password
        body += bytes([len(gateway)]) + gateway + bytes([6]) + mac + bytes([3])

    bus = dbus.SystemBus()
    manager = dbus.Interface(bus.get_object('org.bluez', '/'), 'org.freedesktop.DBus.ObjectManager')
    objects = manager.GetManagedObjects()
    adapter_path = next(path for path, interfaces in objects.items() if 'org.bluez.Adapter1' in interfaces)
    adapter = dbus.Interface(bus.get_object('org.bluez', adapter_path), 'org.bluez.Adapter1')
    adapter.StartDiscovery()
    try:
        deadline = time.monotonic() + 20
        while True:
            objects = manager.GetManagedObjects()
            matches = [str(path) for path, interfaces in objects.items()
                       if str(interfaces.get('org.bluez.Device1', {}).get('Address', '')).upper() == address
                       and str(interfaces['org.bluez.Device1'].get('Name', '')).upper() == expected_name]
            if matches:
                path = matches[0]
                break
            if time.monotonic() >= deadline:
                raise TimeoutError('Selected G3 not discovered')
            time.sleep(.25)
        device = dbus.Interface(bus.get_object('org.bluez', path), 'org.bluez.Device1')
        if objects[path]['org.bluez.Device1']['Connected']:
            device.Disconnect()
            time.sleep(1)
        device.Connect(timeout=25)
        deadline = time.monotonic() + 15
        while True:
            objects = manager.GetManagedObjects()
            if objects[path]['org.bluez.Device1']['ServicesResolved']:
                break
            if time.monotonic() >= deadline:
                raise TimeoutError('G3 GATT services unavailable')
            time.sleep(.25)
    finally:
        adapter.StopDiscovery()

    characteristics = {str(values['org.bluez.GattCharacteristic1']['UUID']): str(item)
                       for item, values in objects.items() if str(item).startswith(path + '/')
                       and 'org.bluez.GattCharacteristic1' in values}
    tx = dbus.Interface(bus.get_object('org.bluez', characteristics['0000ff01-0000-1000-8000-00805f9b34fb']), 'org.bluez.GattCharacteristic1')
    rx_object = bus.get_object('org.bluez', characteristics['0000ff02-0000-1000-8000-00805f9b34fb'])
    rx = dbus.Interface(rx_object, 'org.bluez.GattCharacteristic1')
    properties = dbus.Interface(rx_object, 'org.freedesktop.DBus.Properties')
    sequence = 0

    def send(kind, payload=b'', control=0):
        nonlocal sequence
        frame = bytes([kind, control, sequence, len(payload)]) + payload
        tx.WriteValue(dbus.Array(frame, signature='y'), dbus.Dictionary({'type': 'request'}, signature='sv'))
        sequence = (sequence + 1) & 255

    rx.StartNotify()
    send(0x1c)
    deadline = time.monotonic() + 5
    while True:
        value = bytes(properties.Get('org.bluez.GattCharacteristic1', 'Value'))
        if len(value) >= 6 and value[0] == 0x41:
            print('Selected', expected_name, 'BLUFi version', value[4], value[5], flush=True)
            break
        if time.monotonic() >= deadline:
            raise TimeoutError('G3 BLUFi version response unavailable')
        time.sleep(.1)
    fragments = 0
    while len(body) > 16:
        send(0x4d, len(body).to_bytes(2, 'little') + body[:14], 0x10)
        body = body[14:]
        fragments += 1
        time.sleep(.5)
    send(0x4d, body)
    fragments += 1
    print('Sent stock custom configuration:', fragments, 'fragments; verify DHCP and gateway association next.', flush=True)
    time.sleep(2)
    device.Disconnect()


if __name__ == '__main__':
    main()
