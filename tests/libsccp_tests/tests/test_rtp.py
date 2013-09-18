# -*- coding: utf-8 -*-

import unittest

from libsccp_tests.rtp import RTPPacket


class TestRTPPacket(unittest.TestCase):

    def test_default(self):
        packet = RTPPacket()
        data = (
            '\x80\x00\x00\x00'
            '\x00\x00\x00\x00'
            '\x00\x00\x00\x00'
        )

        self._test_serialize(packet, data)
        self._test_deserialize(data, packet)

    def test_payload_type(self):
        packet = RTPPacket(payload_type=0x42)
        data = (
            '\x80\x42\x00\x00'
            '\x00\x00\x00\x00'
            '\x00\x00\x00\x00'
        )

        self._test_serialize(packet, data)
        self._test_deserialize(data, packet)

    def test_sequence_number(self):
        packet = RTPPacket(seq_num=0x1122)
        data = (
            '\x80\x00\x11\x22'
            '\x00\x00\x00\x00'
            '\x00\x00\x00\x00'
        )

        self._test_serialize(packet, data)
        self._test_deserialize(data, packet)

    def test_timestamp(self):
        packet = RTPPacket(timestamp=0x11223344)
        data = (
            '\x80\x00\x00\x00'
            '\x11\x22\x33\x44'
            '\x00\x00\x00\x00'
        )

        self._test_serialize(packet, data)
        self._test_deserialize(data, packet)

    def test_ssrc(self):
        packet = RTPPacket(ssrc=0x44332211)
        data = (
            '\x80\x00\x00\x00'
            '\x00\x00\x00\x00'
            '\x44\x33\x22\x11'
        )

        self._test_serialize(packet, data)
        self._test_deserialize(data, packet)

    def test_payload(self):
        packet = RTPPacket(payload='\xaa\xbb\xcc\xdd')
        data = (
            '\x80\x00\x00\x00'
            '\x00\x00\x00\x00'
            '\x00\x00\x00\x00'
            '\xaa\xbb\xcc\xdd'
        )

        self._test_serialize(packet, data)
        self._test_deserialize(data, packet)

    def _test_serialize(self, packet, expected_data):
        data = packet.serialize()

        self.assertEqual(expected_data, data)

    def _test_deserialize(self, data, expected_packet):
        packet = RTPPacket()
        packet.deserialize(data)

        self.assertEqual(expected_packet.payload_type, packet.payload_type)
        self.assertEqual(expected_packet.seq_num, packet.seq_num)
        self.assertEqual(expected_packet.timestamp, packet.timestamp)
        self.assertEqual(expected_packet.ssrc, packet.ssrc)
        self.assertEqual(expected_packet.payload, packet.payload)
