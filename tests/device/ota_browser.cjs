const {chromium}=require('playwright');
const fs=require('fs');
(async()=>{
 const model=process.argv[2],ip=process.argv[3],host=process.argv[4],portsFile=process.argv[5],deviceId=process.argv[6];if(!model||!ip||!host||!portsFile||!deviceId)throw Error('Usage: node ota_browser.cjs MODEL IP FIXTURE_HOST PORTS_JSON DEVICE_ID');const out=`build/ota-evidence/${model}/browser`;fs.mkdirSync(out,{recursive:true});
 const browser=await chromium.launch({headless:true});const page=await browser.newPage({locale:'zh-CN',viewport:{width:1280,height:900}});const errors=[],actions=[];
 page.on('pageerror',e=>errors.push(e.message));page.on('request',r=>{if(r.url().endsWith('/api/v1/command')){try{const q=r.postDataJSON();if(q.action.startsWith('ota.'))actions.push({action:q.action,params:q.params,at:Date.now()});}catch{}}});
 try{
 await page.goto(`http://${ip}/#settings/firmware`,{waitUntil:'domcontentloaded',timeout:20000});await page.locator('#otaSources button').nth(2).waitFor();const identity=await page.evaluate(async()=>{const r=await fetch('/api/v1/status');return (await r.json()).data.device_id;});if(identity!==deviceId)throw Error('Device identity mismatch');
 await page.screenshot({path:out+'/sources.png',fullPage:true});
 for(const source of ['cn','global','github']){
  await page.locator(`[data-source=${source}]`).click();await page.waitForFunction(()=>Array.from(document.querySelectorAll('#otaSources button')).every(b=>!b.disabled),null,{timeout:30000});
  console.log(model,'source',source);
 }
 const notes=page.getByRole('button',{name:'发布说明',exact:true});await notes.click();await page.waitForFunction(()=>[...document.querySelectorAll('pre')].some(p=>p.textContent.includes('TLS OTA validation')),null,{timeout:30000});await page.screenshot({path:out+'/notes.png',fullPage:true});
 await page.getByRole('button',{name:'从网址下载',exact:true}).click();const ports=JSON.parse(fs.readFileSync(portsFile));
 await page.locator('#otaUrl').fill(`https://${host}:${ports.tls}/stall/${model}`);await page.locator('#urlForm button[type=submit]').click();const cancel=page.getByRole('button',{name:'终止下载',exact:true});await cancel.waitFor({state:'visible',timeout:15000});await page.screenshot({path:out+'/cancel.png',fullPage:true});const start=Date.now();await cancel.click();await cancel.waitFor({state:'hidden',timeout:20000});
 const status=await page.evaluate(async()=>{const r=await fetch('/api/v1/command',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:'ota.status.get',params:{}})});return (await r.json()).data});if(status.ota_active||status.binding_running===false)throw Error('not restored');if(errors.length)throw Error(errors.join(';'));
 fs.writeFileSync(out+'/results.json',JSON.stringify({model,ip,sources:['cn','global','github'],notes:'TLS OTA validation',cancel_elapsed_ms:Date.now()-start,status,actions,errors},null,2));console.log(model,'PASS UI',Date.now()-start);
 }catch(e){await page.screenshot({path:out+'/failure.png',fullPage:true}).catch(()=>{});fs.writeFileSync(out+'/failure.json',JSON.stringify({error:String(e),actions,errors,body:await page.locator('body').innerText().catch(()=> '')},null,2));throw e;}finally{await browser.close();}
})().catch(e=>{console.error(e);process.exit(1)});
