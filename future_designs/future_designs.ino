#include <bit>

#include <Adafruit_SH110X.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_TinyUSB.h>
#include <RotaryEncoder.h>
#include <Wire.h>
#include "pico/stdlib.h"

// #define DEBUG

// https://stackoverflow.com/a/47990
template <typename T, typename T2>
inline T bit_set(T number, T2 n) {
  return number | (1 << (T)n);
}

template <typename T, typename T2>
inline T bit_clear(T number, T2 n) {
  return number & ~(1 << (T)n);
}

template <typename T, typename T2>
inline bool bit_check(T number, T2 n) {
    return (number >> (T)n) & 1;
}

template <typename T, typename T2>
inline T clamp(T n, T2 lower, T2 upper) {
  return n <= (T)lower ? (T)lower : n >= (T)upper ? (T)upper : n;
}

// config and constants
bool core1_separate_stack = true; // 8KB stack per core (necessary?)
const uint32_t TIME_UNTIL_DISPLAY_SLEEP_MS = 5*60*1000; // turn off display after Xs of total inactivity
const uint32_t DEFAULT_TR_US = 2 * 1000 * 1000;
const uint8_t N_KEYS = 5; // number of physical keys
const uint8_t KEY_ORDER[N_KEYS] = { 12, 9, 3, 6, 10 }; // 1-indexed to match digital pin # (e.g. upper left switch is apparently tied to digital pin 1)
const uint8_t CODES[2][N_KEYS] = {
  { HID_KEY_B, HID_KEY_Y, HID_KEY_R, HID_KEY_G, HID_KEY_T },
  { HID_KEY_1, HID_KEY_2, HID_KEY_4, HID_KEY_3, HID_KEY_5 }
};
const uint32_t OFF_COLORS[N_KEYS] = { 0x0000ff, 0xffff00, 0xff0000, 0x00ff00, 0xff00ff }; // blue, yellow, red, green, purple
const uint32_t ON_COLOR = 0xffffff;
const uint32_t ON_TR_COLOR = 0x880088;
const uint32_t OFF_TR_COLOR = 0;
const uint32_t TR_HOLD_US = 20 * 1000; // 20ms
const uint8_t NEO_ON = 0x80;
const uint8_t NEO_IDLE = 0x08;
bool upload_mode = true;


// menu state (to draw UI),
// key state (for LEDs)
struct State {
  int8_t submenu_sel : 5; // max size to acommodate TR interval selection of -15 to +10 (i.e. 0.5 to 3s in 0.1s intervals)
  uint8_t key_state : 5; // one bit per switch. We'll do the TR blink on core0 for ease
  int8_t tr_interval : 5; // signed int, 0.1s steps relative to 2.0sec
  int8_t menu_sel : 3;
  uint8_t is_nar : 1;
  uint8_t is_number : 1;
  uint8_t in_submenu : 1;
  uint8_t is_scanning : 1;
  uint8_t encoder_press : 1;
  uint8_t encoder_moved : 1;
};


/////////////////////////////////
// Core 0
/////////////////////////////////

// HID init (adapted from https://github.com/adafruit/Adafruit_TinyUSB_Arduino/blob/master/examples/HID/hid_boot_keyboard/hid_boot_keyboard.ino)
uint8_t const desc_hid_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()
};
Adafruit_USBD_HID usb_hid;

RotaryEncoder encoder(PIN_ROTA, PIN_ROTB, RotaryEncoder::LatchMode::FOUR3);
void checkPosition() {  encoder.tick(); } // just call tick() to check the state.

// https://github.com/raspberrypi/pico-examples/blob/b6ac07f1946271de2817f94d8ffc0425ecb7c2a9/timer/hello_timer/hello_timer.c
// https://www.raspberrypi.com/documentation/pico-sdk/high_level.html#group_repeating_timer_1ga028fe2b7d00c1927c24131aae7c375f3
// use a negative value to indicate that subsequent 
struct repeating_timer trig_timer;
volatile bool press_trig = false;

int64_t release_callback(alarm_id_t id, __unused void *user_data) {
  press_trig = false;
  digitalWriteFast(LED_BUILTIN, LOW);
  return 0;
}

bool trig_callback(__unused struct repeating_timer *t) {
    add_alarm_in_us(TR_HOLD_US - 375, release_callback, NULL, false);
    press_trig = true;
    digitalWriteFast(LED_BUILTIN, HIGH);
    return true;
}


void setup()
{
  #ifdef DEBUG
  Serial.begin(9600);
  delay(100); // suggested by Adafruit, but why??
  #endif
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWriteFast(LED_BUILTIN, LOW);
  setup_hid();

  // set up key pins
  for (uint8_t v : KEY_ORDER) {
    pinMode(v, INPUT_PULLUP);
  }
  // init the UI
  State init{};
  rp2040.fifo.push(std::bit_cast<uint32_t>(init));
}

void loop()
{
  static State state{};
  static State prev_state{};
  // keyboard-related variables
  uint8_t count = 0; // number of keys sent
  uint8_t keycodes[6] = {0}; // keycodes sent during this iteration
  static uint8_t ignore_pos = 0;
  uint8_t current_key_state = 0;
  static bool press_prev = false;
  static uint32_t t_trig_on = 0;

  // rotary encoder-related variables
  static int encoder_pos = 0; // temporary to see if it's worth checking the encoder direction
  static int8_t submenu_sel = 0; // keep track outside the State struct to avoid hitting the bounds
  static int8_t menu_sel = 0;
  // leave early if nothing to do
  if (!maintain_hid()) {
    return;
  }

  // figure out the current state of the inputs (keys, dial, ...)
  encoder.tick();
  int new_pos = encoder.getPosition();
  int enc_dir = 0;
  if (new_pos != encoder_pos) {
    state.encoder_moved = true;
    encoder_pos = new_pos;
    enc_dir = (int)encoder.getDirection(); // 0=no rotation, 1=cw, -1=ccw
  } else {
    state.encoder_moved = false;
  }

  // TODO: debounce?
  state.encoder_press = !digitalRead(PIN_SWITCH);

  // figure out what keys to send
  for (uint8_t i = 0; i < N_KEYS; i++) {
    // note the special logic to handle the automatic trigger key
    bool is_auto_trig = (i == 4 && press_trig);
    if (!digitalReadFast(KEY_ORDER[i]) || is_auto_trig) {
      if (state.is_nar || is_auto_trig) {
        keycodes[count++] = CODES[state.is_number][i];
        current_key_state = bit_set(current_key_state, i);
      } else { // autorelease
        // check if the proposed keycode is on the ignore list
        // it wasn't on the ignore list, add it now
        if (!bit_check(ignore_pos, i)) {
          keycodes[count++] = CODES[state.is_number][i];
          ignore_pos = bit_set(ignore_pos, i);
          current_key_state = bit_set(current_key_state, i);
        }
      }
    } else {
      if (!state.is_nar) { // autorelease
        // key isn't down, remove from ignore list
        ignore_pos = bit_clear(ignore_pos, i);
      }
    }
  }

  // wake up if suspended, check if ready
  if (!ready_hid(count > 0)) {
    return;
  }
  // only send a report if the pressed keys have changed,
  // or the first time all keys are released (i.e. don't spam 0)
  if (count > 0) {
    press_prev = true;
    usb_hid.keyboardReport(0, 0, keycodes);
  } else {
    if (press_prev) {
      press_prev = false;
      usb_hid.keyboardRelease(0);
    }
  }

  // end of inputs. Now figure out the UI state
  bool changed = false;
  if (state.encoder_press && !prev_state.encoder_press && state.in_submenu) {
    state.in_submenu = false; // draw a caret or star?
    changed = true;
  }

  if (!changed && state.encoder_press && !prev_state.encoder_press && !state.in_submenu) {
    state.in_submenu = true;
    submenu_sel = 0;
  }

  if (state.in_submenu) {
    submenu_sel -= enc_dir;
    switch (menu_sel) {
      case 0: // start/stop scan
        state.in_submenu = false;
        state.is_scanning = !state.is_scanning;
        if (state.is_scanning && !prev_state.is_scanning) {
          // schedule callbacks for sending T's/5's
          int32_t tm = -1000*(2.0f + 0.1f * state.tr_interval);
          add_repeating_timer_ms(tm, trig_callback, NULL, &trig_timer);
        } else if (!state.is_scanning) {
          // cancel the existing callback
          bool cancelled = cancel_repeating_timer(&trig_timer);
          press_trig = false; // if the timing was unlucky, could this get stuck on otherwise?
        }
        break;
      case 1: // TR period
        submenu_sel = clamp(submenu_sel, -15, 10);
        state.tr_interval = submenu_sel; // 2.0f + 0.1f * submenu_sel
        break;
      case 2: // button mode
        submenu_sel = clamp(submenu_sel, 0, 3);
        switch (submenu_sel) {
          case 0:
            state.is_nar = false;
            state.is_number = false;
            break;
          case 1:
            state.is_nar = false;
            state.is_number = true;
            break;
          case 2:
            state.is_nar = true;
            state.is_number = false;
            break;
          case 3:
            state.is_nar = true;
            state.is_number = true;
            break;
        }
        break;
      case 3: // reset to default
        // only reset the "settings" related things,
        // because otherwise it breaks menu navigation
        state.in_submenu = 0;
        state.submenu_sel = 0;
        state.is_nar = 0;
        state.is_number = 0;
        state.menu_sel = 0;
        state.is_scanning = 0;
        
        menu_sel = 0;
        submenu_sel = 0;
        
        break;
    }
  } else { // top-level menu
    if (!state.is_scanning) {
      menu_sel -= enc_dir;
      menu_sel = clamp(menu_sel, 0, 3);
    }

  }

  // all done, send a message to wake/update the UI
  state.key_state = current_key_state;
  state.menu_sel = menu_sel;
  state.submenu_sel = submenu_sel; // a sanitized version that should fit in 5 bits
  uint32_t msg = std::bit_cast<uint32_t>(state);
  uint32_t last_msg = std::bit_cast<uint32_t>(prev_state);
  if (msg != last_msg) {
    rp2040.fifo.push(msg);
  }
  prev_state = state;
  // If we spin too fast, tinyusb will sometimes send double key events,
  // even in autorelease mode. I don't think it's a physical key deouncing thing?
  delayMicroseconds(500);
}

/////////////////////////////////
// Core 1
/////////////////////////////////
// relevant constants

const int DISPLAY_WIDTH = 64;
const int DISPLAY_HEIGHT = 128;

Adafruit_SH1106G display = Adafruit_SH1106G(DISPLAY_HEIGHT, DISPLAY_WIDTH, &SPI1, OLED_DC, OLED_RST, OLED_CS);
Adafruit_NeoPixel pixels = Adafruit_NeoPixel(NUM_NEOPIXEL, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

void setup1()
{
  // Start OLED
  display.setRotation(3); // 270deg rotation
  display.begin(0, true); // we don't use the i2c address, but we will reset!
  display.display();
  display.setTextSize(1);
  display.setTextWrap(false);
  display.setTextColor(SH110X_WHITE, SH110X_BLACK);
  // enable the speaker
  pinMode(PIN_SPEAKER_ENABLE, OUTPUT);
  digitalWriteFast(PIN_SPEAKER_ENABLE, HIGH);
  // set up neopixels
  pixels.begin();
  pixels.setBrightness(NEO_ON);
  pixels.show();
}

void loop1()
{
  uint32_t t0 = millis();
  bool is_cleared = false;
  static uint8_t tr_key_idx = 0;
  static uint32_t tr_count = 0;
  static State last_state{};
  while (!rp2040.fifo.available()) {
    delay(10);
    uint32_t elapsed = millis() - t0;
    if (!is_cleared && elapsed >= TIME_UNTIL_DISPLAY_SLEEP_MS) {
      display.clearDisplay();
      display.setContrast(0);
      pixels.setBrightness(NEO_IDLE); // "idle" neopixels
      display.display();
      pixels.show();
      is_cleared = true;
    }
  }

  uint32_t start = micros();
  // at least one message. Wake the display and start doing things  
  uint32_t msg = rp2040.fifo.pop();
  State next_state = std::bit_cast<State>(msg);

  // fill in the LEDs
  pixels.setBrightness(NEO_ON);
  for(int i=0; i < N_KEYS; i++) {
    if (bit_check(next_state.key_state, i)) {
      pixels.setPixelColor(KEY_ORDER[i]-1, ON_COLOR);
    } else {
      pixels.setPixelColor(KEY_ORDER[i]-1, OFF_COLORS[i]);
    }
  }

  // compute TR count
  if (next_state.is_scanning) {
    if (bit_check(next_state.key_state, 4) && !bit_check(last_state.key_state, 4)) {
      // increment on "press"
      tr_count++;
    }
  } else {
    tr_count = 0;
  }


  display.clearDisplay();
  display.setContrast(0x7f);
  display.setCursor(0, 0);
  display.print("Fut Designs");
  display.setCursor(8, 10);
  if (next_state.is_scanning) {
    display.print("Stop scan");
  } else {
    display.print("Strt scan");
  }
  display.setCursor(8, 20);
  display.print("TR: ");
  display.print(2.0f + 0.1f * next_state.tr_interval);
  display.print("s");

  display.setCursor(8, 30);
  display.print("M: ");
  if (!next_state.is_nar && !next_state.is_number) {
    display.print("BYRGT");
  } else if (!next_state.is_nar && next_state.is_number) {
    display.print("12345");
  } else if (next_state.is_nar && !next_state.is_number) {
    display.print("NAR B");
  } else {
    display.print("NAR 1");
  }

  display.setCursor(8, 40);
  display.print("Reset");

  if (next_state.is_scanning) {
    display.setCursor(8, 50);
    display.print("#TR: ");
    display.print(tr_count);
  }

  // update cursor
  display.setCursor(0, 10 + 10*next_state.menu_sel);
  if (next_state.in_submenu || next_state.is_scanning) {
    display.print("*");
  } else {
    display.print(">");
  }
  // update display
  pixels.show();

  // just to get a sense of UI timing
  #ifdef DEBUG
  uint32_t stop = micros();
  display.setCursor(8, 80);
  display.print(stop-start);
  display.setCursor(0, 120);
  display.print("DEBUG MODE");
  #else
  if (upload_mode) {
    display.setCursor(0, 100);
    display.print("DEV MODE");
  }
  #endif

  display.display();
  last_state = next_state;
  delay(10);
}

/////////////////////////////////
// Helpers
/////////////////////////////////
void setup_hid()
{
    // Begin HID setup
  if (!TinyUSBDevice.isInitialized()) {
    TinyUSBDevice.begin(0);
  }
  // if not in debug mode, the button at digital pin 1 needs to be held
  // at the start to enable CDC/mass storage
  #ifndef DEBUG
  pinMode(1, INPUT_PULLUP);
  delay(100);
  if (digitalReadFast(1)) {
    TinyUSBDevice.clearConfiguration();
    upload_mode = false;
  }
  #endif
  // Setup HID
  usb_hid.setPollInterval(1);
  usb_hid.setReportDescriptor(desc_hid_report, sizeof(desc_hid_report));
  // Only set the Current Designs descriptors/IDs in regular mode.
  // Setting these during development breaks auto-detection by the Arduino IDE
  if (!upload_mode) {
    TinyUSBDevice.setManufacturerDescriptor("Current Designs, Inc.");
    TinyUSBDevice.setProductDescriptor("932");
    TinyUSBDevice.setID(0x181b, 0x0008);
  }
  usb_hid.begin();
  // If already enumerated, additional class driver begin() e.g msc, hid, midi won't take effect until re-enumeration
  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }
  // end HID setup
}

bool maintain_hid()
{
  #ifdef TINYUSB_NEED_POLLING_TASK
  // Manual call tud_task since it isn't called by Core's background
  TinyUSBDevice.task();
  #endif
  // not enumerated()/mounted() yet: nothing to do
  return TinyUSBDevice.mounted();
}

bool ready_hid(bool any_change)
{
  if (TinyUSBDevice.suspended() && any_change) {
    // Wake up host if we are in suspend mode
    // and REMOTE_WAKEUP feature is enabled by host
    TinyUSBDevice.remoteWakeup();
  }

  // skip if hid is not ready e.g still transferring previous report
  return usb_hid.ready();
}

