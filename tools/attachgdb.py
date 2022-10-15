#!/usr/bin/env python2

import sys, os, subprocess, re, tempfile, getopt, signal

def ex(cmd):
  return subprocess.Popen([ 'bash', '-c', cmd ], stdout = subprocess.PIPE).communicate()[0]

def get_section_offsets(fn):
  obj_out = ex('objdump -h "%s"' % fn)
  ret = {}
  for line in obj_out.split('\n'):
    try:
      if line and re.match(".", line.split()[1]):
        ret[line.split()[1]] = long('0x%s' % line.split()[3], 16)
    except IndexError:
      pass
    except ValueError:
      pass
  return ret

def add_offset(d, off):
  return dict( [section, address + off] for section, address in d.iteritems() )

def get_base_offset(pid, so_file):
  return long(ex('grep "%s" /proc/%s/maps' % (so_file, pid)).split('-')[0], 16)

# Strips chroot directory prefix, if the path contains it
# This is needed because the binary paths in /proc/<pid>/maps contains the full path if
#  you are outside of that specific chroot.  The assumption is that you can be inside of
#  another equivalent chroot, that maps to the same files.  If you aren't in the same
#  type of chroot, gdb will fail with library version mismatch errors
def strip_possible_schroot(file):
  if re.search('schroot',file):
    return '/'+'/'.join(file.split('/')[6:])
  else:
    return file

# The goal of this function is to return a locally accessible path to a binary
# If a pid is in a chroot (or another chroot), then the full path to the
#  binary will be presented.  If we are also in that chroot, we cannot use
#  the full path, and need the truncated version.  The assumption is that if
#  we are in a chroot, then it is the same one, allowing us to properly view
#  the debug information of the requested binary
def get_bin_path(pid, bin, strip=True):
  try:
    path = ''.join(ex('grep "%s" /proc/%s/maps' % (bin, pid)).split('\n')[0].partition('/')[1:])
    # Strip the possible chroot path only if the file doesn't exist
    # Later we will update the solib for gdb appropriately to find the proper libraries
    if strip and not os.path.isfile(path):
      return strip_possible_schroot(path)
    else:
      return path
  except IndexError:
    raise IOError

def find_pintool_name(pid, pintoolname):
  if pintoolname:
    pintoolnames = (pintoolname,)
  else:
    pintoolnames = ('pin_sim.so', 'sift_recorder', 'sniper')
  for pintoolname in pintoolnames:
    if get_bin_path(pid, pintoolname):
      return pintoolname
  print 'No pintool found, please use --toolname'
  sys.exit(1)

def attach_gdb(pid, symoff, pintoolname):
  pinbin = get_bin_path(pid, 'pinbin')
  pintool = get_bin_path(pid, pintoolname)

  symbols = 'add-symbol-file %s %s -s .data %s -s .bss %s' % (pintool, symoff['.text'], symoff['.data'], symoff['.bss'])

  # If we are debugging something in a chroot, and we can access it, change
  #  the solib path in gdb so that it doesn't use our local libraries incorrectly
  # If we cannot access it, then we are also in a chroot, and need the truncated
  #  version, because the full version is not accessible from here
  potential_schroot_path = get_bin_path(pid, 'pinbin', False)
  if re.search('schroot', potential_schroot_path) and os.path.isfile(potential_schroot_path):
    solib = 'set solib-absolute-prefix /'+'/'.join(potential_schroot_path.split('/')[1:6])
  else:
    solib = ''

  fh, fn = tempfile.mkstemp()
  f = open(fn, 'w')
  f.write('%s\nattach %s\n%s\n' % (solib, pid, symbols))
  if action == 'bt':
    f.write('bt\nquit\n')
  f.close()

  os.system('gdb -quiet -command=%s %s' % (fn, '%(pinbin)s' % locals()))

  os.unlink(fn)


if __name__ == '__main__':

  actions = [ 'interactive', 'bt' ]
  pintoolname = None

  def usage():
    print 'Attach GDB to a running Sniper process'
    print 'Usage:'
    print '  %s  [-h|--help] [--all-threads] [--action={bt}] [--abt] [--toolname={auto}] <pid>' % sys.argv[0]
    sys.exit(2)

  action = 'interactive'
  all_threads = False

  if not sys.argv[1:]:
    usage()

  try:
    opts, args = getopt.getopt(sys.argv[1:], "h", [ "help", "all-threads", "action=", "abt", "toolname=" ])
  except getopt.GetoptError, e:
    # print help information and exit:
    print e
    usage()
  for o, a in opts:
    if o == '-h' or o == '--help':
      usage()
      sys.exit()
    if o == '--all-threads':
      all_threads = True
    if o == '--action':
      if a not in actions:
        print 'Invalid action', a
        usage()
      action = a
    if o == '--abt':
      all_threads = True
      action = 'bt'
    if o == '--toolname':
      pintoolname = a

  if len(args) < 1:
    usage()
  if action == 'interactive' and all_threads:
      print 'Cannot combine --interactive with --all-threads'
      sys.exit(2)

  ret_code = 0
  pgm_pid = long(args[0])
  pgm_orig_state = ex('ps -p %u -o s=' % pgm_pid)
  if all_threads:
    pids = map(long, os.listdir(os.path.join('/proc', str(pgm_pid), 'task')))
  else:
    pids = [ pgm_pid ]
  if pgm_orig_state == 'R':
    os.kill(pgm_pid, signal.SIGSTOP)
  try:
    pintoolname = find_pintool_name(pgm_pid, pintoolname)
    pintool = get_bin_path(pgm_pid, pintoolname)
    base_offset = get_base_offset(pgm_pid, pintool)
    symoff = add_offset(get_section_offsets(pintool), base_offset)
    for pid in pids:
      attach_gdb(pid, symoff, pintoolname)
  except IOError:
    print ""
    print "Error: Unable to correctly determine the path to a mapped object."
    print "  This means that either you do not have permission to view the dynamic"
    print "  linking maps, or the pid provided isn't a pin/Sniper program."
    print ""
    ret_code = 1
  if pgm_orig_state == 'R':
    os.kill(pgm_pid, signal.SIGCONT)
  sys.exit(ret_code)
