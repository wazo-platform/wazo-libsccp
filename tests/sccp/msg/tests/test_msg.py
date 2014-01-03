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

import unittest

from sccp.msg.msg import RegisterMsg, KeepAliveMsg, new_msg_from_id


class TestMsg(unittest.TestCase):

    def test_keep_alive_msg(self):
        m = KeepAliveMsg()

        self.assertEqual(0x0000, m.id)

    def test_register_msg(self):
        m = RegisterMsg()

        self.assertEqual(0x0001, m.id)
        self.assertEqual('', m.name)
        self.assertEqual(0, m.user_id)
        self.assertEqual(0, m.line_id)
        self.assertEqual(0, m.ip)
        self.assertEqual(0, m.max_streams)
        self.assertEqual(0, m.active_streams)
        self.assertEqual(0, m.proto_version)


class TestRegistry(unittest.TestCase):

    def test_registered_keep_alive_msg(self):
        m = new_msg_from_id(KeepAliveMsg.id)

        self.assertEqual(m.id, KeepAliveMsg.id)
        self.assertTrue(isinstance(m, KeepAliveMsg))

    def test_registered_msg(self):
        m = new_msg_from_id(RegisterMsg.id)

        self.assertEqual(m.id, RegisterMsg.id)
        self.assertTrue(isinstance(m, RegisterMsg))
