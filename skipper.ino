#include <Adafruit_NeoPixel.h>
#include <LovyanGFX.hpp>
#include <SD.h>
#include <SPIFFS.h>

#include "AudioFileSourceSD.h"
#include "AudioFileSourceBuffer.h"
#include "AudioFileSourceID3.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

#include "src/CC1101-ESP-Arduino/CC1101_ESP_Arduino.h"

#define LED_PIN 38
#define SD_CS 7

#define I2S_LRC 42   // Left-right clock (LRCLK)
#define I2S_BCLK 41  // Bit clock (BCLK)
#define I2S_DOUT 40  // Data output to MAX98357A (DIN)

#define VRX_PIN 1  // X-axis (Analog)
#define VRY_PIN 2  // Y-axis (Analog)
#define SW_PIN 39  // Joystick button (Digital)

#define RADIO_SCK 47
#define RADIO_MOSI 21
#define RADIO_CS 48
#define RADIO_GDO0 45 // TX/input pin
#define RADIO_GDO1 14 // MISO
#define RADIO_GDO2 18 // RX/output pin

Adafruit_NeoPixel pixel = Adafruit_NeoPixel(1, LED_PIN, NEO_GRB + NEO_KHZ800);

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel_instance;
  lgfx::Bus_SPI _bus_instance;
  lgfx::Light_PWM _light_instance;

public:
  LGFX(void) {
    {
      auto cfg = _bus_instance.config();

      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 48000000;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_dc = 46;
      cfg.pin_mosi = 11;
      cfg.pin_miso = -1;
      cfg.pin_sclk = 12;

      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();

      cfg.pin_cs = 15;
      cfg.pin_rst = 16;
      cfg.pin_busy = -1;

      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;

      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = false;
      cfg.invert = false;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = true;

      _panel_instance.config(cfg);
    }
    {
      auto cfg = _light_instance.config();

      cfg.pin_bl = 9;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;

      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }

    setPanel(&_panel_instance);
  }
};

LGFX display;
static File root;

AudioGeneratorMP3 *mp3;
AudioFileSourceSD *file;
AudioFileSourceBuffer *buff;
AudioFileSourceID3 *id3;
AudioOutputI2S *out;

void MDCallback(void *cbData, const char *type, bool isUnicode, const char *string) {
  (void)cbData;
  Serial.printf("ID3 callback for: %s = '", type);

  if (isUnicode) {
    string += 2;
  }

  while (*string) {
    char a = *(string++);
    if (isUnicode) {
      string++;
    }
    Serial.printf("%c", a);
  }
  Serial.printf("'\n");
  Serial.flush();
}

// Called when there's a warning or error (like a buffer underflow or decode hiccup)
void StatusCallback(void *cbData, int code, const char *string) {
  const char *ptr = reinterpret_cast<const char *>(cbData);
  // Note that the string may be in PROGMEM, so copy it to RAM for printf
  char s1[64];
  strncpy_P(s1, string, sizeof(s1));
  s1[sizeof(s1) - 1] = 0;
  Serial.printf("STATUS(%s) '%d' = '%s'\n", ptr, code, s1);
  Serial.flush();
}

unsigned long lastImageChange = 0;

volatile long last_micros;
String buffer;

ICACHE_RAM_ATTR void radioHandlerOnChange() {
	int delta_micros = micros() - last_micros;
	
	bool input = digitalRead(RADIO_GDO1);
	if (input == 1){
		buffer += "\n0 -> 1 after " + String(delta_micros);
	} else {
		buffer += "\n1 -> 0 after " + String(delta_micros);
	}
	
	last_micros = micros();
}

CC1101 cc1101(RADIO_SCK, RADIO_GDO1, RADIO_MOSI, RADIO_CS, RADIO_GDO0, RADIO_GDO2);

void setup() {
  Serial.begin(115200);
  while (!Serial) {}

  Serial.println("Serial communication initialized");

  // LED
  pixel.begin();
  pixel.setPixelColor(0, 0, 255, 0);
  pixel.setBrightness(5);
  pixel.show();

  // LCD
  display.init();
  display.setBrightness(50);
  display.setTextSize(2);

  display.setRotation(1);  // Set display rotation
  display.fillScreen(TFT_BLUE);
  display.setTextColor(TFT_WHITE, TFT_BLACK);

  // SD card
  if (!SD.begin(SD_CS)) {
    display.drawString("SD Card failed!", 5, 40);
    return;
  }

  display.drawString("SD OK!", 5, 40);

  Serial.print("Card type: ");
  switch (SD.cardType()) {
    case CARD_NONE: Serial.println("No SD card attached"); break;
    case CARD_MMC: Serial.println("MMC"); break;
    case CARD_SD: Serial.println("SDSC"); break;
    case CARD_SDHC: Serial.println("SDHC"); break;
    default: Serial.println("Unknown");
  }

  Serial.print("Card size: ");
  Serial.print(SD.cardSize() / (1024 * 1024));
  Serial.println(" MB");

  root = SD.open("/");

  // Audio
  audioLogger = &Serial;

  // Set up I2S output
  out = new AudioOutputI2S(0, 0, 128);
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  out->SetGain(0.1);
  out->SetOutputModeMono(true);

  // Open the MP3 file from SD
  file = new AudioFileSourceSD("/music2.mp3");
  buff = new AudioFileSourceBuffer(file, 4096);
  buff->RegisterStatusCB(StatusCallback, (void *)"buffer");
  id3 = new AudioFileSourceID3(buff);
  id3->RegisterMetadataCB(MDCallback, (void *)"ID3TAG");

  void *space = malloc(29192);
  mp3 = new AudioGeneratorMP3(space, 29192);
  mp3->begin(id3, out);

  // Joystick
  pinMode(SW_PIN, INPUT_PULLUP);

  // Radio
  cc1101.init();
	Serial.printf("CC1101: 0x%02x, version: 0x%02x\n", cc1101.getPartnum(), cc1101.getVersion());
	cc1101.setMHZ(433.42);
	cc1101.setTXPwr(TX_0_DBM);
  cc1101.setRxBW(RX_BW_58_KHZ);
	cc1101.setDataRate(10000);
	cc1101.setModulation(ASK_OOK);
	cc1101.setRx();

  int interruptPin = digitalPinToInterrupt(RADIO_GDO2);
	attachInterrupt(interruptPin, radioHandlerOnChange, CHANGE);

  // Done!
  Serial.println("Hello from Arduino!");
}

volatile long last_millis;

void loop() {
  int xValue = analogRead(VRX_PIN);
  int yValue = analogRead(VRY_PIN);
  int buttonState = digitalRead(SW_PIN);

  if (xValue < 1800 || xValue > 2200 || yValue < 1800 || yValue > 2200) {
    Serial.print("X: ");
    Serial.print(xValue);
    Serial.print(" | Y: ");
    Serial.print(yValue);
    Serial.print(" | Button: ");
    Serial.println(buttonState == LOW ? "Pressed" : "Not Pressed");
  }

  /*if (mp3->isRunning()) {
    if (!mp3->loop()) mp3->stop();
  } else {
    Serial.printf("MP3 done\n");
    delay(1000);
    return;
  }*/

  if (millis() > (last_millis + 5000)){
		cc1101.setIdle();
		Serial.println(buffer);
		buffer = "";
		cc1101.setRx();
		
		last_millis = millis();
	}

  rotateImage();
}

static File entry;

void rotateImage() {
  if (lastImageChange != 0 && millis() - lastImageChange < 10000) {
    return;
  }

  entry = root.openNextFile();
  if (!entry) {
    root.rewindDirectory();
    return;
  }

  if (String(entry.name()).startsWith("._")) {
    return;
  }

  if (!String(entry.name()).endsWith(".bmp")) {
    return;
  }

  String filename = String("/") + entry.name();
  drawBMP(filename.c_str(), 0, 0);
  display.drawString(filename, 5, 8);

  lastImageChange = millis();
}

// Function to load BMP images from SD card
void drawBMP(const char *filename, int16_t x, int16_t y) {
  File file = SD.open(filename);
  if (!file) {
    Serial.print("File not found: ");
    Serial.println(filename);
    return;
  }
  display.drawBmp(&file, x, y);
  file.close();
}