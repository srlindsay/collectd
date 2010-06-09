
from twisted.internet import reactor, protocol

socketpath = {
		'sum':'/opt/collectd/var/run/collectd-aggregator',
		'avg':'/opt/collectd/var/run/collectd-aggregator-avg'
		}

ag_factory = {}

def init():
	global ag_factory
	global socketpath
	for k,v in socketpath.iteritems():
		ag_factory[k] = AgFactory()
		reactor.connectUNIX(v, ag_factory[k])

def send_data(key, value, method='avg'):
	global ag_factory
	if ag_factory == {}:
		init()

	fact = ag_factory[method]
	fact.send_data(key, value)

class AgClientProtocol(protocol.Protocol):

	def connectionMade(self):
		self.factory.client = self
		self.factory.send_pending_data()

	def dataReceived(self, data):
		pass

	def connectionLost(self, reason):
		pass


class AgFactory(protocol.ClientFactory):
	protocol = AgClientProtocol

	def __init__(self):
		self.client = None
		self.pending_data = []

	def send_data(self, key, value):
		buf = "%s %f\n" % (key,value)
		if self.client is None:
			self.pending_data.append(buf)
		else:
			self.client.transport.write(buf)

	def send_pending_data(self):
		if len(self.pending_data) > 0:
			for buf in self.pending_data:
				self.client.transport.write(buf)
			self.pending_data = []

	def clientConnectionMade(self, connector):
		print "connection made"

	def clientConnectionFailed(self, connector, reason):
		connector.connect()

	def clientConnectionLost(self, connector, reason):
		print "connection lost, reconnecting"
		connector.connect()

def main():
	send_data("twistedtest",10.2)
	reactor.callLater(3, send_data, "twistedtest", 50.0)
	reactor.callLater(10, reactor.stop)
	reactor.run()

if __name__ == '__main__':
	main()
