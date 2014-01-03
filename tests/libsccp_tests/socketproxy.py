# -*- coding: utf-8 -*-

# Copyright (C) 2013-2014 Avencall
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>

import collections
import errno
import logging
import select
import socket

from common import TimeoutException

logger = logging.getLogger(__name__)


class SocketProxyFactory(object):

    def __init__(self, poll):
        self._poll = poll
        self._fd_map = {}

    def new_socket_proxy(self, family=socket.AF_INET, type=socket.SOCK_STREAM):
        if type == socket.SOCK_STREAM:
            sock_proxy = _StreamSocketProxy(self)
        elif type == socket.SOCK_DGRAM:
            sock_proxy = _DatagramSocketProxy(self)
        else:
            raise Exception('unsupported socket type')
        sock = socket.socket(family, type)
        sock.setblocking(0)
        sock_proxy._sock = sock
        sock_proxy._poll = self._poll
        self._fd_map[sock.fileno()] = sock_proxy
        return sock_proxy

    def poll(self, timeout=None):
        res = self._poll.poll(timeout)
        if not res:
            raise TimeoutException()
        for fd, poll_ev in res:
            sock_proxy = self._fd_map[fd]
            sock_proxy._on_poll_event(poll_ev)

    def _socket_proxy_close(self, sock_proxy):
        del self._fd_map[sock_proxy._sock.fileno()]


class _BaseSocketProxy(object):

    def __init__(self, sock_proxy_factory):
        self._sock_proxy_factory = sock_proxy_factory
        self._sock = None
        self._poll = None
        self._closed = False

    @property
    def closed(self):
        return self._closed

    def getpeername(self):
        return self._sock.getpeername()

    def getsockname(self):
        return self._sock.getsockname()

    @staticmethod
    def _call_callback(cb, *args):
        if cb is not None:
            cb(*args)


class _StreamSocketProxy(_BaseSocketProxy):

    def __init__(self, socket_proxy_factory):
        _BaseSocketProxy.__init__(self, socket_proxy_factory)
        self._connecting = False
        self._connected = False
        self._event_mask = 0
        self._send_buf = ''

        self.conn_established_cb = None
        self.conn_closed_cb = None
        self.data_received_cb = None

    def close(self):
        if self._closed:
            return

        # order is important
        if self._connected or self._connecting:
            self._poll.unregister(self._sock)
        self._sock_proxy_factory._socket_proxy_close(self)
        self._sock.close()

        self._connected = False
        self._connecting = False
        self._closed = True

    def connect(self, address):
        if self._connected:
            raise Exception('cannot connect: already connected')
        if self._connecting:
            raise Exception('cannot connect: connecting')
        if self._closed:
            raise Exception('cannot connect: closed')

        self._connecting = True
        try:
            self._sock.connect(address)
        except socket.error as e:
            if e.errno == errno.EINPROGRESS:
                logger.info('connection in progress')
                self._event_mask = select.POLLOUT
                self._poll.register(self._sock, self._event_mask)
            else:
                self._connecting = False
                raise
        else:
            self._connected = True
            self._event_mask = select.POLLIN
            self._poll.register(self._sock, self._event_mask)

    def send(self, data):
        if not self._connected and not self._connecting:
            raise Exception('cannot send: not connected')

        self._send_buf += data
        if not self._event_mask & select.POLLOUT:
            self._event_mask |= select.POLLOUT
            self._poll.modify(self._sock, self._event_mask)

    def _on_poll_event(self, poll_ev):
        # case where connect didn't raise EINPROGRESS
        if self._connecting and self._connected:
            self._connecting = False
            self._call_callback(self.conn_established_cb)

        if poll_ev & select.POLLIN:
            data = self._sock.recv(2048)
            if not data:
                # remote connection closed
                self.close()
                self._call_callback(self.conn_closed_cb)
                return
            self._call_callback(self.data_received_cb, data)

        if poll_ev & select.POLLHUP:
            self.close()
            self._call_callback(self.conn_closed_cb)
            return

        if poll_ev & select.POLLERR:
            self.close()
            self._call_callback(self.conn_closed_cb)
            return

        if poll_ev & select.POLLOUT:
            if self._connecting:
                error = self._sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
                if error:
                    # connection failed
                    self.close()
                    self._call_callback(self.conn_closed_cb)
                    return
                self._connecting = False
                self._connected = True
                self._call_callback(self.conn_established_cb)
            if self._send_buf:
                n = self._sock.send(self._send_buf)
                self._send_buf = self._send_buf[n:]

        if self._send_buf:
            self._event_mask = select.POLLIN | select.POLLOUT
        else:
            self._event_mask = select.POLLIN
        self._poll.modify(self._sock, self._event_mask)


class _DatagramSocketProxy(_BaseSocketProxy):

    def __init__(self, socket_proxy_factory):
        _BaseSocketProxy.__init__(self, socket_proxy_factory)
        self._bound = False
        self._event_mask = 0
        self._send_buf = collections.deque()

        self.data_received_cb = None

    def close(self):
        if self._closed:
            return

        # order is important
        if self._bound:
            # FIXME incorrect, bound might not be set but we have registered the
            #       socket to the poll
            self._poll.unregister(self._sock)
        self._sock_proxy_factory._socket_proxy_close(self)
        self._sock.close()

        self._closed = True

    def bind(self, address):
        if self._bound:
            raise Exception('cannot bind: already bound')
        if self._closed:
            raise Exception('cannot bind: closed')

        self._sock.bind(address)
        self._bound = True
        self._poll.register(self._sock, select.POLLIN)

    def sendto(self, data, address):
        self._send_buf.append((data, address))
        if not self._event_mask & select.POLLOUT:
            self._event_mask |= select.POLLOUT
            self._poll.register(self._sock, self._event_mask)

    def _on_poll_event(self, poll_ev):
        if poll_ev & select.POLLIN:
            data, address = self._sock.recvfrom(2048)
            self._call_callback(self.data_received_cb, data, address)

        if poll_ev & select.POLLHUP:
            self.close()
            return

        if poll_ev & select.POLLERR:
            self.close()
            return

        if poll_ev & select.POLLOUT:
            while self._send_buf:
                data, address = self._send_buf.popleft()
                self._sock.sendto(data, address)
                self._bound = True

        if self._send_buf:
            self._event_mask = select.POLLIN | select.POLLOUT
        else:
            self._event_mask = select.POLLIN
        self._poll.register(self._sock, self._event_mask)
