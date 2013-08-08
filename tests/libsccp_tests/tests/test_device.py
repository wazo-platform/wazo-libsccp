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

import unittest

from mock import Mock
from libsccp_tests.device import _MsgIdMap


class TestMsgIdMap(unittest.TestCase):

    msg_class1 = Mock()
    msg_class1.id = 1
    msg_class2 = Mock()
    msg_class2.id = 2

    def test_map(self):
        fun1 = Mock()
        fun2 = Mock()
        msg_class_map = _MsgIdMap()
        msg_handler = msg_class_map.new_decorator_from_msg_class

        msg_handler(self.msg_class1)(fun1)
        msg_handler(self.msg_class2)(fun2)

        self.assertEqual(fun1, msg_class_map[self.msg_class1.id])
        self.assertEqual(fun2, msg_class_map[self.msg_class2.id])
