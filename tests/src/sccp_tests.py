'''
Created on Aug 5, 2019

@author: nballas
'''
from sccp.sccpcapabilities import SCCPCapabilitiesRes
from sccp.sccpcapabilitiesreq import SCCPCapabilitiesReq
from sccp.sccpbuttontemplatereq import SCCPButtonTemplateReq
from sccp.sccpregister import SCCPRegister
from sccp.sccpregisterack import SCCPRegisterAck
from sccp.sccpregisteravailablelines import SCCPRegisterAvailableLines
from sccp.sccptimedatereq import SCCPTimeDateReq
from sccpphone import SCCPPhone
from tests.mock import Mock
import unittest
from sccp.sccpdefinetimedate import SCCPDefineTimeDate
from sccp.sccpsetspeakermode import SCCPSetSpeakerMode
from sccp.sccpcallstate import SCCPCallState
from sccp.sccpkeypadbutton import SCCPKeyPadButton
from gui.softkeys import SKINNY_LBL_NEWCALL, SKINNY_LBL_ANSWER,\
    SKINNY_LBL_ENDCALL
from sccp.sccpsoftkeyevent import SCCPSoftKeyEvent
from sccp.sccpmessagetype import SCCPMessageType
from sccp.sccpmessage import SCCPMessage
from sccp.sccplinestat import SCCPLineStat
from sccp.sccplinestatreq import SCCPLineStatReq
from actors.callactor import CallActor
from network.sccpclientfactory import SCCPClientFactory
from threading import Thread, Lock, Timer
from multiprocessing import Process
import time

from PyQt4.QtCore import QTimer, QThread
from twisted.internet import reactor

class AnyInstanceOf(object):

    def __init__(self, clazz):
        self.clazz = clazz

    def __eq__(self,other):
        return self.clazz.__name__ == other.__class__.__name__

SERVER_HOST = '10.33.0.1'
SERVER_PORT = 2000
DEVICE_NAME1 = 'SEP00164697AAAA'
DEVICE_NAME2 = 'SEP00164697AAAB'
DEVICE_NAME3 = 'SEP00164697AAAC'

def phone_log(msg):
    print(msg)

class SCCPPhoneContoller:
    def __init__(self):
        self.log = phone_log

    def createTimer(self, intervalSecs, timerCallback):
        self.keepalive_timer = QTimer()
        self.keepalive_timer.timeout.connect(timerCallback)
        self.keepalive_timer.start(intervalSecs * 1000)

    def onRegistered(self):
        self.log('Registered...')

    def onLineStat(self, message):
        pass

    def displayLineInfo(self, line, number):
        pass

    def setDateTime(self, day,month,year,hour,minute,seconds):
        pass

    def createOneShotTimer(self, timerInSec, timerHandler):
        pass

number_to_dial = 1003

class TestSCCPPhone(unittest.TestCase):
    def __init__(self, *args, **kwargs):
        super(TestSCCPPhone, self).__init__(*args, **kwargs)
        self.phones = [SCCPPhone(SERVER_HOST, DEVICE_NAME1),
                    SCCPPhone(SERVER_HOST, DEVICE_NAME2),
                    SCCPPhone(SERVER_HOST, DEVICE_NAME3)]
        self.lock = Lock()

    def log(self, msg):
        print(msg)

    def setUp(self):
        super(TestSCCPPhone, self).setUp()

    def tearDown(self):
        super(TestSCCPPhone, self).tearDown()

    def run_phone(self, phone, lock):
        global count
        global number_to_dial
        controller = SCCPPhoneContoller()
        phone.log = phone_log

        callActor = CallActor()
        callActor.setPhone(phone)
        callActor.setTimerProvider(controller)
        callActor.setAutoAnswer(True)

        phone.setTimerProvider(controller)
        phone.setDisplayHandler(controller)
        phone.setRegisteredHandler(controller)
        phone.setDateTimePicker(controller)
        phone.addCallHandler(callActor)
        phone.createClient()
        reactor.connectTCP(SERVER_HOST, SERVER_PORT, phone.client)
        time.sleep(0.5)
        time.sleep(phone.sleep_time)
        if phone.deviceName != DEVICE_NAME1:
            print('dialing:', number_to_dial)
            phone.dial(str(number_to_dial) + '#')

        while not self.phones[0].test_complete:
            time.sleep(0.1)
            pass

        if phone.deviceName == DEVICE_NAME1:
            print('======================= Test Passed =======================')
            if reactor.running:
                print('stopping reactor..')
                time.sleep(0.1)
                reactor.stop()
        else:
            phone.endCall(callActor.currentLine, callActor.currentCallId)

        # self.sccpPhone2 = SCCPPhone(SERVER_HOST,DEVICE_NAME2)
        # self.sccpPhone3 = SCCPPhone(SERVER_HOST,DEVICE_NAME3)

        # self.sccpPhone2.setLogger(self.log)
        # self.sccpPhone3.setLogger(self.log)

    def testOnConnectSuccess(self):
        count = 0
        for phone in self.phones:
            phone.test_complete = False
            phone.sleep_time = count
            count += 5
            Thread(target=self.run_phone, args=(phone,self.lock)).start()
        print('running reactor...')
        reactor.run()

if __name__ == "__main__":
    #import sys;sys.argv = ['', 'Test.testName']
    unittest.main()
