// Copyright (c) 2021 by Rockchip Electronics Co., Ltd. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/*-------------------------------------------
                Includes
-------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <dlfcn.h>
#include <vector>
#include <string>
#include <stdbool.h>

#define _BASETSD_H

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb/stb_image_resize.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#undef cimg_display
#define cimg_display 0
#undef cimg_use_jpeg
#define cimg_use_jpeg 1
#undef cimg_use_png
#define cimg_use_png 1
#include "CImg/CImg.h"

#include "drm_func.h"
#include "rga_func.h"
#include "rknn_api.h"
#include "yolo.h"

#define PERF_WITH_POST 1
#define COCO_IMG_NUMBER 5000
#define DUMP_INPUT 0

using namespace cimg_library;
/*-------------------------------------------
                  Functions
-------------------------------------------*/

static void printRKNNTensor(rknn_tensor_attr *attr)
{
    printf("index=%d name=%s n_dims=%d dims=[%d %d %d %d] n_elems=%d size=%d "
           "fmt=%d type=%d qnt_type=%d fl=%d zp=%d scale=%f\n",
           attr->index, attr->name, attr->n_dims, attr->dims[3], attr->dims[2],
           attr->dims[1], attr->dims[0], attr->n_elems, attr->size, 0, attr->type,
           attr->qnt_type, attr->fl, attr->zp, attr->scale);
}
double __get_us(struct timeval t) { return (t.tv_sec * 1000000 + t.tv_usec); }

static unsigned char *load_data(FILE *fp, size_t ofst, size_t sz)
{
    unsigned char *data;
    int ret;

    data = NULL;

    if (NULL == fp)
    {
        return NULL;
    }

    ret = fseek(fp, ofst, SEEK_SET);
    if (ret != 0)
    {
        printf("blob seek failure.\n");
        return NULL;
    }

    data = (unsigned char *)malloc(sz);
    if (data == NULL)
    {
        printf("buffer malloc failure.\n");
        return NULL;
    }
    ret = fread(data, 1, sz, fp);
    return data;
}

static unsigned char *load_model(const char *filename, int *model_size)
{

    FILE *fp;
    unsigned char *data;

    fp = fopen(filename, "rb");
    if (NULL == fp)
    {
        printf("Open file %s failed.\n", filename);
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    int size = ftell(fp);

    data = load_data(fp, 0, size);

    fclose(fp);

    *model_size = size;
    return data;
}

static int saveFloat(const char *file_name, float *output, int element_size)
{
    FILE *fp;
    fp = fopen(file_name, "w");
    for (int i = 0; i < element_size; i++)
    {
        fprintf(fp, "%.6f\n", output[i]);
    }
    fclose(fp);
    return 0;
}

static unsigned char *load_image(const char *image_path, int *org_height, int *org_width, int *org_ch, rknn_tensor_attr *input_attr)
{
    int req_height = 0;
    int req_width = 0;
    int req_channel = 0;

    switch (input_attr->fmt)
    {
    case RKNN_TENSOR_NHWC:
        req_height = input_attr->dims[2];
        req_width = input_attr->dims[1];
        req_channel = input_attr->dims[0];
        break;
    case RKNN_TENSOR_NCHW:
        req_height = input_attr->dims[1];
        req_width = input_attr->dims[0];
        req_channel = input_attr->dims[2];
        break;
    default:
        printf("meet unsupported layout\n");
        return NULL;
    }

    // printf("w=%d,h=%d,c=%d, fmt=%d\n", req_width, req_height, req_channel, input_attr->fmt);

    int height = 0;
    int width = 0;
    int channel = 0;

    unsigned char *image_data = stbi_load(image_path, &width, &height, &channel, req_channel);
    if (image_data == NULL)
    {
        printf("load image-%s failed!\n", image_path);
        return NULL;
    }

    if (channel == 1){
        printf("image is grey, convert to RGB");
        void* rgb_data = malloc(width* height* 3);
        for(int i=0; i<height; i++){
            for(int j=0; j<width; j++){
                    int offset = (i*width + j)*3;
                    ((unsigned char*)rgb_data)[offset] = ((unsigned char*)image_data)[offset];
                    ((unsigned char*)rgb_data)[offset + 1] = ((unsigned char*)image_data)[offset];
                    ((unsigned char*)rgb_data)[offset + 2] = ((unsigned char*)image_data)[offset];
            }
        }
        free(image_data);
        image_data = (unsigned char*)rgb_data;
        channel = 3;
    }

    if (width%4 != 0){
        int new_width = width+ (4 - width%4);
        printf("%d is not pixel align, resize to %d, this will make the result shift slightly\n",width, new_width);
        void* resize_data = malloc(new_width* height* channel);
        stbir_resize_uint8(image_data, width, height, 0, (unsigned char*)resize_data, new_width, height, 0, channel);
        free(image_data);
        image_data = (unsigned char*)resize_data;
        *org_width = new_width;
    }
    else{
        *org_width = width;
    }

    *org_height = height;
    *org_ch = channel;

    return image_data;
}

int load_hyperm_param(MODEL_INFO *m, int argc, char** argv){
    if (argc != 7)
    {
        // printf("Usage: %s <rknn model path> [yolov5/yolov7/yolox] <anchor file path> [fp/u8] [single_img/multi_imgs] <path>\n", argv[0]);
        printf("Usage: %s [yolov5/yolov7/yolox] [fp/u8] [single_img/multi_imgs] <rknn model path> <anchor file path> <input_path>\n", argv[0]);
        printf("  -- [1] Model type, select from yolov5, yolov7, yolox\n");
        printf("  -- [2] Post process type, select from fp, u8. Only quantize-8bit model could use u8\n");
        printf("  -- [3] Test type, select from single_img, multi_imgs.\n",
        "       For single_img, input_path is image. jpg/bmp/bng is allow\n",
        "       For multi_img, input_path is txt file containing testing images path\n");
        printf("  -- [4] RKNN model path\n");
        printf("  -- [5] anchor file path. If using yolox model, any character is ok.\n");
        printf("  -- [6] input path\n");
        return -1;
    }

    int ret=0;
    m->m_path = (char *)argv[4];
    char* anchor_path = argv[5];
    m->in_path = (char *)argv[6];

    if (strcmp(argv[1], "yolov5") == 0){
        m->m_type = YOLOV5;
        m->color_expect = RK_FORMAT_RGB_888;
        m->anchor_per_branch = 3;
        printf("Runing with yolov5 model\n");
    }
    else if (strcmp(argv[1], "yolov7") == 0){
        m->m_type = YOLOV7;
        m->color_expect = RK_FORMAT_RGB_888;
        m->anchor_per_branch = 3;
        printf("Runing with yolov7 model\n");
    }
    else if (strcmp(argv[1], "yolox") == 0){
        /*
            RK_FORMAT_RGB_888 if normal api
            RK_FORMAT_BGR_888 if pass_through/ zero_copy
         */
        m->m_type = YOLOX;
        m->color_expect = RK_FORMAT_RGB_888;
        m->anchor_per_branch = 1;
        printf("Runing with yolox model\n");
        printf("Ignore anchors file %s\n", anchor_path);
    }
    else{
        printf("Only support yolov5/yolov7/yolox model, but got %s\n", argv[1]);
        return -1;
    }

    // load anchors
    int n = 2* m->out_nodes* m->anchor_per_branch;
    if (m->m_type == YOLOX){
        for (int i=0; i<n; i++){
            m->anchors[i]=1;
        }
    }
    else {
        printf("anchors: ");
        float result[n];
        int valid_number;
        ret = readFloats(anchor_path, &result[0], n, &valid_number);
        for (int i=0; i<valid_number; i++){
            m->anchors[i] = (int)result[i];
            printf("%d ", m->anchors[i]);
        }
        printf("\n");
    }

    if (strcmp(argv[2], "fp") == 0){
        m->post_type = FP;
        printf("Post process with fp\n");
    }
    else if (strcmp(argv[2], "u8") == 0){
        m->post_type = U8;
        printf("Post process with u8\n");
    }
    else{
        printf("Post process type not support: %s\nPlease select from [fp/u8]\n", argv[2]);
        return -1;
    }

    if (strcmp(argv[3], "single_img") == 0){
        m->in_source = SINGLE_IMG;
        printf("Test with single img\n");
    }
    else if (strcmp(argv[3], "multi_imgs") == 0){
        m->in_source = MULTI_IMG;
        printf("Test with multi imgs\n");
    }
    else{
        printf("Test input type is not support: %s\nPlease select from [single_img/multi_imgs]\n", argv[5]);
        return -1;
    }

    return 0;
}


int query_model_info(MODEL_INFO *m, rknn_context ctx){
    int ret;
    /* Query sdk version */
    rknn_sdk_version version;
    ret = rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &version,
                     sizeof(rknn_sdk_version));
    if (ret < 0)
    {
        printf("rknn_init error ret=%d\n", ret);
        return -1;
    }
    printf("sdk version: %s driver version: %s\n", version.api_version,
           version.drv_version);

    /* Get input,output attr */
    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret < 0)
    {
        printf("rknn_init error ret=%d\n", ret);
        return -1;
    }
    printf("model input num: %d, output num: %d\n", io_num.n_input,
           io_num.n_output);
    m->in_nodes = io_num.n_input;
    m->out_nodes = io_num.n_output;
    m->in_attr = (rknn_tensor_attr*)malloc(sizeof(rknn_tensor_attr)* io_num.n_input);
    m->out_attr = (rknn_tensor_attr*)malloc(sizeof(rknn_tensor_attr)* io_num.n_output);
    if (m->in_attr == NULL || m->out_attr == NULL){
        printf("alloc memery failed\n");
        return -1;
    }

    for (int i = 0; i < io_num.n_input; i++)
    {
        m->in_attr[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &m->in_attr[i],
                         sizeof(rknn_tensor_attr));
        if (ret < 0)
        {
            printf("rknn_init error ret=%d\n", ret);
            return -1;
        }
        printRKNNTensor(&m->in_attr[i]);
    }

    for (int i = 0; i < io_num.n_output; i++)
    {
        m->out_attr[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &(m->out_attr[i]),
                         sizeof(rknn_tensor_attr));
        printRKNNTensor(&(m->out_attr[i]));
    }

    /* get input shape */
    if (io_num.n_input > 1){
        printf("expect model have 1 input, but got %d\n", io_num.n_input);
        return -1;
    }

    if (m->in_attr[0].fmt == RKNN_TENSOR_NCHW)
    {
        printf("model is NCHW input fmt\n");
        m->width = m->in_attr[0].dims[0];
        m->height = m->in_attr[0].dims[1];
        m->channel = m->in_attr[0].dims[2];
    }
    else
    {
        printf("model is NHWC input fmt\n");
        m->width = m->in_attr[0].dims[1];
        m->height = m->in_attr[0].dims[2];        
        m->channel = m->in_attr[0].dims[0];
    }
    printf("model input height=%d, width=%d, channel=%d\n", m->height, m->width,
           m->channel);

    return 0;
}


/*-------------------------------------------
                  Main Functions
-------------------------------------------*/
int main(int argc, char **argv)
{
    int status = 0;
    rknn_context ctx;

    rga_context rga_ctx;
    drm_context drm_ctx;
    void *drm_buf = NULL;
    int drm_fd = -1;
    int buf_fd = -1; // converted from buffer handle
    unsigned int handle;

    memset(&rga_ctx, 0, sizeof(rga_context));
    memset(&drm_ctx, 0, sizeof(drm_context));
    drm_fd = drm_init(&drm_ctx);

    MODEL_INFO m_info;
    LETTER_BOX letter_box;

    size_t actual_size = 0;
    int img_width = 0;
    int img_height = 0;
    int img_channel = 0;

    struct timeval start_time, stop_time;
    int ret;

    ret = load_hyperm_param(&m_info, argc, argv);
    if (ret < 0) return -1;

    /* Create the neural network */
    printf("Loading model...\n");
    int model_data_size = 0;
    unsigned char *model_data = load_model(m_info.m_path, &model_data_size);
    ret = rknn_init(&ctx, model_data, model_data_size, 0);
    if (ret < 0)
    {
        printf("rknn_init error ret=%d\n", ret);
        return -1;
    }

    printf("query info\n");
    ret = query_model_info(&m_info, ctx);
    if (ret < 0){
        return -1;
    }

    /* Init input tensor */
    rknn_input inputs[1];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;         /* SAME AS INPUT IMAGE */
    inputs[0].size = m_info.width * m_info.height * m_info.channel;
    inputs[0].fmt = RKNN_TENSOR_NHWC;           /* SAME AS INPUT IMAGE */
    inputs[0].pass_through = 0;

    /* Init output tensor */
    rknn_output outputs[m_info.out_nodes];
    memset(outputs, 0, sizeof(outputs));
    for (int i = 0; i < m_info.out_nodes; i++)
    {
        outputs[i].want_float = m_info.post_type;
    }

    void *resize_buf = malloc(inputs[0].size);
    if (resize_buf == NULL){
        printf("resize buf alloc failed\n");
        return -1;
    }
    unsigned char *input_data = NULL;

    /* Single img input */
    /* Due to different input img size, multi img method has to calculate letterbox param each time*/
    if (m_info.in_source == SINGLE_IMG)
    {
        /* Input preprocess */
        // Load image
        CImg<unsigned char> img(m_info.in_path);
        input_data = load_image(m_info.in_path, &img_height, &img_width, &img_channel, &m_info.in_attr[0]);
        if (!input_data){
            fprintf(stderr, "Error in loading input image\n");
            return -1;
        }

        printf("img_width: %d\nimg_height: %d\nimg_channel: %d\n", img_width, img_height, img_channel);
        // DRM alloc buffer
        drm_buf = drm_buf_alloc(&drm_ctx, drm_fd, img_width, img_height, img_channel * 8,
                                &buf_fd, &handle, &actual_size);
        printf("create drm buffer finish\n");
        memcpy(drm_buf, input_data, img_width * img_height * img_channel);
        memset(resize_buf, 0, inputs[0].size);
        printf("set resize buf finish\n");

        // Letter box resize
        letter_box.target_height = m_info.height;
        letter_box.target_width = m_info.width;
        letter_box.in_height = img_height;
        letter_box.in_width = img_width;
        compute_letter_box(&letter_box);

        if ((m_info.height!= img_height) || (m_info.width!= img_width)){
            // Init rga context
            printf("WARNING: img_size(w:%d h:%d) not match model input_size(w:%d h:%d)\n", img_width, img_height, m_info.width, m_info.height);
            printf("Try resize img with rga. If any error occur, please using image with size(w:%d h:%d) or using newer firmware to update rga driver version\n", m_info.width, m_info.height);
            RGA_init(&rga_ctx);
            img_resize_slow(&rga_ctx, drm_buf, img_width, img_height, resize_buf, letter_box.resize_width, letter_box.resize_height, 
                                letter_box.w_pad, letter_box.h_pad, m_info.color_expect, 
                                letter_box.add_extra_sz_w_pad, letter_box.add_extra_sz_h_pad);
            inputs[0].buf = resize_buf;
        }
        else{
            inputs[0].buf = drm_buf;
        }

#if (DUMP_INPUT == 1)
        FILE* dump_file;
        if ((dump_file = fopen("./demo_c_input_hwc_rgb.txt", "wb")) == NULL){
            printf("Dump input Failed !\n");
        }
        else{
            fwrite(resize_buf, sizeof(uint8_t), inputs[0].size, dump_file);
            printf("Dump input Successed. Path: ./demo_c_input_hwc_rgb.txt !\n");
        }
        fclose(dump_file);
#endif

        gettimeofday(&start_time, NULL);
        rknn_inputs_set(ctx, m_info.in_nodes, inputs);
        ret = rknn_run(ctx, NULL);
        ret = rknn_outputs_get(ctx, m_info.out_nodes, outputs, NULL);

        /* Post process */
        detect_result_group_t detect_result_group;
        post_process(outputs, &m_info, &letter_box, &detect_result_group);

        gettimeofday(&stop_time, NULL);
        printf("once run use %f ms\n",
            (__get_us(stop_time) - __get_us(start_time)) / 1000);

        // Draw Objects
        const unsigned char blue[] = {0, 0, 255};
        char score_result[64];
        for (int i = 0; i < detect_result_group.count; i++)
        {
            detect_result_t *det_result = &(detect_result_group.results[i]);
            printf("%s @ (%d %d %d %d) %f\n",
                det_result->name,
                det_result->box.left, det_result->box.top, det_result->box.right, det_result->box.bottom,
                det_result->prop);
            int x1 = det_result->box.left;
            int y1 = det_result->box.top;
            int x2 = det_result->box.right;
            int y2 = det_result->box.bottom;
            int ret = snprintf(score_result, sizeof score_result, "%f", det_result->prop);
            //draw box
            img.draw_rectangle(x1, y1, x2, y2, blue, 1, ~0U);
            img.draw_text(x1, y1 - 24, det_result->name, blue);
            img.draw_text(x1, y1 - 12, score_result, blue);
        }
        img.save("./out.bmp");
        ret = rknn_outputs_release(ctx, m_info.out_nodes, outputs);

        // loop test without preprocess, postprocess
        int test_count = 10;
        gettimeofday(&start_time, NULL);
        for (int i = 0; i < test_count; ++i)
        {
            rknn_inputs_set(ctx, m_info.in_nodes, inputs);
            ret = rknn_run(ctx, NULL);
            ret = rknn_outputs_get(ctx, m_info.out_nodes, outputs, NULL);
            ret = rknn_outputs_release(ctx, m_info.out_nodes, outputs);
        }
        gettimeofday(&stop_time, NULL);
        printf("WITHOUT POST_PROCESS\n    run loop count = %d , average time: %f ms\n", test_count,
            (__get_us(stop_time) - __get_us(start_time)) / 1000.0 / test_count);

        // loop test with postprocess
        gettimeofday(&start_time, NULL);
        for (int i = 0; i < test_count; ++i)
        {          
            rknn_inputs_set(ctx, m_info.in_nodes, inputs);
            ret = rknn_run(ctx, NULL);
            ret = rknn_outputs_get(ctx, m_info.out_nodes, outputs, NULL);
            post_process(outputs, &m_info, &letter_box, &detect_result_group);
            ret = rknn_outputs_release(ctx, m_info.out_nodes, outputs);
        }
        gettimeofday(&stop_time, NULL);
        printf("WITH POST_PROCESS\n    run loop count = %d , average time: %f ms\n", test_count,
            (__get_us(stop_time) - __get_us(start_time)) / 1000.0 / test_count);

        drm_buf_destroy(&drm_ctx, drm_fd, buf_fd, handle, drm_buf, actual_size);
        drm_deinit(&drm_ctx, drm_fd);
        free(input_data);
    }
    
    if(m_info.in_source == MULTI_IMG){
        FILE *output_file = NULL;
        output_file = fopen("./result_record.txt", "w+");

        char *img_paths[COCO_IMG_NUMBER];
        ret = readLines(m_info.in_path, img_paths, COCO_IMG_NUMBER);

        drm_fd = drm_init(&drm_ctx);
        RGA_init(&rga_ctx);

        for (int j=0; j<COCO_IMG_NUMBER; j++)
        {
            printf("[%d/%d]Detect on %s\n", j+1, COCO_IMG_NUMBER, img_paths[j]);
            /* Input preprocess */
            // Load image
            CImg<unsigned char> img(img_paths[j]);
            input_data = load_image(img_paths[j], &img_height, &img_width, &img_channel, &m_info.in_attr[0]);
            if (!input_data){
                fprintf(stderr, "Error in loading input image");
                return -1;
            }

            // DRM alloc buffer
            drm_buf = drm_buf_alloc(&drm_ctx, drm_fd, img_width, img_height, img_channel * 8,
                                    &buf_fd, &handle, &actual_size);
            memcpy(drm_buf, input_data, img_width * img_height * img_channel);
            memset(resize_buf, 0, inputs[0].size);

            // Letter box resize
            letter_box.target_height = m_info.height;
            letter_box.target_width = m_info.width;
            letter_box.in_height = img_height;
            letter_box.in_width = img_width;
            compute_letter_box(&letter_box);

            // Init rga context
            img_resize_slow(&rga_ctx, drm_buf, img_width, img_height, resize_buf, letter_box.resize_width, letter_box.resize_height, 
                            letter_box.w_pad, letter_box.h_pad, m_info.color_expect, 
                            letter_box.add_extra_sz_w_pad, letter_box.add_extra_sz_h_pad);            
            inputs[0].buf = resize_buf;
            gettimeofday(&start_time, NULL);
            rknn_inputs_set(ctx, m_info.in_nodes, inputs);

            ret = rknn_run(ctx, NULL);
            ret = rknn_outputs_get(ctx, m_info.out_nodes, outputs, NULL);

            /* Post process */
            detect_result_group_t detect_result_group;
            post_process(outputs, &m_info, &letter_box, &detect_result_group);

            gettimeofday(&stop_time, NULL);
            printf("once run use %f ms\n",
                (__get_us(stop_time) - __get_us(start_time)) / 1000);

            // Draw Objects
            const unsigned char blue[] = {0, 0, 255};
            char score_result[64];
            for (int i = 0; i < detect_result_group.count; i++)
            {
                detect_result_t *det_result = &(detect_result_group.results[i]);
                printf("%s @ (%d %d %d %d) %f\n",
                    det_result->name,
                    det_result->box.left, det_result->box.top, det_result->box.right, det_result->box.bottom,
                    det_result->prop);
                fprintf(output_file, "%s,%s,%d,%f,(%d %d %d %d)\n",
                        img_paths[j],
                        det_result->name,
                        det_result->class_index,
                        det_result->prop,
                        det_result->box.left, det_result->box.top, det_result->box.right, det_result->box.bottom);
            }
            ret = rknn_outputs_release(ctx, m_info.out_nodes, outputs);

            drm_buf_destroy(&drm_ctx, drm_fd, buf_fd, handle, drm_buf, actual_size);
            free(input_data);
        }
        fclose(output_file);
        drm_deinit(&drm_ctx, drm_fd);
    }

    
    // release
    ret = rknn_destroy(ctx);

    RGA_deinit(&rga_ctx);
    if (model_data)
    {
        free(model_data);
    }

    if (resize_buf)
    {
        free(resize_buf);
    }

    if (m_info.in_attr){
        free(m_info.in_attr);
    }

    if (m_info.out_attr){
        free(m_info.out_attr);
    }

    return 0;
}
