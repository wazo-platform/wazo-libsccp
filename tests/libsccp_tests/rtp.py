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


class RTPPacket(object):

    _HEADER_FORMAT = struct.Struct('>BBHII')
    _HEADER_FORMAT_SIZE = _HEADER_FORMAT.size

    def __init__(self, payload_type=0, seq_num=0, timestamp=0, ssrc=0, payload=''):
        self.payload_type = payload_type
        self.seq_num = seq_num
        self.timestamp = timestamp
        self.ssrc = ssrc
        self.payload = payload

    def serialize(self):
        return self._serialize_header() + self.payload

    def _serialize_header(self):
        byte1 = 0x80
        byte2 = self.payload_type & 0x7F
        return self._HEADER_FORMAT.pack(byte1, byte2, self.seq_num, self.timestamp, self.ssrc)

    def deserialize(self, data):
        # XXX RTP header can be larger than that but we don't handle these for now
        header = data[:self._HEADER_FORMAT_SIZE]
        body = data[self._HEADER_FORMAT_SIZE:]
        self._deserialize_header(header)
        self.payload = body

    def _deserialize_header(self, header):
        _, byte2, seq_num, timestamp, ssrc = self._HEADER_FORMAT.unpack(header)
        self.payload_type = byte2 & 0x7F
        self.seq_num = seq_num
        self.timestamp = timestamp
        self.ssrc = ssrc
