#include <Adafruit_NeoPixel.h>
#include <LovyanGFX.hpp>
#include <SD.h>
#include <SPIFFS.h>

#include "AudioFileSourceSD.h"
#include "AudioFileSourceBuffer.h"
#include "AudioFileSourceID3.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

#include <SPI.h>
#include <RadioLib.h>

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
#define RADIO_GDO0 45  // TX/input pin
#define RADIO_GDO1 14  // MISO
#define RADIO_GDO2 18  // RX/output pin

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

SPIClass spi2 = SPIClass(HSPI);
SPISettings spiSettings = SPISettings(10000000, MSBFIRST, SPI_MODE0);
CC1101 radio = new Module(RADIO_CS, RADIO_GDO0, RADIOLIB_NC, RADIO_GDO2, spi2, spiSettings);

volatile bool receivedFlag = false;

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
  spi2.begin(RADIO_SCK, RADIO_GDO1, RADIO_MOSI, RADIO_CS);
  //radio.reset();

  int state = radio.beginFSK4(433.92);
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println(F("radio success!"));
  } else {
    Serial.print(F("radio failed, code "));
    Serial.println(state);
    while (true) { delay(10); }
  }

  Serial.print(F("Radio chip version: "));
  Serial.println(radio.getChipVersion());

  radio.setOOK(true);
  radio.setDirectAction(setFlag);
  radio.setPromiscuousMode();

  // start listening for packets
  Serial.print(F("[CC1101] Starting to listen ... "));
  state = radio.receiveDirectAsync();
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println(F("success!"));
  } else {
    Serial.print(F("failed, code "));
    Serial.println(state);
    while (true) { delay(10); }
  }

  // Done!
  Serial.println("Hello from Arduino!");
}

ICACHE_RAM_ATTR void setFlag(void) {
  receivedFlag = true;
}

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

  if (receivedFlag) {
    receivedFlag = false;

    String str;
    int state = radio.readData(str);

    if (radio.getRSSI() > -40) {
      if (state == RADIOLIB_ERR_NONE) {
        Serial.println("[CC1101] Received packet!");

        Serial.print("[CC1101] Data:\t\t");
        Serial.println(str);

        Serial.print("[CC1101] Data (Hex):\t");
        for (size_t i = 0; i < str.length(); i++) {
          Serial.print("0x");
          Serial.print(str[i], HEX);  // Print each character as a hex value
          Serial.print(" ");
        }
        Serial.println();

        Serial.print("[CC1101] RSSI:\t\t");
        Serial.print(radio.getRSSI());
        Serial.println(" dBm");
        Serial.print("[CC1101] LQI:\t\t");
        Serial.println(radio.getLQI());
      } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
        Serial.println("CRC error!");
      } else {
        Serial.print("failed, code ");
        Serial.println(state);
      }
    }

    radio.receiveDirectAsync();
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