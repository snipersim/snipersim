import datetime, subprocess, fcntl, time, os, sys, signal

# exception class
class Timeout(Exception): pass


def command(command, timeout, pollinterval = .1, return_stderr = False):
    """call shell-command and either return its output or kill it
    if it doesn't normally exit within timeout seconds and return None"""

    start = datetime.datetime.now()
    process = subprocess.Popen([ 'bash', '-c', command ], stdout = subprocess.PIPE, stderr = subprocess.PIPE)

    outtxt = ''; errtxt = ''
    outfd = process.stdout.fileno(); fcntl.fcntl(outfd, fcntl.F_SETFL, os.O_NONBLOCK)
    errfd = process.stderr.fileno(); fcntl.fcntl(errfd, fcntl.F_SETFL, os.O_NONBLOCK)

    while process.poll() is None:
      time.sleep(pollinterval)
      now = datetime.datetime.now()
      if (now - start).seconds > timeout:
        try:
          os.kill(process.pid, signal.SIGKILL)
          os.waitpid(process.pid, os.WNOHANG)
        except OSError, e:
          pass  # ignore [Errno 3] No such process: process may have exited between process.poll() and here
        raise Timeout
      try:            outtxt += os.read(outfd, 4096)
      except OSError: pass  # ignore EAGAIN (means 'no new data available now' on non-blocking fd)
      try:            errtxt += os.read(errfd, 4096)
      except OSError: pass

    while True:
      try:
        more = os.read(outfd, 4096)
      except OSError:
        break
      if not more: break
      outtxt += more
    while True:
      try:
        more = os.read(errfd, 4096)
      except OSError:
        break
      if not more: break
      errtxt += more

    if return_stderr:
      return outtxt, errtxt
    else:
      if errtxt: sys.stderr.write(errtxt)
      return outtxt
