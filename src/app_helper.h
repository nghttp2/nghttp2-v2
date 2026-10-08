/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2012 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef APP_HELPER_H
#define APP_HELPER_H

#include "nghttp2_config.h"

#include <cinttypes>
#include <cstdlib>
#ifdef HAVE_SYS_TIME_H
#  include <sys/time.h>
#endif // defined(HAVE_SYS_TIME_H)
#include <poll.h>

#include <chrono>
#include <span>
#include <optional>

#include <nghttp2v2/nghttp2.h>

namespace nghttp2 {

// Set output file when printing HTTP2 frames. By default, stdout is
// used.
void set_output(FILE *file);

void log_write(void *user_data, char *msg, size_t len);

void print_http_begin_request_headers(int64_t stream_id);

void print_http_begin_response_headers(int64_t stream_id);

void print_http_header(int64_t stream_id, const nghttp2_rcbuf *name,
                       const nghttp2_rcbuf *value, uint8_t flags);

void print_http_end_headers(int64_t stream_id);

void print_http_data(int64_t stream_id, std::span<const uint8_t> data);

void print_http_begin_trailers(int64_t stream_id);

void print_http_end_trailers(int64_t stream_id);

void print_http_request_headers(int64_t stream_id,
                                std::span<const nghttp2_nv> nva);

void print_http_response_headers(int64_t stream_id,
                                 std::span<const nghttp2_nv> nva);

void print_http_trailers(int64_t stream_id, std::span<const nghttp2_nv> nva);

void print_http_settings(const nghttp2_proto_settings *settings);

void print_stream_close(int64_t stream_id, std::optional<uint32_t> error_code);

void print_connection_close(uint64_t conn_id);

} // namespace nghttp2

#endif // !defined(APP_HELPER_H)
