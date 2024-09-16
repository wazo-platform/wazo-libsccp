'''
Created on Jun 14, 2011

@author: lebleu1
'''

from struct import unpack
from sccp.sccpmessagetype import SCCPMessageType
from sccp.sccpregisterack import SCCPRegisterAck
from sccp.sccpmessage import SCCPMessage
from sccp.sccpcapabilitiesreq import SCCPCapabilitiesReq
from sccp.sccpkeepaliveack import SCCPKeepAliveAck
from sccp.sccpdefinetimedate import SCCPDefineTimeDate
from sccp.sccpsetspeakermode import SCCPSetSpeakerMode
from sccp.sccpcallstate import SCCPCallState
from sccp.sccpactivatecallplane import SCCPActivateCallPlane
from sccp.sccpstarttone import SCCPStartTone
from sccp.sccplinestat import SCCPLineStat

class MessageFactory():
    '''
    sccp message factory create message from received buffer
    '''

    messages = {
                SCCPMessageType.RegisterAckMessage: SCCPRegisterAck,
                SCCPMessageType.CapabilitiesReqMessage: SCCPCapabilitiesReq,
                SCCPMessageType.KeepAliveAckMessage: SCCPKeepAliveAck,
                SCCPMessageType.DefineTimeDate: SCCPDefineTimeDate,
                SCCPMessageType.SetSpeakerModeMessage: SCCPSetSpeakerMode,
                SCCPMessageType.CallStateMessage: SCCPCallState,
                SCCPMessageType.ActivateCallPlaneMessage: SCCPActivateCallPlane,
                SCCPMessageType.StartToneMessage: SCCPStartTone,
                SCCPMessageType.LineStatMessage:SCCPLineStat}

    def __init__(self):
        '''
        '''
    def create(self,buffer):
        messageType = unpack("I",buffer[4:8])[0]
        msg = SCCPMessage(messageType)

        if messageType in self.messages:
            msg = self.messages[messageType]()

        return msg
