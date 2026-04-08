import webdisplay as wd
import network, time

def startwb():
    print("in main.py")
    # connect wifi
    wlan = network.WLAN(network.STA_IF)
    wlan.active(False)
    wlan.active(True)
    wlan.connect("Dat", "uhg2189k")
    while not wlan.isconnected():
        time.sleep(0.1)

    ip = wlan.ifconfig()[0]
    print(f"http://{ip}:6868")

    # init display
    wd.begin()
    wd.setresolution(320, 240)

    while True:
        wd.poll()
