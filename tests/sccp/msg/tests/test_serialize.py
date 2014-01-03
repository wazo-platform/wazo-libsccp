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
import unittest

from mock import Mock
from sccp.msg.serialize import MsgSerializer, MsgDeserializer


class TestMsgSerializer(unittest.TestCase):

    def setUp(self):
        self.msg_serializer = MsgSerializer()

    def test_serialize(self):
        body = 'foo'
        msg = Mock()
        msg.id = 0x42
        msg.serialize.return_value = body

        data = self.msg_serializer.serialize_msg(msg)

        msg.serialize.assert_called_once_with()
        expected_data = build_msg(msg.id, body)
        self.assertEqual(expected_data, data)


class TestMsgDeserializer(unittest.TestCase):

    def setUp(self):
        self.msg = Mock()
        self.msg_factory = Mock()
        self.msg_factory.return_value = self.msg
        self.msg_deserializer = MsgDeserializer(self.msg_factory)

    def test_deserialize(self):
        msg_id = 0x42
        body = 'foo'
        data = build_msg(msg_id, body)

        msgs = list(self.msg_deserializer.deserialize_msg(data))

        self.msg_factory.assert_called_once_with(msg_id)
        self.assertEqual(1, len(msgs))
        self.assertTrue(msgs[0] is self.msg)
        self.msg.deserialize.assert_called_once_with(body)

    def test_deserialize_2_call_1_msg(self):
        msg_id = 0x42
        body = 'foo'
        data = build_msg(msg_id, body)

        msgs = []
        msgs.extend(self.msg_deserializer.deserialize_msg(data[:8]))
        msgs.extend(self.msg_deserializer.deserialize_msg(data[8:]))

        self.msg_factory.assert_called_once_with(msg_id)
        self.assertEqual(1, len(msgs))
        self.assertTrue(msgs[0] is self.msg)
        self.msg.deserialize.assert_called_once_with(body)

    def test_deserialize_1_call_2_msg(self):
        data = build_msg(0x42, '') * 2

        msgs = list(self.msg_deserializer.deserialize_msg(data))

        self.assertEqual(2, len(msgs))


def build_msg(msg_id, body):
    header = struct.pack('<III', len(body) + 4, 0, msg_id)
    return header + body
