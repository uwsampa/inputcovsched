#!/usr/bin/python -u

# still using python 2.5 :(
from __future__ import with_statement

import os
import pty
import re
import signal
import subprocess
import sys
import threading

#
# Printing with errors
#

GoodColor    = '\033[92m'  # green
ErrorColor   = '\033[91m'  # red
WarningColor = '\033[93m'  # yellow
EndColor     = '\033[0m'

def printError(msg):
    print '%sERROR:%s %s' % (ErrorColor, EndColor, msg)

def printWarning(msg):
    print '%sWARNING:%s %s' % (WarningColor, EndColor, msg)

#
# MAIN
#

if (len(sys.argv) < 2):
    print 'Usage: %s command [args]\n' % sys.argv[0]
    sys.exit(1)

Command = sys.argv[1:]
Verbose = (os.environ.get('VERBOSE','0') != '0')
Timeout = float(os.environ.get('TIMEOUT','0'))

print '%sRunning %s with timeout=%f ...%s' % (GoodColor, ' '.join(Command), Timeout, EndColor)
sys.stdout.flush()

Ok = True
Task = None

def RunProcess(args, env):
    global Ok
    global Task
    global Timeout

    def RunThread():
        global Task
        if Verbose:
            print args
            sys.stdout.flush()

            Task = subprocess.Popen(args, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    bufsize=0, close_fds=True)

            while True:
                buf = Task.stdout.readline()
                sys.stdout.write(buf)
                if buf == '' and Task.poll() != None:
                    break
        else:
            Task = subprocess.Popen(args, stdout=open(os.devnull, 'wb'), stderr=subprocess.STDOUT)
            Task.wait()

    thread = threading.Thread(target=RunThread)
    thread.start()
    if Timeout != 0:
        thread.join(Timeout)
    else:
        thread.join()

    if thread.isAlive():
        self.process.terminate()
        thread.join()
        printError('command timed out')
        return

    if Task.returncode != 0:
        if Task.returncode < 0:
            signo = -Task.returncode
            # Amusing hack, found via the internet
            signame = tuple(v for v,k in signal.__dict__.iteritems() if k == signo)[0]
            printError('command exited with return code %d (killed by signal %s)'
                       % (Task.returncode, signame))
        else:
            printError('command exited with return code %d'
                       % (Task.returncode))
        Ok = False

RunProcess(Command, os.environ)

# Report

if Ok:
    print '%sExecution success!%s' % (GoodColor, EndColor)
else:
    print '%sExecution FAILED!%s' % (ErrorColor, EndColor)

