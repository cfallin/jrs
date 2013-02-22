#!/usr/bin/python

import os
import subprocess
import sys
import time
import threading
import fcntl

biglock = threading.Lock()

class Config(object):
    def __init__(self):
        self.secretfile = ''
        self.nodelist = []
        self.jrs = './jrs'
        self.port = 7082
        self.db = './db'

    def parse(self, configfile):

        self.secretfile = ''
        self.nodelist = []
        self.jrs = './jrs'
        self.port = 7082
        self.db = './db'

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
                elif words[0] == 'db' and len(words) > 1:
                    self.db = words[1]

class Node(object):

    class Req(object):
        def __init__(self, reqline, callback):
            self.reqline = reqline
            self.callback = callback

    # states
    DISCONNECTED = 0
    CONNECTED = 1

    def __init__(self, hostname, cfg):
        global biglock
        self.hostname = hostname
        self.cfg = cfg
        self.state = Node.DISCONNECTED
        self.proc = None
        self.currentreq = None
        self.reqqueue = []

    def connect(self):
        global jrs
        if self.state == Node.CONNECTED: return

        args = [self.cfg.jrs, '-s', self.cfg.secretfile, '-c',
                    '-l', str(self.cfg.port), '-r', self.hostname]
        self.proc = subprocess.Popen(args,
                executable=self.cfg.jrs, stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,stderr=None)

        fd = self.proc.stdout.fileno()
        fcntl.fcntl(fd, fcntl.F_SETFL, fcntl.fcntl(fd, fcntl.F_GETFL) &
                ~os.O_NONBLOCK);

        self.state = Node.CONNECTED

        if len(self.reqqueue) > 0:
            self.sendreq()

    def check(self):
        global biglock
        if self.state == Node.CONNECTED:
            if self.proc.returncode != None:
                self.state = Node.DISCONNECTED
                self.proc = None
                biglock.acquire()
                if self.currentreq != None:
                    if self.currentreq.callback != None:
                        self.currentreq.callback(None)
                    self.currentreq = None
                    for r in self.reqqueue:
                        if r.callback != None:
                            r.callback(None)
                    self.reqqueue = []
                biglock.release()
                return False
            # check if anything has come in on pipe
            line = self.proc.stdout.readline()
            if line != '' and self.currentreq != None:
                if self.currentreq.callback != None:
                    biglock.acquire()
                    self.currentreq.callback(line.strip())
                    biglock.release()
                self.currentreq = None
                if len(self.reqqueue) > 0:
                    self.sendreq()
            return True
        return True

    def connected(self):
        return self.state == Node.CONNECTED

    def request(self, reqline, callback):
        global biglock
        biglock.acquire()
        req = Node.Req(reqline, callback)
        self.reqqueue.append(req)
        if self.currentreq == None and self.state == Node.CONNECTED:
            self.sendreq()
        biglock.release()

    def sendreq(self):
        if self.state != Node.CONNECTED: return
        if self.currentreq != None: return
        if len(self.reqqueue) == 0: return
        self.currentreq = self.reqqueue[0]
        self.reqqueue = self.reqqueue[1:]
        self.proc.stdin.write(self.currentreq.reqline + "\r\n")
        self.proc.stdin.flush()

class NodeThread(threading.Thread):
    def __init__(self, node):
        threading.Thread.__init__(self)
        self.node = node

    def run(self):
        while True:
            conn = False
            if self.node.connected(): conn = self.node.check()
            else: self.node.connect()
            if not conn: time.sleep(0.1)


class Job(object):
    QUEUED = 1
    RUNNING = 2
    def __init__(self, config, path):
        self.config = config
        self.cwd = ''
        self.cmdline = ''
        self.path = path
        self.state = Job.QUEUED
        self.hostname = ''
        self.jobid = ''

    def abspath(self):
        p = self.config.db + '/' + self.path
        return p

    def mkdir_p(self, d):
        if not os.path.exists(os.path.dirname(d)):
            self.mkdir_p(os.path.dirname(d))
        if not os.path.exists(d):
            os.mkdir(d)

    def dump(self):
        p = self.abspath()
        try:
            self.mkdir_p(p)
            with open(p + '/cwd', 'w') as of:
                of.write(self.cwd)
            with open(p + '/cmdline', 'w') as of:
                of.write(self.cmdline)
            if self.state == Job.RUNNING:
                with open(p + '/executing', 'w') as of:
                    of.write('%s,%s' % (self.hostname, self.jobid))
        except:
            pass

    def load(self):
        p = self.abspath()
        try:
            with open(p + '/cwd') as i:
                self.cwd = i.read()
            with open(p + '/cmdline') as i:
                self.cmdline = i.read()
            self.state = Job.QUEUED
            if os.path.exists(p + '/executing'):
                with open(p + '/executing') as i:
                    self.hostname, self.jobid = i.read().split(',')
        except: pass

    def remove(self):
        p = self.abspath()
        try:
            os.unlink(p + '/cwd')
            os.unlink(p + '/cmdline')
            os.unlink(p + '/executing')
            os.rmdir(p)
            # remove any parent directories, up to the root...
            def rmparent(p):
                parent = os.path.dirname(p)
                if parent.startswith(self.config.db) and len(os.listdir(parent)) <= 2:
                    os.rmdir(parent)
                    rmparent(parent)
            rmparent(p)
        except:
            pass

# db/
#    jobpath1/
#       jobpath2/
#          jobpath3/   (running job)
#              cwd       -> contains '/home/myuser'
#              cmdline   -> contains './simulator -a -b -c' 
#              executing -> contains 'hostname,jobid'
#          jobpath4/   (queued job)
#              cwd       -> contains '/home/myser'
#              cmdline   -> contains './simulator -a -b -c'

class Database(object):
    def __init__(self, config):
        self.config = config
        self.jobs = {}

    def load(self):
        for (dirpath, dirnames, filenames) in os.walk(self.config.db):
            if 'cmdline' in filenames:
                j = Job(self.config, dirpath)
                self.jobs[dirpath] = j

    def mkdir_p(self, d):
        if not os.path.exists(os.path.dirname(d)):
            self.mkdir_p(os.path.dirname(d))
        if not os.path.exists(d):
            os.mkdir(d)

    def newjob(self, path):
        j = Job(self.config, path)
        self.jobs[path] = j
        print "db: new job", j, "jobs is", self.jobs
        j.dump()
        return j

    def getprefix(self, prefix=''):
        i = 1
        p = ''
        while True:
            p = prefix + '/' + str(i)
            i += 1
            if not os.path.exists(self.config.db + '/' + p): break
        self.mkdir_p(self.config.db + '/' + p)
        return p

class Cluster(object):

    def __init__(self, config, db):
        self.config = config
        self.db = db
        self.nodes = {}
        self.jobs = []

        for h in config.nodelist:
            n = Node(h, config)
            n.hostname = h
            self.nodes[h] = n
            n.connect()
            n.jobids = []
            n.cores = 0
            n.loadavg = 0
            n.memory = 0
            nth = NodeThread(n)
            nth.start()

    def update_jobs(self):
        for n in self.nodes.values():

            def L_callback(line):
                if line == None: return
                jobids = line.split()
                n.jobids = jobids
            def S_callback(line):
                if line == None: return
                cores, loadavg, memory = line.split()
                n.cores = int(cores)
                n.loadavg = float(loadavg)
                n.memory = int(memory)

            n.request('L', L_callback)
            n.request('S', S_callback)

    def killjob(self, job):

        l = 'K %s' % job.jobid
        n = self.nodes[job.hostname]

        def K_callback(line):
            # retry on disconnect cancelation
            if line == None:
                n.request(l, K_callback)

        # initial enqueue
        K_callback(None)

        # remove right away
        del self.db.jobs[j.path]
        j.remove()


    def startjob(self, job, host):

        l = 'N %s;%s' % (job.cwd, job.cmdline)
        job.hostname = host
        n = self.nodes[host]

        def N_callback(line):
            if line == None:
                n.request(l, N_callback)
            else:
                job.jobid = line
                job.state = Job.RUNNING
                print "job path '%s' got id '%s' on host '%s'" % \
                        (job.path, job.jobid, job.hostname)
                job.dump()

        N_callback(None)

class Controller(object):

    def __init__(self):
        pass

    def step(self, cluster, database):
        # access cluster.nodes; each node has node.jobids (list),
        # node.hostname, node.cores, node.memory, node.loadavg
        #
        # access database.jobs[path]; each job has job.state (Job.QUEUED or
        # Job.RUNNING), job.path
        #
        # call cluster.killjob(job_object), cluster.startjob(job_object,
        # hostname). To create a new job object, call database.newjob(path);
        # use database.getprefix(initprefix) to get initprefix/%d (first unique)
        # after which job names can be postpended.

        jobcmd = 'sleep 30'
        totalcores = 0
        jobid_executing = 0
        print("All nodes:")
        has_empty_cores = []
        for n in cluster.nodes.values():
            print("Host: %s; Cores: %d; Memory: %d; Load: %f" % \
                    (n.hostname, n.cores, n.memory, n.loadavg))
            totalcores += n.cores
            for j in n.jobids:
                print("* jobid: %s" % j)
                jobid_executing += 1
            if len(n.jobids) < n.cores:
                has_empty_cores.append(n.hostname)

        executing = 0
        print("All jobs:")
        for j in database.jobs.values():
            print(" path '%s': state %s" % (j.path, str(j.state)))
            if j.state == Job.RUNNING:
                print("    --> host %s jobid %s" % (j.hostname, j.jobid))
                executing += 1

        print("Executing: %d; executing according to DB: %d; Total cores: %d" % \
                (executing, jobid_executing, totalcores))
        if jobid_executing < totalcores:
            pref = database.getprefix('testjobs')
            j = database.newjob(pref)
            j.cmdline = jobcmd
            j.cwd = '/'
            cluster.startjob(j, has_empty_cores[0])
            print("Spawned job path '%s' to host '%s'." % (j.path, has_empty_cores[0]))

def main(args):
    c = Config()
    c.parse(args[0])
    db = Database(c)
    c = Cluster(c, db)

    p = Controller()
    
    global biglock

    while True:
        print "main loop top"
        c.update_jobs()
        p.step(c, db)

        time.sleep(1)

if __name__ == '__main__':
    main(sys.argv[1:])
