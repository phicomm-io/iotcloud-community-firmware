// One stable object selects all models atomically. No public write API.
export default {
  async fetch(request, env) {
    const headers = {'Access-Control-Allow-Origin':'*','X-Content-Type-Options':'nosniff'};
    if (!['GET','HEAD'].includes(request.method)) return new Response('Method not allowed', {status:405,headers});
    const path = new URL(request.url).pathname;
    try {
      let body, type = 'application/json; charset=utf-8', ttl = 60;
      const current = path.match(/^\/ota\/v1\/(tc1|m1|a1|daikin)\/(full|no-frontend-resources|fresh-air|fresh-air-no-frontend-resources|hub)\/latest\.(json|msgpack)$/);
      const latest = current || path.match(/^\/ota\/(tc1|m1|a1|daikin)(\/(?:no-frontend-resources|fresh-air|fresh-air-no-frontend-resources))?\/latest\.json$/);
      if (latest) {
        const stable = await env.RELEASES.get('stable.json');
        if (!stable) return new Response('No stable release', {status:404,headers});
        let release = await stable.json();
        const newer = release.version.split('.').map(Number);
        if (current && newer[0] < 1) return new Response('No v1 release', {status:404,headers});
        if (!current && (newer[0] > 1 || newer[0] === 1 && (newer[1] > 0 || newer[2] > 0))) {
          const bridge = await env.RELEASES.get('bridge-v1.0.0.json');
          if (!bridge) return new Response('Bridge release unavailable', {status:503,headers});
          release = await bridge.json();
          if (release.version !== '1.0.0' || release.tag !== 'v1.0.0') return new Response('Invalid bridge release', {status:503,headers});
        }
        const original = release.models[latest[1]];
        const variant = current ? current[2] : latest[2] ? latest[2].slice(1) : 'full';
        if (variant === 'hub' && latest[1] !== 'daikin') return new Response('Not found', {status:404,headers});
        if (variant.startsWith('fresh-air') && latest[1] !== 'a1') return new Response('Not found', {status:404});
        if (latest[1] === 'daikin' && !['full','hub'].includes(variant)) return new Response('Not found', {status:404,headers});
        const model = variant === 'full' ? original : original?.variants?.[variant];
        if (!model) return new Response('Model unavailable', {status:404,headers});
        if (current && current[3] === 'msgpack') {
          const asset = release.device_manifests?.[`${latest[1]}/${variant}`]?.msgpack;
          if (!asset || !asset.path.startsWith(`/releases/${release.tag}/`) ||
              !/^\/releases\/v\d+\.\d+\.\d+\/[A-Za-z0-9_.-]+\.latest\.msgpack$/.test(asset.path))
            return new Response('Binary manifest unavailable', {status:404,headers});
          const object = await env.RELEASES.get(asset.path.slice(1));
          if (!object || object.size !== asset.size)
            return new Response('Binary manifest unavailable', {status:503,headers:{...headers,'Cache-Control':'no-store'}});
          return new Response(request.method === 'HEAD' ? null : object.body, {headers:{...headers,
            'Content-Type':'application/msgpack','Content-Length':String(object.size),
            'Cache-Control':'public, max-age=60, no-transform',ETag:object.httpEtag}});
        }
        body = JSON.stringify({schema:1,tag:release.tag,version:release.version,commit:release.commit,model:latest[1],firmware_variant:variant,path:model.path,size:model.size,sha256:model.sha256,release_log:model.release_log});
      } else if (path === '/' || path === '/health') {
        body = JSON.stringify({service:'phicomm-ota',schema:1,index:'/releases/index.json'});
      } else {
        const key = path.slice(1);
        const notesIndex = key === 'releases/notes/index.json';
        const notesManifest = /^releases\/notes\/manifest\.[a-f0-9]{64}\.json$/.test(key);
        if (key !== 'releases/index.json' && !notesIndex && !notesManifest && !/^releases\/v\d+\.\d+\.\d+\/[A-Za-z0-9_.-]+$/.test(key))
          return new Response('Not found', {status:404,headers});
        const object = await env.RELEASES.get(key);
        if (!object) return new Response('Not found', {status:404,headers});
        object.writeHttpMetadata(new Headers());
        type = key.endsWith('.msgpack') ? 'application/msgpack' : key.endsWith('.json') ? type : key.endsWith('.md') ? 'text/plain; charset=utf-8' : key.endsWith('.html') ? 'text/html; charset=utf-8' : 'application/octet-stream';
        ttl = key === 'releases/index.json' || notesIndex ? 60 : 31536000;
        headers['Content-Length'] = String(object.size);
        headers.ETag = object.httpEtag;
        headers['Content-Type'] = type;
        headers['Cache-Control'] = `public, max-age=${ttl}, no-transform`;
        return new Response(request.method === 'HEAD' ? null : object.body, {headers});
      }
      headers['Content-Type'] = type;
      headers['Content-Length'] = String(new TextEncoder().encode(body).length);
      headers['Cache-Control'] = `public, max-age=${ttl}, no-transform`;
      return new Response(request.method === 'HEAD' ? null : body, {headers});
    } catch {
      return new Response('Storage temporarily unavailable', {status:503,headers:{...headers,'Cache-Control':'no-store'}});
    }
  }
};
