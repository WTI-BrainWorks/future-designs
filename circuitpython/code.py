# SPDX-FileCopyrightText: Copyright (c) 2021 Kattni Rembor for Adafruit Industries
#
# SPDX-License-Identifier: Unlicense
"""

"""
# type: ignore
import board
import digitalio
import rotaryio
import keypad
import neopixel
import usb_hid
import terminalio
import displayio
import time
from adafruit_hid.keyboard import Keyboard
from adafruit_hid.keycode import Keycode
from adafruit_debouncer import Debouncer

from adafruit_display_text import label, outlined_label
from adafruit_display_text.scrolling_label import ScrollingLabel

DEFAULT_TR = 2.0
DEFAULT_NAR = False
DEFAULT_NUMBER = False
DEFAULT_SCAN = False
tr_interval = DEFAULT_TR
# two bits to control mode
is_nar = DEFAULT_NAR
is_number = DEFAULT_NUMBER
is_scanning = DEFAULT_SCAN

KEY_PINS = (
    board.KEY12,
    board.KEY9,
    board.KEY3,
    board.KEY6,
)

KEY_IDX = (
    11, 8, 2, 5
)

KEYCODES_BYR = (
    Keycode.B,
    Keycode.Y,
    Keycode.R,
    Keycode.G,
    Keycode.T,
)

# TODO: check ordering
KEYCODES_123 = (
    Keycode.ONE,
    Keycode.TWO,
    Keycode.FOUR,
    Keycode.THREE,
    Keycode.FIVE,
)

CURR_KEYCODES = KEYCODES_BYR


state = [0, 0, 0, 0]

ON_COLOR = (255, 255, 255)
OFF_COLORS = [(0, 0, 255), (255, 255, 0), (255, 0, 0), (0, 255, 0)]

encoder = rotaryio.IncrementalEncoder(board.ROTA, board.ROTB)
button = digitalio.DigitalInOut(board.BUTTON)
button.switch_to_input(pull=digitalio.Pull.UP)
rot_button = Debouncer(button)

last_rot_pos = None
rot_sign = 0

keys = keypad.Keys(KEY_PINS, value_when_pressed=False, pull=True)
neopixels = neopixel.NeoPixel(board.NEOPIXEL, 12, brightness=0.5)
#neopixels.fill(OFF_COLOR)
for count, i in enumerate(KEY_IDX):
    neopixels[i] = OFF_COLORS[count]

kbd = Keyboard(usb_hid.devices)

# UI
# to highlight, use color=0, background_color=0xffffff + padding
DISPLAY_WIDTH = 64
DISPLAY_HEIGHT = 128
text = "     Fake MRI      "
pad = 2
offset_stride = 12
base_offset = 8

ui = displayio.Group()

title = ScrollingLabel(
    terminalio.FONT, text=text, color=0xffffff, background_color=0x0,
    padding_left=pad, padding_right=pad, padding_top=pad, padding_bottom=pad,
    max_characters=10, animate_time=0.3
)
offset_x = 7
offset_y = base_offset

title.x = 2
title.y = offset_y

offset_y += offset_stride
# Start/stop
turn_on_text = "Strt scan"
turn_off_text = "Stop scan"
scanner = label.Label(terminalio.FONT, text=turn_on_text, padding_left=pad,
                      padding_right=pad, padding_top=pad, padding_bottom=pad,
                      )
scanner.x = offset_x
scanner.y = offset_y

offset_y += offset_stride

# TR interval
TR_text = "TR: {:.1f}s"
tr_label = label.Label(terminalio.FONT, text=TR_text.format(tr_interval), padding_left=pad,
                      padding_right=pad, padding_top=pad, padding_bottom=pad,
                      )

tr_label.x = offset_x
tr_label.y = offset_y
offset_y += offset_stride

# Mode
modes = ['BYRGT', '12345', 'NAR B', 'NAR 1']
mode_text = "M: {}"
mode_label = label.Label(terminalio.FONT, text=mode_text.format(modes[0]), padding_left=pad,
                      padding_right=pad, padding_top=pad, padding_bottom=pad,
                      )
mode_label.x = offset_x
mode_label.y = offset_y
offset_y += offset_stride

# Reset
reset_text = "Reset"
reset_label = label.Label(terminalio.FONT, text=reset_text, padding_left=pad,
                          padding_right=pad, padding_top=pad, padding_bottom=pad)
reset_label.x = offset_x
reset_label.y = offset_y

offset_y += offset_stride

# sel
sel_text = '>'
sel_label = label.Label(terminalio.FONT, text=sel_text)
sel_label.x = 0
sel_label.y = base_offset + offset_stride

board.DISPLAY.rotation = 270
ui.append(title)
ui.append(scanner)
ui.append(tr_label)
ui.append(mode_label)
ui.append(sel_label)
ui.append(reset_label)
board.DISPLAY.root_group = ui

menu_sel = 0
in_submenu = False
submenu_sel = 0
t0 = time.monotonic_ns()
S_TO_NS = 1000000000
USB_PERIOD = 8000000 # assuming bInterval of 8 (i.e. 125Hz report rate)
USB_MOST = 125000

while True:
    now = time.monotonic_ns()
    # first send scan events, if in that mode
    # if we're within a ms or two of the alleged next USB frame, schedule
    tr_ns = int(tr_interval*S_TO_NS - USB_MOST)
    if is_scanning and (now - origin) >= last_transition + tr_ns:
        # TODO: NAR vs not?
        kbd.press(CURR_KEYCODES[4])
        kbd.release(CURR_KEYCODES[4])
        last_transition = last_transition + tr_ns
        print(last_transition / S_TO_NS)

    event = keys.events.get()
    if event:
        key_number = event.key_number
        # A key transition occurred.
        if event.pressed and not state[key_number]:
            state[key_number] = 1
            kbd.press(CURR_KEYCODES[key_number])
            if not is_nar:
                kbd.release(CURR_KEYCODES[key_number])
            neopixels[KEY_IDX[key_number]] = ON_COLOR

        if event.released and state[key_number]:
            state[key_number] = 0
            if is_nar:
                kbd.release(CURR_KEYCODES[key_number])
            neopixels[KEY_IDX[key_number]] = OFF_COLORS[key_number]
    
    # address the rotary encoder
    # CW is -, CCW is +
    rot_pos = encoder.position
    if last_rot_pos is None:
        last_rot_pos = rot_pos
    elif rot_pos > last_rot_pos:
        rot_sign = -1
    elif rot_pos < last_rot_pos:
        rot_sign = 1
    else:
        rot_sign = 0

    last_rot_pos = rot_pos

    # encoder button https://docs.circuitpython.org/projects/debouncer/en/stable/
    rot_button.update()

    # update UI
    if not is_scanning:
        title.update()

    state_changed = False
    if rot_button.fell and in_submenu:
        in_submenu = False
        state_changed = True
        sel_label.text = '>'

    if not state_changed and rot_button.fell and not in_submenu:
        state_changed = True
        in_submenu = True
        sel_label.text = '*'
        submenu_sel = 0
    
    if in_submenu:
        submenu_sel += rot_sign
        # TODO: do we have enums?
        if menu_sel == 0: # start/stop scan
            last_transition = 0 # reference time
            origin = now
            in_submenu = False
            is_scanning = not is_scanning
            # update UI
            scanner.text = turn_off_text if is_scanning else turn_on_text
            sel_label.text = '>'
        
        elif menu_sel == 1: # TR period
            submenu_sel = min(10, max(-15, submenu_sel))
            tr_interval = 2.0 + 0.1 * submenu_sel
            # update UI
            tr_label.text = TR_text.format(tr_interval)
        
        elif menu_sel == 2: # button mode
            submenu_sel = min(3, max(0, submenu_sel))
            if submenu_sel == 0:
                is_nar = False
                is_number = False
                CURR_KEYCODES = KEYCODES_BYR
            elif submenu_sel == 1:
                is_nar = False
                is_number = True
                CURR_KEYCODES = KEYCODES_123
            elif submenu_sel == 2:
                is_nar = True
                is_number = False
                CURR_KEYCODES = KEYCODES_BYR
            elif submenu_sel == 3:
                is_nar = True
                is_number = True
                CURR_KEYCODES = KEYCODES_123
            # update UI
            mode_label.text = mode_text.format(modes[submenu_sel])
    
        elif menu_sel == 3: # reset to default
            in_submenu = False
            tr_interval = DEFAULT_TR
            is_nar = DEFAULT_NAR
            is_number = DEFAULT_NUMBER
            is_scanning = DEFAULT_SCAN
            CURR_KEYCODES = KEYCODES_BYR
            # update UI
            scanner.text = turn_on_text
            tr_label.text = TR_text.format(tr_interval)
            mode_label.text = mode_text.format(modes[0])
            sel_label.text = '>'
            menu_sel = 0

    else:
        if not is_scanning:
            menu_sel += rot_sign
        # hardcoded number of menu items
        menu_sel = min(3, max(0, menu_sel))

        sel_label.y = base_offset + (1+menu_sel)*offset_stride
