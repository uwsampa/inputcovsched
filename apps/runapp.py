#!/usr/bin/python -u

# still using python 2.5 :(
from __future__ import with_statement

import os
import pty
import signal
import subprocess
import sys

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

if (len(sys.argv) != 4):
    print 'Usage: %s [testname] [cloud9 command] [output base dir]\n' % sys.argv[0]
    sys.exit(1)

App = sys.argv[1]
Cloud9Cmd = sys.argv[2]
OutputBaseDir = sys.argv[3]
Verbose = (os.environ.get('VERBOSE','') != '')
UseGdb = (os.environ.get('USEGDB','') != '')
UseCpuProf = (os.environ.get('USEPROF','').lower() == 'cpu')
UseHeapProf = (os.environ.get('USEPROF','').lower() == 'heap')
UseHeapCheck = (os.environ.get('USECHECK','').lower() == 'heap')
UseValgrind = (os.environ.get('USEVALGRIND','') != '')
Bytecode = '%s/%s.bc' % (App, App)
ProgramArgs = []

print '%sExecuting %s ...%s' % (GoodColor, App, EndColor)
sys.stdout.flush()

if not os.path.isdir(App) or not os.path.exists(Bytecode):
    printError('not a valid app: %s' % App)
    sys.exit(1)

if UseGdb and (UseCpuProf or UseHeapProf or UseHeapCheck or UseValgrind):
    printError('cannot specify USEGDB and USEPROF/USECHECK at the same time')
    sys.exit(1)

#
# Library settings
#

ProfFile = '/tmp/ics.app.%s.prof' % App

#
# Run the app 
#

Ok = True

def RunShellCommand(cmd):
    if Verbose:
        print cmd
        sys.stdout.flush()
    ret = subprocess.call(cmd, shell=True)

def RunProcess(args, env):
    global Ok
    TerminalTempBasename = '%s/.terminal' % (OutputBaseDir)
    TerminalFinal = '%s/klee-last/terminal' % (OutputBaseDir)

    TerminalTemp = TerminalTempBasename
    iter = 0
    while os.path.exists(TerminalTemp):
        TerminalTemp = ('%s.%d') % (TerminalTempBasename, iter)
        iter = iter + 1

    tee = open(TerminalTemp, 'w')

    if Verbose:
        print args
        sys.stdout.flush()

        task = subprocess.Popen(args, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                bufsize=0, close_fds=True)

        while True:
            buf = task.stdout.readline()
            tee.write(buf)
            sys.stdout.write(buf)
            if buf == '' and task.poll() != None:
                break
    else:
        task = subprocess.Popen(args, stdout=tee, stderr=subprocess.STDOUT, close_fds=True)
        task.wait()

    tee.close()
    RunShellCommand('/bin/mv %s %s' % (TerminalTemp, TerminalFinal))

    if task.returncode != 0:
        if task.returncode < 0:
            signo = -task.returncode
            # Amusing hack, found via the internet
            signame = tuple(v for v,k in signal.__dict__.iteritems() if k == signo)[0]
            printError('command exited with return code %d (killed by signal %s)'
                       % (task.returncode, signame))
        else:
            printError('command exited with return code %d'
                       % (task.returncode))
        Ok = False

# Need an output dir

if not os.path.exists(OutputBaseDir):
    os.system('mkdir -p %s' % OutputBaseDir)

# Now exec Cloud9

if UseCpuProf or UseHeapProf or UseHeapCheck or UseGdb or UseValgrind:
    if UseCpuProf:
        PreCmd = 'CPUPROFILE=%s PROFILEFREQUENCY=1000' % ProfFile
    elif UseHeapProf:
        PreCmd = 'HEAPPROFILE=%s PPROF_PATH=`which pprof`' % ProfFile
    elif UseHeapCheck:
        PreCmd = 'HEAPCHECK=normal PPROF_PATH=`which pprof`'
    elif UseValgrind:
        PreCmd = 'valgrind'
    else:
        PreCmd = 'gdb --args'

    RunShellCommand('%s %s --output-base-dir=%s %s %s' %
                    (PreCmd, Cloud9Cmd, OutputBaseDir, Bytecode, ' '.join(ProgramArgs)))

else:
    Cloud9Args = Cloud9Cmd.split(' ')
    Cloud9Args.append('--output-base-dir=%s' % OutputBaseDir)
    Cloud9Args.append(Bytecode)
    Cloud9Args.extend(ProgramArgs)

    env = os.environ
    RunProcess(Cloud9Args, env)

if Ok:
    print '%sExecution completed!%s' % (GoodColor, EndColor)
else:
    print '%sExecution FAILED!%s' % (ErrorColor, EndColor)

print 'Output directory: %s/klee-last' % OutputBaseDir

if UseCpuProf:
    print 'CpuProfile: %s' % ProfFile
    RunShellCommand('pprof --pdf `which c9-worker` %s > %s.cpu.pdf' % (ProfFile, ProfFile))

if UseHeapProf:
    print 'HeapProfile: %s' % ProfFile
    RunShellCommand('pprof --pdf `which c9-worker` %s.0001.heap > %s.heap.pdf' % (ProfFile, ProfFile))

