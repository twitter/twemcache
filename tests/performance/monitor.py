__doc__='''
Sending stats to the server for a while.
Duration is specified as an argument or it goes on forever.
'''

import sys

from lib.utilities import *
from lib.common import Monitor

server = startServer()

print "sending stats command to server\n"
mcm = Monitor()
mcm.run(int(sys.argv[-1]))
print "stats collected"

stopServer(server)
