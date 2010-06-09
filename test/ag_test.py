import socket
import random
import time

def connect(path='/opt/collectd/var/run/collectd-aggregator'):
	s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
	s.connect(path)

	return s

def send_data(s, key, val):
	buffer = "%s-1 %f\n" % (key, val)
	s.sendall(buffer)

if __name__ == "__main__":

	s = connect()

	for x in range(1000):
		send_data(s, 'random_metric', random.random() * 1000)
		time.sleep(1)

