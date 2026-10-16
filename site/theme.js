/* Apply the saved appearance before first paint so the page never flashes the
 * wrong theme. The storage key is shared with the device WebUI, so a choice
 * made on a device page carries over to the site. */
(()=>{
'use strict';
 const key='phicomm.appearance',choices=['system','light','dark'];
 let selected='system';try{const saved=localStorage.getItem(key);if(choices.includes(saved))selected=saved;}catch{}
 if(selected==='system')document.documentElement.removeAttribute('data-theme');
 else document.documentElement.dataset.theme=selected;
})();
