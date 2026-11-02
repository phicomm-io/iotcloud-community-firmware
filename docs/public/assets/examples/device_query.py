#!/usr/bin/env python3
"""Read a device using an explicitly selected RC5 or 0.9.27 baseline."""
import argparse
import base64
import getpass
import json
import sys
import uuid
from http.client import HTTPException
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener


class NoRedirect(HTTPRedirectHandler):
    # Keep the device's Authorization header on the selected origin.
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def query(address, baseline, action, password=None):
    url = urlsplit(address)
    if (url.scheme not in ('http', 'https') or not url.hostname or url.username
            or url.password or url.path not in ('', '/') or url.query or url.fragment):
        raise ValueError('Use a device origin such as http://192.168.1.100')
    mime = 'application/msgpack' if baseline == 'rc5' else 'application/json'
    command = {'request_id': uuid.uuid4().hex, 'action': action, 'params': {}}
    if baseline == 'rc5':
        import msgpack
        body = msgpack.packb(command, use_bin_type=True)
    else:
        body = json.dumps(command).encode('utf-8')
    headers = {'Content-Type': mime, 'Accept': mime}
    if password is not None:
        headers['Authorization'] = 'Basic ' + base64.b64encode(
            ('admin:' + password).encode('utf-8')).decode('ascii')
    request = Request(address.rstrip('/') + '/api/v1/command', body, headers, method='POST')
    try:
        response = build_opener(NoRedirect).open(request, timeout=10)
    except HTTPError as error:
        response = error
    with response:
        raw = response.read(1024 * 1024 + 1)
        if len(raw) > 1024 * 1024:
            raise ValueError('Response exceeds example client limit')
        response_mime = response.headers.get_content_type()
        if response.status < 400 and response_mime != mime:
            raise ValueError(f'Expected {mime}; check the selected firmware baseline')
        if response_mime == 'application/msgpack':
            import msgpack
            result = msgpack.unpackb(raw, raw=False)
        elif response_mime == 'application/json':
            result = json.loads(raw)
        else:
            raise ValueError(f'HTTP {response.status}: unexpected Content-Type {response_mime}')
        if not isinstance(result, dict):
            raise ValueError('Expected a command result map')
        if response.status >= 400 or result.get('code') not in ('ok', 'accepted'):
            raise ValueError(f'HTTP {response.status}: {result.get("code", "unknown_error")}')
        if result.get('request_id') != command['request_id']:
            raise ValueError('Response request_id does not match')
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('address', help='Device origin, e.g. http://192.168.1.100')
    parser.add_argument('--baseline', choices=('rc5', '0.9.27'), required=True)
    parser.add_argument('--action', choices=('status.get', 'capabilities.get', 'config.get'),
                        default='capabilities.get')
    parser.add_argument('--password', action='store_true', help='Prompt for device Web password')
    args = parser.parse_args()
    try:
        result = query(args.address, args.baseline, args.action,
                       getpass.getpass('Device Web password: ') if args.password else None)
        print(json.dumps(result, ensure_ascii=False, indent=2))
    except (ValueError, URLError, OSError, HTTPException, ImportError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
