#!/usr/bin/env python3

# A 'reflector' for the fs123 distributed cache peer discovery
# "protocol".  The idea is that the distributed cache code is intended
# to work with multicast, but if multicast isn't available, peer
# discovery can bounce unicast UDP packets off this server instead.
#
# Upon receipt of a packet, this server will reflect it back to all
# (subject to rate-limiting) the clients that have pinged it in the
# last 5 minutes.
#
# Rudimentary rate-limiting:
#   - any packet that arrives from a server less than 10 seconds after
#     a previously reflected packet will be ignored.
#
#   - if the number of clients seen in the last 5 minutes, N, is
#     greater than 50, then reflection will occur with probability
#     50/N.  So, on average, no more than 50 packets will be reflected
#     for every non-ignored incoming packet.
#
# With 100 nodes on a switch, each sending one packet per minute, the
# total message traffic is 5000 packets per minute or about 83 packets
# per second.  That shouldn't stress any network.  Even if they all go
# berserk and send one per second, that's about 100 packets per second
# inbound and about 5000 packets every 10 seconds, or 500 packets per
# second outbound, which is still perfectly manageable.

import socket;
import sys;
import time
import random
from collections import defaultdict
import logging

# options:  port number?  logging options?  rate-limiting parameters

logging.basicConfig(level=logging.DEBUG)

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
my_addr = ('', 4444)
sock.bind(my_addr)
contacts = defaultdict(int)

while True:
    data, address = sock.recvfrom(4096)
    logging.info(f"received {int(data[0])} {data[1:-1]} packet from {address}")
    now = time.time()
    last = contacts[address]
    # N.B.  There's a multiplier/amplification here.
    # Don't let things get out of hand...
    if now - last < 10:
        # no more than one loop every 10 seconds from any given address
        # N.B.  this might drop a bona fide 'ABSENT' if it's too soon
        # after a 'PRESENT'.  C'est la vie.
        logging.warning(f"too fast: now={now} last={last}, addr={address}")
        continue 
    contacts[address] = now
    f = 50./(len(contacts)+1) # no more than 50 contacts per loop (average)
    if f < 1.0:
        logging.warning(f"Too many contacts {len(contacts)}.  Will only resend to {f} of them")
    for addr,when in list(contacts.items()):
        if(random.random() < f):
            logging.debug(f"sendto: {addr}")
            sock.sendto(data, 0, addr)
        if now - when > 300:
            del contacts[addr]
            
sys.exit(0)
