#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_ILI9341.h>
#include <Adafruit_GFX.h>
#include <FS.h>
// XPT2046 Lib @  https://github.com/spapadim/XPT2046
#include <XPT2046.h>


// GPIO mapping
#define TFT_DC     2
#define TFT_CS     4
#define TFT_BL     0
#define TOUCH_CS  16
#define TOUCH_IRQ  5

Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC);

//#define TOUCH

#ifdef TOUCH
XPT2046 touch(TOUCH_CS, TOUCH_IRQ);
#endif // TOUCH


void bl_on(bool on)
{
  digitalWrite(TFT_BL, on ? LOW : HIGH);
}

void bmpDraw(char *filename, uint16_t x, uint16_t y);
uint16_t read16(File & f);
uint32_t read32(File & f);

/**************************************************************************/
/*!
    @brief  Converts a 24-bit RGB color to an equivalent 16-bit RGB565 value

    @param[in]  r  8-bit red
    @param[in]  g  8-bit green
    @param[in]  b  8-bit blue

    @section Example

    @code 

    // Get 16-bit equivalent of 24-bit color
    uint16_t gray = drawRGB24toRGB565(0x33, 0x33, 0x33);

    @endcode
*/
/**************************************************************************/
uint16_t drawRGB24toRGB565(uint8_t r, uint8_t g, uint8_t b)
{
  return ((r / 8) << 11) | ((g / 4) << 5) | (b / 8);
}

void spiffs_ls()
{
  Dir dir = SPIFFS.openDir("/");
  while (dir.next()) {
      Serial.print(dir.fileName());
      Serial.printf("  ");
      File f = dir.openFile("r");
      Serial.println(f.size());
  }
}

void setup()
{
//  attachInterrupt(digitalPinToInterrupt(TOUCH_IRQ), touch_irq, RISING);
  SPI.setFrequency(1000000);

  pinMode(TFT_BL, OUTPUT);
  bl_on(false);

  Serial.begin(115200);
  Serial.println("");
  Serial.println("");
  Serial.println("    *** Commadorable  64 Basic V2 ***");
  Serial.println(" 64K system RAM, 38911 BASIC bytes free.");
  Serial.println("");

  
  SPIFFS.begin();
  spiffs_ls();

  tft.begin();
  tft.fillScreen(ILI9341_BLUE);
#ifdef TOUCH
  touch.begin(320, 240);  // Must be done before setting rotation
//  touch.setRotation(touch.ROT270);
//  touch.setCalibration(209, 1759, 1775, 273);
#endif // TOUCH

  bmpDraw((char*) "/c64.bmp", 0, 0);
  bl_on(true);


  Serial.println("READY");
}

void loop(void)
{
#ifdef TOUCH
  if (touch.isTouching()) {
    uint16_t x, y;
//    touch.getRaw (x, y, touch.MODE_DFR, 1);
    touch.getPosition(x, y);
    Serial.print(x);
    Serial.print(" ");
    Serial.println(y);
    delay(80);
  }
#else // TOUCH
  uint16_t light = drawRGB24toRGB565(165, 165, 175);
  uint16_t dark  = drawRGB24toRGB565( 66,  66, 231);
  tft.fillRect(56, 304, 8, 8, light);
  delay(800);
  tft.fillRect(56, 304, 8, 8, dark);
  delay(800);
#endif // TOUCH
}

#define BUFFPIXEL 20

void bmpDraw(char *filename, uint16_t x, uint16_t y)
{
  File     bmpFile;
  int      bmpWidth, bmpHeight;   // W+H in pixels
  uint8_t  bmpDepth;              // Bit depth (currently must be 24)
  uint32_t bmpImageoffset;        // Start of image data in file
  uint32_t rowSize;               // Not always = bmpWidth; may have padding
  uint8_t  sdbuffer[3*BUFFPIXEL]; // pixel buffer (R+G+B per pixel)
  uint8_t  buffidx = sizeof(sdbuffer); // Current position in sdbuffer
  boolean  goodBmp = false;       // Set to true on valid header parse
  boolean  flip    = true;        // BMP is stored bottom-to-top
  int      w, h, row, col;
  uint8_t  r, g, b;
  uint32_t pos = 0, startTime = millis();

  if((x >= tft.width()) || (y >= tft.height())) return;

  Serial.println();
  Serial.print("Loading image '");
  Serial.print(filename);
  Serial.println('\'');

  // Open requested file on SD card
  bmpFile = SPIFFS.open(filename, "r");
  if (!bmpFile) {
    Serial.println("File not found");
    return;
  }

  // Parse BMP header
  if(read16(bmpFile) == 0x4D42) { // BMP signature
    Serial.print("File size: "); Serial.println(read32(bmpFile));
    (void)read32(bmpFile); // Read & ignore creator bytes
    bmpImageoffset = read32(bmpFile); // Start of image data
    Serial.print("Image Offset: "); Serial.println(bmpImageoffset, DEC);
    // Read DIB header
    Serial.print("Header size: "); Serial.println(read32(bmpFile));
    bmpWidth  = read32(bmpFile);
    bmpHeight = read32(bmpFile);
    if(read16(bmpFile) == 1) { // # planes -- must be '1'
      bmpDepth = read16(bmpFile); // bits per pixel
      Serial.print("Bit Depth: "); Serial.println(bmpDepth);
      if((bmpDepth == 24) && (read32(bmpFile) == 0)) { // 0 = uncompressed

        goodBmp = true; // Supported BMP format -- proceed!
        Serial.print("Image size: ");
        Serial.print(bmpWidth);
        Serial.print('x');
        Serial.println(bmpHeight);

        // BMP rows are padded (if needed) to 4-byte boundary
        rowSize = (bmpWidth * 3 + 3) & ~3;

        // If bmpHeight is negative, image is in top-down order.
        // This is not canon but has been observed in the wild.
        if(bmpHeight < 0) {
          bmpHeight = -bmpHeight;
          flip      = false;
        }

        // Crop area to be loaded
        w = bmpWidth;
        h = bmpHeight;
        if((x+w-1) >= tft.width())  w = tft.width()  - x;
        if((y+h-1) >= tft.height()) h = tft.height() - y;

        // Set TFT address window to clipped image bounds
        tft.setAddrWindow(x, y, x+w-1, y+h-1);

        for (row=0; row<h; row++) { // For each scanline...

          // Seek to start of scan line.  It might seem labor-
          // intensive to be doing this on every line, but this
          // method covers a lot of gritty details like cropping
          // and scanline padding.  Also, the seek only takes
          // place if the file position actually needs to change
          // (avoids a lot of cluster math in SD library).
          if(flip) // Bitmap is stored bottom-to-top order (normal BMP)
            pos = bmpImageoffset + (bmpHeight - 1 - row) * rowSize;
          else     // Bitmap is stored top-to-bottom
            pos = bmpImageoffset + row * rowSize;
          if(bmpFile.position() != pos) { // Need seek?
            bmpFile.seek(pos, SeekSet);
            buffidx = sizeof(sdbuffer); // Force buffer reload
          }

          for (col=0; col<w; col++) { // For each pixel...
            // Time to read more pixel data?
            if (buffidx >= sizeof(sdbuffer)) { // Indeed
              bmpFile.read(sdbuffer, sizeof(sdbuffer));
              buffidx = 0; // Set index to beginning
            }

            // Convert pixel from BMP to TFT format, push to display
            b = sdbuffer[buffidx++];
            g = sdbuffer[buffidx++];
            r = sdbuffer[buffidx++];
            tft.pushColor(drawRGB24toRGB565(r,g,b));
          } // end pixel
        } // end scanline
        Serial.print("Loaded in ");
        Serial.print(millis() - startTime);
        Serial.println(" ms");
      } // end goodBmp
    }
  }

  bmpFile.close();
  if(!goodBmp) {
    Serial.println("BMP format not recognized.");
  }
}

// These read 16- and 32-bit types from the SD card file.
// BMP data is stored little-endian, Arduino is little-endian too.
// May need to reverse subscript order if porting elsewhere.

uint16_t read16(File & f)
{
  uint16_t result;
  ((uint8_t *)&result)[0] = f.read(); // LSB
  ((uint8_t *)&result)[1] = f.read(); // MSB
  return result;
}

uint32_t read32(File & f)
{
  uint32_t result;
  ((uint8_t *)&result)[0] = f.read(); // LSB
  ((uint8_t *)&result)[1] = f.read();
  ((uint8_t *)&result)[2] = f.read();
  ((uint8_t *)&result)[3] = f.read(); // MSB
  return result;
}
