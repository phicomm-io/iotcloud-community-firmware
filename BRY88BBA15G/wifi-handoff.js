(() => {
  const setupHost = '192.168.0.1';
  if (location.hostname !== setupHost) return;
  const deviceApi = api;
  const message = (en, zh) => I18N.lang === 'en' ? en : zh;
  const pause = ms => new Promise(resolve => setTimeout(resolve, ms));

  api = async (path, values, options) => {
    const accepted = await deviceApi(path, values, options);
    if (path !== '/wifi/config' || !accepted.wifi_trial_id) return accepted;

    const deadline = Date.now() + 38000;
    while (Date.now() < deadline) {
      const controller = new AbortController();
      const timeout = setTimeout(() => controller.abort(), 1500);
      let status;
      try {
        status = await command('status.get', {}, { signal: controller.signal, priority: 1 });
      } catch (error) {
        if (error.code === 'unauthorized' || error.code === 'identity_changed') throw error;
      } finally {
        clearTimeout(timeout);
      }

      if (status?.wifi_trial_id === accepted.wifi_trial_id) {
        const result = status.wifi_trial_status;
        if (result === 'timeout_rollback' || result === 'storage_failure')
          throw Error(message('Wi-Fi verification failed. The previous settings were kept.',
                              'Wi-Fi 验证失败，已保留原来的配置。'));
        const ip = status.ip;
        if (result === 'success' && status.wifi_connected && typeof ip === 'string' &&
            /^\d{1,3}(\.\d{1,3}){3}$/.test(ip) &&
            ip.split('.').every(part => Number(part) <= 255) && ip !== setupHost && ip !== '0.0.0.0') {
          const destination = 'http://' + ip + '/';
          const notice = document.createElement('p');
          notice.className = 'help';
          notice.setAttribute('role', 'status');
          const link = document.createElement('a');
          link.href = destination;
          link.textContent = ip;
          notice.append(message('Wi-Fi setup complete. Rejoin the same local network to open ',
                                '配网成功。手机切回同一局域网后可打开 '), link);
          document.querySelector('#wifiForm')?.append(notice);
          setTimeout(() => location.assign(destination), 8000);
          return accepted;
        }
      }
      await pause(350);
    }
    throw Error(message('Wi-Fi result unavailable. Check the device connection.',
                        '暂时无法读取配网结果，请检查设备联网状态。'));
  };
})();
