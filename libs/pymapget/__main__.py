"""Forward python -m mapget arguments to the native CLI and return its exit status."""

import mapget
import sys
import time

if __name__ == "__main__":
    ret_code = mapget.run(sys.argv[1:], True)
    exit(ret_code)
