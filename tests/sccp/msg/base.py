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

import itertools
import logging
import operator
import struct

logger = logging.getLogger(__name__)


class _Field(object):

    _instance_num_generator = itertools.count().next

    def __init__(self, field_type):
        # to be usable, self.name must be set, but this information is not
        # available on instantiation
        self.field_type = field_type
        self.instance_num = self._instance_num_generator()

    def __get__(self, obj, objtype):
        return obj.__dict__.get(self.name, self.field_type.default)

    def __set__(self, obj, value):
        self.field_type.check(value)
        obj.__dict__[self.name] = value


class _Uint32FieldType(object):

    _MAXVAL = 2 ** 32 - 1
    _FORMAT = struct.Struct('<I')

    default = 0
    size = 4

    def check(self, value):
        if not isinstance(value, int):
            raise ValueError('expected integer type; got %s type' % type(value).__name__)
        if not 0 <= value <= self._MAXVAL:
            raise ValueError('value %s is out of range' % value)

    def serialize(self, value):
        return self._FORMAT.pack(value)

    def deserialize(self, data):
        return self._FORMAT.unpack(data)[0]


class _Uint8FieldType(object):

    _MAXVAL = 2 ** 8 - 1

    default = 0
    size = 1

    def check(self, value):
        if not isinstance(value, int):
            raise ValueError('expected integer type; got %s type' % type(value).__name__)
        if not 0 <= value <= self._MAXVAL:
            raise ValueError('value %s is out of range' % value)

    def serialize(self, value):
        return chr(value)

    def deserialize(self, data):
        return ord(data[0])


class _BytesFieldType(object):

    default = ''

    def __init__(self, size):
        self.size = size

    def check(self, value):
        # str instead of basestring since unicode is not valid
        if not isinstance(value, str):
            raise ValueError('expected str type; got %s type' % type(value).__name__)
        if len(value) > self.size:
            raise ValueError('value %s is too long' % value)

    def serialize(self, value):
        return value.ljust(self.size, '\x00')

    def deserialize(self, data):
        return data.rstrip('\x00')


class _MsgMetaclass(type):

    def __new__(cls, name, bases, dct):
        fields = []
        for field_name, obj in dct.iteritems():
            if not isinstance(obj, _Field):
                continue
            obj.name = field_name
            fields.append(obj)
        fields.sort(key=operator.attrgetter('instance_num'))
        dct['_fields'] = fields
        return type.__new__(cls, name, bases, dct)


class BaseMsg(object):

    __metaclass__ = _MsgMetaclass

    def __init__(self, **kwargs):
        for name, value in kwargs.iteritems():
            setattr(self, name, value)

    def serialize(self):
        return ''.join(field.field_type.serialize(getattr(self, field.name))
                       for field in self._fields)

    def deserialize(self, body):
        offset = 0
        for field in self._fields:
            size = field.field_type.size
            data = body[offset:offset + size]
            offset += size
            try:
                setattr(self, field.name, field.field_type.deserialize(data))
            except Exception:
                logger.exception('error while decoding field %s of msg 0x%04X', field.name, self.id)
                raise

    def __repr__(self):
        return '<%s>' % self.__class__.__name__


def Uint32():
    return _Field(_Uint32FieldType())


def Uint8():
    return _Field(_Uint8FieldType())


def Bytes(length):
    return _Field(_BytesFieldType(length))
