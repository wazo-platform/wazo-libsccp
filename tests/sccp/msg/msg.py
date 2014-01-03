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

from sccp.msg.base import BaseMsg, Bytes, Uint32, Uint8

_registry = {}


def registered(msg_class):
    _registry[msg_class.id] = msg_class
    return msg_class


@registered
class KeepAliveMsg(BaseMsg):

    id = 0x0000


@registered
class RegisterMsg(BaseMsg):

    id = 0x0001

    name = Bytes(16)
    user_id = Uint32()
    line_id = Uint32()
    ip = Uint32()
    type = Uint32()
    max_streams = Uint32()
    active_streams = Uint32()
    proto_version = Uint8()


@registered
class KeypadButtonMsg(BaseMsg):

    id = 0x0003

    button = Uint32()
    line_id = Uint32()
    call_id = Uint32()

    def set_button(self, c):
        self.button = self._char_to_button(c)
        return self

    @staticmethod
    def _char_to_button(c):
        if c >= '0' and c <= '9':
            return ord(c) - ord('0')
        elif c == '*':
            return 14
        elif c == '#':
            return 15
        else:
            raise Exception('cannot map %s to button' % c)


@registered
class AlarmMsg(BaseMsg):

    id = 0x0020

    alarm_severity = Uint32()
    display_message = Bytes(80)
    alarm_param1 = Uint32()
    alarm_param2 = Uint32()


@registered
class OpenReceiveChannelAckMsg(BaseMsg):

    id = 0x0022

    status = Uint32()
    ip_addr = Bytes(4)
    port = Uint32()
    pass_thru_id = Uint32()


@registered
class SoftkeyEventMsg(BaseMsg):

    id = 0x0026

    softkey_event = Uint32()
    line_id = Uint32()
    call_id = Uint32()


@registered
class RegisterAckMsg(BaseMsg):

    id = 0x0081

    keepalive = Uint32()
    date_template = Bytes(6)
    res = Bytes(2)
    secondary_keep_alive = Uint32()
    proto_version = Uint8()
    unknown1 = Uint8()
    unknown2 = Uint8()
    unknown3 = Uint8()


@registered
class SetRingerMsg(BaseMsg):

    id = 0x0085

    ringer_mode = Uint32()
    unknown1 = Uint32()
    unknown2 = Uint32()
    space = Bytes(8)


@registered
class StartMediaTransmissionMsg(BaseMsg):

    id = 0x008A

    conference_id = Uint32()
    pass_thru_party_id = Uint32()
    remote_ip = Bytes(4)
    remote_port = Uint32()
    packet_size = Uint32()
    payload_type = Uint32()
    # media qualifier struct
    precedence = Uint32()
    vad = Uint32()
    packets = Uint32()
    bit_rate = Uint32()
    # end media qualifier struct
    conference_id1 = Uint32()
    space = Bytes(56)
    rtp_dtmf_payload = Uint32()
    rtp_timeout = Uint32()
    mixing_mode = Uint32()
    mixing_party = Uint32()


@registered
class StopMediaTransmissionMsg(BaseMsg):

    id = 0x008B

    conference_id = Uint32()
    party_id = Uint32()
    conference_id1 = Uint32()
    unknown1 = Uint32()


@registered
class CallInfoMsg(BaseMsg):

    id = 0x008F

    calling_party_name = Bytes(40)
    calling_party = Bytes(24)
    called_party_name = Bytes(40)
    called_party = Bytes(24)
    line_id = Uint32()
    call_id = Uint32()
    type = Uint32()
    original_called_party_name = Bytes(40)
    original_called_party = Bytes(24)
    last_redirecting_party_name = Bytes(40)
    last_redirecting_party = Bytes(24)
    original_called_party_redirect_reason = Uint32()
    last_redirecting_reason = Uint32()
    calling_party_voice_mailbox = Bytes(24)
    called_party_voice_mailbox = Bytes(24)
    original_called_party_voice_mailbox = Bytes(24)
    last_redirecting_voice_mailbox = Bytes(24)
    space = Bytes(12)


@registered
class RegisterRejMsg(BaseMsg):

    id = 0x009D

    err_msg = Bytes(33)


@registered
class ResetMsg(BaseMsg):

    id = 0x009F

    type = Uint32()


@registered
class KeepAliveAckMsg(BaseMsg):

    id = 0x0100


@registered
class OpenReceiveChannelMsg(BaseMsg):

    id = 0x0105

    conference_id = Uint32()
    party_id = Uint32()
    packets = Uint32()
    capability = Uint32()
    echo = Uint32()
    bitrate = Uint32()
    conference_id1 = Uint32()
    unknown = Bytes(56)
    rtp_dtmf_payload = Uint32()
    rtp_timeout = Uint32()
    mixing_mode = Uint32()
    mixing_party = Uint32()
    ip_addr = Uint32()
    unknown17 = Bytes(4)


@registered
class CloseReceiveChannelMsg(BaseMsg):

    id = 0x0106

    conference_id = Uint32()
    party_id = Uint32()
    conference_id1 = Uint32()


@registered
class CallStateMsg(BaseMsg):

    id = 0x0111

    call_state = Uint32()
    line_id = Uint32()
    call_id = Uint32()
    visiblity = Uint32()
    priority = Uint32()
    unknown = Uint32()


@registered
class StartMediaTransmissionAckMsg(BaseMsg):

    id = 0x0159


class GenericMsg(object):

    def __init__(self, msg_id):
        self.id = msg_id
        self.data = ''

    def serialize(self):
        return self.data

    def deserialize(self, body):
        self.data = body

    def __repr__(self):
        return '<GenericMsg (id 0x%04X)>' % self.id


def new_msg_from_id(msg_id):
    msg_factory = _registry.get(msg_id)
    if msg_factory:
        return msg_factory()
    else:
        return GenericMsg(msg_id)
