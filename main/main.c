/* Simple HTTP + SSL Server Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include "esp_timer.h"
#include <sys/param.h>
#include "esp_netif.h"
#include "esp_eth.h"
#include <string.h>
#include "ota_server.h"
#include "protocol_examples_common.h"
#include "esp_camera.h"

#include <esp_http_server.h>

#include "httpd_basic_auth.h"

/* A simple example that demonstrates how to create GET and POST
 * handlers and start an HTTPS server.
*/
#define WEB_LOGIN       "tsn"
#define WEB_PASSWORD    "tsnpass"

#define CAM_PIN_PWDN    32 //power down is not used
#define CAM_PIN_RESET   -1 //software reset will be performed
#define CAM_PIN_XCLK    0
#define CAM_PIN_SIOD    26
#define CAM_PIN_SIOC    27

#define CAM_PIN_D7      35
#define CAM_PIN_D6      34
#define CAM_PIN_D5      39
#define CAM_PIN_D4      36
#define CAM_PIN_D3      21
#define CAM_PIN_D2      19
#define CAM_PIN_D1      18
#define CAM_PIN_D0       5
#define CAM_PIN_VSYNC   25
#define CAM_PIN_HREF    23
#define CAM_PIN_PCLK    22

static camera_config_t camera_config = {
    .pin_pwdn  = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk = CAM_PIN_XCLK,
    .pin_sscb_sda = CAM_PIN_SIOD,
    .pin_sscb_scl = CAM_PIN_SIOC,

    .pin_d7 = CAM_PIN_D7,
    .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5,
    .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3,
    .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1,
    .pin_d0 = CAM_PIN_D0,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href = CAM_PIN_HREF,
    .pin_pclk = CAM_PIN_PCLK,

    .xclk_freq_hz = 20000000,//EXPERIMENTAL: Set to 16MHz on ESP32-S2 or ESP32-S3 to enable EDMA mode
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_JPEG,//YUV422,GRAYSCALE,RGB565,JPEG
    .frame_size = FRAMESIZE_VGA,//QQVGA-QXGA Do not use sizes above QVGA when not JPEG

    .jpeg_quality = 12, //0-63 lower number means higher quality
    .fb_count = 6, //if more than one, i2s runs in continuous mode. Use only with JPEG
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY//CAMERA_GRAB_LATEST. Sets when buffers should be filled
};

SemaphoreHandle_t baton;

static const char *TAG = "TSN_CAM";


#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

esp_err_t jpg_stream_httpd_handler(httpd_req_t *req){
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len;
    uint8_t * _jpg_buf;
    char * part_buf[64];
    static int64_t last_frame = 0;
    if(!last_frame) {
        last_frame = esp_timer_get_time();
    }

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if(res != ESP_OK){
        return res;
    }

    while(true){
        xSemaphoreTake( baton, portMAX_DELAY );
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
            break;
        }
        if(fb->format != PIXFORMAT_JPEG){
            bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
            if(!jpeg_converted){
                ESP_LOGE(TAG, "JPEG compression failed");
                esp_camera_fb_return(fb);
                res = ESP_FAIL;
            }
        } else {
            _jpg_buf_len = fb->len;
            _jpg_buf = fb->buf;
        }
        xSemaphoreGive( baton );
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if(res == ESP_OK){
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);

            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if(fb->format != PIXFORMAT_JPEG){
            free(_jpg_buf);
        }
        esp_camera_fb_return(fb);
        if(res != ESP_OK){
            break;
        }
        int64_t fr_end = esp_timer_get_time();
        int64_t frame_time = fr_end - last_frame;
        last_frame = fr_end;
        frame_time /= 1000;
        ESP_LOGI(TAG, "MJPG: %uKB %ums (%.1ffps)",
            (uint32_t)(_jpg_buf_len/1024),
            (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time);
    }

    last_frame = 0;
    return res;
}

/* An HTTP GET handler */

static esp_err_t index_handler(httpd_req_t *req) {
	if(httpd_basic_auth(req, WEB_LOGIN, WEB_PASSWORD) == ESP_OK) {
		jpg_stream_httpd_handler(req);
	} else {
		httpd_basic_auth_resp_send_401(req);
		httpd_resp_sendstr(req, "Not Authorized");
		return ESP_FAIL;
	}

	return ESP_OK;
}

static int clamp(int n, int lower, int upper)
{
    return n <= lower ? lower : n >= upper ? upper : n;
}

uint8_t temprature_sens_read();
static esp_err_t metrics_handler(httpd_req_t *req) {
	if(httpd_basic_auth(req, WEB_LOGIN, WEB_PASSWORD) == ESP_OK) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_set_hdr(req, "Connection", "keep-alive");
        char* p = NULL;
        float temperature = ((temprature_sens_read() - 32) / 1.8);
        wifi_ap_record_t info;
        int rssi;
        esp_wifi_sta_get_ap_info(&info);
        rssi = info.rssi;
        asprintf(&p, "esp32_uptime %lld\nesp32_temperature %f\nesp32_free_heap %d\nesp32_wifi_signal %d",  esp_timer_get_time() / 1000000, temperature, esp_get_free_heap_size(), clamp((rssi + 100) * 2, 0, 100));
        httpd_resp_send(req, p, strlen(p));
        free(p);
        return ESP_OK;
	} else {
		httpd_basic_auth_resp_send_401(req);
		httpd_resp_sendstr(req, "Not Authorized");
		return ESP_FAIL;
	}

	return ESP_OK;
}

static const httpd_uri_t cam = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_handler
};
static const httpd_uri_t cam2 = {
    .uri       = "/metrics",
    .method    = HTTP_GET,
    .handler   = metrics_handler
};



static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server");

    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Camera init failed with error 0x%x", err);
    }
    app_ota_main();
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    esp_err_t ret = httpd_start(&server, &config);
    if (ESP_OK != ret) {
        ESP_LOGI(TAG, "Error starting server!");
        return NULL;
    }

    // Set URI handlers
    ESP_LOGI(TAG, "Registering URI handlers");
    //httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &cam);
    httpd_register_uri_handler(server, &cam2);
    return server;
}

static void stop_webserver(httpd_handle_t server)
{
    // Stop the httpd server
    httpd_stop(server);
}

static void disconnect_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server) {
        stop_webserver(*server);
        *server = NULL;
    }
}

static void connect_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data)
{

    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server == NULL) {
        *server = start_webserver();
    }
}

void app_main(void)
{
    static httpd_handle_t server = NULL;
    baton = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Register event handlers to start server when Wi-Fi is connected,
     * and stop server when disconnection happens.
     */
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));

    /* This helper function configures Wi-Fi, as selected in menuconfig.
     * Read "Establishing Wi-Fi Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());
}
