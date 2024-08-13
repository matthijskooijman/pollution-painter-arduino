#include <Arduino.h>
#include <Wire.h>
#include <sps30.h>
#include <Adafruit_DotStar.h>
#include <U8g2lib.h>

auto const num_leds = 240;
auto const brightness = 20;
auto const wait = 30; // ms to wait between frames
auto const fade = 3000;

#if defined(ARDUINO_MJS2020)
auto const START_STOP_BUTTON_PIN = PIN_BUTTON;

Adafruit_DotStar strip(num_leds, DOTSTAR_BGR, &SPI1);
#else
  #error "Unknown board"
#endif

// U8G2 instance that renders to the OLED display
U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// LED strip output variables
unsigned long lastUpdate = 0;
unsigned long fadeStartTime = 0;
int state = 0;
unsigned currentBrightness;
unsigned frame;

auto const COLOR_ON = strip.gamma32(strip.Color(255, 255, 255));
auto const COLOR_OFF = strip.gamma32(strip.Color(0, 0, 0));
auto const TEXT_COLOR = strip.gamma32(strip.Color(255, 0, 0));

// These values are to locate the top visible pixel on the beam
// (handling the case where a couple of pixels are obscured by tape).
//
// Here upward means pixels on the piece of LED strip running upwards on
// the beam, and downward on the piece of LED strip that turns around
// and runs down again.
unsigned const top_pixel_upward = num_leds / 2 - 10;
unsigned const top_pixel_downward = num_leds / 2 + 10;

// Set from ISR
volatile bool start_stop_button_pressed;

// Measurement unit, set by read_pm
const char * unit = nullptr;

// U8G2 instance that renders to memory for the field-of-view text line
auto const u8g2_bitmap_width = 128;
auto const u8g2_bitmap_height = 10;
U8G2_BITMAP u8g2_bitmap(u8g2_bitmap_width, u8g2_bitmap_height, U8G2_R0);

///////////////////////////
// PM measurements
///////////////////////////

void setup_pm() {
  sensirion_i2c_init();
  sps30_start_measurement();
}

float read_pm() {
  Serial.println("Waiting for SPS30 data ready");

  uint16_t data_ready;
  do {
    int16_t ret = sps30_read_data_ready(&data_ready);
    if (ret < 0) {
      Serial.print("Error reading SPS30 data-ready flag: ");
      Serial.println(ret);
      return NAN;
    }
  } while (!data_ready);

  struct sps30_measurement res = {};
  int16_t ret = sps30_read_measurement(&res);
  if (ret < 0) {
    Serial.print("Error reading SPS30 measurement: ");
    Serial.println(ret);
    return NAN;
  }
  #define SYMBOL_MICRO "\xb5"
  #define SYMBOL_3_SUPER "\xb3"
  unit = SYMBOL_MICRO "g/m" SYMBOL_3_SUPER;
  return res.mc_2p5;
}

///////////////////////////
// OLED output
///////////////////////////
void setup_oled() {
  oled.begin();
}

void display_oled(float value) {
  oled.clearBuffer();          // clear the internal memory
  oled.setFont(u8g2_font_ncenB08_tr); // choose a suitable font
  oled.setFontPosTop();
  oled.setCursor(0, 0);
  oled.print("PM2.5: ");  // write something to the internal memory
  oled.print(value);
  oled.print(unit);
  oled.setCursor(0, 15);
  oled.print("Brightness: ");  // write something to the internal memory
  oled.print(brightness);
  oled.print("/255");
  oled.setCursor(0, 30);
  oled.print("Frametime: ");  // write something to the internal memory
  oled.print(wait);
  oled.print("ms");
  oled.setCursor(0, 45);
  oled.print("Fadetime: ");  // write something to the internal memory
  oled.print(fade);
  oled.print("ms");
  oled.sendBuffer();          // transfer internal memory to the display
}

///////////////////////////
// LED strip output
///////////////////////////
void setup_ledstrip() {
  strip.begin(); // Initialize pins for output
  strip.show();  // Turn all LEDs off ASAP

  u8g2_bitmap.begin();
  u8g2_bitmap.setFont(u8g2_font_6x10_tf);
}

// This translates a logical pixel number to the actual led that
// displays it. The first u8g2_bitmap_height pixels are for the header
// line and mapped to the topmost downward pixels (not also upward
// pixels to prevent horizontal shift which makes the text blurry at the
// expense of halving vertical resolution).
// Then all other pixels are mapped to the remaining pixels alternating
// upward and downward for maximum vertical resolution.
// Here upward means pixels on the piece of LED strip running upwards on
// the beam, and downward on the piece of LED strip that turns around
// and runs down again.
unsigned pixel_translate(unsigned i) {
  unsigned res;
  if (i < u8g2_bitmap_height)
    res = top_pixel_downward + i;
  else if (i % 2 == 0) {
    res = top_pixel_upward - (u8g2_bitmap_height / 2 + i / 2);
  } else {
    res = top_pixel_downward + (u8g2_bitmap_height / 2 + i / 2);
  }

  return res;
}

// Render header pixels to show at top of image
void render_header_line(float value) {
  u8g2_bitmap.clearBuffer();
  u8g2_bitmap.setFontPosTop();
  u8g2_bitmap.setCursor(0, 0);
  u8g2_bitmap.print("PM2.5: ");
  u8g2_bitmap.print(value);
  u8g2_bitmap.print(unit);
  u8g2_bitmap.sendBuffer();

  
  // Uncomment this to test text rendering
  /*
  for (unsigned y=0; y < u8g2_bitmap_height; ++y) {
    Serial.print("|");
    for (unsigned x=0; x < u8g2_bitmap_width; ++x) {
      if (u8x8_GetBitmapPixel(u8g2_bitmap.getU8x8(), x, y))
        Serial.print("X");
      else
        Serial.print(" ");
    }
    Serial.println("|");
  }
  */
}

// Update ledstrip display (if it is time for the next frame)
void display_ledstrip(float value, uint8_t cutoff) {
  if (millis() - lastUpdate > wait) {
    /* Decide pixel values to show */
    for (int i = 0; i < strip.numPixels(); ++i) {
      auto color = COLOR_OFF;
      if (i < u8g2_bitmap_height) {
        if (state == 2) {
          auto column = frame / 2;
          if ((frame % 1 == 0) && column < u8g2_bitmap_width) {
            if (u8x8_GetBitmapPixel(nullptr, column, i)) {
              color = TEXT_COLOR;
            }
          }
        }
      } else if (state != 0) {
        if (cutoff < random(256)) {
          color = COLOR_ON;
        }
      }
      strip.setPixelColor(pixel_translate(i), color);
    }
    ++frame;

    /* Decide brightness (fade in/out) and next state */
    switch (state) {
      case 0:
        break;
      case 1:
        if (millis() - fadeStartTime < fade) {
          currentBrightness = map(millis() - fadeStartTime, 0, fade, 0, brightness);
        } else {
          currentBrightness = brightness;
          state = 2;
          frame = 0;
        }
        break;
      case 2:
        currentBrightness = brightness;
        break;
      case 3:
        if (millis() - fadeStartTime < fade) {
          currentBrightness = map(millis() - fadeStartTime, 0, fade, currentBrightness, 0);
        } else {
          currentBrightness = 0;
          state = 0;
        }
        break;
    }
    strip.setBrightness(currentBrightness);

    /* Output frame */
    strip.show();

    lastUpdate += wait;
    if (millis() - lastUpdate > wait) {
      Serial.print("Missed frame by ");
      Serial.print(millis() - lastUpdate - wait);
      Serial.println("ms");
      lastUpdate = millis();
    }
  }
}

///////////////////////////
// Main code
///////////////////////////

// Called from ISR
void on_start_stop_button_pressed() {
  start_stop_button_pressed = true;
}

void setup() {
  Serial.begin(115200);
  //while (!Serial);
  Serial.println("Starting...");

  #if defined(ARDUINO_MJS2020)
  // The MJS2020 board has some disabled-by-default regulators, enable
  // them to enable PM sensor
  pinMode(PIN_ENABLE_5V, OUTPUT);
  digitalWrite(PIN_ENABLE_5V, HIGH);

  pinMode(PIN_ENABLE_3V_SENS, OUTPUT);
  digitalWrite(PIN_ENABLE_3V_SENS, HIGH);
  delay(500);
  #endif

  pinMode(START_STOP_BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(START_STOP_BUTTON_PIN, on_start_stop_button_pressed, FALLING);

  setup_pm();
  setup_ledstrip();
  setup_oled();

  // Uncomment these to help pinpoint the top of a strip folded
  // back down to double resolution
  /*
  strip.setPixelColor(pixel_translate(0), strip.gamma32(strip.Color(255, 0, 0)));
  strip.setPixelColor(pixel_translate(1), strip.gamma32(strip.Color(0, 255, 0)));
  strip.setPixelColor(pixel_translate(2), strip.gamma32(strip.Color(0, 0, 255)));
  strip.show();
  */

  Serial.println("Setup complete");
}

void loop() {
  // Read the PM sensor every second
  delay(1000);
  auto value = read_pm();

  auto cutoff = constrain(map(value, 0, 550, 255, 45), 0, 255);

  Serial.print("Value PM2.5: ");
  Serial.println(value);

  Serial.print("Cutoff: ");
  Serial.println(cutoff);

  display_oled(value);

  // Then see if the button is pressed to start, and if so, run the
  // animation (without further PM readings) until the button is pressed
  // again.
  do {
    if (start_stop_button_pressed) {
      start_stop_button_pressed = false;
      if (state == 0) {
        Serial.println("Starting");
        // Render header line now to prevent delay later
        render_header_line(value);
        // Start fade in
        state = 1;
        fadeStartTime = millis();
        lastUpdate = millis();
      } else {
        Serial.println("Stopping");
        // Start fade out
        state = 3;
        fadeStartTime = millis();
      }
    }
    if (state != 0)
      display_ledstrip(value, cutoff);
  } while (state != 0);
}
