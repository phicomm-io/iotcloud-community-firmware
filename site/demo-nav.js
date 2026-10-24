'use strict';
(() => {
 const home = document.querySelector('[data-demo-home]');
 if (!home) return;
 const label = home.querySelector('[data-demo-home-label]');
 function update() {
  const text = document.documentElement.lang.startsWith('en') ? 'Back to website home' : '返回网站首页';
  if (label.textContent !== text) label.textContent = text;
  // Vue can replace the platform header when navigating between app routes.
  const target = document.querySelector('.console-nav, .topbar .prefs, .auth-topbar');
  if (target && home.parentElement !== target) target.prepend(home);
  else if (!home.isConnected) document.body.prepend(home);
 }
 new MutationObserver(update).observe(document.documentElement, {
  subtree: true, childList: true, attributes: true, attributeFilter: ['lang']
 });
 update();
})();
