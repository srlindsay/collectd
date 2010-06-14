import logging
from twisted.internet import reactor, protocol

socketpath = {
		'sum':'/var/run/collectd-aggregator',
		'avg':'/var/run/collectd-aggregator-avg'
		}

ag_factory = {}

def init():
	global ag_factory
	global socketpath
	for k,v in socketpath.iteritems():
		ag_factory[k] = AgFactory()
		reactor.connectUNIX(v, ag_factory[k])

def send_data(plugin, plugin_instance="", type_instance="", value=0.0, method='avg'):
	"""
	builds the key that uniquely identifies the metric you're collecting.
	"%(plugin)-%(plugin_inst
ance)/gauge-%(type_instance)"
	For instance, if you're measuring a meeboasync metric, you could set
	plugin=meeboasync, plugin_instance=<handler domain name>, 
	type_instance=<specific metric>
	"""
	global ag_factory
	if ag_factory == {}:
		init()

	key = (plugin, plugin_instance, type_instance)
	fact = ag_factory[method]
	fact.send_data(key, value)

class AgClientProtocol(protocol.Protocol):

	def connectionMade(self):
		self.factory.client = self
		self.factory.send_pending_data()

	def dataReceived(self, data):
		pass

	def connectionLost(self, reason):
		self.factory.client = None


class AgFactory(protocol.ClientFactory):
	protocol = AgClientProtocol

	def __init__(self):
		self.client = None
		self.pending_data = []

	def send_data(self, key, value):
		plugin, plugin_instance, type_instance = key
		if plugin_instance:
			pi_str = "-%s" % plugin_instance
		else:
			pi_str = ""

		buf = "%s %s %s %f\n" % (plugin, plugin_instance, type_instance ,value)
		if self.client is None:
			if len(self.pending_data) < 10000:
				self.pending_data.append(buf)
			else:
				logging.error("No valid connection to collectd and queue is full -- discarding data point")
		else:
			self.client.transport.write(buf)

	def send_pending_data(self):
		if len(self.pending_data) > 0:
			for buf in self.pending_data:
				self.client.transport.write(buf)
			self.pending_data = []

	def clientConnectionFailed(self, connector, reason):
		print "connection attempt to collectd daemon failed, reconnecting"
		reactor.callLater(5, connector.connect)

	def clientConnectionLost(self, connector, reason):
		print "lost connection to collectd daemon, reconnecting"
		connector.connect()

def main():
	send_data("twistedtest",10.2)
	reactor.callLater(3, send_data, "twistedtest", 50.0)
	reactor.callLater(10, reactor.stop)
	reactor.run()

if __name__ == '__main__':
	main()
