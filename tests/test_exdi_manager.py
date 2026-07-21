from __future__ import annotations

import unittest
from unittest.mock import Mock, patch

import exdi_manager


def make_config(auto_start: bool = True) -> exdi_manager.ManagerConfig:
    target = exdi_manager.Target(
        vm_name="test-vm",
        vmx=r"D:\VMs\test-vm.vmx",
        snapshot_name="memory",
        snapshot_uid="7",
        includes_memory=True,
    )
    return exdi_manager.ManagerConfig(target, auto_start)


class WatcherVmxGenerationTests(unittest.TestCase):
    def make_active_watcher(self, auto_start: bool = True) -> tuple[exdi_manager.Watcher, list[str]]:
        notifications: list[str] = []
        watcher = exdi_manager.Watcher(make_config(auto_start), notifications.append)
        watcher.target_matches = True
        watcher.session_phase = "active"
        watcher._automatic_start_consumed = True
        watcher._session_observed_active = True
        watcher._vmx_pid = 100
        return watcher, notifications

    def test_same_snapshot_restore_discards_old_session_and_relaunches(self) -> None:
        watcher, notifications = self.make_active_watcher()
        query_status = Mock(side_effect=AssertionError("失效会话不应再查询 windbgskill"))

        with (
            patch.object(exdi_manager, "gdb_server_pid", return_value=200),
            patch.object(
                exdi_manager,
                "target_vmware_state",
                return_value=(True, True, "目标快照正在运行"),
            ),
            patch.object(exdi_manager, "query_skill_status", query_status),
            patch.object(exdi_manager, "discard_stale_session", return_value="已清理失效会话。") as discard,
            patch.object(exdi_manager, "launch_windbg", return_value=300) as launch,
        ):
            watcher.poll_once()

        discard.assert_called_once_with()
        launch.assert_called_once_with(watcher.config.target)
        self.assertEqual(watcher._vmx_pid, 200)
        self.assertEqual(watcher.session_phase, "starting")
        self.assertIn("PID 100 -> 200", notifications[0])
        self.assertEqual(notifications[1], "已清理失效会话。")
        self.assertIn("已自动启动 WinDbgX", notifications[2])

    def test_restore_with_auto_start_disabled_only_discards_old_session(self) -> None:
        watcher, notifications = self.make_active_watcher(auto_start=False)

        with (
            patch.object(exdi_manager, "gdb_server_pid", return_value=200),
            patch.object(
                exdi_manager,
                "target_vmware_state",
                return_value=(True, True, "目标快照正在运行"),
            ),
            patch.object(exdi_manager, "query_skill_status") as query_status,
            patch.object(exdi_manager, "discard_stale_session", return_value="已清理失效会话。"),
            patch.object(exdi_manager, "launch_windbg") as launch,
        ):
            watcher.poll_once()

        query_status.assert_not_called()
        launch.assert_not_called()
        self.assertEqual(watcher.session_phase, "idle")
        self.assertEqual(notifications[-1], "已清理失效会话。")

    def test_new_vmx_is_detected_after_an_observed_power_off(self) -> None:
        watcher, notifications = self.make_active_watcher()
        watcher.target_matches = False
        watcher.session_phase = "error"

        with (
            patch.object(exdi_manager, "gdb_server_pid", return_value=200),
            patch.object(
                exdi_manager,
                "target_vmware_state",
                return_value=(True, True, "目标快照正在运行"),
            ),
            patch.object(exdi_manager, "query_skill_status") as query_status,
            patch.object(exdi_manager, "discard_stale_session", return_value="已清理失效会话。") as discard,
            patch.object(exdi_manager, "launch_windbg", return_value=300) as launch,
        ):
            watcher.poll_once()

        query_status.assert_not_called()
        discard.assert_called_once_with()
        launch.assert_called_once_with(watcher.config.target)
        self.assertEqual(watcher.session_phase, "starting")
        self.assertIn("PID 100 -> 200", notifications[0])

    def test_unchanged_vmx_generation_keeps_active_session(self) -> None:
        watcher, notifications = self.make_active_watcher()

        with (
            patch.object(exdi_manager, "gdb_server_pid", return_value=100),
            patch.object(
                exdi_manager,
                "target_vmware_state",
                return_value=(True, True, "目标快照正在运行"),
            ),
            patch.object(exdi_manager, "query_skill_status", return_value={"state": "running"}),
            patch.object(exdi_manager, "discard_stale_session") as discard,
            patch.object(exdi_manager, "launch_windbg") as launch,
        ):
            watcher.poll_once()

        discard.assert_not_called()
        launch.assert_not_called()
        self.assertEqual(watcher.session_phase, "active")
        self.assertEqual(notifications, [])

    def test_missing_gdb_listener_waits_for_new_vmx_generation(self) -> None:
        watcher, notifications = self.make_active_watcher()

        with (
            patch.object(exdi_manager, "gdb_server_pid", return_value=None),
            patch.object(
                exdi_manager,
                "target_vmware_state",
                return_value=(True, True, "目标快照正在运行"),
            ),
            patch.object(exdi_manager, "query_skill_status") as query_status,
            patch.object(exdi_manager, "discard_stale_session") as discard,
            patch.object(exdi_manager, "launch_windbg") as launch,
        ):
            watcher.poll_once()
            watcher.poll_once()

        query_status.assert_not_called()
        discard.assert_not_called()
        launch.assert_not_called()
        self.assertEqual(watcher.session_phase, "checking")
        self.assertEqual(len(notifications), 1)
        self.assertIn("GDB 监听暂时消失", notifications[0])


class WatcherManualStartTests(unittest.TestCase):
    def test_manual_start_is_only_logged_by_the_gui_task(self) -> None:
        notifications: list[str] = []
        watcher = exdi_manager.Watcher(make_config(), notifications.append)

        with (
            patch.object(
                exdi_manager,
                "target_vmware_state",
                return_value=(True, True, "目标快照正在运行"),
            ),
            patch.object(exdi_manager, "query_skill_status", return_value=None),
            patch.object(exdi_manager, "surrogate_pids", return_value=[]),
            patch.object(exdi_manager, "managed_windbg_pids", return_value=[]),
            patch.object(exdi_manager, "launch_windbg", return_value=300),
        ):
            detail = watcher.start_session()

        self.assertIn("已手动启动", detail)
        self.assertEqual(notifications, [])


if __name__ == "__main__":
    unittest.main()
