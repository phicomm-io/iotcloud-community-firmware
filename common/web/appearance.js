(()=>{
 'use strict';
 const key='phicomm.appearance',choices=['system','light','dark'];
 let selected='system';try{const saved=localStorage.getItem(key);if(choices.includes(saved))selected=saved;}catch{}
 function apply(value){if(value==='system')document.documentElement.removeAttribute('data-theme');else document.documentElement.dataset.theme=value;}
 apply(selected);
 const row=document.createElement('div'),label=document.createElement('label'),select=document.createElement('select');
 row.className='appearance';label.htmlFor='appearance';label.textContent='外观';select.id='appearance';
 ['跟随系统','浅色','深色'].forEach((text,i)=>{const option=document.createElement('option');option.value=choices[i];option.textContent=text;select.append(option);});
 select.value=selected;select.addEventListener('change',()=>{apply(select.value);try{localStorage.setItem(key,select.value);}catch{}});
 row.append(label,select);document.body.prepend(row);
})();
