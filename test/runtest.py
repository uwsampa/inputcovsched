#!/usr/bin/python -u

# still using python 2.5 :(
from __future__ import with_statement

import os
import pty
import re
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

Test = sys.argv[1]
Cloud9Cmd = sys.argv[2]
OutputBaseDir = sys.argv[3]
Verbose = (os.environ.get('VERBOSE','0') != '0')
UseGdb = (os.environ.get('USEGDB','0') != '0')
UseCpuProf = (os.environ.get('USEPROF','').lower() == 'cpu')
UseHeapProf = (os.environ.get('USEPROF','').lower() == 'heap')

print '%sTesting %s ...%s' % (GoodColor, Test, EndColor)
sys.stdout.flush()

if (not os.path.isdir(Test)
        or not os.path.exists(Test + '/test.bc')
        or not os.path.exists(Test + '/test.cpp')):
    printError('not a valid test: %s' % Test)
    sys.exit(1)

if UseGdb and (UseCpuProf or UseHeapProf):
    printError('cannot specify USEGDB and USEPROF at the same time')
    sys.exit(1)

#
# Load test manifest
#

ManifestDir = Test + '/expected-output'
ManifestFile = ManifestDir + '/manifest'
TestManifest = {}
Verified = True

if not os.path.isdir(ManifestDir) or not os.path.exists(ManifestFile):
    printWarning("manifest file %s not found, won't verify results" % ManifestFile)
    TestManifest['expected'] = ()
    TestManifest['unexpected'] = ()
    Verified = False
else:
    execfile(ManifestFile, TestManifest)
    for key in ('expected','unexpected'):
        if not TestManifest.has_key(key):
            printWarning("manifest file %s missing list of '%s' files" % (ManifestFile,key))
            TestManifest[key] = ()

#
# Library settings
#

ProfFile = '/tmp/ics.test.%s.prof' % Test

#
# Look for extra args
# These are the first few lines of the test.cpp file that start with "//cl:"
#

FirstSrcLineMagic = '//cl: '
with open(Test + '/test.cpp', 'r') as f:
    line = f.readline().rstrip()
    while line.startswith(FirstSrcLineMagic):
        Cloud9Cmd += ' ' + line[len(FirstSrcLineMagic):]
        line = f.readline().rstrip()

#
# Run the test
#

Ok = True

def RunShellCommand(cmd):
    if Verbose:
        print cmd
        sys.stdout.flush()
    os.system(cmd)

def RunProcess(args, env):
    global Ok
    TerminalTemp = '%s/.terminal' % (OutputBaseDir)
    TerminalFinal= '%s/klee-last/terminal' % (OutputBaseDir)
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

if UseCpuProf or UseHeapProf or UseGdb:
    if UseCpuProf:
        PreCmd = 'CPUPROFILE=%s PROFILEFREQUENCY=1000' % ProfFile
    elif UseHeapProf:
        PreCmd = 'HEAPPROFILE=%s' % ProfFile
    else:
        PreCmd = 'gdb --args'

    RunShellCommand('%s %s --output-base-dir=%s %s/test.bc' %
                    (PreCmd, Cloud9Cmd, OutputBaseDir, Test))

else:
    Cloud9Args = Cloud9Cmd.split(' ')
    Cloud9Args.append('--output-base-dir=%s' % OutputBaseDir)
    Cloud9Args.append('%s/test.bc' % Test)

    env = os.environ
    RunProcess(Cloud9Args, env)

#
# Check the outputs
#

def GetOutputFile(basename):
    return '%s/klee-last/%s' % (OutputBaseDir,basename)

def GetExpectedFile(basename):
    return '%s/%s' % (ManifestDir,basename)

def CheckIfExists(basename):
    global Ok
    if not os.path.exists(GetOutputFile(basename)):
        printError("expected output file %s not found" % basename)
        Ok = False
        return False
    else:
        return True

def CheckIfExistsAndReadContents(basename):
    if CheckIfExists(basename):
        return open(GetOutputFile(basename), 'r').read()
    else:
        return ''

def SameOutputFiles(a, b):
    # Ignore LLVM dbg symbol numbers
    ignore = re.compile(r'!dbg !\d+')
    a = ignore.sub('!dbg !xxxx', a)
    b = ignore.sub('!dbg !xxxx', b)
    # Ignore raw memory addresses (these show up in stack traces)
    ignore = re.compile(r'0x[a-fA-F0-9]+ (.*:)')  # addr file:line in regionSummary
    a = ignore.sub('0xXXXX \2', a)
    b = ignore.sub('0xXXXX \2', b)
    # Ignore lines that start with # (comments)
    ignore = re.compile(r'^#.*\n', re.MULTILINE)
    a = ignore.sub('', a)
    b = ignore.sub('', b)
    return a == b

# Expected

GroupMapping = {}

for group in TestManifest['expected']:
    if isinstance(group, tuple) or isinstance(group, list):
        # Build sets of expected and actual file contents
        actual   = [CheckIfExistsAndReadContents(f) for f in group]
        expected = [open(GetExpectedFile(f), 'r').read() for f in group]
        mapped   = [False for f in group]
        # Match individual files
        L = len(group)
        for a in xrange(L):
            fileok = False
            # For the first group: setup mappings from expected[]->actual[]
            if a not in GroupMapping:
                for e in xrange(L):
                    if mapped[e]:
                        continue
                    if SameOutputFiles(actual[a], expected[e]):
                        GroupMapping[a] = e
                        mapped[e] = True
                        fileok = True
                        break
            # For the following groups: use mappings
            else:
                if SameOutputFiles(actual[a], expected[GroupMapping[a]]):
                    fileok = True
            # Ensure a match was found for actual[a]
            if not fileok:
                printError("no match found for contents of (actual) output file %s" % group[a])
                Ok = False
    else:
        # Match an individual file
        actual   = CheckIfExistsAndReadContents(group)
        expected = open(GetExpectedFile(group), 'r').read()
        if not SameOutputFiles(actual, expected):
            printError("no match found for contents of (actual) output file %s" % group)
            Ok = False

# Unexpected
for f in TestManifest['unexpected']:
    file = '%s/klee-last/%s' % (OutputBaseDir,f)
    if os.path.exists(file):
        printError("unexpected output file %s found" % f)
        Ok = False

# Check if the warnings file printed any errors
warningsFile = GetOutputFile('warnings.txt')
if not os.path.exists(warningsFile):
    printError("warnings.txt file does not exist")
    Ok = False
elif re.search('KLEE: ERROR', open(warningsFile, 'r').read()) != None:
    printError("warnings.txt file had errors")
    Ok = False

# Report mappings to help debug
# N.B.: This assumes that grouped files are named {"*0001", "*0002", etc.}
if not Ok:
    for k in GroupMapping:
       print "Expected[%04d] -> Actual[%04d]" % (k+1, GroupMapping[k]+1)

# Report

if not Verified:
    print '%sWARNING:%s Test not verified' % (WarningColor, EndColor)
elif Ok:
    print '%sTest passed!%s' % (GoodColor, EndColor)
else:
    print '%sTest FAILED!%s' % (ErrorColor, EndColor)

print 'Output directory: %s/klee-last' % OutputBaseDir

if UseCpuProf:
    print 'CpuProfile: %s' % ProfFile
    RunShellCommand('pprof --pdf `which c9-worker` %s > %s.cpu.pdf' % (ProfFile, ProfFile))

if UseHeapProf:
    print 'HeapProfile: %s' % ProfFile
    RunShellCommand('pprof --pdf `which c9-worker` %s.0001.heap > %s.heap.pdf' % (ProfFile, ProfFile))

