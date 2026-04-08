# This file is executed on every boot (including wake-boot from deepsleep)
#import esp
#esp.osdebug(None)
#import webrepl
#webrepl.start()

from start import *
from main import *
if wait_for_yes(10) == False:
    raise SystemExit
else:
    print("starting...")
    print("starting wb")
    startwb()