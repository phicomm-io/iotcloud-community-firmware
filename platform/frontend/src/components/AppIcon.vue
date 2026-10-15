<script setup>
defineProps({ name: { type: String, default: 'plug' }, size: { type: [Number, String], default: 20 } });
const paths = {
 plug: ['M9 3v4m6-4v4M7 7h10v5a5 5 0 0 1-10 0V7Zm5 10v4'],
 air: ['M4 7h12a3 3 0 1 0-3-3M4 12h15a3 3 0 1 1-3 3M4 17h5a2 2 0 1 1-2 2'],
 fan: ['M12 10c-4-7 1-10 4-7 2 2 0 5-4 7Zm2 2c7-4 10 1 7 4-2 2-5 0-7-4Zm-2 2c4 7-1 10-4 7-2-2 0-5 4-7Zm-2-2C3 16 0 11 3 8c2-2 5 0 7 4Z', 'M14 12a2 2 0 1 1-4 0 2 2 0 0 1 4 0Z'],
 cloud: ['M7 18h11a4 4 0 0 0 .5-8A6.5 6.5 0 0 0 6 8.5 4.8 4.8 0 0 0 7 18Z'],
 arrow: ['M5 12h14m-5-5 5 5-5 5'],
 plus: ['M12 5v14M5 12h14'],
 settings: ['M4 6h16M4 12h16M4 18h16M8 4v4m8 2v4m-5 2v4'],
 shield: ['m12 3 8 3v6c0 5-8 9-8 9s-8-4-8-9V6l8-3Z', 'm8 12 3 3 5-6'],
 guest: ['M16 8a4 4 0 1 1-8 0 4 4 0 0 1 8 0ZM4 21v-2a8 8 0 0 1 16 0v2'],
 eye: ['M2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12Z', 'M15 12a3 3 0 1 1-6 0 3 3 0 0 1 6 0Z'],
 logout: ['M10 4H4v16h6m4-12 4 4-4 4m-6-4h10'],
 moon: ['M20 15.5A8.5 8.5 0 0 1 8.5 4 8.5 8.5 0 1 0 20 15.5Z'],
 sun: ['M16 12a4 4 0 1 1-8 0 4 4 0 0 1 8 0ZM12 2v2m0 16v2M2 12h2m16 0h2M5 5l1.5 1.5m11 11L19 19M5 19l1.5-1.5m11-11L19 5'],
 monitor: ['M3 4h18v13H3V4Zm9 13v4m-4 0h8'],
 edit: ['m15 4 5 5M4 20l4-1L20 7a2 2 0 0 0-3-3L5 16l-1 4Z'],
 close: ['m6 6 12 12M6 18 18 6'],
 check: ['m5 12 4 4L19 6'],
 signal: ['M3 8a14 14 0 0 1 18 0M6 12a9 9 0 0 1 12 0m-9 4a4 4 0 0 1 6 0m-3 4h.01'],
};
</script>
<template><svg :width="size" :height="size" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path v-for="(d,i) in paths[name] || paths.plug" :key="i" :d="d" /></svg></template>
