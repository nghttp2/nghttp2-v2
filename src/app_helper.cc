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
#include <sys/types.h>
#ifdef HAVE_SYS_SOCKET_H
#  include <sys/socket.h>
#endif // defined(HAVE_SYS_SOCKET_H)
#ifdef HAVE_NETDB_H
#  include <netdb.h>
#endif // defined(HAVE_NETDB_H)
#ifdef HAVE_UNISTD_H
#  include <unistd.h>
#endif // defined(HAVE_UNISTD_H)
#ifdef HAVE_FCNTL_H
#  include <fcntl.h>
#endif // defined(HAVE_FCNTL_H)
#ifdef HAVE_NETINET_IN_H
#  include <netinet/in.h>
#endif // defined(HAVE_NETINET_IN_H)
#include <netinet/tcp.h>
#include <poll.h>

#include <cassert>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <print>

#include "app_helper.h"
#include "util.h"
#include "http2.h"
#include "template.h"

namespace nghttp2 {

namespace {
FILE *outfile = stdout;
} // namespace

void set_output(FILE *file) { outfile = file; }

void log_write(void *user_data, char *msg, size_t len) {
  msg[len++] = '\n';

  while (write(fileno(stderr), msg, len) == -1 && errno == EINTR)
    ;
}

void print_http_begin_request_headers(int64_t stream_id) {
  std::println(outfile, "http: stream {:#x} request headers started",
               stream_id);
}

void print_http_begin_response_headers(int64_t stream_id) {
  std::println(outfile, "http: stream {:#x} response headers started",
               stream_id);
}

namespace {
void print_header(std::span<const uint8_t> name, std::span<const uint8_t> value,
                  uint8_t flags) {
  std::println(outfile, "[{}: {}]{}", as_string_view(name),
               as_string_view(value),
               (flags & NGHTTP2_NV_FLAG_NEVER_INDEX) ? "(sensitive)" : "");
}
} // namespace

namespace {
void print_header(const nghttp2_rcbuf *name, const nghttp2_rcbuf *value,
                  uint8_t flags) {
  auto namebuf = nghttp2_rcbuf_get_buf(name);
  auto valuebuf = nghttp2_rcbuf_get_buf(value);
  print_header({namebuf.base, namebuf.len}, {valuebuf.base, valuebuf.len},
               flags);
}
} // namespace

namespace {
void print_header(const nghttp2_nv &nv) {
  print_header({nv.name, nv.namelen}, {nv.value, nv.valuelen}, nv.flags);
}
} // namespace

void print_http_header(int64_t stream_id, const nghttp2_rcbuf *name,
                       const nghttp2_rcbuf *value, uint8_t flags) {
  std::print(outfile, "http: stream {:#x} ", stream_id);
  print_header(name, value, flags);
}

void print_http_end_headers(int64_t stream_id) {
  std::println(outfile, "http: stream {:#x} headers ended", stream_id);
}

void print_http_data(int64_t stream_id, std::span<const uint8_t> data) {
  std::println(outfile, "http: stream {:#x} body {} bytes", stream_id,
               data.size());
  (void)util::hexdump(outfile, data.data(), data.size());
}

void print_http_begin_trailers(int64_t stream_id) {
  std::println(outfile, "http: stream {:#x} trailers started", stream_id);
}

void print_http_end_trailers(int64_t stream_id) {
  std::println(outfile, "http: stream {:#x} trailers ended", stream_id);
}

void print_http_request_headers(int64_t stream_id,
                                std::span<const nghttp2_nv> nva) {
  std::println(outfile, "http: stream {:#x} submit request headers", stream_id);
  for (auto &nv : nva) {
    print_header(nv);
  }
}

void print_http_response_headers(int64_t stream_id,
                                 std::span<const nghttp2_nv> nva) {
  std::println(outfile, "http: stream {:#x} submit response headers",
               stream_id);
  for (auto &nv : nva) {
    print_header(nv);
  }
}

void print_http_settings(const nghttp2_proto_settings *settings) {
  std::println(
    outfile, R"(http: remote settings
http: SETTINGS_HEADER_TABLE_SIZE={}
http: SETTINGS_MAX_CONCURRENT_STREAMS={}
http: SETTINGS_INITIAL_WINDOW_SIZE={}
http: SETTINGS_MAX_HEADER_LIST_SIZE={}
http: SETTINGS_ENABLE_CONNECT_PROTOCOL={})",
    settings->hpack_max_dtable_capacity, settings->max_concurrent_streams,
    settings->initial_max_stream_data, settings->max_field_section_size,
    settings->enable_connect_protocol);
}

void print_stream_close(int64_t stream_id, std::optional<uint32_t> error_code) {
  if (error_code) {
    std::println(outfile, "stream {:#x} closed with {}({:#x})", stream_id,
                 nghttp2_http2_strerror(*error_code), *error_code);
  } else {
    std::println(outfile, "stream {:#x} closed without error", stream_id);
  }
}

void print_connection_close(uint64_t conn_id) {
  std::println(outfile, "connection {:#x} closed", conn_id);
}

} // namespace nghttp2
