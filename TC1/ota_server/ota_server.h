/**
 ******************************************************************************
 * @file    ota_sever.h
 * @author  QQ ding
 * @version V1.0.0
 * @date    19-Oct-2016
 * @brief   Provide ota server header files.
 ******************************************************************************
 *
 *  The MIT License
 *  Copyright (c) 2014 MXCHIP Inc.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is furnished
 *  to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 *  WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
 *  IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 ******************************************************************************
 */
#ifndef __ota_server_H
#define __ota_server_H
#include "mico.h"

/* Direct HTTP 200 + Content-Length only, with mandatory signed OTA V1.
 * No redirects, compression, chunking, resume or broker-provided trust hashes.
 */
typedef enum { OTA_LOADING, OTA_SUCCE, OTA_FAIL } OTA_STATE_E;
typedef void (*ota_server_cb_fn)(OTA_STATE_E,float);
OSStatus ota_server_start(char *url,char *md5,ota_server_cb_fn callback);
#endif
