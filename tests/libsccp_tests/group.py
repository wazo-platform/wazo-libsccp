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

import operator

from libsccp_tests.event import MatchEventHandler, CompositeEventHandler, UnexpectedEventHandler, \
    CONNECTION_SUCCESS, CONNECTION_FAILURE, REGISTRATION_SUCCESS, \
    REGISTRATION_FAILURE, BufferEventHandler


class DevicesGroup(object):

    DEFAULT_TIMEOUT = 10

    def __init__(self, ev_handler, sock_proxy_factory):
        self._sock_proxy_factory = sock_proxy_factory
        # devices first send events in the buffer_ev_handler; then, we read
        # events that have been buffered and send them to ev_handler, the
        # real "worker" handler.
        # This is done this way because we want to exit the wait_* function
        # without consuming more events than the strict minimum
        self._buffer_ev_handler = BufferEventHandler()
        self._ev_handler = CompositeEventHandler()
        self._ev_handler.append(ev_handler)
        self.devices = []

    def close(self):
        for device in self.devices:
            self._deinit_device(device)
        self.devices = []

    def add_device(self, device):
        self._init_device(device)
        self.devices.append(device)

    def _init_device(self, device):
        device.ev_handler = self._buffer_ev_handler

    def remove_device(self, device):
        self._deinit_device(device)
        self.devices.remove(device)

    def _deinit_device(self, device):
        device.ev_handler = None
        device.close()

    def connect_all(self, timeout=DEFAULT_TIMEOUT):
        for device in self.devices:
            device.connect()

        cond = IntegerCondition(0, len(self.devices))
        success_handler = MatchEventHandler(lambda ev: ev.name == CONNECTION_SUCCESS, lambda ev: cond.add(1))
        failure_handler = UnexpectedEventHandler(lambda ev: ev.name == CONNECTION_FAILURE)
        return self._wait_for_condition_with_handlers(cond, [success_handler, failure_handler], timeout)

    def register_all(self, timeout=DEFAULT_TIMEOUT):
        for device in self.devices:
            device.register()

        cond = IntegerCondition(0, len(self.devices))
        success_handler = MatchEventHandler(lambda ev: ev.name == REGISTRATION_SUCCESS, lambda ev: cond.add(1))
        failure_handler = UnexpectedEventHandler(lambda ev: ev.name == REGISTRATION_FAILURE)
        return self._wait_for_condition_with_handlers(cond, [success_handler, failure_handler], timeout)

    def wait(self, timeout=DEFAULT_TIMEOUT):
        self._sock_proxy_factory.poll(timeout * 1000)

    def wait_for_condition(self, condition, timeout=DEFAULT_TIMEOUT):
        # TODO make use of timeout correctly
        if condition:
            return
        while True:
            for ev in self._buffer_ev_handler:
                self._ev_handler(ev)
                if condition:
                    return
            self.wait(timeout)

    def _wait_for_condition_with_handlers(self, condition, handlers, timeout):
        self._ev_handler.extend(handlers)
        try:
            self.wait_for_condition(condition, timeout)
        finally:
            for handler in handlers:
                self._ev_handler.remove(handler)

    def wait_for_event(self, ev_matcher, timeout=DEFAULT_TIMEOUT):
        cond = BooleanCondition(False)
        handler = MatchEventHandler(ev_matcher, lambda ev: cond.set(True))
        self._wait_for_condition_with_handlers(cond, [handler], timeout)

    def wait_for_all_events(self, ev_matchers, timeout=DEFAULT_TIMEOUT):
        handler_cond = MatchAllEventHandlerCondition(ev_matchers)
        self._wait_for_condition_with_handlers(handler_cond, [handler_cond], timeout)


class BooleanCondition(object):

    def __init__(self, value):
        self.value = value

    def set(self, value):
        self.value = value

    def __nonzero__(self):
        return self.value


class IntegerCondition(object):

    def __init__(self, value, target, op=operator.eq):
        self.value = value
        self.target = target
        self._op = op

    def add(self, inc):
        self.value += inc

    def __nonzero__(self):
        return self._op(self.value, self.target)


class AllCondition(object):

    def __init__(self, conditions):
        self.conditions = conditions

    def __nonzero__(self):
        return all(self.conditions)


class UnaryCondition(object):

    def __init__(self, value, op):
        self.value = value
        self._op = op

    def __nonzero__(self):
        return self._op(self.value)


class BinaryCondition(object):

    def __init__(self, lhs, rhs, op):
        self.lhs = lhs
        self.rhs = rhs
        self._op = op

    def __nonzero__(self):
        return self._op(self.lhs, self.rhs)


class MatchAllEventHandlerCondition(object):

    # XXX might be a bit weird, is both a handler and a condition

    def __init__(self, ev_matchers):
        self._ev_matchers = ev_matchers
        self._unmatched_ev_matchers = set(ev_matchers)

    def __call__(self, ev):
        # XXX not terribly efficient
        for ev_matcher in self._ev_matchers:
            if ev_matcher(ev):
                self._unmatched_ev_matchers.discard(ev_matcher)

    def __nonzero__(self):
        # the condition is true once all matchers have matched at least once
        return not self._unmatched_ev_matchers
