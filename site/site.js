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
