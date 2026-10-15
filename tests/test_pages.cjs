const {chromium}=require('../platform/frontend/node_modules/@playwright/test');
const assert=require('node:assert/strict');
(async()=>{const browser=await chromium.launch({headless:true});
for(const model of ['tc1','m1','a1','platform']){
 const page=await browser.newPage(),errors=[],requests=[];
 page.on('pageerror',e=>errors.push(e.message));page.on('request',r=>{if(!r.url().startsWith('http://127.0.0.1:18765/'))requests.push(r.url());});
 await page.goto('http://127.0.0.1:18765/demos/'+model+'/');
 if(model==='platform'){
  await page.waitForSelector('.device-select');
  assert.equal(await page.locator('.device-select').count(),3);
  for(const button of await page.locator('.device-select').all())await button.click();
 } else {
  await page.waitForSelector('#otaSource',{state:'attached'});
  await page.evaluate(()=>{location.hash='settings/firmware';});
  await page.locator('#otaSource').selectOption('global');
  await page.getByRole('button',{name:/检查更新|Check for updates/}).click();
  await page.getByText(/当前已是最新版本|Already up to date/).waitFor();
  if(model==='tc1'){
   await page.evaluate(()=>{location.hash='home';});
   await page.locator('#socket0').click();
  }
 }
 await page.waitForTimeout(300);
 assert.deepEqual(errors,[],model+' console errors');assert.deepEqual(requests,[],model+' must make no external requests');
 await page.screenshot({path:'build/pages-'+model+'.png',fullPage:true});await page.close();console.log(model+' interactive demo passed');
}
const page=await browser.newPage();await page.goto('http://127.0.0.1:18765/');await page.locator('#archive .release').first().waitFor();assert((await page.locator('#archive .release').count())>=5);await page.locator('#archive summary').first().click();await page.locator('#archive .notes[data-loaded="1"]').first().waitFor();await page.screenshot({path:'build/pages-home.png',fullPage:true});await browser.close();})().catch(e=>{console.error(e);process.exit(1)});
