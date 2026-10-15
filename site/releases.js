'use strict';
const status=document.getElementById('release-status'),source=document.getElementById('source');
const api='https://phicomm.fjrcn.com';
const el=(tag,text)=>{const n=document.createElement(tag);if(text)n.textContent=text;return n;};
function link(label,path){const a=el('a',label);a.href=source.value+path;a.rel='noopener';return a;}
let catalog=null;
function releaseCard(release,latest){
 const box=el(latest?'article':'details');box.className='release';
 const head=el(latest?'h3':'summary',release.tag);box.append(head);
 if(latest){const badge=el('span','最新版');badge.className='tag';head.append(badge);}
 const table=el('table');table.className='downloads';
 const tr=el('tr');for(const text of ['型号','固件下载'])tr.append(el('th',text));table.append(tr);
 for(const [model,value] of Object.entries(release.models)){
  const row=el('tr');row.append(el('td',model.toUpperCase()));const cell=el('td');
  for(const [kind,label] of [['signed','签名 OTA'],['raw','原始 OTA'],['full','完整镜像']])if(value.downloads[kind])cell.append(link(label,value.downloads[kind].path));
  row.append(cell);table.append(row);
 }
 box.append(table,link('SHA256 校验文件',`/releases/${release.tag}/SHA256SUMS`));
 const note=el('pre');note.className='notes';
 const load=async()=>{if(note.dataset.loaded)return;try{const r=await fetch(api+release.notes);if(!r.ok)throw Error('HTTP '+r.status);note.textContent=await r.text();note.dataset.loaded='1';}catch{note.textContent='发布说明暂时无法读取，请稍后重试。';}};
 box.append(link('发布说明 ↗',release.notes),note);
 if(latest)load();else box.addEventListener('toggle',()=>{if(box.open)load();});
 return box;
}
function render(){
 const releases=catalog.releases;document.getElementById('latest').replaceChildren();document.getElementById('archive').replaceChildren();
 // Catalog is descending numeric SemVer. Every published version remains in
 // the archive, including stable, so it is a permanent lookup surface.
 if(releases.length)document.getElementById('latest').append(releaseCard(releases.find(r=>r.tag===catalog.stable)||releases[0],true));
 for(const r of releases)document.getElementById('archive').append(releaseCard(r,false));
 status.textContent=releases.length?`共 ${releases.length} 个版本 · 历史文件按版本保留`:'尚无已发布固件。';
}
source.addEventListener('change',()=>{if(catalog)render();});
fetch(api+'/releases/index.json').then(r=>{if(!r.ok)throw Error('HTTP '+r.status);return r.json();}).then(data=>{catalog=data;render();}).catch(()=>{status.textContent='暂时无法连接发布服务器，请稍后重试。';});
