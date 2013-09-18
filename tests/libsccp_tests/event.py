# -*- coding: utf-8 -*-

# Copyright (C) 2013 Avencall
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
import logging

from common import UnexpectedEventException

CONNECTION_SUCCESS = 'CONNECTION_SUCCESS'
CONNECTION_FAILURE = 'CONNECTION_FAILURE'
CONNECTION_CLOSED = 'CONNECTION_CLOSED'
REGISTRATION_SUCCESS = 'REGISTRATION_SUCCESS'
REGISTRATION_FAILURE = 'REGISTRATION_FAILURE'
CALL_INCOMING = 'CALL_INCOMING'
CALL_OUTGOING = 'CALL_OUTGOING'
CALL_CONNECTED = 'CALL_CONNECTED'
CALL_HANGUP = 'CALL_HANGUP'
SCCP_MSG_RECEIVED = 'SCCP_MSG_RECEIVED'
RTP_PACKET_RECEIVED = 'RTP_PACKET_RECEIVED'
ERROR = 'ERROR'

logger = logging.getLogger(__name__)


class Event(object):

    def __init__(self, name, device, data=None):
        self.name = name
        self.device = device
        self.data = data


class NullEventHandler(object):

    def __call__(self, ev):
        pass


class LogEventHandler(object):

    def __call__(self, ev):
        logger.info('on event: %s %s %s', ev.name, id(ev.device), ev.data)


class CompositeEventHandler(object):

    def __init__(self):
        self._ev_handlers = []

    def append(self, ev_handler):
        self._ev_handlers.append(ev_handler)

    def extend(self, ev_handlers):
        self._ev_handlers.extend(ev_handlers)

    def remove(self, ev_handler):
        self._ev_handlers.remove(ev_handler)

    def __call__(self, ev):
        for ev_handler in self._ev_handlers:
            ev_handler(ev)


class MatchEventHandler(object):

    def __init__(self, ev_matcher, callback):
        self._ev_matcher = ev_matcher
        self._callback = callback

    def __call__(self, ev):
        if self._ev_matcher(ev):
            self._callback(ev)


class UnexpectedEventHandler(object):

    def __init__(self, ev_matcher):
        self._ev_matcher = ev_matcher

    def __call__(self, ev):
        if self._ev_matcher(ev):
            raise UnexpectedEventException(ev)


class BufferEventHandler(object):

    def __init__(self):
        self._ev_queue = collections.deque()

    def __call__(self, ev):
        self._ev_queue.append(ev)

    def __iter__(self):
        return self

    def next(self):
        if not self._ev_queue:
            raise StopIteration()
        return self._ev_queue.popleft()
