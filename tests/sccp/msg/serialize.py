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

import struct

from sccp.msg.msg import new_msg_from_id

_HEADER_FORMAT = struct.Struct('<III')
_HEADER_SIZE = _HEADER_FORMAT.size
_LENGTH_FORMAT = struct.Struct('<I')
_LENGTH_SIZE = _LENGTH_FORMAT.size
_RESERVED_SIZE = 4
_MSG_ID_SIZE = 4


class MsgSerializer(object):

    def serialize_msg(self, msg):
        body = msg.serialize()
        header = _HEADER_FORMAT.pack(len(body) + _MSG_ID_SIZE, 0, msg.id)
        return header + body


class MsgDeserializer(object):

    def __init__(self, msg_factory=new_msg_from_id):
        self._msg_factory = msg_factory
        self._msg_splitter = _MsgSplitter()

    def deserialize_msg(self, data):
        self._msg_splitter.append(data)
        for raw_msg in self._msg_splitter.split():
            yield self._deserialize_raw_msg(raw_msg)

    def _deserialize_raw_msg(self, raw_msg):
        header, body = raw_msg[:_HEADER_SIZE], raw_msg[_HEADER_SIZE:]
        _, _, msg_id = _HEADER_FORMAT.unpack(header)
        msg = self._msg_factory(msg_id)
        msg.deserialize(body)
        return msg


class _MsgSplitter(object):

    _MIN_LENGTH = _MSG_ID_SIZE
    _MAX_LENGTH = 2000

    def __init__(self):
        self._buf = ''

    def append(self, data):
        self._buf += data

    def split(self):
        while True:
            raw_msg, self._buf = self._split_one(self._buf)
            if not raw_msg:
                return
            yield raw_msg

    def _split_one(self, data):
        if len(data) < _LENGTH_SIZE:
            return None, data

        length, = _LENGTH_FORMAT.unpack(data[:_LENGTH_SIZE])
        if length < self._MIN_LENGTH or length > self._MAX_LENGTH:
            raise Exception('invalid length: %d' % length)

        total_length = _LENGTH_SIZE + _RESERVED_SIZE + length
        if len(data) < total_length:
            return None, data

        return data[:total_length], data[total_length:]
