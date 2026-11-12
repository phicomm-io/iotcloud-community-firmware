#!/bin/sh
set -eu
cd "$(dirname "$0")"
app="${1:-$PWD/IotcloudG3Provision.app}"
mkdir -p "$app/Contents/MacOS"
cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?><!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd"><plist version="1.0"><dict><key>CFBundleIdentifier</key><string>io.phicomm.g3.provision</string><key>CFBundleName</key><string>iotcloud G3 Provision</string><key>CFBundleExecutable</key><string>provision</string><key>NSBluetoothAlwaysUsageDescription</key><string>Configure the selected Daikin G3 detector over Bluetooth.</string><key>NSBluetoothPeripheralUsageDescription</key><string>Configure the selected Daikin G3 detector over Bluetooth.</string></dict></plist>
PLIST
swiftc macos_blufi_provision.swift -o "$app/Contents/MacOS/provision"
codesign --force --deep --sign - "$app"
printf 'Built %s\n' "$app"
