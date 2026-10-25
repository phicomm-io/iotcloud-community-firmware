/* Optional browser I/O. Firmware never downloads or stores these resources. */
(function (root, factory) {
  'use strict';
  if (typeof module === 'object' && module.exports) module.exports = factory(root);
  else root.ReleaseNotesClient = factory(root);
}(typeof globalThis !== 'undefined' ? globalThis : this, function (root) {
  'use strict';
  class ClientError extends Error {
    constructor(code, reason) { super(reason); this.code = code; }
  }
  function fail(code, reason) { throw new ClientError(code, reason); }
  function freeze(value) {
    if (value && typeof value === 'object' && !Object.isFrozen(value)) {
      Object.freeze(value); for (const v of Object.values(value)) freeze(v);
    }
    return value;
  }
  function baseURL(source) {
    if (!source) return '';
    const url = new URL(source);
    if (!['https:', 'http:'].includes(url.protocol) || url.username || url.password || url.search || url.hash || url.pathname !== '/')
      fail('INVALID_SOURCE', 'Select a release server root URL.');
    return url.origin;
  }
  async function bytes(response, limit) {
    if (Number(response.headers?.get('content-length')) > limit) fail('DATA_TOO_LARGE', 'The response exceeds its allowed size.');
    if (response.body?.getReader) {
      const reader = response.body.getReader(), parts = []; let length = 0;
      try {
        for (;;) {
          const {done, value} = await reader.read(); if (done) break;
          length += value.byteLength;
          if (length > limit) fail('DATA_TOO_LARGE', 'The response exceeds its allowed size.');
          parts.push(value);
        }
      } catch (error) { await reader.cancel().catch(() => {}); throw error; }
      finally { reader.releaseLock(); }
      const result = new Uint8Array(length); let offset = 0;
      for (const part of parts) { result.set(part, offset); offset += part.byteLength; }
      return result;
    }
    const result = new Uint8Array(await response.arrayBuffer());
    if (result.byteLength > limit) fail('DATA_TOO_LARGE', 'The response exceeds its allowed size.');
    return result;
  }
  function document(data) {
    try { return JSON.parse(new TextDecoder().decode(data)); }
    catch (_) { fail('INVALID_RESPONSE', 'The server did not return valid JSON.'); }
  }
  async function withTimeout(options, work) {
    const controller = new AbortController(); let expired = false;
    const cancel = () => controller.abort();
    if (options.signal?.aborted) controller.abort();
    options.signal?.addEventListener('abort', cancel, {once: true});
    const timer = setTimeout(() => { expired = true; controller.abort(); }, options.timeout || 7000);
    try { return await work(controller.signal); }
    catch (error) {
      if (expired) fail('TIMEOUT', 'The request timed out. Manual selection remains available.');
      if (controller.signal.aborted) fail('REQUEST_ABORTED', 'The request was superseded or cancelled.');
      if (error instanceof ClientError) throw error;
      if (error.name === 'NotAllowedError' || error.name === 'SecurityError') fail('LOCAL_NETWORK_DENIED', 'Browser permission to access the local network was denied.');
      fail('NETWORK_BLOCKED', 'The request failed. Network access, CORS or local-network permissions may block it.');
    } finally {
      clearTimeout(timer); options.signal?.removeEventListener('abort', cancel);
    }
  }
  async function sha256(data, options) {
    const crypto = options.crypto || root.crypto;
    if (crypto?.subtle)
      return Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', data)), b => b.toString(16).padStart(2, '0')).join('');
    // LAN firmware pages use HTTP, where Web Crypto is unavailable. Hash only
    // the size-bounded public release documents; no device secrets enter here.
    const k = new Uint32Array([
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
      0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
      0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
      0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
      0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
      0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
      0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2]);
    const state = new Uint32Array([0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]);
    const padded = new Uint8Array(Math.ceil((data.length + 9) / 64) * 64), words = new Uint32Array(64);
    padded.set(data); padded[data.length] = 0x80;
    const view = new DataView(padded.buffer);
    view.setUint32(padded.length - 4, data.length * 8);
    const rotate = (x, n) => (x >>> n) | (x << (32 - n));
    for (let offset = 0; offset < padded.length; offset += 64) {
      for (let i = 0; i < 16; i++) words[i] = view.getUint32(offset + i * 4);
      for (let i = 16; i < 64; i++) {
        const x = words[i - 15], y = words[i - 2];
        words[i] = words[i - 16] + (rotate(x,7) ^ rotate(x,18) ^ (x >>> 3)) + words[i - 7] + (rotate(y,17) ^ rotate(y,19) ^ (y >>> 10));
      }
      let [a,b,c,d,e,f,g,h] = state;
      for (let i = 0; i < 64; i++) {
        const t1 = (h + (rotate(e,6) ^ rotate(e,11) ^ rotate(e,25)) + ((e & f) ^ (~e & g)) + k[i] + words[i]) >>> 0;
        const t2 = ((rotate(a,2) ^ rotate(a,13) ^ rotate(a,22)) + ((a & b) ^ (a & c) ^ (b & c))) >>> 0;
        h=g; g=f; f=e; e=(d+t1)>>>0; d=c; c=b; b=a; a=(t1+t2)>>>0;
      }
      [a,b,c,d,e,f,g,h].forEach((value,i) => { state[i] += value; });
    }
    return Array.from(state, value => value.toString(16).padStart(8, '0')).join('');
  }
  async function snapshot(source = '', options = {}) {
    return withTimeout(options, async signal => {
      const base = baseURL(source), fetcher = options.fetch || root.fetch.bind(root);
      const get = async (path, limit) => {
        const response = await fetcher(base + path, {signal, credentials: 'omit', cache: 'no-store', redirect: 'error'});
        if (!response.ok) fail('SOURCE_UNAVAILABLE', 'Release data is unavailable (HTTP ' + response.status + ').');
        return bytes(response, limit);
      };
      const indexBytes = await get('/releases/notes/index.json', 16384), index = document(indexBytes);
      if (index.schema !== 1 || !index.manifest || !index.catalog ||
        !/^\/releases\/notes\/manifest\.[a-f0-9]{64}\.json$/.test(index.manifest.path) || index.catalog.path !== '/releases/index.json')
        fail('INVALID_INDEX', 'The release-note index is invalid.');
      async function verified(entry) {
        if (!Number.isSafeInteger(entry.size) || entry.size < 1 || entry.size > 4 * 1024 * 1024 || !/^[a-f0-9]{64}$/.test(entry.sha256))
          fail('INVALID_INDEX', 'Invalid release-note resource identity.');
        const data = await get(entry.path, entry.size);
        if (data.length !== entry.size || await sha256(data, options) !== entry.sha256)
          fail('SOURCE_MISMATCH', 'Release-note data and archive metadata are out of sync. Choose a consistent source and retry.');
        return document(data);
      }
      const values = await Promise.all([verified(index.manifest), verified(index.catalog)]);
      if (values[0].archive_sha256 !== index.catalog.sha256 || !index.manifest.path.endsWith(index.manifest.sha256 + '.json'))
        fail('SOURCE_MISMATCH', 'Release-note data and archive metadata belong to different snapshots.');
      return freeze({source: base, id: index.manifest.sha256, manifest: values[0], catalog: values[1]});
    });
  }
  function deviceURL(address) {
    let url;
    try { url = new URL(address.includes('://') ? address : 'http://' + address); }
    catch (_) { fail('INVALID_DEVICE_ADDRESS', 'Enter a valid local device address.'); }
    const host = url.hostname.toLowerCase(), octets = host.split('.').map(Number);
    const ipv4 = octets.length === 4 && octets.every((v, i) => Number.isInteger(v) && v >= 0 && v <= 255 && String(v) === host.split('.')[i]);
    const local = host === 'localhost' || host.endsWith('.local') ||
      /^\[(::1|f[cd][a-f0-9:]+|fe[89ab][a-f0-9:]+)\]$/.test(host) ||
      (ipv4 && (octets[0] === 10 || octets[0] === 127 || (octets[0] === 192 && octets[1] === 168) ||
        (octets[0] === 172 && octets[1] >= 16 && octets[1] <= 31) || (octets[0] === 169 && octets[1] === 254)));
    if (!local || !['http:', 'https:'].includes(url.protocol) || url.username || url.password || url.search || url.hash || url.pathname !== '/')
      fail('INVALID_DEVICE_ADDRESS', 'Use a local device IP or .local address without credentials or paths.');
    return url.origin + '/api/v1/info';
  }
  async function readDevice(address, password = '', options = {}) {
    const url = deviceURL(address);
    return withTimeout(options, async signal => {
      const headers = {};
      if (password) {
        const data = new TextEncoder().encode('admin:' + password);
        headers.Authorization = 'Basic ' + root.btoa(Array.from(data, b => String.fromCharCode(b)).join(''));
      }
      const response = await (options.fetch || root.fetch.bind(root))(url, {method: 'GET', headers,
        signal, credentials: 'omit', mode: 'cors', cache: 'no-store', redirect: 'error'});
      if (response.status === 401) fail('DEVICE_UNAUTHORIZED', 'The web management password is incorrect.');
      if ([429, 503].includes(response.status)) fail('DEVICE_BUSY', 'The device is busy. Retry later or select versions manually.');
      if ([404, 405].includes(response.status)) fail('OLD_DEVICE_API', 'This device does not provide the required information endpoint.');
      if (!response.ok) fail('DEVICE_HTTP_ERROR', 'The device returned HTTP ' + response.status + '.');
      const reply = document(await bytes(response, 16384));
      if (reply.code === 'busy') fail('DEVICE_BUSY', 'The device is busy. Retry later or use manual selection.');
      const data = reply.code === 'ok' && reply.data;
      if (!data || typeof data.model !== 'string' || typeof data.firmware_variant !== 'string' ||
        typeof data.version !== 'string' || !/^v?\d+\.\d+\.\d+(?:-[a-zA-Z0-9.-]+)?$/.test(data.version))
        fail('DEVICE_IDENTITY_INCOMPLETE', 'The response lacks a firmware version, model or variant. Protocol v1/v2 is not a firmware version.');
      return freeze({model: data.model.toLowerCase(), variant: data.firmware_variant, version: data.version});
    });
  }
  return Object.freeze({snapshot, readDevice, deviceURL, ClientError});
}));
