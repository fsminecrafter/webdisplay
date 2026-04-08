import sys
import select
import time

def wait_for_yes(timeout=10):
    print("Press Y to start (auto-exit in {}s)...".format(timeout))

    rlist, _, _ = select.select([sys.stdin], [], [], timeout)

    if rlist:
        user_input = sys.stdin.readline().strip()
        return user_input.lower() == "y"
    else:
        return False
    
print("Starting...")