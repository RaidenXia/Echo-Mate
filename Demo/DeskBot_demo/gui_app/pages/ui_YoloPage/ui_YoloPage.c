#include "ui_YoloPage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <curl/curl.h>
#include <json-c/json.h>

// 屏幕尺寸
#define SCREEN_W 800
#define SCREEN_H 480

// 摄像头采集分辨率
#define CAM_W    320
#define CAM_H    240

// 摄像头设备节点（请根据实际修改，例如 /dev/video0 或 /dev/video1）
#define CAM_DEV   "/dev/video1"

// 主机服务地址（修改为你的主机IP和端口）
#define SERVER_URL "http://10.3.123.36:8765/detect"

#define MAX_BUFFERS 4

typedef struct {
    float x, y, w, h;
    int   label;
    float prob;
    char  name[64];
} detection_t;

// 全局变量
static lv_obj_t *yolo_page = NULL;
static lv_obj_t *img_cam = NULL;
static lv_obj_t *label_status = NULL;
static lv_obj_t *back_btn = NULL;
static lv_timer_t *refresh_timer = NULL;

// 摄像头相关
static int camera_fd = -1;
static void *buffer_start[MAX_BUFFERS];
static unsigned int buffer_length[MAX_BUFFERS];
static int camera_streaming = 0;
static pthread_t capture_thread;
static int capture_running = 0;

// 最新帧（RGB565）和互斥锁
static uint16_t *latest_rgb565 = NULL;
static pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;

// 检测结果
static detection_t detections[32];
static int det_count = 0;
static pthread_mutex_t det_mutex = PTHREAD_MUTEX_INITIALIZER;

// 用于保存标签对象（每个检测结果一个标签）
static lv_obj_t *text_labels[32];
static int text_label_count = 0;
static lv_obj_t *rect_boxes[32];
static int rect_count = 0;

// CURL 句柄
static CURL *curl = NULL;

// ------------------- base64 编码（简易）-------------------
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_encode(const unsigned char *data, size_t len) {
    size_t out_len = ((len + 2) / 3) * 4 + 1;
    char *out = malloc(out_len);
    if (!out) return NULL;

    size_t i = 0, j = 0;
    while (i < len) {
        uint32_t octet_a = i < len ? data[i++] : 0;
        uint32_t octet_b = i < len ? data[i++] : 0;
        uint32_t octet_c = i < len ? data[i++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = b64_table[(triple >> 6) & 0x3F];
        out[j++] = b64_table[triple & 0x3F];
    }
    size_t mod = len % 3;
    if (mod == 1) {
        out[j-2] = '=';
        out[j-1] = '=';
    } else if (mod == 2) {
        out[j-1] = '=';
    }
    out[j] = '\0';
    return out;
}

// ------------------- 摄像头初始化、采集等 -------------------
static int camera_init() {
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;

    camera_fd = open(CAM_DEV, O_RDWR);
    if (camera_fd == -1) {
        perror("Failed to open camera device");
        return -1;
    }

    if (ioctl(camera_fd, VIDIOC_QUERYCAP, &cap) == -1) {
        perror("Failed to query camera capabilities");
        close(camera_fd);
        return -1;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "Device does not support video capture\n");
        close(camera_fd);
        return -1;
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = CAM_W;
    fmt.fmt.pix.height = CAM_H;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(camera_fd, VIDIOC_S_FMT, &fmt) == -1) {
        perror("Failed to set video format");
        close(camera_fd);
        return -1;
    }

    printf("Camera format: YUYV %dx%d\n", CAM_W, CAM_H);

    memset(&req, 0, sizeof(req));
    req.count = MAX_BUFFERS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(camera_fd, VIDIOC_REQBUFS, &req) == -1) {
        perror("Failed to request buffers");
        close(camera_fd);
        return -1;
    }

    for (unsigned int i = 0; i < req.count; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(camera_fd, VIDIOC_QUERYBUF, &buf) == -1) {
            perror("Failed to query buffer");
            close(camera_fd);
            return -1;
        }

        buffer_start[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, camera_fd, buf.m.offset);
        if (buffer_start[i] == MAP_FAILED) {
            perror("Failed to map buffer");
            for (unsigned int j = 0; j < i; j++)
                munmap(buffer_start[j], buffer_length[j]);
            close(camera_fd);
            return -1;
        }
        buffer_length[i] = buf.length;
    }

    for (unsigned int i = 0; i < req.count; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(camera_fd, VIDIOC_QBUF, &buf) == -1) {
            perror("Failed to queue buffer");
            close(camera_fd);
            return -1;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(camera_fd, VIDIOC_STREAMON, &type) == -1) {
        perror("Failed to start streaming");
        close(camera_fd);
        return -1;
    }

    camera_streaming = 1;
    printf("Camera initialized successfully\n");
    return 0;
}

static void camera_cleanup() {
    if (camera_streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(camera_fd, VIDIOC_STREAMOFF, &type);
        camera_streaming = 0;
    }

    if (camera_fd != -1) {
        for (int i = 0; i < MAX_BUFFERS; i++) {
            if (buffer_start[i] && buffer_start[i] != MAP_FAILED) {
                munmap(buffer_start[i], buffer_length[i]);
            }
        }
        close(camera_fd);
        camera_fd = -1;
    }
    printf("Camera resources cleaned up\n");
}

// YUYV -> RGB888 (用于发送)
static void yuyv_to_rgb888(const uint8_t *yuyv, uint8_t *rgb, int width, int height) {
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j += 2) {
            uint8_t y0 = yuyv[0], u = yuyv[1], y1 = yuyv[2], v = yuyv[3];
            int r0 = y0 + ((359 * (v - 128)) >> 8);
            int g0 = y0 - ((88 * (u - 128) + 183 * (v - 128)) >> 8);
            int b0 = y0 + ((454 * (u - 128)) >> 8);
            r0 = r0 < 0 ? 0 : (r0 > 255 ? 255 : r0);
            g0 = g0 < 0 ? 0 : (g0 > 255 ? 255 : g0);
            b0 = b0 < 0 ? 0 : (b0 > 255 ? 255 : b0);
            *rgb++ = r0; *rgb++ = g0; *rgb++ = b0;

            int r1 = y1 + ((359 * (v - 128)) >> 8);
            int g1 = y1 - ((88 * (u - 128) + 183 * (v - 128)) >> 8);
            int b1 = y1 + ((454 * (u - 128)) >> 8);
            r1 = r1 < 0 ? 0 : (r1 > 255 ? 255 : r1);
            g1 = g1 < 0 ? 0 : (g1 > 255 ? 255 : g1);
            b1 = b1 < 0 ? 0 : (b1 > 255 ? 255 : b1);
            *rgb++ = r1; *rgb++ = g1; *rgb++ = b1;

            yuyv += 4;
        }
    }
}

// YUYV -> RGB565 (用于显示)
static inline uint16_t yuyv_to_rgb565(uint8_t y, uint8_t u, uint8_t v) {
    int r = y + ((359 * (v - 128)) >> 8);
    int g = y - ((88 * (u - 128) + 183 * (v - 128)) >> 8);
    int b = y + ((454 * (u - 128)) >> 8);
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static void yuyv_to_rgb565_full(const uint8_t *yuyv, uint16_t *rgb, int width, int height) {
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j += 2) {
            uint8_t y0 = yuyv[0], u = yuyv[1], y1 = yuyv[2], v = yuyv[3];
            *rgb++ = yuyv_to_rgb565(y0, u, v);
            *rgb++ = yuyv_to_rgb565(y1, u, v);
            yuyv += 4;
        }
    }
}

// ------------------- HTTP 通信 -------------------
struct memory {
    char *response;
    size_t size;
};

static size_t http_write_callback(void *ptr, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct memory *mem = (struct memory*)userp;
    char *new_ptr = realloc(mem->response, mem->size + realsize + 1);
    if (!new_ptr) return 0;
    mem->response = new_ptr;
    memcpy(&(mem->response[mem->size]), ptr, realsize);
    mem->size += realsize;
    mem->response[mem->size] = 0;
    return realsize;
}

static char* send_image_to_server(const uint8_t *rgb888, int width, int height) {
    if (!curl) return NULL;

    size_t img_size = width * height * 3;
    char *base64 = base64_encode(rgb888, img_size);
    if (!base64) return NULL;

    char *json_data = malloc(strlen(base64) + 100);
    if (!json_data) {
        free(base64);
        return NULL;
    }
    sprintf(json_data, "{\"image\":\"%s\"}", base64);
    free(base64);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    struct memory chunk = {0};
    chunk.response = malloc(1);
    chunk.size = 0;

    curl_easy_setopt(curl, CURLOPT_URL, SERVER_URL);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_data);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&chunk);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    free(json_data);

    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform failed: %s\n", curl_easy_strerror(res));
        free(chunk.response);
        return NULL;
    }

    return chunk.response;
}

static void parse_detections(const char *json_str) {
    // printf("parse_detections called with JSON: %s\n", json_str);
    json_object *root = json_tokener_parse(json_str);
    if (!root || !json_object_is_type(root, json_type_array)) {
        if (root) json_object_put(root);
        return;
    }

    int len = json_object_array_length(root);
    if (len > 32) len = 32;

    pthread_mutex_lock(&det_mutex);
    det_count = len;
    for (int i = 0; i < len; i++) {
        json_object *obj = json_object_array_get_idx(root, i);
        json_object *x_obj, *y_obj, *w_obj, *h_obj, *label_obj, *name_obj, *prob_obj;
        json_object_object_get_ex(obj, "x", &x_obj);
        json_object_object_get_ex(obj, "y", &y_obj);
        json_object_object_get_ex(obj, "w", &w_obj);
        json_object_object_get_ex(obj, "h", &h_obj);
        json_object_object_get_ex(obj, "label", &label_obj);
        json_object_object_get_ex(obj, "name", &name_obj);
        json_object_object_get_ex(obj, "prob", &prob_obj);

        detections[i].x = json_object_get_double(x_obj);
        detections[i].y = json_object_get_double(y_obj);
        detections[i].w = json_object_get_double(w_obj);
        detections[i].h = json_object_get_double(h_obj);
        detections[i].label = json_object_get_int(label_obj);
        detections[i].prob = json_object_get_double(prob_obj);
        strncpy(detections[i].name, json_object_get_string(name_obj), sizeof(detections[i].name)-1);
        detections[i].name[sizeof(detections[i].name)-1] = '\0';
    }
    pthread_mutex_unlock(&det_mutex);
    json_object_put(root);
}

// ------------------- 采集线程 -------------------
static void* capture_thread_func(void *arg) {
    struct v4l2_buffer buf;
    uint16_t *rgb565 = (uint16_t*)malloc(CAM_W * CAM_H * 2);
    uint8_t *rgb888 = (uint8_t*)malloc(CAM_W * CAM_H * 3);
    if (!rgb565 || !rgb888) {
        fprintf(stderr, "Failed to allocate frame buffers\n");
        capture_running = 0;
        free(rgb565); free(rgb888);
        return NULL;
    }

    while (capture_running) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(camera_fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) {
                usleep(10000);
                continue;
            }
            perror("VIDIOC_DQBUF");
            break;
        }

        // 转换为 RGB565（用于显示）和 RGB888（用于发送）
        yuyv_to_rgb565_full((uint8_t*)buffer_start[buf.index], rgb565, CAM_W, CAM_H);
        yuyv_to_rgb888((uint8_t*)buffer_start[buf.index], rgb888, CAM_W, CAM_H);

        // 发送到服务器（非阻塞，但这里同步发送会降低帧率，可后续优化）
        char *response = send_image_to_server(rgb888, CAM_W, CAM_H);
        if (response) {
            // printf("Server response: %s, ui_YoloPage.c: 395\n", response);
            parse_detections(response);
            free(response);
        } else {
            printf("send_image_to_server failed\n");
        }

        // 更新显示帧
        pthread_mutex_lock(&frame_mutex);
        if (latest_rgb565) free(latest_rgb565);
        latest_rgb565 = (uint16_t*)malloc(CAM_W * CAM_H * 2);
        if (latest_rgb565) {
            memcpy(latest_rgb565, rgb565, CAM_W * CAM_H * 2);
        }
        pthread_mutex_unlock(&frame_mutex);

        if (ioctl(camera_fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            break;
        }
    }

    free(rgb565);
    free(rgb888);
    return NULL;
}

// ------------------- 显示刷新定时器 -------------------
static void refresh_timer_cb(lv_timer_t *timer) {
    (void)timer;

    // 显示最新帧
    pthread_mutex_lock(&frame_mutex);
    if (latest_rgb565) {
        static lv_image_dsc_t img_dsc = {
            .header.w = CAM_W,
            .header.h = CAM_H,
            .data_size = CAM_W * CAM_H * 2,
            .header.cf = LV_COLOR_FORMAT_RGB565,
            .data = NULL,
        };
        img_dsc.data = (uint8_t*)latest_rgb565;
        lv_img_set_src(img_cam, &img_dsc);
    }
    pthread_mutex_unlock(&frame_mutex);

    // 删除旧的矩形框和标签
    for (int i = 0; i < text_label_count; i++) {
        if (text_labels[i]) lv_obj_del(text_labels[i]);
        if (rect_boxes[i]) lv_obj_del(rect_boxes[i]);
    }
    text_label_count = 0;
    rect_count = 0;

     // 绘制新检测结果
    pthread_mutex_lock(&det_mutex);
    for (int i = 0; i < det_count; i++) {
        detection_t *d = &detections[i];
        if (d->prob < 0.3) continue;  // 过滤低置信度

        // 坐标映射
        float screen_x = d->x + 240;
        float screen_y = d->y + 80;
        float screen_w = d->w;
        float screen_h = d->h;

        // 创建矩形框（透明背景，红色边框）
        lv_obj_t *rect = lv_obj_create(yolo_page);
        lv_obj_set_pos(rect, screen_x, screen_y);
        lv_obj_set_size(rect, screen_w, screen_h);
        lv_obj_set_style_border_width(rect, 2, 0);
        lv_obj_set_style_border_color(rect, lv_color_hex(0xFF0000), 0);
        lv_obj_set_style_bg_opa(rect, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(rect, LV_OBJ_FLAG_CLICKABLE);
        rect_boxes[rect_count++] = rect;

        // 创建标签（在矩形上方或内部）
        char text[64];
        snprintf(text, sizeof(text), "%s: %.1f%%", d->name, d->prob * 100);
        lv_obj_t *label = lv_label_create(yolo_page);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_set_style_bg_color(label, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_50, 0);
        lv_obj_set_style_pad_all(label, 2, 0);
        lv_obj_set_style_radius(label, 4, 0);
        if (screen_y - 20 > 0) {
            lv_obj_set_pos(label, screen_x, screen_y - 20);
        } else {
            lv_obj_set_pos(label, screen_x, screen_y + 5);
        }
        text_labels[text_label_count++] = label;
        if (text_label_count >= 32) break;
    }
    pthread_mutex_unlock(&det_mutex);
}

// ------------------- 返回按钮事件 -------------------
static void back_btn_event_handler(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_lib_pm_OpenPrePage(&page_manager);
    }
}

// ------------------- 页面初始化 -------------------
void ui_YoloPage_init(void) {
    // 创建页面
    yolo_page = lv_obj_create(NULL);
    lv_obj_set_size(yolo_page, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(yolo_page, lv_color_black(), 0);

    // 图像显示区域
    img_cam = lv_img_create(yolo_page);
    lv_obj_set_size(img_cam, SCREEN_W, SCREEN_H - 80);
    lv_obj_align(img_cam, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_bg_color(img_cam, lv_color_hex(0x333333), 0);

    // 状态标签
    label_status = lv_label_create(yolo_page);
    lv_label_set_text(label_status, "Initializing...");
    lv_obj_align(label_status, LV_ALIGN_BOTTOM_MID, 0, -10);

    // 返回按钮
    back_btn = lv_btn_create(yolo_page);
    lv_obj_set_size(back_btn, 60, 40);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, back_btn_event_handler, LV_EVENT_CLICKED, NULL);

    // 初始化摄像头
    if (camera_init() != 0) {
        lv_label_set_text(label_status, "Camera init failed");
        printf("Camera init failed\n");
        return;
    }

    // 初始化 CURL
    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();
    if (!curl) {
        lv_label_set_text(label_status, "CURL init failed");
        printf("CURL init failed\n");
        camera_cleanup();
        return;
    }

    // 启动采集线程
    capture_running = 1;
    if (pthread_create(&capture_thread, NULL, capture_thread_func, NULL) != 0) {
        capture_running = 0;
        lv_label_set_text(label_status, "Thread create failed");
        printf("Thread create failed\n");
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        camera_cleanup();
        return;
    }

    // 启动刷新定时器（约 30 FPS）
    refresh_timer = lv_timer_create(refresh_timer_cb, 33, NULL);
    lv_label_set_text(label_status, "YOLO Client Ready");

    lv_scr_load_anim(yolo_page, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 100, 0, true);
}

// ------------------- 页面销毁 -------------------
void ui_YoloPage_deinit(void) {
    // 停止采集线程
    capture_running = 0;
    if (capture_thread) {
        pthread_join(capture_thread, NULL);
        capture_thread = 0;
    }

    // 删除定时器
    if (refresh_timer) {
        lv_timer_del(refresh_timer);
        refresh_timer = NULL;
    }

    // 清理 CURL
    if (curl) {
        curl_easy_cleanup(curl);
        curl = NULL;
    }
    curl_global_cleanup();

    // 清理摄像头
    camera_cleanup();

    // 释放最新帧内存
    pthread_mutex_lock(&frame_mutex);
    if (latest_rgb565) {
        free(latest_rgb565);
        latest_rgb565 = NULL;
    }
    pthread_mutex_unlock(&frame_mutex);

    // 删除所有标签
    for (int i = 0; i < text_label_count; i++) {
        if (text_labels[i]) lv_obj_del(text_labels[i]);
    }
    text_label_count = 0;

    printf("YOLO page deinitialized\n");
}