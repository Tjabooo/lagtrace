"""A virtual mouse (or gamepad) through /dev/uinput, standard library only."""

from __future__ import annotations

import fcntl
import os
import struct
import time

EV_SYN, EV_KEY, EV_REL = 0x00, 0x01, 0x02
SYN_REPORT = 0
BTN_LEFT, BTN_RIGHT, BTN_MIDDLE = 0x110, 0x111, 0x112
BTN_SOUTH, BTN_EAST = 0x130, 0x131
REL_X, REL_Y, REL_WHEEL = 0x00, 0x01, 0x08
BUS_VIRTUAL = 0x06


def _ioc(direction: int, nr: int, size: int) -> int:
    return (direction << 30) | (size << 16) | (ord("U") << 8) | nr


UI_DEV_CREATE = _ioc(0, 1, 0)
UI_DEV_DESTROY = _ioc(0, 2, 0)
UI_DEV_SETUP = _ioc(1, 3, 92)       # struct uinput_setup
UI_SET_EVBIT = _ioc(1, 100, 4)
UI_SET_KEYBIT = _ioc(1, 101, 4)
UI_SET_RELBIT = _ioc(1, 102, 4)

_EVENT = struct.Struct("llHHi")    # struct input_event on 64-bit Linux


def now_ns() -> int:
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)


class VirtualDevice:
    """kind="mouse": a normal pointer the compositor routes to the focused window.
    kind="pad": only gamepad buttons; libinput ignores joysticks, so the desktop never
    sees it (used by `lagtrace bench --direct`, where the bench reads it itself)."""

    def __init__(self, name: str, kind: str = "mouse"):
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK | os.O_CLOEXEC)
        self.button = BTN_LEFT if kind == "mouse" else BTN_SOUTH
        self.down = False
        try:
            fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
            if kind == "mouse":
                for b in (BTN_LEFT, BTN_RIGHT, BTN_MIDDLE):
                    fcntl.ioctl(self.fd, UI_SET_KEYBIT, b)
                fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_REL)
                for r in (REL_X, REL_Y, REL_WHEEL):
                    fcntl.ioctl(self.fd, UI_SET_RELBIT, r)
            else:
                for b in (BTN_SOUTH, BTN_EAST):
                    fcntl.ioctl(self.fd, UI_SET_KEYBIT, b)
            setup = struct.pack("HHHH80sI", BUS_VIRTUAL, 0, 0, 1, name.encode()[:79], 0)
            fcntl.ioctl(self.fd, UI_DEV_SETUP, setup)
            fcntl.ioctl(self.fd, UI_DEV_CREATE)
        except OSError:
            os.close(self.fd)
            raise

    def _button(self, value: int) -> int:
        data = _EVENT.pack(0, 0, EV_KEY, self.button, value) + _EVENT.pack(0, 0, EV_SYN, SYN_REPORT, 0)
        t = now_ns()
        os.write(self.fd, data)
        return t

    def press(self) -> int:
        self.down = True
        return self._button(1)

    def release(self) -> int:
        self.down = False
        return self._button(0)

    def close(self) -> None:
        if self.fd < 0:
            return
        try:
            if self.down:
                self.release()
            fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        finally:
            os.close(self.fd)
            self.fd = -1

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
