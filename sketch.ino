/*
Author: Suhas.M
E-Mail: suhas10m@gmail.com
*/

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "esp32-hal-ledc.h"

#define DEBUG_PRINTS 1

#if DEBUG_PRINTS
  #define DBG_PRINT(...)   Serial.print(__VA_ARGS__)
  #define DBG_PRINTLN(...) Serial.println(__VA_ARGS__)
#else
  #define DBG_PRINT(...)
  #define DBG_PRINTLN(...)
#endif

#define BTN1_PIN 21
#define BTN2_PIN 47
#define BTN3_PIN 48

#define SR_DS   4
#define SR_OE   5
#define SR_STCP 6
#define SR_SHCP 7
#define SR_MR   15

#define PWM_FREQ 5000
#define PWM_RES  8

#define BTN_SCN_TICKS  pdMS_TO_TICKS(5)
#define DEBOUNCE_TICKS pdMS_TO_TICKS(20)
#define SINGLE_TICKS   pdMS_TO_TICKS(300)
#define DOUBLE_TICKS   pdMS_TO_TICKS(500)
#define TRIPLE_TICKS   pdMS_TO_TICKS(700)
#define LONG_TICKS     pdMS_TO_TICKS(1500)

// Named button indices (matches pins[] order in ButtonTask)
enum {
  BTN1 = 0,
  BTN2 = 1,
  BTN3 = 2
};

typedef enum {
  EVT_SINGLE,
  EVT_DOUBLE,
  EVT_TRIPLE,
  EVT_LONG
} btn_evt_t;

//btn_event_t
typedef struct {
  uint8_t btn;
  btn_evt_t type;
  uint32_t time;
} btn_event_t;

//btn_state_t
typedef struct {
  bool stable_state;
  bool prev_sample;
  TickType_t last_change;
  TickType_t press_start;
  TickType_t first_press;     // time of first press in current tap sequence
  TickType_t last_release;    // time of most recent release in sequence
  bool long_sent;
  uint8_t tap_count;           // number of completed taps so far (0-3)
  bool waiting_resolution;     // true if a tap sequence is pending timeout
} btn_state_t;

//menu state
typedef enum {
  MENU_MAIN,
  MENU_BRIGHTNESS,
  MENU_MODE,
  MENU_MANUAL,
  MENU_AUTO,
  MENU_INFO,
  MENU_RESET,
  MENU_POWER_OFF
} menu_state_t;


btn_state_t btn[3];
menu_state_t menu_state = MENU_MAIN;
uint8_t main_idx = 0;
uint8_t brightness = 5;        // 0-10
bool auto_mode = false;
uint8_t pattern_idx = 0;        // committed (live) pattern index
uint8_t pending_pattern_idx = 0; // staged pattern index while in MENU_MANUAL

TimerHandle_t auto_timer = NULL;

//queues
QueueHandle_t btn_evt_queue = NULL;
QueueHandle_t display_queue = NULL;
SemaphoreHandle_t shiftreg_mutex = NULL;

//for testing buttons
const char* evt_to_str(btn_evt_t type) {
  switch (type) {
    case EVT_SINGLE: return "SINGLE";
    case EVT_DOUBLE: return "DOUBLE";
    case EVT_TRIPLE: return "TRIPLE";
    case EVT_LONG:   return "LONG";
    default: return "UNKNOWN";
  }
}

uint16_t gen_pattern() {
  switch (menu_state) {
    case MENU_MAIN:       return (1 << main_idx);
    case MENU_BRIGHTNESS: return (brightness == 0) ? 0 : ((1 << brightness) - 1);
    case MENU_MODE:       return auto_mode ? 0xAAAA : 0x5555;
    case MENU_MANUAL:     return (1 << (pending_pattern_idx % 16));
    case MENU_AUTO:       return 0xF0F0;
    case MENU_INFO:       return 0x0F0F;
    case MENU_RESET:      return 0xFFFF;
    case MENU_POWER_OFF:  return 0x0000;
    default:              return 0;
  }
}

uint16_t swap_bytes(uint16_t x) {
  return (x << 8) | (x >> 8);
}

// Brightness scaling: maps 0-10 -> PWM duty 0-255 on SR_OE (active LOW OE,
// so higher duty on OE = dimmer; we invert so higher "brightness" = brighter)
void set_brightness(uint8_t level) {
  if (level > 10) level = 10;

  // OE is active LOW: 0 = fully enabled (bright), 255 = fully disabled (off)
  // Map brightness 0..10 -> OE PWM 255..0 (inverted)
  uint8_t duty = map(level, 0, 10, 255, 0);
  ledcWrite(SR_OE, duty);
}

void sr_enable() {
  // Restore PWM-driven OE based on current brightness
  set_brightness(brightness);
}

void sr_disable() {
  // Force OE high (outputs disabled / all LEDs off)
  ledcWrite(SR_OE, 255);
}

void AutoTimerCallback(TimerHandle_t xTimer) {
  if (menu_state == MENU_AUTO) {
    pattern_idx++;
    uint16_t pattern = gen_pattern();
    xQueueSend(display_queue, &pattern, 0);
  }
}

// Resolve the current tap sequence for button i into a btn_evt_t and push
// it to the queue, then reset sequence state.
void resolve_tap_sequence(uint8_t i, TickType_t now) {
  if (btn[i].tap_count == 0) return;

  btn_evt_t type;
  switch (btn[i].tap_count) {
    case 1: type = EVT_SINGLE; break;
    case 2: type = EVT_DOUBLE; break;
    default: type = EVT_TRIPLE; break; // 3 or more (capped at 3)
  }

  DBG_PRINT("BTN: ");
  DBG_PRINT(i);
  DBG_PRINT(" : ");
  DBG_PRINTLN(evt_to_str(type));

  btn_event_t e = {i, type, (uint32_t)now};
  xQueueSend(btn_evt_queue, &e, 0);

  btn[i].tap_count = 0;
  btn[i].waiting_resolution = false;
}

void ButtonTask(void *arg) {
  const uint8_t pins[3] = {BTN1_PIN, BTN2_PIN, BTN3_PIN};
  // window[k] = max time allowed since first_press to accept (k+1)-th tap
  // window[0] -> deadline for resolving as SINGLE (no 2nd tap arrived)
  // window[1] -> deadline for resolving as DOUBLE (no 3rd tap arrived)
  // window[2] -> deadline for resolving as TRIPLE (hard cap, resolve immediately)
  const TickType_t window[3] = {SINGLE_TICKS, DOUBLE_TICKS, TRIPLE_TICKS};

  while (1) {
    TickType_t now = xTaskGetTickCount();

    for (int i = 0; i < 3; i++) {
      bool curr = !digitalRead(pins[i]);

      //debounce
      if (curr != btn[i].prev_sample) {
        btn[i].last_change = now;
        btn[i].prev_sample = curr;
      }

      if ((now - btn[i].last_change) < DEBOUNCE_TICKS) {
        // still bouncing; fall through to timeout checks below
      } else if (curr != btn[i].stable_state) {
        btn[i].stable_state = curr;

        if (curr) {
          // ---- PRESS edge ----
          btn[i].press_start = now;
          btn[i].long_sent = false;

          if (btn[i].tap_count == 0) {
            btn[i].first_press = now;
          }
        } else {
          // ---- RELEASE edge ----
          uint32_t hold = now - btn[i].press_start;
          DBG_PRINT("Hold: ");
          DBG_PRINTLN(hold);

          btn[i].press_start = 0;
          btn[i].last_release = now;

          if (btn[i].long_sent) {
            // long press already reported on this press; ignore this release
            btn[i].tap_count = 0;
            btn[i].waiting_resolution = false;
            continue;
          }

          if (hold <= SINGLE_TICKS) {
            if (btn[i].tap_count < 3) {
              btn[i].tap_count++;
            }

            if (btn[i].tap_count >= 3) {
              // Triple reached -> resolve immediately, no need to wait
              resolve_tap_sequence(i, now);
            } else {
              btn[i].waiting_resolution = true;
            }
          } else {
            // held too long for a "tap" but didn't trigger long press
            // (e.g. just under LONG_TICKS) -> discard sequence
            btn[i].tap_count = 0;
            btn[i].waiting_resolution = false;
            continue;
          }
        }
      }

      // ---- long-press detection (checked every loop while held) ----
      if (btn[i].stable_state && !btn[i].long_sent) {
        uint32_t hold = now - btn[i].press_start;
        if (hold >= LONG_TICKS) {
          DBG_PRINT("BTN: ");
          DBG_PRINT(i);
          DBG_PRINTLN(" LONG");

          btn_event_t e = {(uint8_t)i, EVT_LONG, (uint32_t)now};
          xQueueSend(btn_evt_queue, &e, 0);

          btn[i].long_sent = true;
          // Cancel any pending tap sequence; this press belongs to LONG
          btn[i].tap_count = 0;
          btn[i].waiting_resolution = false;
        }
      }

      // ---- resolve pending tap sequence on timeout ----
      if (btn[i].waiting_resolution && !btn[i].stable_state) {
        uint32_t since_first = now - btn[i].first_press;
        uint8_t idx = btn[i].tap_count - 1; // 0 -> single, 1 -> double
        if (idx > 2) idx = 2;               // safety clamp

        if (since_first > window[idx]) {
          resolve_tap_sequence(i, now);
        }
      }
    }

    vTaskDelay(BTN_SCN_TICKS);
  }
}

void enter_power_off() {
  menu_state = MENU_POWER_OFF;
  sr_disable();
  xTimerStop(auto_timer, 0);
}

void MenuTask(void *arg) {
  btn_event_t evt;

  while (1) {
    xQueueReceive(btn_evt_queue, &evt, portMAX_DELAY);

    //power off state to turn on
    if (menu_state == MENU_POWER_OFF) {
      if (evt.btn == BTN1 && evt.type == EVT_LONG) {
        menu_state = MENU_MAIN;
        sr_enable();

        uint16_t p = gen_pattern();
        xQueueSend(display_queue, &p, 0);
      }
      continue;
    }

    switch (menu_state) {
      //main
      case MENU_MAIN:
        if (evt.type == EVT_SINGLE) {

          if (evt.btn == BTN1) //forward
            main_idx = (main_idx + 1) % 4;

          else if (evt.btn == BTN3) //back
            main_idx = (main_idx + 3) % 4;

          else if (evt.btn == BTN2) { //enter
            switch (main_idx) {
              case 0: menu_state = MENU_BRIGHTNESS; break;
              case 1: menu_state = MENU_MODE; break;
              case 2: menu_state = MENU_INFO; break;
              case 3: menu_state = MENU_RESET; break;
            }
          }
        }

        if (evt.btn == BTN3 && evt.type == EVT_LONG) {
          enter_power_off();
        }
        break;

      //brightness
      case MENU_BRIGHTNESS:
        if (evt.btn == BTN1 && evt.type == EVT_SINGLE) {
          menu_state = MENU_MAIN;
        }
        else if (evt.btn == BTN2 && evt.type == EVT_SINGLE && brightness < 10) {
          brightness++;
          set_brightness(brightness);
        }
        else if (evt.btn == BTN3 && evt.type == EVT_SINGLE && brightness > 0) {
          brightness--;
          set_brightness(brightness);
        }
        break;

      //mode
      case MENU_MODE:
        if (evt.btn == BTN1 && evt.type == EVT_SINGLE)
          auto_mode = !auto_mode;

        else if (evt.btn == BTN2 && evt.type == EVT_SINGLE) {
          if (auto_mode) {
            menu_state = MENU_AUTO;
            xTimerStart(auto_timer, 0);
          } else {
            menu_state = MENU_MANUAL;
            pending_pattern_idx = pattern_idx; // stage current pattern
          }
        }

        else if (evt.btn == BTN3 && evt.type == EVT_SINGLE)
          menu_state = MENU_MAIN;
        break;

      //manual
      case MENU_MANUAL:
        if (evt.btn == BTN1 && evt.type == EVT_SINGLE) {
          pending_pattern_idx = (pending_pattern_idx + 1) % 16;
        }
        else if (evt.btn == BTN2 && evt.type == EVT_SINGLE) {
          // Save: commit staged pattern
          pattern_idx = pending_pattern_idx;
          menu_state = MENU_MAIN;
        }
        else if (evt.btn == BTN3 && evt.type == EVT_SINGLE) {
          // Cancel: discard staged pattern
          pending_pattern_idx = pattern_idx;
          menu_state = MENU_MAIN;
        }
        break;

      //auto
      case MENU_AUTO:
        if (evt.btn == BTN1 && evt.type == EVT_SINGLE) {
          menu_state = MENU_MAIN;
          xTimerStop(auto_timer, 0);
        }
        break;

      //info
      case MENU_INFO:
        if (evt.btn == BTN1 && evt.type == EVT_SINGLE) {
          pattern_idx++;
        }
        else if (evt.btn == BTN3 && evt.type == EVT_SINGLE) {
          menu_state = MENU_MAIN;
        }
        break;

      //reset
      case MENU_RESET:
        if (evt.btn == BTN2 && evt.type == EVT_DOUBLE) {
          brightness = 5;
          set_brightness(brightness);
          auto_mode = false;
          pattern_idx = 0;
          pending_pattern_idx = 0;
          main_idx = 0;
          menu_state = MENU_MAIN;
          xTimerStop(auto_timer, 0);
        }
        else if (evt.btn == BTN3 && evt.type == EVT_SINGLE) {
          menu_state = MENU_MAIN;
        }
        break;

      default: break;
    }

    uint16_t pattern = gen_pattern();

    //test menu
    DBG_PRINT("STATE: ");
    DBG_PRINT(menu_state);
    DBG_PRINT(" main_idx: ");
    DBG_PRINT(main_idx);
    DBG_PRINT(" brightness: ");
    DBG_PRINT(brightness);
    DBG_PRINT(" auto: ");
    DBG_PRINTLN(auto_mode);

    if (xQueueSend(display_queue, &pattern, 0) != pdTRUE) {
      DBG_PRINTLN("Display queue full");
    }
  }
}

void shiftreg_write(uint16_t value) {
  digitalWrite(SR_STCP, LOW);

  for (int i = 15; i >= 0; i--) {
    digitalWrite(SR_SHCP, LOW);
    digitalWrite(SR_DS, (value >> i) & 0x01);
    digitalWrite(SR_SHCP, HIGH);
  }

  digitalWrite(SR_STCP, HIGH);
}

void DisplayTask(void *arg) {
  uint16_t pattern;

  while (1) {
    if (xQueueReceive(display_queue, &pattern, portMAX_DELAY) == pdTRUE) {
      xSemaphoreTake(shiftreg_mutex, portMAX_DELAY);
      shiftreg_write(swap_bytes(pattern));
      xSemaphoreGive(shiftreg_mutex);
    }
  }
}

void setup() {
  pinMode(BTN1_PIN, INPUT_PULLUP);
  pinMode(BTN2_PIN, INPUT_PULLUP);
  pinMode(BTN3_PIN, INPUT_PULLUP);

  pinMode(SR_DS, OUTPUT);
  pinMode(SR_OE, OUTPUT);
  pinMode(SR_STCP, OUTPUT);
  pinMode(SR_SHCP, OUTPUT);
  pinMode(SR_MR, OUTPUT);

  digitalWrite(SR_MR, HIGH);

  digitalWrite(SR_STCP, LOW);
  digitalWrite(SR_SHCP, LOW);

  // PWM setup for brightness scaling on SR_OE (ESP32 core 3.x API)
  ledcAttach(SR_OE, PWM_FREQ, PWM_RES);
  set_brightness(brightness); // apply default brightness

#if DEBUG_PRINTS
  Serial.begin(115200);
  delay(1000);
#endif

  btn_evt_queue = xQueueCreate(10, sizeof(btn_event_t));
  display_queue = xQueueCreate(5, sizeof(uint16_t));
  shiftreg_mutex = xSemaphoreCreateMutex();
  auto_timer = xTimerCreate(
    "AutoTimer",
    pdMS_TO_TICKS(2000),
    pdTRUE,
    NULL,
    AutoTimerCallback
  );

  xTaskCreatePinnedToCore(ButtonTask, "ButtonTask", 4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(MenuTask, "MenuTask", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(DisplayTask, "DisplayTask", 4096, NULL, 3, NULL, 1);

  // Push initial pattern so LEDs reflect MENU_MAIN at boot
  uint16_t pattern = gen_pattern();
  xQueueSend(display_queue, &pattern, 0);
}

void loop() {
  // Nothing here; all work is done in FreeRTOS tasks
}
