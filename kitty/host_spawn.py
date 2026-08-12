#!/usr/bin/env python
# License: GPL v3 Copyright: 2026, Kitty Chrome contributors

"""Run terminal children on the host when kitty itself runs inside Flatpak.

Flatpak confines kitty to the runtime, so a shell forked by kitty sees the
runtime's /usr rather than the user's system. This module escapes that
confinement the same way Ptyxis does: a helper process (``kitty-host-agent``)
starts a ``ptyxis-agent`` on the host, asks it to create a PTY there and to
spawn the shell attached to it, and hands the PTY back to kitty over
SCM_RIGHTS.

The descriptor kitty ends up with is a real PTY on the host, so termios, window
resizing and job control all work without any relaying. What is lost is the
ability to inspect the child through /proc, because the sandbox has its own PID
namespace; the agent answers those questions instead (see :meth:`HostAgent.cwd`
and :meth:`HostAgent.foreground`).
"""

import os
import socket
import threading
from collections.abc import Iterable, Mapping, Sequence
from typing import NamedTuple

from .types import run_once

MAX_MESSAGE = 1024 * 1024
AGENT_TIMEOUT = 10.0  # seconds to wait on the host agent before giving up
HOST_CONTAINER = ('session', 'session')

# Set in the environment of children so that nested kitty instances and
# shell integration can tell that they are already outside the sandbox.
HOST_MARKER = 'KITTY_RAN_ON_HOST'

# Variables that describe the sandbox and are actively wrong on the host. The
# host session supplies its own values for all of these.
SANDBOX_ONLY_ENV = frozenset(
    {
        'container',
        'FLATPAK_ID',
        'FLATPAK_SANDBOX_DIR',
        'GDK_BACKEND',
        'GI_TYPELIB_PATH',
        'GIO_EXTRA_MODULES',
        'GST_PLUGIN_SYSTEM_PATH',
        'GST_PLUGIN_SYSTEM_PATH_1_0',
        'LD_LIBRARY_PATH',
        'LD_PRELOAD',
        'PATH',
        'PYTHONHOME',
        'PYTHONPATH',
        'XDG_CONFIG_DIRS',
        'XDG_DATA_DIRS',
    }
)

# Always forwarded even when identical to kitty's own environment, because the
# host session's value is not necessarily the one the user wants in a terminal.
ALWAYS_FORWARD_ENV = frozenset({'COLORTERM', 'PWD', 'TERM', 'TERMINFO'})


class HostSpawnError(Exception):
    pass


class Foreground(NamedTuple):
    has_foreground_process: bool
    pid: int
    cmdline: str
    leader_kind: str


@run_once
def is_flatpak() -> bool:
    return os.path.exists('/.flatpak-info')


@run_once
def flatpak_app_path() -> str:
    """Where /app inside the sandbox lives on the host filesystem."""
    from configparser import ConfigParser

    try:
        parser = ConfigParser(interpolation=None)
        with open('/.flatpak-info', encoding='utf-8') as f:
            parser.read_file(f)
        return parser.get('Instance', 'app-path', fallback='')
    except Exception:
        return ''


def translate_path(value: str, app_path: str | None = None) -> str:
    """Rewrite /app paths so that host processes can reach them.

    The Flatpak app's files are ordinary files on the host, they are simply
    mounted at /app inside the sandbox. Values may be search paths, so every
    colon separated component is translated individually.
    """
    if app_path is None:
        app_path = flatpak_app_path()
    if not app_path:
        return value

    def one(component: str) -> str:
        if component == '/app':
            return app_path
        if component.startswith('/app/'):
            return app_path + component[4:]
        return component

    if ':' in value:
        return ':'.join(one(x) for x in value.split(':'))
    return one(value)


def host_env(env: Mapping[str, str], base: Mapping[str, str], app_path: str | None = None) -> dict[str, str]:
    """Reduce a child environment to the parts that should override the host session.

    ``Container.Spawn`` starts from the host session's own environment, so
    anything kitty did not deliberately change is better left alone. Passing
    the sandbox's PATH or XDG_DATA_DIRS through would hide the user's actual
    system from the shell, which is the whole problem we are solving.
    """
    ans: dict[str, str] = {}
    for key, value in env.items():
        if key in SANDBOX_ONLY_ENV:
            continue
        if key not in ALWAYS_FORWARD_ENV and base.get(key) == value:
            continue
        ans[key] = translate_path(value, app_path)
    ans[HOST_MARKER] = '1'
    return ans


@run_once
def agent_exe() -> str:
    ans = os.environ.get('KITTY_HOST_AGENT')
    if ans:
        return ans
    return '/app/libexec/kitty-host-agent'


def _encode(fields: Iterable[str]) -> bytes:
    return b''.join(x.encode('utf-8', 'surrogateescape') + b'\0' for x in fields)


def _decode(data: bytes) -> list[str]:
    parts = data.split(b'\0')
    if parts and not parts[-1]:
        parts.pop()
    return [x.decode('utf-8', 'surrogateescape') for x in parts]


class HostAgent:
    """Synchronous client for the kitty-host-agent helper."""

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.sock: socket.socket | None = None
        self.pid = -1

    def start(self) -> None:
        if self.sock is not None:
            return
        exe = agent_exe()
        ours, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        try:
            self.pid = os.posix_spawn(
                exe,
                [exe, '--socket-fd=3'],
                os.environ,
                file_actions=((os.POSIX_SPAWN_DUP2, theirs.fileno(), 3),),
            )
        except OSError as err:
            ours.close()
            theirs.close()
            raise HostSpawnError(f'Could not start {exe}: {err}') from err
        finally:
            theirs.close()
        # The agent services requests serially and makes synchronous D-Bus calls
        # to the host, and this socket is read on the main thread. Without a
        # deadline an agent that is alive but wedged freezes the UI for good,
        # with no way back; agent death is already handled, this covers the
        # rest.
        ours.settimeout(AGENT_TIMEOUT)
        self.sock = ours

    def close(self) -> None:
        with self.lock:
            if self.sock is not None:
                self.sock.close()
                self.sock = None

    def _call(self, fields: Sequence[str], fds: Sequence[int] = ()) -> tuple[list[str], list[int]]:
        with self.lock:
            if self.sock is None:
                raise HostSpawnError('Host agent is not running')
            try:
                socket.send_fds(self.sock, [_encode(fields)], list(fds))
                data, received, _flags, _addr = socket.recv_fds(self.sock, MAX_MESSAGE, 1)
            except OSError as err:
                raise HostSpawnError(f'Host agent communication failed: {err}') from err
        for fd in received:
            # recv_fds does not set close-on-exec. A PTY leaking into another
            # child would keep the host process alive after we hang it up.
            os.set_inheritable(fd, False)
        if not data:
            for fd in received:
                os.close(fd)
            raise HostSpawnError('Host agent exited')
        reply = _decode(data)
        if not reply or reply[0] != 'ok':
            for fd in received:
                os.close(fd)
            raise HostSpawnError(reply[1] if len(reply) > 1 else 'Host agent request failed')
        return reply[1:], received

    def _call_simple(self, fields: Sequence[str], fds: Sequence[int] = ()) -> list[str]:
        reply, received = self._call(fields, fds)
        for fd in received:
            os.close(fd)
        return reply

    def spawn(
        self,
        argv: Sequence[str],
        cwd: str,
        env: Mapping[str, str],
        exit_fd: int,
        stdin_fd: int = -1,
        container: tuple[str, str] = HOST_CONTAINER,
    ) -> tuple[int, int]:
        """Start argv on the host. Returns (token, pty_fd)."""
        fields = [
            'spawn',
            container[0],
            container[1],
            cwd,
            '1' if stdin_fd > -1 else '0',
            str(len(argv)),
            *argv,
            str(len(env)),
            *(f'{k}={v}' for k, v in env.items()),
        ]
        fds = [exit_fd] if stdin_fd < 0 else [exit_fd, stdin_fd]
        reply, received = self._call(fields, fds)
        if not received:
            raise HostSpawnError('Host agent did not return a PTY')
        try:
            token = int(reply[0])
        except (IndexError, ValueError) as err:
            for fd in received:
                os.close(fd)
            raise HostSpawnError('Host agent returned a malformed token') from err
        for extra in received[1:]:
            os.close(extra)
        return token, received[0]

    def signal(self, token: int, signum: int) -> None:
        self._call_simple(('signal', str(token), str(signum)))

    def cwd(self, token: int, pty_fd: int) -> str:
        return self._call_simple(('cwd', str(token)), (pty_fd,))[0]

    def foreground(self, token: int, pty_fd: int) -> Foreground:
        reply = self._call_simple(('foreground', str(token)), (pty_fd,))
        return Foreground(reply[0] == '1', int(reply[1]), reply[2], reply[3])

    def preferred_shell(self) -> str:
        return self._call_simple(('shell',))[0]

    def containers(self) -> list[tuple[str, str, str]]:
        reply = self._call_simple(('containers',))
        count = int(reply[0])
        rest = reply[1:]
        return [(rest[3 * i], rest[3 * i + 1], rest[3 * i + 2]) for i in range(count)]

    def release(self, token: int) -> None:
        self._call_simple(('release', str(token)))


_agent: HostAgent | None = None
_agent_failed = False


def host_agent() -> HostAgent | None:
    """The process wide agent, or None if the host cannot be reached."""
    global _agent, _agent_failed
    if _agent_failed:
        return None
    if _agent is None:
        agent = HostAgent()
        try:
            agent.start()
        except HostSpawnError as err:
            from .utils import log_error

            log_error(f'Falling back to sandboxed children: {err}')
            _agent_failed = True
            return None
        _agent = agent
    return _agent


def is_sandbox_only(exe: str) -> bool:
    """Whether exe is part of kitty itself and must not leave the sandbox.

    Kittens rendered as windows (``kitten ask``, ``kitten hints``, ...) reach
    back into this kitty instance through its runtime directory, and Flatpak
    gives the sandbox a different XDG_RUNTIME_DIR than the host has. Running
    them outside would point them at the wrong one.
    """
    from .constants import kitten_exe, kitty_exe

    try:
        return exe in (kitten_exe(), kitty_exe())
    except Exception:
        return False


def should_run_on_host(argv: Sequence[str], pass_fds: Sequence[int], remote_control_fd: int) -> bool:
    """Whether a child should be started on the host rather than in the sandbox.

    ``argv`` is the command as requested, before any wrapping kitty does for
    shell integration or ``--hold``; those wrappers are static binaries that run
    happily on the host, so they should not drag the user's command back inside.

    Descriptors other than stdio cannot be handed to a host process without
    renumbering them, so children that rely on inherited descriptors (remote
    control, ``launch --pass-fds``) stay in the sandbox where those descriptors
    mean something.
    """
    if not is_flatpak() or not argv:
        return False
    if os.environ.get('KITTY_FLATPAK_HOST_CHILDREN') == '0':
        return False
    if pass_fds or remote_control_fd > -1:
        return False
    return not is_sandbox_only(argv[0])
