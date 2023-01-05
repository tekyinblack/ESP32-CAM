/*********
  Rui Santos
  Complete instructions at https://RandomNerdTutorials.com/esp32-cam-projects-ebook/

  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files.
  The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
*********/

#include "esp_camera.h"
#include "ESPmDNS.h"
#include <WiFi.h>
#include "esp_timer.h"
#include "img_converters.h"
#include "Arduino.h"
#include "fb_gfx.h"
#include "soc/soc.h"           // disable brownout problems
#include "soc/rtc_cntl_reg.h"  // disable brownout problems
#include "esp_http_server.h"
#include "FS.h"
#include "SD_MMC.h"
#include "SPI.h"

#define WIFI 1
#define CMDFILE 2
#define CONFIG 3
#define EMAIL 4
#define STATUS 5
#define LED_BUILTIN 33

int rightMotorPin = 4;
int leftMotorPin = 0;
const int PWMFreq = 1000; /* 1 KHz */
const int PWMResolution = 8;
const int PWMSpeedChannel = 4;
const int rightMotorPWMSpeedChannel = 4;
const int leftMotorPWMSpeedChannel = 5;
int leftLDR = 12;
int rightLDR = 13;

long runTimer = 0;

char *htmlPage;

// network credentials
char configWifi[20] = "/config.txt";    // << file to be used from SD card for network setup
char ssid[33] = "DUMMY_SSID";           // dummy ssid text indicating an error
char password[33] = "DUMMY_PASSWORD";   // dummy wifi password indicating an error
char configHostname[20] = "DummyHost";  // dummy host name
char configWiFiType[4] = "STA";         // either run the board in client or access point mode
char htmlFile[20] = "notfound";         // web page to be used to command robot and view video
long configWebPort= 80;           // port number to be used for web server
long configStreamPort = 81;        // port number to be used for video stream



// webserver part boundary
#define PART_BOUNDARY "123456789000000000000987654321"

// camera connectivity defines

#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27

#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22


static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t camera_httpd = NULL;
httpd_handle_t stream_httpd = NULL;


// following is default webpage if html file load fails. It's intended to show a video stream on port 81
static const char PROGMEM BASIC[] = R"rawliteral(
<html>
  <head>
    <title>Default Robot Page</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
  </head>
  <body>
    <h1 style="font-family: Arial; text-align: center; margin:0px auto; padding-top: 30px;;">Robot Video Feed</h1>
    <img src="" id="photo" alt="Robot video Feed" style="display: block;margin-left: auto;margin-right: auto;width: 70%;">
   <script>
   window.onload = document.getElementById("photo").src = window.location.protocol + "//" + window.location.hostname + ":81/stream";
  </script>
  </body>
</html>
)rawliteral";

// initialise PWM for direct control from the board
void initPWM() {
  //Set up PWM for motor speed
  ledcSetup(rightMotorPWMSpeedChannel, PWMFreq, PWMResolution);
  ledcSetup(leftMotorPWMSpeedChannel, PWMFreq, PWMResolution);
  ledcAttachPin(rightMotorPin, rightMotorPWMSpeedChannel);
  ledcAttachPin(leftMotorPin, leftMotorPWMSpeedChannel);
}

// file parse routine to obtain data from SD card files
void fileParse(char *line, int fileType, int lineNo) {
  String workingString;
  char port_temp[6];
  switch (fileType) {
    case WIFI:
      switch (lineNo) {
        case 1:
          strcpy(configHostname, line);
          Serial.println(configHostname);
          break;
        case 2:
          strcpy(configWiFiType, line);
          Serial.println(configWiFiType);
          break;
        case 3:
          strcpy(ssid, line);
          Serial.println(ssid);
          break;
        case 4:
          strcpy(password, line);
          Serial.println(password);
          break;
        case 5:
          strcpy(htmlFile, line);
          Serial.println(htmlFile);
          break;
        case 6:
          workingString = String(line);
          configWebPort = workingString.toInt();
          Serial.println(configWebPort);
          break;
        case 7:
          workingString = String(line);
          configStreamPort = workingString.toInt();
          Serial.println(configStreamPort);
          break;
      }
      break;
    case CMDFILE:
      switch (lineNo) {
        case 1:
          //          strcpy(author_email, line);
          break;
        case 2:
          //         strcpy(author_password, line);
          break;
        case 3:
          //        strcpy(smtp_host, line);
          break;
        case 4:
          //        workingString = String(line);
          //         smtp_port = workingString.toInt();
          break;
      }
      break;
  }
}

// routine to read a data file and parse according to the type ----------------------------------
int read_datafile(fs::FS &fs, const char *path, int fileType) {
  char temp;
  char test = '\n';
  char buf[200];
  int i = 0;
  int linecount = 0;

  Serial.printf("Reading file: %s\n", path);

  File file = fs.open(path);
  if (!file) {
    Serial.println("Failed to open file for reading");
    return 1;
  }

  while (file.available()) {
    int l = file.readBytesUntil('\n', buf, sizeof(buf));
    if (l > 0 && buf[l - 1] == '\r') {
      l--;
    }
    buf[l] = 0;
    linecount++;
    fileParse(buf, fileType, linecount);
    i = 0;
  }

  Serial.println("File closed");
  file.close();
  return 0;
}

// routine to setup SD card for file attachments ----------------------------------
int setup_sdcard() {
  Serial.println("Mount SD card");
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("Card Mount Failed");
    return 1;
  }

  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("No SD card attached");
    return 1;
  }
  return 0;
}

// routine to load html page into variable
int readHtml(fs::FS &fs, const char *htmlf) {

  // get the file name
  // list the root directory checking for a filename match
  // on match, get the file size
  // allocate (or reallocate) the buffer to the size of file + 10 bytes
  // close files

  int linecount = 0;
  long htmlSize = 0;
  char htmlOpen[21] = "/";

  Serial.printf("Reading file: %s\n", htmlf);

  File root = fs.open("/");
  if (!root) {
    Serial.println("Failed to open directory");
    return 3;
  }
  if (!root.isDirectory()) {
    Serial.println("Not a directory");
    return 2;
  }

  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      Serial.print("  FILE: ");
      Serial.print(file.name());
      Serial.print("  SIZE: ");
      Serial.println(file.size());
      if (strcmp(htmlf, file.name()) == 0) {
        htmlSize = file.size();
        Serial.print("Match found:");
        Serial.println(htmlSize);
        break;
      }
    }
    file = root.openNextFile();
  }
  file.close();
  root.close();
  for (int i = 1; i < 20; i++) {
    htmlOpen[i] = htmlf[i - 1];
  }
  Serial.printf("Reading file: %s\n", htmlOpen);
  File page = fs.open(htmlOpen);
  if (!page) {
    Serial.println("Failed to open file for reading");
    return 1;
  }
  htmlPage = (char *)malloc(htmlSize + 10);

  page.readBytes(htmlPage, htmlSize);
  htmlPage[htmlSize] = 0;
  Serial.println("File closed");
  page.close();
  return 0;
}

// startup routine to get config details and then wifi and email details ----------------------------------
void read_SDcard(void) {
  Serial.println("Read data file");
  read_datafile(SD_MMC, configWifi, WIFI);
  //  read_datafile(SD, configEmail, SMTP);
  //  read_datafile(SD, "/status.txt", STATUS);

  if (readHtml(SD_MMC, htmlFile)) {
    // load default basic page if SD based page doesn't load
    htmlPage = (char *)malloc(strlen(BASIC) + 10);
    strcpy(htmlPage, BASIC);
  }
}


// send index page on first contact and refresh
static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, (const char *)htmlPage, strlen(htmlPage));
}

// send video stream data
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t *_jpg_buf = NULL;
  char *part_buf[64];

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) {
    return res;
  }

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
    } else {
      if (fb->width > 400) {
        if (fb->format != PIXFORMAT_JPEG) {
          bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
          esp_camera_fb_return(fb);
          fb = NULL;
          if (!jpeg_converted) {
            Serial.println("JPEG compression failed");
            res = ESP_FAIL;
          }
        } else {
          _jpg_buf_len = fb->len;
          _jpg_buf = fb->buf;
        }
      }
    }
    if (res == ESP_OK) {
      size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }
    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    } else if (_jpg_buf) {
      free(_jpg_buf);
      _jpg_buf = NULL;
    }
    if (res != ESP_OK) {
      break;
    }
  }
  return res;
}

// process received data from web page
int cmdProcessor(char variable[]) {
  Serial.print("Command = ");
  Serial.println(variable);

  char sendChar[32] = {
    0,
  };
  // process remote processor commands by schedule sending
  if (variable[0] == 'X') {
    int i;
    for (i = 1; i < strlen(variable); i++) {
      sendChar[i - 1] = variable[i];
    }
    sendChar[i] = '/0';
    Serial.println(sendChar);
  }
  // process command file by schedule opening
  else if (variable[0] == 'F') {
    int i;
    for (i = 1; i < strlen(variable); i++) {
      sendChar[i - 1] = variable[i];
    }
    sendChar[i] = '/0';
    Serial.println(sendChar);
    // set file status to 1 , process file
    // put filename in file open format
  }
  // process local comand directly ie reset
  else if (variable[0] == 'L') {
    int i;
    for (i = 1; i < strlen(variable); i++) {
      sendChar[i - 1] = variable[i];
    }
    sendChar[i] = '/0';
    Serial.println(sendChar);
  } else {
    return 1;
  }
  return 0;
}

// process received GET request. This primarily gets the variable and passes it to the cmdprocessor
static esp_err_t cmd_handler(httpd_req_t *req) {
  char *buf;
  size_t buf_len;
  char variable[32] = {
    0,
  };

  buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1) {
    buf = (char *)malloc(buf_len);
    if (!buf) {
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
      if (httpd_query_key_value(buf, "go", variable, sizeof(variable)) == ESP_OK) {
      } else {
        free(buf);
        httpd_resp_send_404(req);
        return ESP_FAIL;
      }
    } else {
      free(buf);
      httpd_resp_send_404(req);
      return ESP_FAIL;
    }
    free(buf);
  } else {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  sensor_t *s = esp_camera_sensor_get();


  if (cmdProcessor(variable)) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

// start the video stream server
void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = configWebPort;
  httpd_uri_t index_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_handler,
    .user_ctx = NULL
  };

  httpd_uri_t cmd_uri = {
    .uri = "/action",
    .method = HTTP_GET,
    .handler = cmd_handler,
    .user_ctx = NULL
  };
  httpd_uri_t stream_uri = {
    .uri = "/stream",
    .method = HTTP_GET,
    .handler = stream_handler,
    .user_ctx = NULL
  };
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &cmd_uri);
  }
 // config.server_port += 1;
  config.server_port = configStreamPort;
  config.ctrl_port += 1;
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}

// the main startup routine
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  //disable brownout detector
                                              //  #if INDEX_HTML == BASIC

  Serial.begin(115200);
  Serial.setDebugOutput(false);
  //delay(5000);
  Serial.println("Starting....");
  Serial.println(String(__FILE__) + " " + String(__DATE__) + " " + String(__TIME__));
  //delay(5000);

  // get the sd card data
  setup_sdcard();
  read_SDcard();


  Serial.println("Init PWM");
  initPWM();
  pinMode(rightLDR, INPUT);
  pinMode(leftLDR, INPUT);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // use camera defines to setup cammera access
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  // config.pixel_format = PIXFORMAT_RGB565;
  config.pixel_format = PIXFORMAT_JPEG;

  //  if(psramFound()){
  //    config.frame_size = FRAMESIZE_VGA;
  //    config.jpeg_quality = 10;
  //    config.fb_count = 2;
  //  } else {
  //    config.frame_size = FRAMESIZE_SVGA;
  //    config.jpeg_quality = 12;
  //    config.fb_count = 1;
  //  }

  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 10;
  config.fb_count = 2;

  // Camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  // Wi-Fi connection
  Serial.println(ssid);
  Serial.println(password);
  //  WiFi.config(local_IP,gateway,subnet,primaryDNS,secondaryDNS);
  WiFi.setHostname(configHostname);
  // if not APT then setup a wifi clinet
  if (strcmp(configWiFiType, "APT")) {
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    Serial.println("");
    Serial.println("WiFi connected");
  } else {  // otherwise create a wifi hot spot access point
    Serial.println("Configuring access point...");
    WiFi.softAP(ssid, password);
    IPAddress myIP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(myIP);
  }
  // register the hostname for use with DNS
  if (!MDNS.begin(configHostname)) {
    Serial.println("Error starting mDNS");
    return;
  }

  Serial.print("Camera Stream Ready! Go to: http://");
  Serial.println(WiFi.localIP());

  // Start streaming web server
  startCameraServer();
}

void loop() {
  int fileStatus = 0;
  int fileProcess = 0;
  delay(10);
  if (WiFi.status() == WL_CONNECTED) {
    delay(500);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(500);
    digitalWrite(LED_BUILTIN, LOW);
  } else {
    digitalWrite(LED_BUILTIN, LOW);
  }
  /* File command processor
  File is opened and contents fed through common processor routine
  open file
  read line
  send to command processor
  cycle until command processor indicates command executed
  if end of file, close file and wait again
  send check byte
  wait for ack byte, if none then assume not connected, resend after 10 seconds
  when ack byte received, send next command and flag sent
  
  */
  switch (fileStatus) {
    case 0:
      // file processing not scheduled
      break;
    case 1:
      // file processing scheduled
      // open file
      // if file open ok, update status to 2, and file process to 0
      // if open fails, update status to 0
      break;
    case 2:
      // file read process cycle in operation
      switch (fileProcess) {
        case 0:
          // read next file line
          // if end of file, close file, update status to 0 and break
          // update file process to 1
          break;
        case 1:
          // handover command to command processor
          // if not accepted break
          // if accepted, update file process to 2
          break;
        case 2:
          // check if command processor now free, ie command complete
          // if so, update file process to 0
          break;
      }
  }

  // if (rightLDR) {
  //   Serial.println("Right LDR");
  //   ledcWrite(rightMotorPWMSpeedChannel, abs(255));
  // } else ledcWrite(rightMotorPWMSpeedChannel, abs(0));
  // if (leftLDR) {
  //   Serial.println("Left LDR");
  //   ledcWrite(leftMotorPWMSpeedChannel, abs(255));
  // } else ledcWrite(leftMotorPWMSpeedChannel, abs(0));
}
