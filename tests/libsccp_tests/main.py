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

import argparse
import logging
import select
import time

from libsccp_tests.common import ConnectionInfo
from libsccp_tests.event import CompositeEventHandler, LogEventHandler, UnexpectedEventHandler, \
    CONNECTION_CLOSED, CALL_INCOMING, SCCP_MSG_RECEIVED, RTP_PACKET_RECEIVED
from libsccp_tests.group import DevicesGroup
from libsccp_tests.rtp import RTPPacket
from libsccp_tests.socketproxy import SocketProxyFactory
from libsccp_tests.device import SCCPDevice, SCCPDeviceInfo
from sccp.msg.msg import OpenReceiveChannelMsg, StartMediaTransmissionMsg

logger = logging.getLogger(__name__)


def main():
    parsed_args = _parse_args()

    logging.basicConfig(level=logging.INFO)

    sock_proxy_factory = SocketProxyFactory(select.poll())

    conn_info = ConnectionInfo.new_from_hostname(parsed_args.hostname)

    # tests
    main_test_rtp_direct_media_off(sock_proxy_factory, conn_info)


def main_test_rtp_direct_media_off(sock_proxy_factory, conn_info):
    dev1_info = SCCPDeviceInfo('SEP001122334401')
    dev2_info = SCCPDeviceInfo('SEP001122334402')
    dev1 = SCCPDevice(dev1_info, conn_info, sock_proxy_factory)
    dev2 = SCCPDevice(dev2_info, conn_info, sock_proxy_factory)

    ev_handler = CompositeEventHandler()
    ev_handler.append(LogEventHandler())
    ev_handler.append(UnexpectedEventHandler(lambda ev: ev.name == CONNECTION_CLOSED))

    group = DevicesGroup(ev_handler, sock_proxy_factory)
    group.add_device(dev1)
    group.add_device(dev2)

    group.connect_all()

    group.register_all()

    group.devices[0].call('02')

    group.wait_for_event(lambda ev: ev.name == CALL_INCOMING and ev.device is group.devices[1])

    group.devices[1].calls[0].answer()

    group.wait_for_all_events([
        lambda ev: ev.name == SCCP_MSG_RECEIVED and ev.device is group.devices[0] and ev.data.id == OpenReceiveChannelMsg.id,
        lambda ev: ev.name == SCCP_MSG_RECEIVED and ev.device is group.devices[0] and ev.data.id == StartMediaTransmissionMsg.id,
        lambda ev: ev.name == SCCP_MSG_RECEIVED and ev.device is group.devices[1] and ev.data.id == OpenReceiveChannelMsg.id,
        lambda ev: ev.name == SCCP_MSG_RECEIVED and ev.device is group.devices[1] and ev.data.id == StartMediaTransmissionMsg.id,
    ])

    # assert RTP informations
    if group.devices[0].calls[0].remote_rtp_address[0] == conn_info.host_ipv4 and \
       group.devices[1].calls[0].remote_rtp_address[0] == conn_info.host_ipv4:
        print 'success'
    else:
        print group.devices[0].calls[0].remote_rtp_address
        print group.devices[1].calls[0].remote_rtp_address
        raise Exception('not good')

    # try sending a frame from 0 to 1, check that the frame is received
    # try sending a frame from 1 to 0, check that the frame is received
    packet0 = RTPPacket()
    packet0.ssrc = 1
    packet0.payload = 'from device 0'
    # XXX hack using the _rtp_sock directly
    group.devices[0]._rtp_sock.sendto(packet0.serialize(), group.devices[0].calls[0].remote_rtp_address)

    packet1 = RTPPacket()
    packet1.ssrc = 2
    packet1.payload = 'from device 1'
    # XXX hack using the _rtp_sock directly
    group.devices[1]._rtp_sock.sendto(packet1.serialize(), group.devices[1].calls[0].remote_rtp_address)

    group.wait_for_all_events([
        lambda ev: ev.name == RTP_PACKET_RECEIVED and ev.device is group.devices[0] and ev.data[0].payload == packet1.payload,
        lambda ev: ev.name == RTP_PACKET_RECEIVED and ev.device is group.devices[1] and ev.data[0].payload == packet0.payload,
    ])

    print 'yayayay'
    time.sleep(1)

    group.close()


def _parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument('hostname')
    return parser.parse_args()
