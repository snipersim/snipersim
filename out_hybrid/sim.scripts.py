
import sys
from importlib import util
def load_file_as_module(name, location):
    sys.path.insert(0,location.rsplit('/', 1)[0])
    spec = util.spec_from_file_location(name, location)
    module = util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module
sys.argv = [ "/home/shinsaku/projects/COSC_498/snipersim/scripts/stop-by-icount.py", "5000000" ]
load_file_as_module("stop-by-icount","/home/shinsaku/projects/COSC_498/snipersim/scripts/stop-by-icount.py")

