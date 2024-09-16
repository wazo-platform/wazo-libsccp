'''
Created on Jun 20, 2011

@author: lebleu1
'''
from network.sccpclientfactory import SCCPClientFactory
from sccp.sccpmessagetype import SCCPMessageType
from sccp.sccpregister import SCCPRegister
from sccp.sccpcapabilities import SCCPCapabilitiesRes
from sccp.sccpbuttontemplatereq import SCCPButtonTemplateReq
from sccp.sccpregisteravailablelines import SCCPRegisterAvailableLines
from sccp.sccptimedatereq import SCCPTimeDateReq
from sccp.sccpcallstate import SCCPCallState
from sccp.sccpkeypadbutton import SCCPKeyPadButton
from sccp.sccpsoftkeyevent import SCCPSoftKeyEvent
from gui.softkeys import SKINNY_LBL_NEWCALL, SKINNY_LBL_ANSWER,\
    SKINNY_LBL_ENDCALL
from sccp.sccpmessage import SCCPMessage
from sccp.sccplinestatreq import SCCPLineStatReq

class SCCPPhone():
    '''
    Main sccp phone class
    '''


    def __init__(self,host,deviceName):
        self.host = host
        self.deviceName = deviceName
        self.callHandlers = set()

    def setLogger(self,logger):
        self.log = logger

    def setTimerProvider(self,timerProvider):
        self.timerProvider = timerProvider

    def setRegisteredHandler(self,registeredHandler):
        self.registeredHandler = registeredHandler

    def setDateTimePicker(self,dateTimePicker):
        self.dateTimePicker = dateTimePicker

    def setDisplayHandler(self,displayHandler):
        self.displayHandler = displayHandler

    def addCallHandler(self,callHandler):
        self.log(self.deviceName + ' adding call handler')
        self.callHandlers.add(callHandler)

    def createClient(self):
        self.log('creating sccp client factory')
        self.client = SCCPClientFactory(
                        self.on_sccp_connect_success,
                        self.on_sccp_connect_fail)
        self.client.handleUnknownMessage(self.onUnknownMessage)
        self.client.addHandler(SCCPMessageType.RegisterAckMessage,self.onRegisteredAck)
        self.client.addHandler(SCCPMessageType.CapabilitiesReqMessage,self.onCapabilitiesReq)
        self.client.addHandler(SCCPMessageType.KeepAliveAckMessage,self.onKeepAliveAck)
        self.client.addHandler(SCCPMessageType.DefineTimeDate,self.onDefineTimeDate)
        self.client.addHandler(SCCPMessageType.SetSpeakerModeMessage,self.onSetSpeakerMode)
        self.client.addHandler(SCCPMessageType.CallStateMessage,self.onCallState)
        self.client.addHandler(SCCPMessageType.ActivateCallPlaneMessage,self.onActivateCallPlane)
        self.client.addHandler(SCCPMessageType.StartToneMessage,self.onStartTone)
        self.client.addHandler(SCCPMessageType.LineStatMessage,self.onLineStat)
        self.client.addHandler(SCCPMessageType.RegisterRejectMessage,self.onRegisterRejectMessage)
        self.client.addHandler(SCCPMessageType.SetRingerMessage,self.onSetRingerMessage)
        return self.client

    def on_sccp_connect_success(self):
        self.log('Connected to server. Sending register with phone set : ' + self.deviceName)
        registerMessage = SCCPRegister(self.deviceName, self.host)
        self.client.sendSccpMessage(registerMessage)

    def on_sccp_connect_fail(self, reason):
        # reason is a twisted.python.failure.Failure  object
        self.log('Connection failed: %s' % reason.getErrorMessage())

    def onRegisterRejectMessage(self, message):
        self.log('register failed ' + message.toStr())

    def onSetRingerMessage(self, message):
        self.log('ringer mode: ' + message.toStr())

    def onKeepAliveTimer(self):
        self.log('on keep alive')
        message = SCCPMessage(SCCPMessageType.KeepAliveMessage)
        self.client.sendSccpMessage(message)

    def onUnknownMessage(self,message):
        self.log('receive unkown message ' + message.toStr())

    def onRegisteredAck(self,registerAck):
        self.log("sccp phone registered")
        self.log("--          keepAliveInterval : " + `registerAck.keepAliveInterval`)
        self.log("--               dateTemplate : " + `registerAck.dateTemplate`)
        self.log("-- secondaryKeepAliveInterval : " + `registerAck.secondaryKeepAliveInterval`)
        self.timerProvider.createTimer(registerAck.keepAliveInterval,self.onKeepAliveTimer)
        self.registeredHandler.onRegistered()


    def onKeepAliveAck(self,message):
        self.log("Keepalive ack")

    def onCapabilitiesReq(self,message):
        self.log("sending capabilities response")
        self.client.sendSccpMessage(SCCPCapabilitiesRes())
        self.log("sending button template request message")
        self.client.sendSccpMessage(SCCPButtonTemplateReq())
        self.log("sending line status request message")
        self.client.sendSccpMessage(SCCPLineStatReq(1))
        self.log("sending register available lines")
        self.client.sendSccpMessage(SCCPRegisterAvailableLines())
        self.log("sending time date request message")
        self.client.sendSccpMessage(SCCPTimeDateReq())


    def onDefineTimeDate(self,message):
        self.log('define time and date')
        self.dateTimePicker.setDateTime(message.day,message.month,message.year,message.hour,message.minute,message.seconds)

    def onSetSpeakerMode(self,message):
        self.log('set speaker mode '+`message.mode`)

    def onCallState(self,message):
        self.log('call state line : ' + `message.line` + ' for callId '+ `message.callId` + ' is ' + SCCPCallState.sccp_channelstates[message.callState])
        self.currentLine = message.line
        self.currentCallId=message.callId
        self.callState=message.callState

        for callHandler in self.callHandlers:
            callHandler.handleCall(message.line,message.callId,message.callState)

    def onLineStat(self,message):
        self.log('line stat ' + `message.line` + ' : ' + `message.dirNumber`)
        self.displayHandler.displayLineInfo(message.line,message.dirNumber)

    def onStartTone(self,message):
        self.log('start tone : '+`message.tone` + ' timeout ' + `message.toneTimeout` + ' line ' + `message.line` + ' for callId '+ `message.callId`)

    def onActivateCallPlane(self,message):
        self.log('Activate call plane on line '+`message.line`)

    def onDialPadButtonPushed(self,car):
        self.log("dialed : " + car)
        if (car == '#'):
            event = 15
        elif (car == '*'):
            event = 14
        else:
            event = int(car)
        message = SCCPKeyPadButton(event)
        self.client.sendSccpMessage(message)

    def dial(self,numberToDial):
        self.log('dialing : '+numberToDial)
        self.client.sendSccpMessage(SCCPSoftKeyEvent(SKINNY_LBL_NEWCALL))
        for digit in numberToDial:
            self.onDialPadButtonPushed(digit)

    def onSoftKey(self,event):
        self.log('on soft key '+`event`)
        if (event != SKINNY_LBL_NEWCALL):
            message = SCCPSoftKeyEvent(event,self.currentLine,self.currentCallId)
        else:
            message = SCCPSoftKeyEvent(event)
        self.client.sendSccpMessage(message)

    def answerCall(self):
        self.onSoftKey(SKINNY_LBL_ANSWER)

    def endCall(self,line,callId):
        message = SCCPSoftKeyEvent(SKINNY_LBL_ENDCALL,line,callId)
        self.client.sendSccpMessage(message)
