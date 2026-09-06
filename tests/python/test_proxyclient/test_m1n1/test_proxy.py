# SPDX-License-Identifier: MIT
"""Host baud transitions must happen before reading the target reply."""

import struct
from types import SimpleNamespace

import pytest

from proxyclient.m1n1.proxy import M1N1Proxy, UartTimeout


@pytest.mark.parametrize("fail", [False, True])
def test_set_baud_runs_callback_before_reply(fail):
    iface = SimpleNamespace(dev=SimpleNamespace(baudrate=115200), tty_enable=True)
    drained = []

    def flush():
        assert iface.dev.baudrate == 115200
        drained.append(True)

    iface.dev.flush = flush

    def proxyreq(req, *, pre_reply, **kwargs):
        opcode, baud, *_ = struct.unpack("<7Q", req)
        assert opcode == M1N1Proxy.P_SET_BAUD
        assert not iface.tty_enable
        assert iface.dev.baudrate == 115200
        pre_reply()
        assert drained == [True]
        assert iface.dev.baudrate == baud == 1500000
        if fail:
            raise UartTimeout("test timeout")
        return struct.pack("<QqQ", opcode, 0, 0)

    iface.proxyreq = proxyreq
    proxy = M1N1Proxy(iface)
    if fail:
        with pytest.raises(UartTimeout):
            proxy.set_baud(1500000)
    else:
        proxy.set_baud(1500000)
    assert iface.tty_enable
