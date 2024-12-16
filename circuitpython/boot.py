import board, digitalio
import usb_hid
import usb_midi
import storage
import usb_cdc

button = digitalio.DigitalInOut(board.KEY1)
button.pull = digitalio.Pull.UP
DEBUG = True # just a temporary thing till it's good to go

# if KEY1 is pressed, don't disable storage/midi/cdc
if not DEBUG and button.value:
    storage.disable_usb_device()
    usb_midi.disable()
    usb_cdc.disable()

usb_hid.enable((usb_hid.Device.KEYBOARD,))
usb_hid.set_interface_name('Current Designs, Inc. 932')
