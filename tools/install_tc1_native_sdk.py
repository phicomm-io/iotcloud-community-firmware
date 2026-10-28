#!/usr/bin/env python3
"""TC1 private SDK profile: native MP app provisioning, no SDK JSON server.

Do not run on another product's SDK tree. WiFi driver, AP, station and EasyLink
sources remain intact. TC1 does not enable SDK WLAN autoconfiguration: its
physical-button/recovery AP and native wifi.* actions own configuration.
"""
from pathlib import Path
import sys


def install(path):
    sdk = Path(path)
    system = sdk/'MiCO/system/system.mk'
    text = system.read_text()
    old = '$(NAME)_SOURCES += config_server/config_server_menu.c \\\n                   config_server/config_server.c'
    new = '# TC1 native MP profile: legacy SDK JSON config server/menu excluded.'
    if old in text:
        text = text.replace(old, new)
    elif new not in text:
        raise RuntimeError('unsupported SDK config server source layout')
    system_text = text
    core = sdk/'MiCO/core/core.mk'
    text = core.read_text()
    old = 'GLOBAL_LDFLAGS   += $$(CLIB_LDFLAGS_NANO_FLOAT)'
    new = 'GLOBAL_LDFLAGS += $$(CLIB_LDFLAGS_NANO) # TC1: no printf float client'
    if old in text:
        text = text.replace(old, new)
    elif new not in text:
        raise RuntimeError('unsupported SDK nano libc flags')
    # Validate both inputs before modifying either SDK makefile, and preserve
    # mtimes on repeated builds so this profile does not force recompilation.
    if system.read_text() != system_text:
        system.write_text(system_text)
    if core.read_text() != text:
        core.write_text(text)


if __name__ == '__main__':
    install(sys.argv[1])
