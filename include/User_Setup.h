// TFT_eSPI configuration for the ESP32-2432S028R "Cheap Yellow Display" (CYD).
// Pin map matches the community reference:
//   https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display
// Loaded via platformio.ini: -DUSER_SETUP_LOADED=1 -include include/User_Setup.h

// Two hardware variants are sold under the same name:
//   - ONE USB port (micro-USB)            -> ILI9341 panel  (env:cyd)
//   - TWO USB ports (USB-C + micro-USB)   -> ST7789 panel   (env:cyd2usb)
// Wrong variant = inverted / wrong colours. Pick the matching PlatformIO env.
#ifdef CYD_2USB
#define ST7789_DRIVER
#define TFT_RGB_ORDER TFT_BGR
#define TFT_INVERSION_OFF
#else
#define ILI9341_2_DRIVER  // alternative init sequence used by the CYD panel
#endif

#define TFT_WIDTH 240
#define TFT_HEIGHT 320

// Display SPI. USE_HSPI_PORT matters: without it TFT_eSPI drives the display
// from VSPI, the same peripheral src/ui/ui.cpp uses for the touch controller.
#define USE_HSPI_PORT
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS 15
#define TFT_DC 2
#define TFT_RST -1  // display reset tied to ESP32 EN
#define TFT_BL 21
#define TFT_BACKLIGHT_ON HIGH

// Note: the XPT2046 touch controller is NOT driven by TFT_eSPI here — it sits on
// its own SPI bus (CLK 25 / MOSI 32 / MISO 39 / CS 33 / IRQ 36) and is handled by
// the XPT2046_Touchscreen library. See src/ui/ui.cpp.

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

#define SPI_FREQUENCY 55000000
#define SPI_READ_FREQUENCY 20000000
