import mapget
import sys
import time

if __name__ == "__main__":
    ret_code = mapget.run(sys.argv[1:])
    exit(ret_code)
