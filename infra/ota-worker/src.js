// One stable object selects all three models atomically. No public write API.
export default {
  async fetch(request, env) {
    const headers = {'Access-Control-Allow-Origin':'*','X-Content-Type-Options':'nosniff'};
    if (!['GET','HEAD'].includes(request.method)) return new Response('Method not allowed', {status:405,headers});
    const path = new URL(request.url).pathname;
    try {
      let body, type = 'application/json; charset=utf-8', ttl = 60;
      const latest = path.match(/^\/ota\/(tc1|m1|a1)(\/no-frontend-resources)?\/latest\.json$/);
      if (latest) {
        const stable = await env.RELEASES.get('stable.json');
        if (!stable) return new Response('No stable release', {status:404,headers});
        const release = await stable.json();
        const original = release.models[latest[1]];
        const variant = latest[2] ? 'no-frontend-resources' : 'full';
        const model = latest[2] ? original?.variants?.[variant] : original;
        if (!model) return new Response('Model unavailable', {status:404,headers});
        body = JSON.stringify({schema:1,tag:release.tag,version:release.version,commit:release.commit,model:latest[1],firmware_variant:variant,path:model.path,size:model.size,sha256:model.sha256,release_log:model.release_log});
      } else if (path === '/' || path === '/health') {
        body = JSON.stringify({service:'phicomm-ota',schema:1,index:'/releases/index.json'});
      } else {
        const key = path.slice(1);
        if (key !== 'releases/index.json' && !/^releases\/v\d+\.\d+\.\d+\/[A-Za-z0-9_.-]+$/.test(key))
          return new Response('Not found', {status:404,headers});
        const object = await env.RELEASES.get(key);
        if (!object) return new Response('Not found', {status:404,headers});
        object.writeHttpMetadata(new Headers());
        type = key.endsWith('.json') ? type : key.endsWith('.md') ? 'text/plain; charset=utf-8' : key.endsWith('.html') ? 'text/html; charset=utf-8' : 'application/octet-stream';
        ttl = key === 'releases/index.json' ? 60 : 31536000;
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
