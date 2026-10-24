'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const {chromium}=require(process.env.PHICOMM_PLAYWRIGHT||'@playwright/test');
(async()=>{
 const browser=await chromium.launch({headless:true});const base=process.env.PHICOMM_SITE_TEST_URL||'http://127.0.0.1:18765';
 fs.mkdirSync('build/notes-evidence',{recursive:true});let cases=0;
 try{
  for(const locale of ['zh','en'])for(const width of [1280,390]){
   const page=await browser.newPage({viewport:{width,height:900}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
   const path=locale==='en'?'/guides/en/release-log.html':'/guides/release-log.html';
   await page.goto(base+path+'?from=0.9.14&to=0.9.23&model=a1&variant=full');
   await page.locator('[data-report-state="complete"]').waitFor();
   assert.ok(await page.locator('.notes-change').count()>5);assert.deepEqual(errors,[]);
   assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),true,locale+' '+width+' overflow');
   await page.screenshot({path:`build/notes-evidence/${locale}-${width}.png`,fullPage:true});
   await page.locator('#notes-to').selectOption('0.9.14');await page.locator('#notes-form').evaluate(f=>f.requestSubmit());
   await page.locator('.notes-empty').waitFor();assert.equal(await page.locator('.notes-change').count(),0);
   await page.locator('#notes-from').selectOption('0.9.5');await page.locator('#notes-to').selectOption('0.9.23');await page.locator('#notes-form').evaluate(f=>f.requestSubmit());
   await page.locator('[data-report-state="error"]').waitFor();assert.equal(await page.locator('#notes-share').isVisible(),false);
   await page.close();cases++;
  }
  for(const locale of ['zh','en']){
   const page=await browser.newPage({javaScriptEnabled:false,viewport:{width:390,height:900}});
   await page.goto(base+(locale==='en'?'/guides/en/release-log.html':'/guides/release-log.html'));
   assert.equal(await page.locator('.notes-history-version').count(),23);assert.equal(await page.locator('#notes-history').isVisible(),true);
   assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),true);
   await page.screenshot({path:`build/notes-evidence/${locale}-nojs.png`,fullPage:true});await page.close();cases++;
  }
  const page=await browser.newPage();await page.goto(base+'/');await page.locator('#archive .release').first().waitFor();
  assert.equal(await page.locator('#archive .release').count(),23);assert.equal(await page.locator('#latest .release').count(),1);
  await page.locator('#archive summary').first().click();assert.ok(await page.locator('#archive .notes').first().textContent());await page.close();cases++;
  fs.writeFileSync('build/notes-evidence/browser.json',JSON.stringify({status:'PASS',cases,local_site:base,real_device:false,production:false},null,2)+'\n');
  console.log(`${cases} release history / comparison browser scenarios passed`);
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
