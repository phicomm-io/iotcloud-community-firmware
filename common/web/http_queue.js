/* Serialize API requests through body completion. */
(()=>{
 const nativeFetch=window.fetch.bind(window);
 let tail=Promise.resolve();
 window.fetch=(url,options={})=>{
  if(url!=='/api/v1/command')return nativeFetch(url,options);
  const signal=options.signal;
  const aborted=()=>new DOMException('Request aborted','AbortError');
  if(signal?.aborted)return Promise.reject(aborted());
  return new Promise((resolve,reject)=>{
   const cancel=()=>reject(aborted());
   signal?.addEventListener('abort',cancel,{once:true});
   const work=tail.then(async()=>{
    if(signal?.aborted)throw aborted();
    const response=await nativeFetch(url,options);
    const body=await response.arrayBuffer();
    return new Response(body,{status:response.status,statusText:response.statusText,headers:response.headers});
   });
   tail=work.then(()=>{},()=>{});
   const cleanup=()=>signal?.removeEventListener('abort',cancel);
   work.then(value=>{cleanup();resolve(value);},error=>{cleanup();reject(error);});
  });
 };
})();
