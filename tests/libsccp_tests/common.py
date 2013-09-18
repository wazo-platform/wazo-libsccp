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

import socket


class ConnectionInfo(object):

    DEFAULT_SCCP_PORT = 2000

    def __init__(self, host, host_ipv4):
        """
        host -- either a hostname in Internet domain notation like
            'foo.example.org' or an IPv4 address like '10.0.0.2'
        host_ipv4 -- an IPv4 address like '10.0.0.2'

        """
        self.host = host
        self.host_ipv4 = host_ipv4
        self.sccp_port = self.DEFAULT_SCCP_PORT

    @classmethod
    def new_from_hostname(cls, hostname):
        host_ipv4 = socket.gethostbyname(hostname)
        return cls(hostname, host_ipv4)


class UnexpectedEventException(Exception):

    pass


class TimeoutException(Exception):

    pass
