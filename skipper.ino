#include <Adafruit_NeoPixel.h>
#include <LovyanGFX.hpp>
#include <SD.h>

#define LED_PIN 38
#define SD_CS 7

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
      cfg.freq_write = 40000000;
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

  // Done!
  Serial.println("Hello from Arduino!");
}

static File entry;

void loop() {
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
  delay(1000);
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

