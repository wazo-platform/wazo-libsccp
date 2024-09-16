'''
Created on Jun 14, 2011

@author: lebleu1
'''
from twisted.internet.protocol import ClientFactory
from network.sccpclientprotocol import SCCPClientProtocol

class SCCPClientFactory(ClientFactory):
    """ Created with callbacks for connection and receiving.
        send_msg can be used to send messages when connected.
    """
    protocol = SCCPClientProtocol
    UNKNOWN_KEY = 'UNKNOWN'

    def __init__(
            self,
            connect_success_callback,
            connect_fail_callback):
        self.connect_success_callback = connect_success_callback
        self.connect_fail_callback = connect_fail_callback
        self.client = None
        self.messageHandlers={}

    def clientConnectionFailed(self, connector, reason):
        self.connect_fail_callback(reason)

    def clientReady(self, client):
        self.client = client
        self.connect_success_callback()

    def send_msg(self, msg):
        if self.client:
            self.client.sendString(msg)

    def sendSccpMessage(self,message):
        if self.client:
            self.client.sendString(message.pack())

    def clientConnectionLost(self, connector, reason):
        print 'Lost connection.  Reason:', reason

    def addHandler(self,messageType,callback):
        self.messageHandlers[messageType]=callback

    def handleUnknownMessage(self,unknownHandler):
        self.messageHandlers[self.UNKNOWN_KEY]=unknownHandler

    def handleMessage(self,message):
        if (self.messageHandlers.has_key(message.sccpmessageType)):
            self.messageHandlers[message.sccpmessageType](message)
        else:
            if (self.messageHandlers.has_key(self.UNKNOWN_KEY)):
                self.messageHandlers[self.UNKNOWN_KEY](message)
            else:
                print "ERROR unknown message "+str(message.sccpmessageType) +" no handler"
