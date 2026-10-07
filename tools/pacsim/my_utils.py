import os
def mkdir_s(path):
  try:
    os.makedirs(path)
  except OSError:
    raise "Failed to create directory at [%s]" % path
def clear_dir(dst_dir):
    files = os.listdir(dst_dir)
    for tmp in files:
        os.system("rm %s"%os.path.join(dst_dir,tmp))
def check_dir( dst_dir ):
    if not os.path.isdir( dst_dir ):
      print(dst_dir)
      mkdir_s(dst_dir)

