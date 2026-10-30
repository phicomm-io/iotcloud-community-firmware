/* Real Chromium against the production shared HTTP server, with no API mocks. */
const fs=require('node:fs'),assert=require('node:assert/strict'),path=require('node:path');
const {chromium}=require(process.env.PHICOMM_PLAYWRIGHT_MODULE||'playwright');
const url=process.argv[2],run=process.argv[3];
(async()=>{
 const browser=await chromium.launch({headless:true,executablePath:process.env.PHICOMM_CHROMIUM});
 const context=await browser.newContext({viewport:{width:1280,height:1000},recordVideo:{dir:path.join(run,'browser-video'),size:{width:1280,height:1000}}});
 const page=await context.newPage(),errors=[],requests=[];page.setDefaultTimeout(15000);page.on('pageerror',e=>errors.push(e.message));
 page.on('request',q=>{if(q.url().includes('/api/v1/'))requests.push({url:q.url(),method:q.method(),headers:q.headers(),body:q.postDataBuffer()?.toString('base64')});});
 try{
  await page.goto(url);await page.waitForFunction(()=>globalThis.UI?.state?.socket_states?.length===1);
  assert.equal(await page.title(),'iotcloud · S30-V1B');assert.equal(await page.locator('#socket0').count(),1);
  assert.equal(await page.locator('input[type=password]:visible').count(),0);
  await page.locator('#socket0').click();await page.waitForFunction(()=>UI.state.socket_states[0]===true&&document.getElementById('socket0').getAttribute('aria-busy')==='false');
  assert.match(await page.locator('#onCount').textContent(),/1 of 1/);
  await page.locator('a[href="#settings/network"]').first().click();await page.locator('#ssid').waitFor({state:'visible'});assert.equal(await page.locator('#ssid').inputValue(),'lab');
  await page.locator('a[href="#settings/mqtt"]').click();await page.locator('#mqttHost').waitFor({state:'visible'});
  assert.equal(await page.locator('#mqttPassword').getAttribute('maxlength'),'127');
  await page.locator('#mqttForm button[type=submit]').click();await page.waitForFunction(()=>!document.querySelector('#mqttForm button[type=submit]').disabled);
  await page.locator('a[href="#settings/device"]').click();await page.locator('#deviceName').fill('S30 desk');await page.locator('#nameForm button[type=submit]').click();await page.waitForFunction(()=>UI.state.name==='S30 desk');
  await page.locator('#nameFields input').fill('Desk outlet');await page.locator('#socketNamesForm button').click();await page.waitForFunction(()=>UI.state.socket_names[0]==='Desk outlet');
  await page.locator('#reportSeconds').fill('8');await page.locator('#reportForm button').click();await page.waitForFunction(()=>UI.state.report_seconds===8);
  await page.locator('#coefForm').evaluate(el=>el.closest('details').open=true);
  assert.equal(await page.locator('#voltageCoef').inputValue(),'1.894288');
  assert.equal(await page.locator('#powerCoef').inputValue(),'1.88835008906368');
  assert.equal(await page.locator('#coefForm').evaluate(el=>el.checkValidity()),true,'Stock coefficients must pass native number-input validation');
  await page.locator('#currentCoef').fill('1.01023456789');
  await page.evaluate(()=>UI.reload({full:true}));assert.equal(await page.locator('#currentCoef').inputValue(),'1.01023456789','Polling must preserve edits');
  await page.locator('#coefForm button').click();await page.waitForFunction(()=>UI.state.current_coefficient===1.01023456789);
  await page.locator('#bootPower').selectOption('on');await page.locator('#bootForm button').click();await page.waitForFunction(()=>UI.state.power_on_mode==='on');
  await page.locator('#bootPower').selectOption('restore');await page.locator('#bootForm button').click();await page.waitForFunction(()=>UI.state.power_on_mode==='restore');
  await page.locator('#childLockDevice').check();await page.waitForFunction(()=>UI.confirmed.child_lock===true&&!UI.controlBusy);
  await page.locator('#ledEnabledDevice').uncheck();await page.waitForFunction(()=>UI.confirmed.led_enabled===false&&!UI.controlBusy);
  await page.locator('a[href="#settings/schedule"]').click();
  const schedule=page.locator('[data-settings-page="schedule"]');
  await schedule.locator('[data-power-watts]').fill('5.123');await schedule.locator('[data-power-seconds]').fill('0');
  await schedule.locator('form button[type=submit]').click();await schedule.locator('.schedule-task').waitFor();
  assert.match(await schedule.locator('.schedule-task').textContent(),/5.123 W/);
  await schedule.getByRole('button',{name:'Edit',exact:true}).click();
  await schedule.locator('[data-power-watts]').fill('7.125');
  await page.evaluate(()=>UI.reload({full:true}));assert.equal(await schedule.locator('[data-power-watts]').inputValue(),'7.125','Status refresh must preserve unsaved automation edits');
  await schedule.locator('form button[type=submit]').click();await page.waitForFunction(()=>document.querySelector('.schedule-task')?.textContent.includes('7.125 W'));
  await schedule.getByRole('button',{name:'Disable',exact:true}).click();await schedule.getByRole('button',{name:'Enable',exact:true}).waitFor();
  await schedule.getByRole('button',{name:'Delete',exact:true}).click();await page.waitForFunction(()=>!document.querySelector('.schedule-task'));
  await page.locator('a[href="#settings/device"]').click();
  await page.locator('#langPick').selectOption('zh-CN');
  await page.screenshot({path:path.join(run,'frontend-desktop-settings.png'),fullPage:true});
  await page.locator('a[href="#settings/firmware"]').click();await page.locator('#otaUrl').waitFor({state:'visible'});assert.equal(await page.locator('#fileForm').isVisible(),false);assert.equal(await page.locator('#otaUrl').getAttribute('maxlength'),'127');
  await page.locator('a[href="#home"]').click();await page.locator('#watts').waitFor({state:'visible'});await page.waitForFunction(()=>document.getElementById('meterStatus').textContent.length>0);assert.match(await page.locator('#watts').textContent(),/—/);
  assert.equal(await page.locator('#meterLive.ok').count(),0);assert.match(await page.locator('#onCount').textContent(),/1 路中 1 路/);
  assert.equal(await page.locator('#meterStatus').textContent(),'等待有效计量数据');
  await page.waitForFunction(()=>!document.querySelector('#toasts')?.childElementCount);
  await page.screenshot({path:path.join(run,'frontend-desktop.png'),fullPage:true});
  await page.locator('#themePick').selectOption('dark');await page.locator('#langPick').selectOption('en');
  assert.match(await page.locator('#onCount').textContent(),/1 of 1/);assert.equal(await page.locator('#meterStatus').textContent(),'Waiting for a fresh meter sample');
  await page.setViewportSize({width:390,height:844});await page.screenshot({path:path.join(run,'frontend-mobile-dark.png'),fullPage:true});
  assert.ok(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),'Mobile page overflows');
  for(let i=0;i<6;i++){await page.reload();await page.waitForFunction(()=>globalThis.UI?.state?.socket_states?.length===1);}
  assert.equal(await page.locator('#themePick').inputValue(),'dark');
  const loginRequests=requests.filter(r=>r.url.includes('/auth/login'));
  assert.ok(loginRequests.length<=7,'Probe empty-password policy at most once per document');
  assert.ok(requests.every(r=>!r.headers.authorization),'Sessionless pages must not send stale credentials');
  for(const r of requests){
   const discovery=new URL(r.url).pathname==='/api/v1/info';
   assert.equal(r.headers.accept,discovery?'application/msgpack, application/json':'application/msgpack');
   if(r.method==='POST')assert.equal(r.headers['content-type'],'application/msgpack');
  }
  assert.deepEqual(errors,[]);
  fs.writeFileSync(path.join(run,'browser-result.json'),JSON.stringify({status:'PASS',actual_shared_http:true,api_mocks:false,empty_password_reloads:6,desktop_mobile:true,languages:['zh-CN','en'],forms:['mqtt','name','socket_name','report','meter','power_on_modes','power_schedules','child_lock','indicator'],page_errors:errors,request_count:requests.length},null,2)+'\n');
  console.log('PASS: shared frontend on real HTTP, all product forms, empty auth, mobile, theme, metering validity');
 }catch(e){
  await page.screenshot({path:path.join(run,'frontend-failure.png'),fullPage:true}).catch(()=>{});
  const state=await page.evaluate(()=>({hash:location.hash,state:globalThis.UI?.state,confirmed:globalThis.UI?.confirmed,controlBusy:globalThis.UI?.controlBusy})).catch(()=>null);
  fs.writeFileSync(path.join(run,'browser-failure.json'),JSON.stringify({error:String(e),state,page_errors:errors},null,2)+'\n');throw e;
 }finally{await context.close();await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
