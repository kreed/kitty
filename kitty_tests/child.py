#!/usr/bin/env python
# License: GPL v3 Copyright: 2026, Kovid Goyal <kovid at kovidgoyal.net>

import os
import subprocess

from kitty.child import memory_used_by_process_tree_rooted_at
from kitty.constants import is_macos, kitten_exe, kitty_exe
from kitty.host_spawn import HOST_MARKER, _decode, _encode, host_env, is_sandbox_only, should_run_on_host, translate_path

from .base import BaseTest

APP_PATH = '/var/home/u/.local/share/flatpak/app/io.github.kreed.KittyChrome/x86_64/master/deadbeef/files'


class HostSpawnTest(BaseTest):
    def test_translate_path_rewrites_app_paths(self):
        self.ae(translate_path('/app/bin/kitten', APP_PATH), APP_PATH + '/bin/kitten')
        self.ae(translate_path('/app', APP_PATH), APP_PATH)

    def test_translate_path_leaves_other_paths_alone(self):
        # /apple must not be mistaken for a prefix of /app
        self.ae(translate_path('/apple/pie', APP_PATH), '/apple/pie')
        self.ae(translate_path('/usr/bin/sh', APP_PATH), '/usr/bin/sh')

    def test_translate_path_handles_search_paths(self):
        self.ae(
            translate_path('/x:/app/lib/kitty/terminfo:/y', APP_PATH),
            f'/x:{APP_PATH}/lib/kitty/terminfo:/y',
        )

    def test_translate_path_without_app_path_is_identity(self):
        self.ae(translate_path('/app/bin/kitten', ''), '/app/bin/kitten')

    def test_host_env_drops_sandbox_environment(self):
        base = {'PATH': '/app/bin:/usr/bin', 'HOME': '/home/u', 'LANG': 'en_US.UTF-8'}
        env = dict(base, LD_LIBRARY_PATH='/app/lib', XDG_DATA_DIRS='/app/share')
        ans = host_env(env, base, APP_PATH)
        # Sandbox paths would hide the user's actual system from the shell
        for key in ('PATH', 'LD_LIBRARY_PATH', 'XDG_DATA_DIRS'):
            self.assertNotIn(key, ans)
        # Unchanged values are left to the host session
        for key in ('HOME', 'LANG'):
            self.assertNotIn(key, ans)

    def test_host_env_forwards_kitty_settings(self):
        base = {'TERM': 'xterm', 'PATH': '/app/bin'}
        env = {'TERM': 'xterm-kitty', 'TERMINFO': '/app/lib/kitty/terminfo', 'KITTY_PID': '7', 'PATH': '/app/bin'}
        ans = host_env(env, base, APP_PATH)
        self.ae(ans['TERM'], 'xterm-kitty')
        self.ae(ans['TERMINFO'], APP_PATH + '/lib/kitty/terminfo')
        self.ae(ans['KITTY_PID'], '7')
        self.ae(ans[HOST_MARKER], '1')

    def test_host_env_always_forwards_terminal_settings(self):
        # Identical to the sandbox value, but the host session's own TERM is
        # not what a kitty window should use.
        base = {'TERM': 'xterm-kitty', 'COLORTERM': 'truecolor'}
        ans = host_env(dict(base), base, APP_PATH)
        self.ae(ans['TERM'], 'xterm-kitty')
        self.ae(ans['COLORTERM'], 'truecolor')

    def test_message_codec_roundtrip(self):
        for fields in (['spawn'], ['spawn', 'session', ''], ['a b', 'c\td', '']):
            self.ae(_decode(_encode(fields)), fields)

    def test_kittens_are_recognised_as_sandbox_only(self):
        # Kittens rendered as windows talk to this kitty through its runtime
        # directory, which differs inside and outside the sandbox.
        self.assertTrue(is_sandbox_only(kitten_exe()))
        self.assertTrue(is_sandbox_only(kitty_exe()))
        self.assertFalse(is_sandbox_only('/bin/bash'))

    def test_children_needing_descriptors_stay_in_the_sandbox(self):
        # Descriptors cannot be handed to a host process without renumbering
        if not os.path.exists('/.flatpak-info'):
            self.assertFalse(should_run_on_host(['sh'], (), -1))
            self.skipTest('not running inside Flatpak')
        self.assertFalse(should_run_on_host(['sh'], (5,), -1))
        self.assertFalse(should_run_on_host(['sh'], (), 5))
        self.assertFalse(should_run_on_host([], (), -1))
        self.assertFalse(should_run_on_host([kitten_exe(), 'ask'], (), -1))
        self.assertTrue(should_run_on_host(['sh'], (), -1))


class ChildMemoryTest(BaseTest):
    def _spawn_allocating_child(self, alloc_bytes: int) -> subprocess.Popen:
        p = subprocess.Popen(
            [
                kitty_exe(),
                '+runpy',
                f"""\
import sys, time
buf = bytearray({alloc_bytes})
for i in range(0, {alloc_bytes}, 4096):
    buf[i] = 1
sys.stdout.write("ready\\n")
sys.stdout.flush()
time.sleep(300)
""",
            ],
            stdout=subprocess.PIPE,
        )
        line = p.stdout.readline().strip()
        p.stdout.close()
        if line != b'ready':
            p.kill()
            p.wait()
            raise AssertionError(f'Unexpected output from allocating child: {line!r}')
        return p

    def _terminate(self, p: subprocess.Popen) -> None:
        p.terminate()
        p.wait()

    def test_memory_returns_positive_for_live_process(self):
        mem = memory_used_by_process_tree_rooted_at(os.getpid())
        self.assertGreater(mem, 0)

    def test_memory_returns_minus_one_for_nonexistent_pid(self):
        self.ae(memory_used_by_process_tree_rooted_at(99999999), -1)

    def test_memory_accounts_for_child_allocation(self):
        # Verify that a child's known resident allocation shows up in the
        # measurement.  check_if_cgroup_root=True falls back to per-process
        # tree walk when pid is not the cgroup root (the common case when
        # running under a shared session cgroup), so this exercises the tree
        # walk path on Linux and the always-tree-walk path on macOS.
        alloc = 20 * 1024 * 1024  # 20 MiB
        child = self._spawn_allocating_child(alloc)
        try:
            mem = memory_used_by_process_tree_rooted_at(child.pid, check_if_cgroup_root=True)
            self.assertGreater(
                mem,
                alloc // 2,
                f'Expected at least {alloc // 2} bytes for a {alloc}-byte allocation, got {mem}',
            )
        finally:
            self._terminate(child)

    def test_memory_of_parent_tree_includes_child(self):
        # Measuring the current process tree must yield more than measuring
        # the child alone, because the test runner itself occupies memory.
        alloc = 20 * 1024 * 1024  # 20 MiB
        child = self._spawn_allocating_child(alloc)
        try:
            mem_child = memory_used_by_process_tree_rooted_at(child.pid, check_if_cgroup_root=True)
            mem_tree = memory_used_by_process_tree_rooted_at(os.getpid(), check_if_cgroup_root=True)
            self.assertGreater(
                mem_tree,
                mem_child,
                'Parent tree memory should exceed child-only memory',
            )
        finally:
            self._terminate(child)

    def test_memory_cgroup_path_returns_positive(self):
        # The fast cgroup path (check_if_cgroup_root=False, the default) must
        # return a usable value on Linux.
        if is_macos:
            self.skipTest('cgroup not available on macOS')
        mem = memory_used_by_process_tree_rooted_at(os.getpid())
        self.assertGreater(mem, 0)

    def test_memory_cgroup_and_tree_walk_both_positive(self):
        # Both the cgroup path and the tree-walk path should give positive
        # results for a process that is alive.
        if is_macos:
            self.skipTest('cgroup path not applicable on macOS')
        alloc = 10 * 1024 * 1024  # 10 MiB
        child = self._spawn_allocating_child(alloc)
        try:
            mem_cgroup = memory_used_by_process_tree_rooted_at(child.pid, check_if_cgroup_root=False)
            mem_walk = memory_used_by_process_tree_rooted_at(child.pid, check_if_cgroup_root=True)
            self.assertGreater(mem_cgroup, 0)
            self.assertGreater(mem_walk, 0)
        finally:
            self._terminate(child)
