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

from sccp.msg.base import BaseMsg, _MsgMetaclass, \
    _Uint32FieldType, _Uint8FieldType, _BytesFieldType, Uint32, _Field


class TestMsgMetaclass(unittest.TestCase):

    def test_class_name(self):
        class_name = 'FoofooMsg'

        msg_class = _MsgMetaclass(class_name, (object,), {'a': _Field(None)})

        self.assertEqual(msg_class.__name__, class_name)

    def test_fields_order(self):
        f1 = _Field(None)
        f2 = _Field(None)
        f3 = _Field(None)
        f4 = _Field(None)
        dct = {
            'a': f3,
            'b': f2,
            'c': f1,
            'd': f4,
        }

        msg_class = _MsgMetaclass('FooMsg', (object,), dct)

        self.assertEqual(msg_class._fields, [f1, f2, f3, f4])


class TestMsg(unittest.TestCase):

    msg_id = 42

    class FakeMsg(BaseMsg):

        id = 42

        foo = Uint32()
        bar = Uint32()

    def setUp(self):
        self.m = self.FakeMsg()

    def test_msg_id(self):
        self.assertEqual(self.FakeMsg.id, self.msg_id)
        self.assertEqual(self.m.id, self.msg_id)

    def test_msg_default_value(self):
        self.assertEqual(self.m.foo, 0)
        self.assertEqual(self.m.bar, 0)

    def test_msg_init_with_kwargs(self):
        m = self.FakeMsg(foo=7, bar=3)

        self.assertEqual(m.foo, 7)
        self.assertEqual(m.bar, 3)

    def test_msg_set_value(self):
        self.m.foo = 42

        self.assertEqual(self.m.foo, 42)

    def test_msg_set_invalid_value(self):
        try:
            self.m.foo = -1
        except ValueError:
            pass
        else:
            self.fail('exception not raised')

    def test_serialize(self):
        self.m.foo = 0x1122

        string = self.m.serialize()

        self.assertEqual(string, '\x22\x11\x00\x00' '\x00\x00\x00\x00')

    def test_deserialize(self):
        self.m.deserialize('\x22\x11\x00\x00' '\x00\x00\x00\x00')

        self.assertEqual(self.m.foo, 0x1122)
        self.assertEqual(self.m.bar, 0)


class TestField(unittest.TestCase):

    def test_instance_num_increase(self):
        f1 = _Field(None)
        f2 = _Field(None)

        self.assertGreater(f2.instance_num, f1.instance_num)


class BaseTestFieldType(object):

    def test_default_value(self):
        self.assertEqual(self.default_value, self.field_type.default)

    def test_check_valid_value(self):
        for value in self.valid_values:
            self.field_type.check(value)

    def test_check_invalid_values(self):
        for value in self.invalid_values:
            self.assertRaises(ValueError, self.field_type.check, value)

    def test_serialize(self):
        for value, expected_string in self.serialize_test_table:
            string = self.field_type.serialize(value)

            self.assertEqual(expected_string, string)

    def test_deserialize(self):
        for expected_value, string in self.serialize_test_table:
            value = self.field_type.deserialize(string)

            self.assertEqual(expected_value, value)


class TestUint32(BaseTestFieldType, unittest.TestCase):

    field_type = _Uint32FieldType()

    default_value = 0
    valid_values = [0, 42, 2 ** 32 - 1]
    invalid_values = [-1, 2 ** 32, 3.14]

    serialize_test_table = [
        (0x1122, '\x22\x11\x00\x00'),
    ]


class TestUint8(BaseTestFieldType, unittest.TestCase):

    field_type = _Uint8FieldType()

    default_value = 0
    valid_values = [0, 42, 2 ** 8 - 1]
    invalid_values = [-1, 2 ** 8, 3.14]

    serialize_test_table = [
        (0x11, '\x11'),
    ]


class TestBytes(BaseTestFieldType, unittest.TestCase):

    field_type = _BytesFieldType(4)

    default_value = ''
    valid_values = ['', 'abcd']
    invalid_values = ['abcde', 1, 3.14]

    serialize_test_table = [
        ('abcd', 'abcd'),
        ('ab', 'ab\x00\x00'),
        ('', '\x00\x00\x00\x00'),
    ]
