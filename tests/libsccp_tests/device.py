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

import logging
import socket

from libsccp_tests import event
from libsccp_tests.rtp import RTPPacket
from sccp import const
from sccp.msg.serialize import MsgSerializer, MsgDeserializer
from sccp.msg.msg import RegisterAckMsg, RegisterRejMsg, CallStateMsg, \
    RegisterMsg, SoftkeyEventMsg, KeypadButtonMsg, OpenReceiveChannelMsg, \
    OpenReceiveChannelAckMsg, StartMediaTransmissionMsg, \
    StopMediaTransmissionMsg, CloseReceiveChannelMsg

logger = logging.getLogger(__name__)


class _MsgIdMap(dict):

    def register(self, m_id, value):
        self[m_id] = value

    def new_decorator_from_msg_class(self, m_class):
        def decorator(fun):
            self.register(m_class.id, fun)
            return fun
        return decorator


class SCCPDevice(object):

    _msg_id_map = _MsgIdMap()
    _msg_handler = _msg_id_map.new_decorator_from_msg_class

    def __init__(self, device_info, conn_info, sock_proxy_factory):
        self.device_info = device_info
        self.conn_info = conn_info
        self.calls = []
        self.ev_handler = None
        self._sock_proxy_factory = sock_proxy_factory
        self._sccp_sock = None
        self._rtp_sock = None
        self._msg_serializer = MsgSerializer()
        self._msg_deserializer = MsgDeserializer()

        # state
        self.registered = False
        self.closed = False

    def close(self):
        if self.closed:
            return

        self.closed = True
        if self._rtp_sock is not None:
            self._rtp_sock.close()
            self._rtp_sock = None
        if self._sccp_sock is not None:
            self._sccp_sock.close()
            self._sccp_sock = None
            self._publish_event(event.CONNECTION_CLOSED)

    def _publish_event(self, name, data=None):
        if self.ev_handler is not None:
            ev = event.Event(name, self, data)
            self.ev_handler(ev)

    def connect(self):
        # only made to be called once in the object lifetime
        if self._sccp_sock is not None:
            raise Exception('already connected')

        sock = self._sock_proxy_factory.new_socket_proxy(socket.AF_INET, socket.SOCK_STREAM)
        try:
            sock.connect((self.conn_info.host, self.conn_info.sccp_port))
        except Exception:
            self._publish_event(event.CONNECTION_FAILURE)
            sock.close()
            raise
        self._sccp_sock = sock
        self._sccp_sock.conn_established_cb = self._on_sccp_conn_established
        self._sccp_sock.conn_closed_cb = self._on_sccp_conn_closed
        self._sccp_sock.data_received_cb = self._on_sccp_data_received
        # XXX we'll be raising connection closed event instead of failure event
        #     with the transition to socket proxy

    def _on_sccp_conn_established(self):
        self._publish_event(event.CONNECTION_SUCCESS)

    def _on_sccp_conn_closed(self):
        self.close()

    def _on_sccp_data_received(self, data):
        for m in self._msg_deserializer.deserialize_msg(data):
            self._on_msg_received(m)

    def _on_rtp_data_received(self, data, address):
        logger.info('received data on RTP sock from address: %s', address)
        packet = RTPPacket()
        packet.deserialize(data)
        self._publish_event(event.RTP_PACKET_RECEIVED, data=(packet, address))

    def _on_msg_received(self, m):
        self._publish_event(event.SCCP_MSG_RECEIVED, data=m)
        fun = self._msg_id_map.get(m.id)
        if fun:
            fun(self, m)

    @_msg_handler(RegisterAckMsg)
    def _on_register_ack_msg_received(self, m):
        # device registration succeed
        if self.registered:
            logger.warning('registration succeed but already registered')
        self.registered = True
        self._publish_event(event.REGISTRATION_SUCCESS)

    @_msg_handler(RegisterRejMsg)
    def _on_register_rej_msg_received(self, m):
        # device registration failed
        self._publish_event(event.REGISTRATION_FAILURE)

    @_msg_handler(CallStateMsg)
    def _on_call_state_msg_received(self, m):
        call = self._find_call_by_call_id(m.call_id)
        if m.call_state == const.STATE_OFFHOOK:
            # call started
            if call:
                logger.warning('call state is offhook but call already exist')
                return
            self.calls.append(SCCPCall(self, m.call_id))
        elif m.call_state == const.STATE_ONHOOK:
            # call ended
            if not call:
                logger.warning('call state is onhook but call doesn\'t exist')
                return
            self.calls.remove(call)
            self._publish_event(event.CALL_HANGUP)
        elif m.call_state == const.STATE_RINGIN:
            if not call:
                self.calls.append(SCCPCall(self, m.call_id))
            self._publish_event(event.CALL_INCOMING)
        elif m.call_state == const.STATE_RINGOUT:
            self._publish_event(event.CALL_OUTGOING)
        elif m.call_state == const.STATE_CONNECTED:
            self._publish_event(event.CALL_CONNECTED)

    @_msg_handler(OpenReceiveChannelMsg)
    def _on_open_receive_channel_msg(self, m):
        call = self._find_call_by_call_id(m.conference_id)
        if not call:
            logger.warning('open receive channel msg could not be mapped to a call')
            self._publish_event(event.ERROR)
            return
        if self._rtp_sock is not None:
            logger.warning('rtp sock is not None')
            self._publish_event(event.ERROR)
            return
        self._rtp_sock = self._sock_proxy_factory.new_socket_proxy(socket.AF_INET, socket.SOCK_DGRAM)
        self._rtp_sock.bind((self._sccp_sock.getsockname()[0], 0))
        self._rtp_sock.data_received_cb = self._on_rtp_data_received
        bind_addr, bind_port = self._rtp_sock.getsockname()

        m = OpenReceiveChannelAckMsg()
        m.ip_addr = socket.inet_aton(bind_addr)
        m.port = bind_port
        self._send_msg(m)

    @_msg_handler(CloseReceiveChannelMsg)
    def _on_close_receive_channel_msg(self, m):
        if self._rtp_sock is None:
            logger.warning('rtp sock is None')
            self._publish_event(event.ERROR)
            return
        self._rtp_sock.close()
        self._rtp_sock = None

    @_msg_handler(StartMediaTransmissionMsg)
    def _on_start_media_transmission_msg(self, m):
        call = self._find_call_by_call_id(m.conference_id)
        if not call:
            logger.warning('start media transmission msg could not be mapped to a call')
            self._publish_event(event.ERROR)
            return
        call.remote_rtp_address = (socket.inet_ntoa(m.remote_ip), m.remote_port)

    @_msg_handler(StopMediaTransmissionMsg)
    def _on_stop_media_transmission_msg(self, m):
        # TODO
        call = self._find_call_by_call_id(m.conference_id)
        if not call:
            logger.warning('stop media transmission msg could not be mapped to a call')
            self._publish_event(event.ERROR)
            return

    def _find_call_by_call_id(self, call_id):
        for call in self.calls:
            if call.call_id == call_id:
                return call

    def register(self):
        m = RegisterMsg()
        m.name = self.device_info.name
        m.type = self.device_info.type
        m.proto_version = self.device_info.proto_version
        self._send_msg(m)

    def call(self, exten):
        m = []
        m.append(SoftkeyEventMsg(softkey_event=const.SOFTKEY_NEWCALL))
        line_id = 1
        exten += '#'
        for c in exten:
            m.append(KeypadButtonMsg(line_id=line_id).set_button(c))
        self._send_msgs(m)

    def _send_msg(self, msg):
        data = self._msg_serializer.serialize_msg(msg)
        self._sccp_sock.send(data)

    def _send_msgs(self, msgs):
        data = ''.join(self._msg_serializer.serialize_msg(m) for m in msgs)
        self._sccp_sock.send(data)


class SCCPCall(object):

    def __init__(self, device, call_id):
        self._device = device
        self.call_id = call_id
        self.remote_rtp_address = None

    def answer(self):
        m = SoftkeyEventMsg(softkey_event=const.SOFTKEY_ANSWER, line_id=1, call_id=self.call_id)
        self._device._send_msg(m)


class SCCPDeviceInfo(object):

    DEFAULT_DEVICE_TYPE = const.DEVICE_TYPE_7940
    DEFAULT_PROTO_VERSION = 11

    def __init__(self, name):
        self.name = name
        self.type = self.DEFAULT_DEVICE_TYPE
        self.proto_version = self.DEFAULT_PROTO_VERSION
