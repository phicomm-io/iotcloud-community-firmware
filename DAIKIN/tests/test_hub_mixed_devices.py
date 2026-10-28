"""Real TCP MQTT clients through production hub routing and business models.

HAP/controller and external Bemfa/cloud service I/O are simulated at their
production callbacks; actual HomeSpan pairing and internet services are not used.
"""
from pathlib import Path
import socket
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1',0))
        return s.getsockname()[1]

class MixedHub(unittest.TestCase):
    def test_tcp_devices_use_discovered_codec_through_business_roundtrips(self):
        with tempfile.TemporaryDirectory() as d:
            temp=Path(d)
            source=(ROOT/'src/hub.inc').read_text()
            event=source[source.index('    iotcloud::hubWire.observe(topic,body);'):source.index('    if (topic == "homeassistant/status"')]
            (temp/'mixed-event.inc').write_text(event)
            exe=temp/'mixed'
            subprocess.run(['c++','-std=c++11','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(temp),'-I'+str(ROOT/'include'),'-I'+str(ROOT/'.pio/libdeps/bry88ab-hub/ArduinoJson/src'),str(ROOT/'tests/hub/mixed_devices.cpp'),'-o',str(exe)],check=True)
            tcp,ws=port(),port()
            while ws==tcp:ws=port()
            subprocess.run([str(exe),str(tcp),str(ws)],check=True,timeout=30)

if __name__=='__main__':unittest.main()
