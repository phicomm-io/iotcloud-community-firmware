'use strict';
(()=>{
 // The navigation wraps on narrow screens; keep anchored headings visible.
 const header=document.querySelector('.site-header');
 if(header){
  const measure=()=>document.documentElement.style.setProperty('--header-offset',`${header.offsetHeight+20}px`);
  measure();
  if('ResizeObserver'in window)new ResizeObserver(measure).observe(header);
  else window.addEventListener('resize',measure);
 }
 // Keep the reader at the same main section when changing home-page language.
 const languageLink=document.querySelector('[data-home-language]');
 if(languageLink){
  const preserveSection=()=>{languageLink.hash=location.hash;};
  preserveSection();window.addEventListener('hashchange',preserveSection);
 }
 const key='phicomm.appearance',choices=['system','light','dark'];
 const pick=document.getElementById('themePick');
 if(pick){
  let saved='system';try{saved=localStorage.getItem(key)||'system';}catch{}
  if(!choices.includes(saved))saved='system';
  pick.value=saved;
  pick.addEventListener('change',()=>{
   const v=pick.value;
   if(v==='system')document.documentElement.removeAttribute('data-theme');
   else document.documentElement.dataset.theme=v;
   try{localStorage.setItem(key,v);}catch{}
  });
 }
 // One choice filters the entire guide. Without JS every instruction is readable.
 const osPicker=document.querySelector('.guide-os-picker');
 if(osPicker){
  const systems=['mac','windows','linux'],osKey='phicomm.guide-os';
  const radios=[...osPicker.querySelectorAll('input[name="guide-os"]')];
  const blocks=[...document.querySelectorAll('[data-guide-os]')];
  const languageLinks=[...document.querySelectorAll('.guide-language a')];
  let saved;try{saved=localStorage.getItem(osKey);}catch{}
  const platform=navigator.userAgentData?.platform||navigator.platform||'';
  const detected=/mac/i.test(platform)?'mac':/linux/i.test(platform)?'linux':'windows';
  const fromUrl=()=>new URL(location.href).searchParams.get('os');
  const show=(os)=>{
   radios.forEach(r=>{r.checked=r.value===os;});
   blocks.forEach(block=>{block.hidden=!block.dataset.guideOs.split(' ').includes(os);});
   languageLinks.forEach(link=>{
    const url=new URL(link.href);url.searchParams.set('os',os);link.href=url.href;
   });
  };
  show(systems.includes(fromUrl())?fromUrl():systems.includes(saved)?saved:detected);
  osPicker.hidden=false;
  osPicker.addEventListener('change',event=>{
   const os=event.target.value;if(!systems.includes(os))return;
   show(os);
   try{localStorage.setItem(osKey,os);}catch{}
   const url=new URL(location.href);url.searchParams.set('os',os);
   history.replaceState(null,'',url);
  });
  window.addEventListener('popstate',()=>{
   const os=fromUrl();if(systems.includes(os))show(os);
  });
 }
 // Highlight the section currently in view. Progressive: without this the nav
 // still works as plain anchors.
 const links=[...document.querySelectorAll('.site-nav a[href^="#"]')];
 if(!links.length||!('IntersectionObserver'in window))return;
 const byId=new Map(links.map(a=>[a.getAttribute('href').slice(1),a]));
 const seen=new Set();
 const io=new IntersectionObserver(entries=>{
  for(const e of entries){if(e.isIntersecting)seen.add(e.target.id);else seen.delete(e.target.id);}
  for(const [id,a] of byId)a.toggleAttribute('aria-current',seen.has(id));
 },{rootMargin:'-88px 0px -65% 0px'});
 for(const id of byId.keys()){const el=document.getElementById(id);if(el)io.observe(el);}
})();
