#!/usr/bin/python

import os
import subprocess
import sys
import fcntl
import time

class Config(object):
    def __init__(self):
        self.secretfile = ''
        self.nodelist = []
        self.jrs = './jrs'
        self.port = 7082

    def parse(self, configfile):

        self.secretfile = ''
        self.nodelist = []
        self.jrs = './jrs'
        self.port = 7082

        with open(configfile) as of:
            for line in of.readlines():
                line = line.strip()
                if line == '': continue
                if line[0] == '#': continue

                words = line.split()

                if words[0] == 'secret' and len(words) > 1:
                    self.secretfile = words[1]
                elif words[0] == 'node':
                    self.nodelist.extend(words[1:])
                elif words[0] == 'port' and len(words) > 1:
                    self.port = int(words[1])
                elif words[0] == 'jrs' and len(words) > 1:
                    self.jrs = words[1]

class Node(object):

    class Req(object):
        def __init__(self, reqline, callback):
            self.reqline = reqline
            self.callback = callback

    # states
    DISCONNECTED = 0
    CONNECTED = 1

    def __init__(self, hostname, cfg):
        self.hostname = hostname
        self.cfg = cfg
        self.state = Node.DISCONNECTED
        self.proc = None
        self.currentreq = None
        self.reqqueue = []

    def connect(self):
        global jrs
        if self.state == Node.CONNECTED: return

        self.proc = subprocess.Popen(
                [self.cfg.jrs, '-s', self.cfg.secretfile, '-c',
                    '-l', str(self.cfg.port), '-r', self.hostname],
                executable=self.cfg.jrs, stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,stderr=None)

        fd = self.proc.stdout.fileno()
        fcntl.fcntl(fd, fcntl.F_SETFL, fcntl.fcntl(fd, fcntl.F_GETFL) |
                os.O_NONBLOCK)

        self.state = Node.CONNECTED

        if len(self.reqqueue) > 0:
            self.sendreq()

    def check(self):
        if self.state == Node.CONNECTED:
            if self.proc.returncode != None:
                self.state = Node.DISCONNECTED
                self.proc = None
                if self.currentreq != None:
                    self.currentreq.callback(None)
                    self.currentreq = None
                    for r in self.reqqueue:
                        r.callback(None)
                    self.reqqueue = []
            # check if anything has come in on pipe
            line = ''
            try: line = self.proc.stdout.readline()
            except: pass
            if line != '' and self.currentreq != None:
                self.currentreq.callback(line.strip())
                self.currentreq = None
                if len(self.reqqueue) > 0:
                    self.sendreq()

    def connected(self):
        return self.state == Node.CONNECTED

    def request(self, reqline, callback):
        req = Node.Req(reqline, callback)
        self.reqqueue.append(req)
        if self.currentreq == None and self.state == Node.CONNECTED:
            self.sendreq()

    def sendreq(self):
        if self.state != Node.CONNECTED: return
        if self.currentreq != None: return
        if len(self.reqqueue) == 0: return
        self.currentreq = self.reqqueue[0]
        self.reqqueue = self.reqqueue[1:]
        self.proc.stdin.write(self.currentreq.reqline + "\r\n")
        self.proc.stdin.flush()

def main(args):
    c = Config()
    c.parse(args[0])
    n = Node('localhost', c)
    n.connect()
    while True:
        n.check()
        if not n.connected():
            print("Disconnected")
            break
        def callback(line):
            print("ret: " + line)
        n.request("S", callback)
        time.sleep(1)

if __name__ == '__main__':
    main(sys.argv[1:])
