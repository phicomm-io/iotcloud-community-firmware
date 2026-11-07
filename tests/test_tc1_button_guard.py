"""TC1: the SDK button path must not reach the parameter partitions."""
import pathlib, unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class TC1ButtonGuard(unittest.TestCase):
    def test_sdk_button_callback_cannot_erase_the_parameter_partitions(self):
        guard = (ROOT / 'TC1/easylink-button-guard.patch').read_text()
        # Both SDK button callbacks must leave before the SDK reaches Flash, the
        # power daemon or the EasyLink autoconfig path.
        self.assertIn('PlatformEasyLinkButtonLongPressedCallback', guard)
        self.assertIn('PlatformEasyLinkButtonClickedCallback', guard)
        self.assertIn('app_easylink_button_event(1)', guard)
        self.assertIn('app_easylink_button_event(0)', guard)
        # The pin interrupt must not re-arm edges or start timers: those are
        # kernel calls in interrupt context and end in the erasing callback.
        self.assertIn('button_falling_irq_handler', guard)
        self.assertIn('app_easylink_button_event(unsigned long_press)', guard)
        added = [line for line in guard.splitlines() if line.startswith('+')]
        for line in added:
            for forbidden in ('MicoFlashErase', 'mico_system_power_perform',
                              'mico_system_wlan_start_autoconf', 'mico_rtos_start_timer',
                              'MicoGpioEnableIRQ'):
                self.assertNotIn(forbidden, line)

    def test_guard_and_monitor_traces_are_applied_by_the_tc1_build(self):
        build = (ROOT / 'TC1/build.sh').read_text()
        for patch in ('easylink-button-guard.patch', 'system-monitor-trace.patch'):
            self.assertIn(patch, build)
            self.assertTrue((ROOT / 'TC1' / patch).is_file())
        monitor = (ROOT / 'TC1/system-monitor-trace.patch').read_text()
        self.assertIn('SYS MONITOR: point %d stalled', monitor)
        self.assertIn('MicoSystemReboot', monitor)


if __name__ == '__main__':
    unittest.main()
