import logging
import os
import pytest
from hamcrest import assert_that
from hamcrest import empty, has_item
from requests.exceptions import HTTPError
from xivo_test_helpers import until
from xivo_test_helpers.bus import BusClient
from xivo_test_helpers.asset_launching_test_case import AssetLaunchingTestCase
from xivo_test_helpers.hamcrest.raises import raises
from functools import partial
from pprint import pprint
import time
import requests
import docker as docker_client
import subprocess

log_level = logging.DEBUG if os.environ.get('TEST_LOGS') == 'verbose' else logging.INFO
logging.basicConfig(level=log_level)


class HTTPRequestHandler:
    def __init__(self, address, port):
        self.address = address
        self.port = port

    @property
    def url(self):
        return 'http://{}:{}/'.format(self.address, self.port)

    def register(self, host, port, device):
        url = self.url + "register/{}/{}/{}".format(host, port, device)
        response = requests.post(url)
        return response


    def place_call(self, number_to_call):
        url = self.url + "dial/{}".format(number_to_call)
        response = requests.post(url)
        return response

    def get_history(self):
        url = self.url + "history"
        response = requests.get(url)
        return response.json()

    def get_callstates(self):
        return self.get_history()['callStates']

    def get_callevents(self):
        return self.get_history()['events']

    def get_status(self):
        url = self.url + "status"
        response = requests.get(url)
        return response.json()

    def hangup(self):
        url = self.url + "hangup"
        response = requests.post(url)
        return response

    def pickup(self):
        url = self.url + "answer"
        response = requests.post(url)
        return response

    def clear(self):
        url = self.url + "clear"
        response = requests.delete(url)
        return response
class AssetLauncher(AssetLaunchingTestCase):

    assets_root = os.path.join(os.path.dirname(__file__), '..', 'assets')
    asset = 'sccp_phone_controller'
    service = 'asterisk'


@pytest.fixture()
def http_servers():
    AssetLauncher.kill_containers()
    AssetLauncher.rm_containers()
    AssetLauncher.launch_service_with_asset()
    http_server_1 = HTTPRequestHandler('127.0.0.1', AssetLauncher.service_port(8000, 'sccp_phone_controller_1'))
    http_server_2 = HTTPRequestHandler('127.0.0.1', AssetLauncher.service_port(8000, 'sccp_phone_controller_2'))
    http_server_3 = HTTPRequestHandler('127.0.0.1', AssetLauncher.service_port(8000, 'sccp_phone_controller_3'))
    yield http_server_1, http_server_2, http_server_3
    AssetLauncher.kill_containers()


# Will be given by docker-compose tools

sccp_port = 2000
docker = docker_client.from_env().api

# Fake MAC addresses
device_name_a = 'SEP00164697AAAA'
device_name_b = 'SEP00164697AAAB'
device_name_c = 'SEP00164697AAAC'

def test_callwaiting(http_servers):
    sccp_host = docker.inspect_container(AssetLauncher._container_id('asterisk'))['NetworkSettings']['Networks']['asterisk_default']['IPAddress']
    http_server_1 = http_servers[0]
    http_server_2 = http_servers[1]
    http_server_3 = http_servers[2]

    response = http_server_1.register(sccp_host, sccp_port, device_name_a)
    assert response.status_code == 200
    response = http_server_2.register(sccp_host, sccp_port, device_name_b)
    assert response.status_code == 200
    response = http_server_3.register(sccp_host, sccp_port, device_name_c)
    assert response.status_code == 200

    http_server_2.place_call('1003')

    while not http_server_1.get_status()['ringing']:
        time.sleep(0.1)

    http_server_1.pickup()
    time.sleep(1)
    http_server_3.place_call('1003')
    states = http_server_1.get_callstates()

    assert_that(states, has_item('CALLWAITING'))
