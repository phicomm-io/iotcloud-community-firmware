import { createRouter, createWebHistory } from 'vue-router'

const routes = [
  { path: "/login", component: () => import("./views/LoginPage.vue"), meta: { requiresApiPing: true } },
  {
    path: '/',
    name: 'Console',
    component: () => import('./App.vue')
  },
  {
    path: '/register',
    name: 'Register',
    component: () => import('./views/RegisterPage.vue'),
    meta: { requiresApiPing: true }
  },
  {
    path: '/admin',
    name: 'Admin',
    component: () => import('./views/AdminPage.vue'),
    meta: { requiresApiPing: true }
  }
]

const router = createRouter({
  history: createWebHistory(),
  routes
})

const buildReturnTarget = (to) => {
  const target = to.fullPath || '/'
  return target.startsWith('/') ? target : '/'
}

router.beforeEach(async (to) => {
  if (!to.matched.some(record => record.meta.requiresApiPing)) {
    return true
  }

  try {
    const res = await fetch('/api/ping', {
      method: 'GET',
      cache: 'no-store',
      credentials: 'same-origin'
    })
    if (res.ok) {
      return true
    }
  } catch (error) {
    console.warn('API ping failed, fallback to browser auth flow', error)
  }

  const returnTo = encodeURIComponent(buildReturnTarget(to))
  window.location.assign(`/api/ping?return_to=${returnTo}`)
  return false
})

export default router
