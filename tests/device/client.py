"""Native v1 API client for explicitly selected test devices; no implicit writes."""
import gzip
import json
import pathlib
import time
import urllib.error
import urllib.request


def redact(value):
    if isinstance(value, dict):
        return {k: ('[redacted]' if k in ('ssid', 'mqtt_user', 'token', 'group_token') or
                    ('password' in k and not k.endswith('_set')) else redact(v))
                for k, v in value.items()}
    if isinstance(value, list):
        return [redact(v) for v in value]
    return value


def save(path, value, private=False):
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value if private else redact(value), indent=2, ensure_ascii=False))
    if private:
        path.chmod(0o600)


class Device:
    def __init__(self, ip, timeout=5):
        self.ip, self.timeout = ip, timeout

    def request(self, path, data=None):
        # urlopen may fail before response headers expose the current socket.
        # Never attribute a previous request's port to that failure.
        self.last_source_port = None
        request = urllib.request.Request('http://' + self.ip + path,
            data=None if data is None else json.dumps(data).encode(),
            headers={'Content-Type': 'application/json'})
        try:
            response = urllib.request.urlopen(request, timeout=self.timeout)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            try:
                self.last_source_port = response.fp.raw._sock.getsockname()[1]
            except (AttributeError, OSError):
                self.last_source_port = None
            body = response.read()
            if response.headers.get('Content-Encoding') == 'gzip':
                body = gzip.decompress(body)
            return response.status, body

    def command(self, action, params=None, expected=('ok', 'accepted'), request_id=None):
        payload = {'action': action, 'params': {} if params is None else params}
        if request_id is not None:
            payload['request_id'] = request_id
        status, body = self.request('/api/v1/command', payload)
        result = json.loads(body)
        if status != 200 or (expected is not None and result.get('code') not in expected):
            raise AssertionError((self.ip, action, status, redact(result)))
        return result

    def get(self, action='status.get'):
        return self.command(action)['data']

    def set(self, action, params):
        params = dict(params)
        params.pop('revision', None)
        return self.command(action, params)

    def wait(self, predicate, action='status.get', seconds=40):
        end = time.monotonic() + seconds
        last = None
        while time.monotonic() < end:
            try:
                last = self.get(action)
                if predicate(last):
                    return last
            except (OSError, TimeoutError):
                pass
            time.sleep(1)
        raise AssertionError((self.ip, action, 'timeout', redact(last)))
