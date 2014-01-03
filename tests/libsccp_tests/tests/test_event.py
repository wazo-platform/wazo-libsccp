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

from mock import Mock
from libsccp_tests.event import CompositeEventHandler


class TestCompositeEventHandler(unittest.TestCase):

    def test_composite(self):
        ev = Mock()
        ev_handler = Mock()
        composite_ev_handler = CompositeEventHandler()
        composite_ev_handler.append(ev_handler)

        composite_ev_handler(ev)

        ev_handler.assert_called_once_with(ev)
