/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2014 Tatsuhiro Tsujikawa
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
#include "h2load_http2_session.h"

#include <cassert>
#include <cerrno>
#include <print>

#include "ssl_compat.h"

#ifdef NGHTTP2_OPENSSL_IS_WOLFSSL
#  include <wolfssl/options.h>
#  include <wolfssl/openssl/rand.h>
#else // !defined(NGHTTP2_OPENSSL_IS_WOLFSSL)
#  include <openssl/rand.h>
#endif // !defined(NGHTTP2_OPENSSL_IS_WOLFSSL)

#include "h2load.h"
#include "util.h"
#include "template.h"
#include "app_helper.h"

using namespace nghttp2;

namespace h2load {

Http2Session::Http2Session(Client *client) : client_(client) {}

Http2Session::~Http2Session() { nghttp2_conn_del(conn_); }

namespace {
int recv_header(nghttp2_conn *conn, int64_t stream_id, int32_t token,
                nghttp2_rcbuf *name, nghttp2_rcbuf *value, uint8_t flags,
                void *conn_user_data, void *stream_user_data) {
  auto client = static_cast<Client *>(conn_user_data);

  auto namebuf = nghttp2_rcbuf_get_buf(name);
  auto valuebuf = nghttp2_rcbuf_get_buf(value);

  client->on_header(stream_id, {namebuf.base, namebuf.len},
                    {valuebuf.base, valuebuf.len});
  client->worker->stats.bytes_head_decomp += namebuf.len + valuebuf.len;

  if (client->worker->config->verbose) {
    std::println("[stream_id={}] {}: {}", stream_id,
                 as_string_view(namebuf.base, namebuf.len),
                 as_string_view(valuebuf.base, valuebuf.len));
  }

  return 0;
}
} // namespace

namespace {
int end_headers(nghttp2_conn *conn, int64_t stream_id, int fin,
                void *conn_user_data, void *stream_user_data) {
  auto client = static_cast<Client *>(conn_user_data);

  client->worker->stats.bytes_head +=
    nghttp2_conn_get_headers_field_blocklen(conn);

  if (fin) {
    client->record_ttfb();
  }

  return 0;
}
} // namespace

namespace {
int recv_data(nghttp2_conn *conn, int64_t stream_id, const uint8_t *data,
              size_t datalen, void *conn_user_data, void *stream_user_data) {
  auto client = static_cast<Client *>(conn_user_data);
  client->record_ttfb();
  client->worker->stats.bytes_body += datalen;
  return 0;
}
} // namespace

namespace {
int end_stream(nghttp2_conn *conn, int64_t stream_id, void *conn_user_data,
               void *stream_user_data) {
  auto client = static_cast<Client *>(conn_user_data);

  client->record_ttfb();

  return 0;
}
} // namespace

namespace {
int stream_close(nghttp2_conn *conn, uint32_t flags, int64_t stream_id,
                 uint32_t error_code, void *conn_user_data,
                 void *stream_user_data) {
  auto client = static_cast<Client *>(conn_user_data);

  client->on_stream_close(stream_id,
                          (flags & NGHTTP2_STREAM_CLOSE_FLAG_ERROR_CODE_SET)
                            ? error_code == NGHTTP2_NO_ERROR
                            : true);

  return 0;
}
} // namespace

namespace {
nghttp2_ssize read_data(nghttp2_conn *conn, int64_t stream_id, nghttp2_vec *vec,
                        size_t veccnt, uint32_t *pflags, void *conn_user_data,
                        void *stream_user_data) {
  auto client = static_cast<Client *>(conn_user_data);
  auto config = client->worker->config;
  auto req_stat = client->get_req_stat(stream_id);
  assert(req_stat);

  vec[0].base = config->data;
  vec[0].len = static_cast<size_t>(config->data_length);
  *pflags |= NGHTTP2_READ_DATA_FLAG_EOF;

  return 1;
}
} // namespace

void Http2Session::on_connect() {
  int rv;

  // This is required with --disable-assert.
  (void)rv;

  static constexpr auto callbacks = nghttp2_callbacks{
    .rand = util::secure_random,
    .stream_close = stream_close,
    .recv_header = recv_header,
    .end_headers = end_headers,
    .recv_data = recv_data,
    .end_stream = end_stream,
  };

  nghttp2_settings settings;
  nghttp2_settings_default(&settings);

  auto config = client_->worker->config;

  util::secure_random(reinterpret_cast<uint8_t *>(&settings.conn_id),
                      sizeof(settings.conn_id));
  settings.hpack_encoder_max_dtable_capacity =
    config->encoder_header_table_size;
  settings.initial_max_stream_data = (1 << config->window_bits) - 1;
  settings.initial_max_data = (1 << config->connection_window_bits) - 1;
  settings.hpack_max_dtable_capacity = config->header_table_size;

  if (config->verbose) {
    settings.log_write = log_write;
  }

  nghttp2_conn_client_new(&conn_, &callbacks, &settings, nullptr, client_);

  client_->signal_write();
}

std::expected<void, Error> Http2Session::submit_request() {
  if (nghttp2_conn_get_next_stream_id(conn_) >
      std::numeric_limits<int32_t>::max()) {
    return std::unexpected{Error::HTTP2};
  }

  auto config = client_->worker->config;
  auto &nva = config->nva[client_->reqidx++];

  if (client_->reqidx == config->nva.size()) {
    client_->reqidx = 0;
  }

  static constexpr auto dr = nghttp2_data_reader{
    .read_data = read_data,
  };

  auto stream_id =
    nghttp2_conn_submit_request(conn_, nva.data(), nva.size(),
                                config->data_fd == -1 ? nullptr : &dr, nullptr);
  if (stream_id < 0) {
    return std::unexpected{Error::HTTP2};
  }

  client_->on_request(stream_id);
  auto req_stat = client_->get_req_stat(stream_id);
  assert(req_stat);
  client_->record_request_time(req_stat);

  return {};
}

std::expected<void, Error>
Http2Session::on_read(std::span<const uint8_t> data) {
  auto rv =
    nghttp2_conn_read(conn_, data.data(), data.size(), util::timestamp());
  if (rv != 0) {
    return std::unexpected{Error::HTTP2};
  }

  client_->signal_write();

  return {};
}

std::expected<void, Error> Http2Session::on_write() {
  if (client_->wb.rleft()) {
    return {};
  }

  return client_->wb.append_or_error(
    16_k, std::bind_front(&Http2Session::write_frames, this));
}

std::expected<size_t, Error>
Http2Session::write_frames(std::span<uint8_t> dest) {
  auto nwrite =
    nghttp2_conn_write(conn_, dest.data(), dest.size(), util::timestamp());
  if (nwrite < 0) {
    return std::unexpected{Error::HTTP2};
  }

  return as_unsigned(nwrite);
}

void Http2Session::terminate() {
  nghttp2_conn_terminate(conn_, NGHTTP2_NO_ERROR);
}

size_t Http2Session::max_concurrent_streams() {
  return client_->worker->config->max_concurrent_streams;
}

} // namespace h2load
