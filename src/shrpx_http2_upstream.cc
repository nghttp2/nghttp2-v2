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
#include "shrpx_http2_upstream.h"

#include <netinet/tcp.h>
#include <assert.h>
#include <cerrno>

#include "shrpx_client_handler.h"
#include "shrpx_https_upstream.h"
#include "shrpx_downstream.h"
#include "shrpx_downstream_connection.h"
#include "shrpx_config.h"
#include "shrpx_http.h"
#include "shrpx_worker.h"
#include "shrpx_http2_session.h"
#include "shrpx_log.h"
#ifdef HAVE_MRUBY
#  include "shrpx_mruby.h"
#endif // defined(HAVE_MRUBY)
#include "http2.h"
#include "util.h"
#include "base64.h"
#include "app_helper.h"
#include "template.h"

using namespace nghttp2;

namespace shrpx {

namespace {
int stream_close(nghttp2_conn *conn, uint32_t flags, int64_t stream_id,
                 uint32_t error_code, void *conn_user_data,
                 void *stream_user_data) {
  auto upstream = static_cast<Http2Upstream *>(conn_user_data);
  if (log_enabled(INFO)) {
    Log{INFO, upstream} << "Stream stream_id=" << stream_id
                        << " is being closed";
  }

  auto downstream = static_cast<Downstream *>(stream_user_data);
  if (!downstream) {
    return 0;
  }

  auto &req = downstream->request();

  if (!upstream->consume(stream_id, req.unconsumed_body_length)) {
    return NGHTTP2_ERR_CALLBACK_FAILURE;
  }

  req.unconsumed_body_length = 0;

  if (downstream->get_request_state() == DownstreamState::CONNECT_FAIL) {
    if (!upstream->remove_downstream(downstream)) {
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    // downstream was deleted

    return 0;
  }

  if (downstream->can_detach_downstream_connection()) {
    // Keep-alive
    if (!downstream->detach_downstream_connection()) {
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
  }

  downstream->set_request_state(DownstreamState::STREAM_CLOSED);

  // At this point, downstream read may be paused.

  if (!upstream->remove_downstream(downstream)) {
    return NGHTTP2_ERR_CALLBACK_FAILURE;
  }

  // downstream was deleted

  // How to test this case? Request sufficient large download
  // and make client send RST_STREAM after it gets first DATA
  // frame chunk.

  return 0;
}
} // namespace

namespace {
int recv_header(nghttp2_conn *conn, int64_t stream_id, int32_t token,
                nghttp2_rcbuf *name, nghttp2_rcbuf *value, uint8_t flags,
                void *conn_user_data, void *stream_user_data) {
  auto namebuf = nghttp2_rcbuf_get_buf(name);
  auto valuebuf = nghttp2_rcbuf_get_buf(value);
  auto config = get_config();

  auto upstream = static_cast<Http2Upstream *>(conn_user_data);
  auto downstream = static_cast<Downstream *>(stream_user_data);
  if (!downstream || downstream->get_stop_reading()) {
    return 0;
  }

  auto &req = downstream->request();

  auto &httpconf = config->http;

  if (req.fs.buffer_size() + namebuf.len + valuebuf.len >
        httpconf.request_header_field_buffer ||
      req.fs.num_fields() >= httpconf.max_request_header_fields) {
    downstream->set_stop_reading(true);

    if (downstream->get_response_state() == DownstreamState::MSG_COMPLETE) {
      return 0;
    }

    if (log_enabled(INFO)) {
      Log{INFO, upstream} << "Too large or many header field size="
                          << req.fs.buffer_size() + namebuf.len + valuebuf.len
                          << ", num=" << req.fs.num_fields() + 1;
    }

    if (!upstream->error_reply(downstream, 431)) {
      nghttp2_conn_shutdown_stream(conn, 0x01, stream_id,
                                   NGHTTP2_INTERNAL_ERROR);

      return 0;
    }

    return 0;
  }

  auto nameref = as_string_view(namebuf.base, namebuf.len);
  auto valueref = as_string_view(valuebuf.base, valuebuf.len);
  auto never_index = flags & NGHTTP2_NV_FLAG_NEVER_INDEX;

  downstream->add_rcbuf(name);
  downstream->add_rcbuf(value);

  req.fs.add_header_token(nameref, valueref, never_index, token);
  return 0;
}
} // namespace

namespace {
int recv_trailer(nghttp2_conn *conn, int64_t stream_id, int32_t token,
                 nghttp2_rcbuf *name, nghttp2_rcbuf *value, uint8_t flags,
                 void *conn_user_data, void *stream_user_data) {
  auto namebuf = nghttp2_rcbuf_get_buf(name);
  auto valuebuf = nghttp2_rcbuf_get_buf(value);
  auto config = get_config();

  auto upstream = static_cast<Http2Upstream *>(conn_user_data);
  auto downstream = static_cast<Downstream *>(stream_user_data);
  if (!downstream || downstream->get_stop_reading()) {
    return 0;
  }

  auto &req = downstream->request();

  auto &httpconf = config->http;

  if (req.fs.buffer_size() + namebuf.len + valuebuf.len >
        httpconf.request_header_field_buffer ||
      req.fs.num_fields() >= httpconf.max_request_header_fields) {
    if (downstream->get_response_state() == DownstreamState::MSG_COMPLETE) {
      return 0;
    }

    if (log_enabled(INFO)) {
      Log{INFO, upstream} << "Too large or many header field size="
                          << req.fs.buffer_size() + namebuf.len + valuebuf.len
                          << ", num=" << req.fs.num_fields() + 1;
    }

    // We don't care trailer part exceeds header size limit; just
    // discard it.

    return 0;
  }

  auto nameref = as_string_view(namebuf.base, namebuf.len);
  auto valueref = as_string_view(valuebuf.base, valuebuf.len);
  auto never_index = flags & NGHTTP2_NV_FLAG_NEVER_INDEX;

  downstream->add_rcbuf(name);
  downstream->add_rcbuf(value);

  req.fs.add_trailer_token(nameref, valueref, never_index, token);
  return 0;
}
} // namespace

namespace {
int begin_headers(nghttp2_conn *conn, int64_t stream_id, void *conn_user_data,
                  void *stream_user_data) {
  auto upstream = static_cast<Http2Upstream *>(conn_user_data);

  if (log_enabled(INFO)) {
    Log{INFO, upstream} << "Received upstream request HEADERS stream_id="
                        << stream_id;
  }

  upstream->on_start_request(stream_id);

  return 0;
}
} // namespace

void Http2Upstream::on_start_request(int64_t stream_id) {
  auto downstream =
    std::make_unique<Downstream>(this, handler_->get_mcpool(), stream_id);
  nghttp2_conn_set_stream_user_data(conn_, stream_id, downstream.get());

  downstream->reset_upstream_rtimer();

  auto config = get_config();
  auto &httpconf = config->http;

  handler_->reset_upstream_read_timeout(httpconf.timeout.header);

  auto &req = downstream->request();

  // Although, we deprecated minor version from HTTP/2, we supply
  // minor version 0 to use via header field in a conventional way.
  req.http_major = 2;
  req.http_minor = 0;

  add_pending_downstream(std::move(downstream));

  ++num_requests_;

  if (httpconf.max_requests <= num_requests_) {
    start_graceful_shutdown();
  }
}

namespace {
int end_headers(nghttp2_conn *conn, int64_t stream_id, int fin,
                void *conn_user_data, void *stream_user_data) {
  auto downstream = static_cast<Downstream *>(stream_user_data);
  if (!downstream || downstream->get_stop_reading()) {
    return 0;
  }

  downstream->reset_upstream_rtimer();

  auto upstream = static_cast<Http2Upstream *>(conn_user_data);
  auto handler = upstream->get_client_handler();

  handler->stop_read_timer();

  if (!upstream->on_request_headers(downstream, fin)) {
    return NGHTTP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

std::expected<void, Error>
Http2Upstream::on_request_headers(Downstream *downstream, bool fin) {
  auto lgconf = log_config();
  lgconf->update_tstamp(std::chrono::system_clock::now());
  auto &req = downstream->request();
  req.tstamp = lgconf->tstamp;

  if (downstream->get_response_state() == DownstreamState::MSG_COMPLETE) {
    return {};
  }

  auto &nva = req.fs.headers();

  if (log_enabled(INFO)) {
    std::string ss;
    for (auto &nv : nva) {
      if (nv.name == "authorization"sv) {
        ss += tty_http_hd();
        ss += nv.name;
        ss += tty_rst();
        ss += ": <redacted>\n";
        continue;
      }
      ss += tty_http_hd();
      ss += nv.name;
      ss += tty_rst();
      ss += ": ";
      ss += nv.value;
      ss += '\n';
    }
    Log{INFO, this} << "HTTP request headers. stream_id="
                    << downstream->get_stream_id() << "\n"
                    << ss;
  }

  auto config = get_config();
  auto &dump = config->http2.upstream.debug.dump;

  if (dump.request_header) {
    http2::dump_nv(dump.request_header, nva);
  }

  auto content_length = req.fs.header(NGHTTP2_HPACK_TOKEN_CONTENT_LENGTH);
  if (content_length) {
    // libnghttp2 guarantees this can be parsed
    req.fs.content_length =
      static_cast<int64_t>(*util::parse_uint(content_length->value));
  }

  // presence of mandatory header fields are guaranteed by libnghttp2.
  auto authority = req.fs.header(NGHTTP2_HPACK_TOKEN__AUTHORITY);
  auto path = req.fs.header(NGHTTP2_HPACK_TOKEN__PATH);
  auto method = req.fs.header(NGHTTP2_HPACK_TOKEN__METHOD);
  auto scheme = req.fs.header(NGHTTP2_HPACK_TOKEN__SCHEME);

  auto method_token = http2::lookup_method_token(method->value);
  if (method_token == -1) {
    return error_reply(downstream, 501);
  }

  if (method_token == HTTP_CONNECT && content_length) {
    if (log_enabled(INFO)) {
      Log{INFO, this} << "content-length are not allowed in CONNECT request";
    }

    return error_reply(downstream, 400);
  }

  auto faddr = handler_->get_upstream_addr();

  // For HTTP/2 proxy, we require :authority.
  if (method_token != HTTP_CONNECT && config->http2_proxy &&
      faddr->alt_mode == UpstreamAltMode::NONE && !authority) {
    shutdown_stream(downstream, NGHTTP2_PROTOCOL_ERROR);
    return {};
  }

  req.method = method_token;
  if (scheme) {
    req.scheme = scheme->value;
  }

  // nghttp2 library guarantees either :authority or host exist
  if (!authority) {
    req.no_authority = true;
    authority = req.fs.header(NGHTTP2_HPACK_TOKEN_HOST);
  }

  if (authority) {
    req.authority = authority->value;
  }

  if (path) {
    if (method_token == HTTP_OPTIONS && path->value == "*"sv) {
      // Server-wide OPTIONS request.  Path is empty.
    } else if (config->http2_proxy &&
               faddr->alt_mode == UpstreamAltMode::NONE) {
      req.path = path->value;
    } else {
      req.path = http2::rewrite_clean_path(downstream->get_block_allocator(),
                                           path->value);
    }
  }

  auto connect_proto = req.fs.header(NGHTTP2_HPACK_TOKEN__PROTOCOL);
  if (connect_proto) {
    if (connect_proto->value != "websocket"sv) {
      return error_reply(downstream, 400);
    }
    req.connect_proto = ConnectProto::WEBSOCKET;
  }

  if (!fin) {
    req.http2_expect_body = true;
  } else if (req.fs.content_length == -1) {
    // If END_STREAM flag is set to HEADERS frame, we are sure that
    // content-length is 0.
    req.fs.content_length = 0;
  }

  downstream->inspect_http2_request();

  downstream->set_request_state(DownstreamState::HEADER_COMPLETE);

  if (config->http.require_http_scheme &&
      !http::check_http_scheme(req.scheme, handler_->get_ssl() != nullptr)) {
    return error_reply(downstream, 400);
  }

#ifdef HAVE_MRUBY
  auto worker = handler_->get_worker();
  auto mruby_ctx = worker->get_mruby_context();

  if (!mruby_ctx->run_on_request_proc(downstream)) {
    return error_reply(downstream, 500);
  }
#endif // defined(HAVE_MRUBY)

  if (fin) {
    downstream->disable_upstream_rtimer();

    downstream->set_request_state(DownstreamState::MSG_COMPLETE);
  }

  if (downstream->get_response_state() == DownstreamState::MSG_COMPLETE) {
    return {};
  }

  return start_downstream(downstream);
}

std::expected<void, Error>
Http2Upstream::start_downstream(Downstream *downstream) {
  if (downstream_queue_.can_activate(downstream->request().authority)) {
    return initiate_downstream(downstream);
  }

  downstream_queue_.mark_blocked(downstream);

  return {};
}

std::expected<void, Error>
Http2Upstream::initiate_downstream(Downstream *downstream) {
#ifdef HAVE_MRUBY
  DownstreamConnection *dconn_ptr;
#endif // defined(HAVE_MRUBY)

  for (;;) {
    auto maybe_dconn = handler_->get_downstream_connection(downstream);
    if (!maybe_dconn) {
      if (!(maybe_dconn.error() == Error::TLS_REQUIRED
              ? redirect_to_https(downstream)
              : error_reply(downstream, 502))) {
        shutdown_stream(downstream, NGHTTP2_INTERNAL_ERROR);
      }

      downstream->set_request_state(DownstreamState::CONNECT_FAIL);
      downstream_queue_.mark_failure(downstream);

      return {};
    }

    auto dconn = std::move(*maybe_dconn);

#ifdef HAVE_MRUBY
    dconn_ptr = dconn.get();
#endif // defined(HAVE_MRUBY)
    if (downstream->attach_downstream_connection(std::move(dconn))) {
      break;
    }
  }

#ifdef HAVE_MRUBY
  const auto &group = dconn_ptr->get_downstream_addr_group();
  if (group) {
    const auto &mruby_ctx = group->shared_addr->mruby_ctx;
    if (!mruby_ctx->run_on_request_proc(downstream)) {
      if (!error_reply(downstream, 500)) {
        shutdown_stream(downstream, NGHTTP2_INTERNAL_ERROR);
      }

      downstream_queue_.mark_failure(downstream);

      return {};
    }

    if (downstream->get_response_state() == DownstreamState::MSG_COMPLETE) {
      return {};
    }
  }
#endif // defined(HAVE_MRUBY)

  if (!downstream->push_request_headers()) {
    if (!error_reply(downstream, 502)) {
      shutdown_stream(downstream, NGHTTP2_INTERNAL_ERROR);
    }

    downstream_queue_.mark_failure(downstream);

    return {};
  }

  downstream_queue_.mark_active(downstream);

  auto &req = downstream->request();
  if (!req.http2_expect_body && !downstream->end_upload_data()) {
    shutdown_stream(downstream, NGHTTP2_INTERNAL_ERROR);
  }

  return {};
}

namespace {
int remote_end_stream(nghttp2_conn *conn, int64_t stream_id,
                      void *conn_user_data, void *stream_user_data) {
  auto downstream = static_cast<Downstream *>(stream_user_data);
  if (!downstream || downstream->get_stop_reading()) {
    return 0;
  }

  auto upstream = static_cast<Http2Upstream *>(conn_user_data);

  downstream->disable_upstream_rtimer();

  if (!downstream->end_upload_data() &&
      downstream->get_response_state() != DownstreamState::MSG_COMPLETE) {
    upstream->shutdown_stream(downstream, NGHTTP2_INTERNAL_ERROR);
  }

  downstream->set_request_state(DownstreamState::MSG_COMPLETE);

  return 0;
}
} // namespace

namespace {
int local_end_stream(nghttp2_conn *conn, int64_t stream_id,
                     void *conn_user_data, void *stream_user_data) {
  // RST_STREAM if request is still incomplete.
  auto downstream = static_cast<Downstream *>(stream_user_data);
  if (!downstream || downstream->get_stop_reading()) {
    return 0;
  }

  auto upstream = static_cast<Http2Upstream *>(conn_user_data);

  // For tunneling, issue RST_STREAM to finish the stream.
  if (downstream->get_upgraded()) {
    if (log_enabled(INFO)) {
      Log{INFO, upstream} << "Send RST_STREAM to "
                          << (downstream->get_upgraded() ? "tunneled " : "")
                          << "stream stream_id=" << downstream->get_stream_id()
                          << " to finish off incomplete request";
    }

    upstream->shutdown_stream(downstream, NGHTTP2_NO_ERROR);
  }

  return 0;
}
} // namespace

namespace {
int http2_shutdown(nghttp2_conn *conn, int64_t last_stream_id,
                   uint32_t error_code, void *conn_user_data) {
  auto upstream = static_cast<Http2Upstream *>(conn_user_data);

  if (log_enabled(INFO)) {
    Log{INFO, upstream} << "GOAWAY received: last-stream-id=" << last_stream_id
                        << ", error_code=" << error_code;
  }

  return 0;
}
} // namespace

namespace {
int recv_data(nghttp2_conn *conn, int64_t stream_id, const uint8_t *data,
              size_t datalen, void *conn_user_data, void *stream_user_data) {
  auto upstream = static_cast<Http2Upstream *>(conn_user_data);
  auto downstream = static_cast<Downstream *>(stream_user_data);

  if (!downstream) {
    if (!upstream->consume(stream_id, datalen)) {
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    return 0;
  }

  downstream->reset_upstream_rtimer();

  if (!downstream->push_upload_data_chunk({data, datalen})) {
    if (downstream->get_response_state() != DownstreamState::MSG_COMPLETE) {
      upstream->shutdown_stream(downstream, NGHTTP2_INTERNAL_ERROR);
    }

    if (!upstream->consume(stream_id, datalen)) {
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    return 0;
  }

  return 0;
}
} // namespace

namespace {
int write_stream_data_offset(nghttp2_conn *conn, int64_t stream_id,
                             uint64_t offset, size_t len, void *conn_user_data,
                             void *stream_user_data) {
  auto downstream = static_cast<Downstream *>(stream_user_data);
  if (!downstream) {
    return 0;
  }

  auto body = downstream->get_response_buf();
  body->drain(len);

  if (body->rleft()) {
    downstream->reset_upstream_wtimer();
  } else {
    downstream->disable_upstream_wtimer();
  }

  // We have to add length here, so that we can log this amount of
  // data transferred.
  downstream->response_sent_body_length += len;

  if (!downstream->resume_read(SHRPX_NO_BUFFER, len)) {
    return NGHTTP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
uint32_t infer_upstream_rst_stream_error_code(uint32_t downstream_error_code) {
  // NGHTTP2_REFUSED_STREAM is important because it tells upstream
  // client to retry.
  switch (downstream_error_code) {
  case NGHTTP2_NO_ERROR:
  case NGHTTP2_REFUSED_STREAM:
    return downstream_error_code;
  default:
    return NGHTTP2_INTERNAL_ERROR;
  }
}
} // namespace

namespace {
void shutdown_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto upstream = static_cast<Http2Upstream *>(w->data);
  auto handler = upstream->get_client_handler();
  upstream->submit_goaway();
  handler->signal_write();
}
} // namespace

namespace {
void prepare_cb(struct ev_loop *loop, ev_prepare *w, int revents) {
  auto upstream = static_cast<Http2Upstream *>(w->data);
  upstream->check_shutdown();
}
} // namespace

void Http2Upstream::submit_goaway() { nghttp2_conn_shutdown(conn_); }

void Http2Upstream::check_shutdown() {
  auto worker = handler_->get_worker();

  if (!worker->get_graceful_shutdown()) {
    return;
  }

  ev_prepare_stop(handler_->get_loop(), &prep_);

  start_graceful_shutdown();
}

void Http2Upstream::start_graceful_shutdown() {
  if (ev_is_active(&shutdown_timer_)) {
    return;
  }

  nghttp2_conn_submit_shutdown_notice(conn_);

  handler_->signal_write();

  ev_timer_start(handler_->get_loop(), &shutdown_timer_);
}

namespace {
size_t downstream_queue_size(Worker *worker) {
  auto &downstreamconf = *worker->get_downstream_config();

  if (get_config()->http2_proxy) {
    return downstreamconf.connections_per_host;
  }

  return downstreamconf.connections_per_frontend;
}
} // namespace

Http2Upstream::Http2Upstream(ClientHandler *handler)
  : wb_(handler->get_worker()->get_mcpool()),
    downstream_queue_(downstream_queue_size(handler->get_worker()),
                      !get_config()->http2_proxy),
    handler_(handler) {
  int rv;

  auto config = get_config();
  auto &http2conf = config->http2;

  auto faddr = handler_->get_upstream_addr();

  static constexpr auto callbacks = nghttp2_callbacks{
    .rand = util::secure_random,
    .stream_close = stream_close,
    .write_stream_data_offset = write_stream_data_offset,
    .begin_headers = begin_headers,
    .recv_header = recv_header,
    .end_headers = end_headers,
    .recv_trailer = recv_trailer,
    .recv_data = recv_data,
    .local_end_stream = local_end_stream,
    .remote_end_stream = remote_end_stream,
    .shutdown = http2_shutdown,
  };

  nghttp2_settings settings;
  nghttp2_settings_default(&settings);

  if (http2conf.upstream.debug.frame_debug) {
    settings.log_write = log_write;
  }

  util::secure_random(reinterpret_cast<uint8_t *>(&settings.conn_id),
                      sizeof(settings.conn_id));
  settings.initial_ts = util::timestamp();
  settings.max_concurrent_streams_remote =
    http2conf.upstream.max_concurrent_streams;

  if (faddr->alt_mode != UpstreamAltMode::NONE) {
    settings.initial_max_stream_data = (1u << 31) - 1;
  } else {
    settings.initial_max_stream_data =
      as_unsigned(http2conf.upstream.window_size);
  }

  settings.enable_connect_protocol = !config->http2_proxy;
  settings.hpack_max_dtable_capacity =
    http2conf.upstream.decoder_dynamic_table_size;
  settings.hpack_encoder_max_dtable_capacity =
    http2conf.upstream.encoder_dynamic_table_size;

  if (faddr->alt_mode != UpstreamAltMode::NONE) {
    settings.initial_max_data = std::numeric_limits<int32_t>::max();
  } else {
    settings.initial_max_data =
      static_cast<size_t>(http2conf.upstream.connection_window_size);
  }

  settings.settings_timeout = static_cast<nghttp2_duration>(
    std::chrono::floor<std::chrono::nanoseconds>(
      util::duration_from(http2conf.upstream.timeout.settings))
      .count());

  rv = nghttp2_conn_server_new(&conn_, &callbacks, &settings, nullptr, this);

  assert(rv == 0);

  rv = nghttp2_conn_read(
    conn_, reinterpret_cast<const uint8_t *>(NGHTTP2_CLIENT_HTTP2_PREFACE),
    sizeof(NGHTTP2_CLIENT_HTTP2_PREFACE) - 1, util::timestamp());

  assert(rv == 0);

  ev_timer_init(
    &http2_timer_,
    [](struct ev_loop *loop, ev_timer *w, int revents) {
      auto upstream = static_cast<Http2Upstream *>(w->data);
      upstream->handle_http2_timeout();
    },
    0., 0.);
  http2_timer_.data = this;

  // timer for 2nd GOAWAY.  HTTP/2 spec recommend 1 RTT.  We wait for
  // 2 seconds.
  ev_timer_init(&shutdown_timer_, shutdown_timeout_cb, 2., 0);
  shutdown_timer_.data = this;

  ev_prepare_init(&prep_, prepare_cb);
  prep_.data = this;
  ev_prepare_start(handler_->get_loop(), &prep_);

#if defined(TCP_INFO) && defined(TCP_NOTSENT_LOWAT)
  if (http2conf.upstream.optimize_write_buffer_size) {
    auto conn = handler_->get_connection();
    conn->tls_dyn_rec_warmup_threshold = 0;

    uint32_t pollout_thres = 1;
    rv = setsockopt(conn->fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &pollout_thres,
                    static_cast<socklen_t>(sizeof(pollout_thres)));

    if (rv != 0) {
      if (log_enabled(INFO)) {
        auto error = errno;
        Log{INFO} << "setsockopt(TCP_NOTSENT_LOWAT, " << pollout_thres
                  << ") failed: errno=" << error;
      }
    }
  }
#endif // defined(TCP_INFO) && defined(TCP_NOTSENT_LOWAT)

  handler_->reset_upstream_read_timeout(
    config->conn.upstream.timeout.http2_idle);

  handler_->signal_write();
}

Http2Upstream::~Http2Upstream() {
  nghttp2_conn_del(conn_);
  ev_prepare_stop(handler_->get_loop(), &prep_);
  ev_timer_stop(handler_->get_loop(), &shutdown_timer_);
  ev_timer_stop(handler_->get_loop(), &http2_timer_);
}

std::expected<void, Error> Http2Upstream::on_read() {
  auto rb = handler_->get_rb();
  auto rlimit = handler_->get_rlimit();

  if (rb->rleft()) {
    auto rv =
      nghttp2_conn_read(conn_, rb->pos(), rb->rleft(), util::timestamp());
    if (rv != 0) {
      Log{ERROR, this} << "nghttp2_session_mem_recv2() returned error: "
                       << nghttp2_strerror(static_cast<int>(rv));

      return std::unexpected{Error::HTTP2};
    }

    rb->reset();
    rlimit->startw();
  }

  handler_->signal_write();
  return {};
}

// After this function call, downstream may be deleted.
std::expected<void, Error> Http2Upstream::on_write() {
  auto config = get_config();
  auto &http2conf = config->http2;

  if (http2conf.upstream.optimize_write_buffer_size && handler_->get_ssl()) {
    auto conn = handler_->get_connection();
    auto maybe_hint = conn->get_tcp_hint();
    if (maybe_hint) {
      const auto &hint = *maybe_hint;

      max_buffer_size_ =
        std::min(SHRPX_HTTP2_MAX_BUFFER_SIZE, hint.write_buffer_size);
    }
  }

  if (wb_.rleft()) {
    return {};
  }

  return wb_.append_or_error(
    16_k, [this](std::span<uint8_t> dest) -> std::expected<size_t, Error> {
      auto nwrite =
        nghttp2_conn_write(conn_, dest.data(), dest.size(), util::timestamp());
      if (nwrite < 0) {
        Log{ERROR, this} << "nghttp2_conn_write() returned error: "
                         << nghttp2_strerror(static_cast<int>(nwrite));

        return std::unexpected{Error::HTTP2};
      }

      return as_unsigned(nwrite);
    });
}

std::expected<void, Error> Http2Upstream::after_write() {
  reset_http2_timer();
  return {};
}

void Http2Upstream::reset_http2_timer() {
  auto loop = handler_->get_loop();

  auto expiry = nghttp2_conn_get_expiry(conn_);
  if (expiry == UINT64_MAX) {
    if (ev_is_active(&http2_timer_)) {
      ev_timer_stop(loop, &http2_timer_);
    }

    return;
  }

  auto now = util::timestamp();

  if (expiry <= now) {
    ev_feed_event(loop, &http2_timer_, EV_TIMER);

    return;
  }

  auto t = static_cast<ev_tstamp>(expiry - now) / NGHTTP2_SECONDS;

  http2_timer_.repeat = t;
  ev_timer_again(loop, &http2_timer_);
}

void Http2Upstream::handle_http2_timeout() {
  auto loop = handler_->get_loop();

  ev_timer_stop(loop, &http2_timer_);

  auto rv = nghttp2_conn_handle_expiry(conn_, util::timestamp());
  if (rv != 0) {
    Log{ERROR, this} << "nghttp2_conn_handle_expiry() returned error: "
                     << nghttp2_strerror(rv);

    terminate_session(nghttp2_err_infer_http2_error_code(rv));
  }

  handler_->signal_write();
}

ClientHandler *Http2Upstream::get_client_handler() const { return handler_; }

std::expected<void, Error>
Http2Upstream::downstream_read(DownstreamConnection *dconn) {
  auto downstream = dconn->get_downstream();

  if (downstream->get_response_state() == DownstreamState::MSG_RESET) {
    // The downstream stream was reset (canceled). In this case,
    // RST_STREAM to the upstream and delete downstream connection
    // here. Deleting downstream will be taken place at
    // on_stream_close_callback.
    shutdown_stream(downstream,
                    infer_upstream_rst_stream_error_code(
                      downstream->get_response_rst_stream_error_code()));
    downstream->pop_downstream_connection();
    // dconn was deleted
    dconn = nullptr;
  } else if (downstream->get_response_state() ==
             DownstreamState::MSG_BAD_HEADER) {
    if (auto rv = error_reply(downstream, 502); !rv) {
      return rv;
    }
    downstream->pop_downstream_connection();
    // dconn was deleted
    dconn = nullptr;
  } else {
    auto rv = downstream->on_read();
    if (!rv) {
      if (rv.error() == Error::RECV_EOF) {
        if (downstream->get_request_header_sent()) {
          return downstream_eof(dconn);
        }
        return std::unexpected{Error::DCONN_RETRY};
      }
      if (rv.error() == Error::DCONN_CANCELED) {
        downstream->pop_downstream_connection();
        handler_->signal_write();
        return {};
      }
      if (rv.error() != Error::NETWORK) {
        if (log_enabled(INFO)) {
          Log{INFO, dconn} << "HTTP parser failure";
        }
      }
      return downstream_error(dconn, Downstream::EVENT_ERROR);
    }

    if (downstream->can_detach_downstream_connection()) {
      // Keep-alive
      if (auto rv = downstream->detach_downstream_connection(); !rv) {
        return rv;
      }
    }
  }

  handler_->signal_write();

  // At this point, downstream may be deleted.

  return {};
}

std::expected<void, Error>
Http2Upstream::downstream_write(DownstreamConnection *dconn) {
  auto rv = dconn->on_write();
  if (!rv) {
    if (rv.error() == Error::NETWORK) {
      return downstream_error(dconn, Downstream::EVENT_ERROR);
    }

    return rv;
  }

  return {};
}

std::expected<void, Error>
Http2Upstream::downstream_eof(DownstreamConnection *dconn) {
  auto downstream = dconn->get_downstream();

  if (log_enabled(INFO)) {
    Log{INFO, dconn} << "EOF. stream_id=" << downstream->get_stream_id();
  }

  // Delete downstream connection. If we don't delete it here, it will
  // be pooled in on_stream_close_callback.
  downstream->pop_downstream_connection();
  // dconn was deleted
  dconn = nullptr;
  // downstream will be deleted in on_stream_close_callback.
  if (downstream->get_response_state() == DownstreamState::HEADER_COMPLETE) {
    // Server may indicate the end of the request by EOF
    if (log_enabled(INFO)) {
      Log{INFO, this} << "Downstream body was ended by EOF";
    }
    downstream->set_response_state(DownstreamState::MSG_COMPLETE);

    // For tunneled connection, MSG_COMPLETE signals
    // downstream_data_read_callback to send RST_STREAM after pending
    // response body is sent. This is needed to ensure that RST_STREAM
    // is sent after all pending data are sent.
    if (auto rv = on_downstream_body_complete(downstream); !rv) {
      return rv;
    }
  } else if (downstream->get_response_state() !=
             DownstreamState::MSG_COMPLETE) {
    // If stream was not closed, then we set MSG_COMPLETE and let
    // on_stream_close_callback delete downstream.
    if (auto rv = error_reply(downstream, 502); !rv) {
      return rv;
    }
  }
  handler_->signal_write();
  // At this point, downstream may be deleted.
  return {};
}

std::expected<void, Error>
Http2Upstream::downstream_error(DownstreamConnection *dconn, int events) {
  auto downstream = dconn->get_downstream();

  if (log_enabled(INFO)) {
    if (events & Downstream::EVENT_ERROR) {
      Log{INFO, dconn} << "Downstream network/general error";
    } else {
      Log{INFO, dconn} << "Timeout";
    }
    if (downstream->get_upgraded()) {
      Log{INFO, dconn} << "Note: this is tunnel connection";
    }
  }

  // Delete downstream connection. If we don't delete it here, it will
  // be pooled in on_stream_close_callback.
  downstream->pop_downstream_connection();
  // dconn was deleted
  dconn = nullptr;

  if (downstream->get_response_state() == DownstreamState::MSG_COMPLETE) {
    // For SSL tunneling, we issue RST_STREAM. For other types of
    // stream, we don't have to do anything since response was
    // complete.
    if (downstream->get_upgraded()) {
      shutdown_stream(downstream, NGHTTP2_NO_ERROR);
    }
  } else {
    if (downstream->get_response_state() == DownstreamState::HEADER_COMPLETE) {
      if (downstream->get_upgraded()) {
        if (auto rv = on_downstream_body_complete(downstream); !rv) {
          return rv;
        }
      } else {
        shutdown_stream(downstream, NGHTTP2_INTERNAL_ERROR);
      }
    } else {
      unsigned int status;
      if (events & Downstream::EVENT_TIMEOUT) {
        if (downstream->get_request_header_sent()) {
          status = 504;
        } else {
          status = 408;
        }
      } else {
        status = 502;
      }
      if (auto rv = error_reply(downstream, status); !rv) {
        return rv;
      }
    }
    downstream->set_response_state(DownstreamState::MSG_COMPLETE);
  }
  handler_->signal_write();
  // At this point, downstream may be deleted.
  return {};
}

void Http2Upstream::shutdown_stream(Downstream *downstream,
                                    uint32_t error_code) {
  if (log_enabled(INFO)) {
    Log{INFO, this} << "RST_STREAM stream_id=" << downstream->get_stream_id()
                    << " with error_code=" << error_code;
  }

  nghttp2_conn_shutdown_stream(conn_, 0x00, downstream->get_stream_id(),
                               error_code);
}

void Http2Upstream::terminate_session(uint32_t error_code) {
  nghttp2_conn_terminate(conn_, error_code);
}

namespace {
nghttp2_ssize downstream_read_data(nghttp2_conn *conn, int64_t stream_id,
                                   nghttp2_vec *vec, size_t veccnt,
                                   uint32_t *pflags, void *conn_user_data,
                                   void *stream_user_data) {
  auto downstream = static_cast<Downstream *>(stream_user_data);
  auto body = downstream->get_response_buf();
  assert(body);
  const auto &resp = downstream->response();

  if (downstream->get_response_state() == DownstreamState::MSG_COMPLETE) {
    *pflags |= NGHTTP2_READ_DATA_FLAG_EOF;

    if (!downstream->get_upgraded()) {
      const auto &trailers = resp.fs.trailers();
      if (!trailers.empty()) {
        std::vector<nghttp2_nv> nva;
        nva.reserve(trailers.size());
        http2::copy_headers_to_nva_nocopy(nva, trailers, http2::HDOP_STRIP_ALL);
        if (!nva.empty()) {
          if (auto rv = nghttp2_conn_submit_trailers(conn, stream_id,
                                                     nva.data(), nva.size());
              rv != 0) {
            if (nghttp2_err_is_fatal(rv)) {
              return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
          }
        }
      }
    }
  } else if (body->rleft() == 0) {
    downstream->disable_upstream_wtimer();
    return NGHTTP2_ERR_WOULDBLOCK;
  }

  auto v = body->riovec({vec, veccnt});

  return as_signed(v.size());
}
} // namespace

std::expected<void, Error>
Http2Upstream::send_reply(Downstream *downstream,
                          std::span<const uint8_t> body) {
  int rv;

  static constexpr auto dr = nghttp2_data_reader{
    .read_data = downstream_read_data,
  };
  const nghttp2_data_reader *pdr = nullptr;

  const auto &req = downstream->request();

  if (req.method != HTTP_HEAD && !body.empty()) {
    pdr = &dr;

    auto buf = downstream->get_response_buf();

    buf->append(body);
  }

  const auto &resp = downstream->response();
  auto config = get_config();
  auto &httpconf = config->http;

  auto &balloc = downstream->get_block_allocator();

  const auto &headers = resp.fs.headers();
  auto nva = std::vector<nghttp2_nv>();
  // 2 for :status and server
  nva.reserve(2 + headers.size() + httpconf.add_response_headers.size());

  auto response_status = http2::stringify_status(balloc, resp.http_status);

  nva.push_back(http2::make_field(":status"sv, response_status));

  for (auto &kv : headers) {
    if (kv.name.empty() || kv.name[0] == ':') {
      continue;
    }
    switch (kv.token) {
    case NGHTTP2_HPACK_TOKEN_CONNECTION:
    case NGHTTP2_HPACK_TOKEN_KEEP_ALIVE:
    case NGHTTP2_HPACK_TOKEN_PROXY_CONNECTION:
    case NGHTTP2_HPACK_TOKEN_TE:
    case NGHTTP2_HPACK_TOKEN_TRANSFER_ENCODING:
    case NGHTTP2_HPACK_TOKEN_UPGRADE:
      continue;
    }
    nva.push_back(
      http2::make_field(kv.name, kv.value, http2::never_index(kv.never_index)));
  }

  if (!resp.fs.header(NGHTTP2_HPACK_TOKEN_SERVER)) {
    nva.push_back(http2::make_field("server"sv, config->http.server_name));
  }

  for (auto &p : httpconf.add_response_headers) {
    nva.push_back(http2::make_field(p.name, p.value));
  }

  rv = nghttp2_conn_submit_response(conn_, downstream->get_stream_id(),
                                    nva.data(), nva.size(), pdr);
  if (nghttp2_err_is_fatal(rv)) {
    Log{FATAL, this} << "nghttp2_conn_submit_response() failed: "
                     << nghttp2_strerror(rv);

    return std::unexpected{Error::HTTP2};
  }

  downstream->set_response_state(DownstreamState::MSG_COMPLETE);

  if (pdr) {
    downstream->reset_upstream_wtimer();
    downstream->register_upstream_write_rate_timer();
  }

  return {};
}

std::expected<void, Error>
Http2Upstream::error_reply(Downstream *downstream, unsigned int status_code) {
  int rv;
  auto &resp = downstream->response();

  auto &balloc = downstream->get_block_allocator();

  auto html = http::create_error_html(balloc, status_code);
  resp.http_status = status_code;

  static constexpr auto dr = nghttp2_data_reader{
    .read_data = downstream_read_data,
  };
  const nghttp2_data_reader *pdr = nullptr;

  const auto &req = downstream->request();

  if (req.method != HTTP_HEAD) {
    pdr = &dr;

    auto body = downstream->get_response_buf();

    body->append(html);
  }

  downstream->set_response_state(DownstreamState::MSG_COMPLETE);

  auto lgconf = log_config();
  lgconf->update_tstamp(std::chrono::system_clock::now());

  auto response_status = http2::stringify_status(balloc, status_code);
  auto content_length = util::make_string_ref_uint(balloc, html.size());
  auto date = make_string_ref(balloc, lgconf->tstamp->time_http);

  auto nva = std::to_array(
    {http2::make_field(":status"sv, response_status),
     http2::make_field("content-type"sv, "text/html; charset=UTF-8"sv),
     http2::make_field("server"sv, get_config()->http.server_name),
     http2::make_field("content-length"sv, content_length),
     http2::make_field("date"sv, date)});

  rv = nghttp2_conn_submit_response(conn_, downstream->get_stream_id(),
                                    nva.data(), nva.size(), pdr);
  if (rv < NGHTTP2_ERR_FATAL) {
    Log{FATAL, this} << "nghttp2_conn_submit_response() failed: "
                     << nghttp2_strerror(rv);
    return std::unexpected{Error::HTTP2};
  }

  if (pdr) {
    downstream->reset_upstream_wtimer();
    downstream->register_upstream_write_rate_timer();
  }

  return {};
}

void Http2Upstream::add_pending_downstream(
  std::unique_ptr<Downstream> downstream) {
  downstream_queue_.add_pending(std::move(downstream));
}

std::expected<void, Error>
Http2Upstream::remove_downstream(Downstream *downstream) {
  if (downstream->accesslog_ready()) {
    handler_->write_accesslog(downstream);
  }

  nghttp2_conn_set_stream_user_data(conn_, downstream->get_stream_id(),
                                    nullptr);

  auto next_downstream = downstream_queue_.remove_and_get_blocked(downstream);

  if (next_downstream) {
    if (auto rv = initiate_downstream(next_downstream); !rv) {
      return rv;
    }
  }

  if (downstream_queue_.get_downstreams() == nullptr) {
    // There is no downstream at the moment.  Start idle timer now.
    auto config = get_config();
    auto &upstreamconf = config->conn.upstream;

    handler_->reset_upstream_read_timeout(upstreamconf.timeout.http2_idle);
  }

  return {};
}

// WARNING: Never call directly or indirectly nghttp2_conn_read or
// nghttp2_conn_write. These calls may delete downstream.
std::expected<void, Error>
Http2Upstream::on_downstream_header_complete(Downstream *downstream) {
  int rv;

  const auto &req = downstream->request();
  auto &resp = downstream->response();

  auto &balloc = downstream->get_block_allocator();

  if (log_enabled(INFO)) {
    if (downstream->get_non_final_response()) {
      Log{INFO, downstream} << "HTTP non-final response header";
    } else {
      Log{INFO, downstream} << "HTTP response header completed";
    }
  }

  auto config = get_config();
  auto &httpconf = config->http;

  if (!config->http2_proxy && !httpconf.no_location_rewrite) {
    downstream->rewrite_location_response_header(req.scheme);
  }

#ifdef HAVE_MRUBY
  if (!downstream->get_non_final_response()) {
    auto dconn = downstream->get_downstream_connection();
    const auto &group = dconn->get_downstream_addr_group();
    if (group) {
      const auto &dmruby_ctx = group->shared_addr->mruby_ctx;

      if (auto rv = dmruby_ctx->run_on_response_proc(downstream); !rv) {
        if (auto rv = error_reply(downstream, 500); !rv) {
          return rv;
        }
        // Returning an error will signal deletion of dconn.
        return rv;
      }

      if (downstream->get_response_state() == DownstreamState::MSG_COMPLETE) {
        return std::unexpected{Error::INTERNAL};
      }
    }

    auto worker = handler_->get_worker();
    auto mruby_ctx = worker->get_mruby_context();

    if (auto rv = mruby_ctx->run_on_response_proc(downstream); !rv) {
      if (auto rv = error_reply(downstream, 500); !rv) {
        return rv;
      }
      // Returning an error will signal deletion of dconn.
      return rv;
    }

    if (downstream->get_response_state() == DownstreamState::MSG_COMPLETE) {
      return std::unexpected{Error::INTERNAL};
    }
  }
#endif // defined(HAVE_MRUBY)

  auto &http2conf = config->http2;

  auto nva = std::vector<nghttp2_nv>();
  // 6 means :status and possible server, via, x-http2-push, alt-svc,
  // and set-cookie (for affinity cookie) header field.
  nva.reserve(resp.fs.headers().size() + 6 +
              httpconf.add_response_headers.size());

  if (downstream->get_non_final_response()) {
    auto response_status = http2::stringify_status(balloc, resp.http_status);

    nva.push_back(http2::make_field(":status"sv, response_status));

    http2::copy_headers_to_nva_nocopy(nva, resp.fs.headers(),
                                      http2::HDOP_STRIP_ALL);

    if (log_enabled(INFO)) {
      log_response_headers(downstream, nva);
    }

    rv = nghttp2_conn_submit_info(conn_, downstream->get_stream_id(),
                                  nva.data(), nva.size());

    resp.fs.clear_headers();

    if (rv != 0) {
      Log{FATAL, this} << "nghttp2_conn_submit_info() failed";
      return std::unexpected{Error::HTTP2};
    }

    return {};
  }

  auto striphd_flags =
    static_cast<uint32_t>(http2::HDOP_STRIP_ALL & ~http2::HDOP_STRIP_VIA);
  std::string_view response_status;

  if (req.connect_proto == ConnectProto::WEBSOCKET && resp.http_status == 101) {
    response_status = http2::stringify_status(balloc, 200);
    striphd_flags |= http2::HDOP_STRIP_SEC_WEBSOCKET_ACCEPT;
  } else {
    response_status = http2::stringify_status(balloc, resp.http_status);
  }

  nva.push_back(http2::make_field(":status"sv, response_status));

  http2::copy_headers_to_nva_nocopy(nva, resp.fs.headers(), striphd_flags);

  if (!config->http2_proxy && !httpconf.no_server_rewrite) {
    nva.push_back(http2::make_field("server"sv, httpconf.server_name));
  } else {
    auto server = resp.fs.header(NGHTTP2_HPACK_TOKEN_SERVER);
    if (server) {
      nva.push_back(http2::make_field("server"sv, (*server).value));
    }
  }

  if (!req.regular_connect_method() || !downstream->get_upgraded()) {
    auto affinity_cookie = downstream->get_affinity_cookie_to_send();
    if (affinity_cookie) {
      auto dconn = downstream->get_downstream_connection();
      assert(dconn);
      auto &group = dconn->get_downstream_addr_group();
      auto &shared_addr = group->shared_addr;
      auto &cookieconf = shared_addr->affinity.cookie;
      auto secure =
        http::require_cookie_secure_attribute(cookieconf.secure, req.scheme);
      auto cookie_str = http::create_affinity_cookie(
        balloc, cookieconf.name, affinity_cookie, cookieconf.path, secure);
      nva.push_back(http2::make_field("set-cookie"sv, cookie_str));
    }
  }

  if (!resp.fs.header(NGHTTP2_HPACK_TOKEN_ALT_SVC)) {
    // We won't change or alter alt-svc from backend for now
    if (!httpconf.http2_altsvc_header_value.empty()) {
      nva.push_back(
        http2::make_field("alt-svc"sv, httpconf.http2_altsvc_header_value));
    }
  }

  auto via = resp.fs.header(NGHTTP2_HPACK_TOKEN_VIA);
  if (httpconf.no_via) {
    if (via) {
      nva.push_back(http2::make_field("via"sv, (*via).value));
    }
  } else {
    // we don't create more than 16 bytes in
    // http::create_via_header_value.
    size_t len = 16;
    if (via) {
      len += via->value.size() + 2;
    }

    auto iov = make_byte_ref(balloc, len + 1);
    auto p = std::ranges::begin(iov);
    if (via) {
      p = std::ranges::copy(via->value, p).out;
      p = std::ranges::copy(", "sv, p).out;
    }
    p = http::create_via_header_value(p, resp.http_major, resp.http_minor);
    *p = '\0';

    nva.push_back(
      http2::make_field("via"sv, as_string_view(std::ranges::begin(iov), p)));
  }

  for (auto &p : httpconf.add_response_headers) {
    nva.push_back(http2::make_field(p.name, p.value));
  }

  if (downstream->get_stream_id() % 2 == 0) {
    // This header field is basically for human on client side to
    // figure out that the resource is pushed.
    nva.push_back(http2::make_field("x-http2-push"sv, "1"sv));
  }

  if (log_enabled(INFO)) {
    log_response_headers(downstream, nva);
  }

  if (http2conf.upstream.debug.dump.response_header) {
    http2::dump_nv(http2conf.upstream.debug.dump.response_header, nva.data(),
                   nva.size());
  }

  if (auto priority = resp.fs.header(NGHTTP2_HPACK_TOKEN_PRIORITY); priority) {
    nghttp2_pri pri;

    if (nghttp2_conn_get_stream_priority(conn_, &pri,
                                         downstream->get_stream_id()) == 0 &&
        nghttp2_pri_parse_priority(
          &pri, reinterpret_cast<const uint8_t *>(priority->value.data()),
          priority->value.size()) == 0) {
      if (auto rv = nghttp2_conn_set_server_stream_priority(
            conn_, downstream->get_stream_id(), &pri);
          rv != 0) {
        Log{ERROR, this} << "nghttp2_conn_set_server_stream_priority: "
                         << nghttp2_strerror(rv);
      }
    }
  }

  static constexpr auto dr = nghttp2_data_reader{
    .read_data = downstream_read_data,
  };
  const nghttp2_data_reader *pdr;

  if (downstream->expect_response_body() ||
      downstream->expect_response_trailer()) {
    pdr = &dr;
  } else {
    pdr = nullptr;
  }

  rv = nghttp2_conn_submit_response(conn_, downstream->get_stream_id(),
                                    nva.data(), nva.size(), pdr);
  if (rv != 0) {
    Log{FATAL, this} << "nghttp2_conn_submit_response() failed: "
                     << nghttp2_strerror(rv);

    return std::unexpected{Error::HTTP2};
  }

  return {};
}

// WARNING: Never call directly or indirectly nghttp2_session_send or
// nghttp2_session_recv. These calls may delete downstream.
std::expected<void, Error>
Http2Upstream::on_downstream_body(Downstream *downstream,
                                  std::span<const uint8_t> data, bool flush) {
  auto body = downstream->get_response_buf();
  body->append(data);

  if (flush) {
    if (auto rv =
          nghttp2_conn_resume_stream(conn_, downstream->get_stream_id());
        rv != 0) {
      Log{FATAL, this} << "nghttp2_conn_resume_stream() failed: "
                       << nghttp2_strerror(rv);

      return std::unexpected{Error::HTTP2};
    }

    downstream->ensure_upstream_wtimer();
    downstream->register_upstream_write_rate_timer();
  }

  return {};
}

// WARNING: Never call directly or indirectly nghttp2_session_send or
// nghttp2_session_recv. These calls may delete downstream.
std::expected<void, Error>
Http2Upstream::on_downstream_body_complete(Downstream *downstream) {
  if (log_enabled(INFO)) {
    Log{INFO, downstream} << "HTTP response completed";
  }

  auto &resp = downstream->response();

  if (!downstream->validate_response_recv_body_length()) {
    shutdown_stream(downstream, NGHTTP2_PROTOCOL_ERROR);
    resp.connection_close = true;

    return {};
  }

  nghttp2_conn_resume_stream(conn_, downstream->get_stream_id());
  downstream->ensure_upstream_wtimer();
  downstream->register_upstream_write_rate_timer();

  return {};
}

bool Http2Upstream::get_flow_control() const { return flow_control_; }

void Http2Upstream::pause_read(IOCtrlReason reason) {}

std::expected<void, Error> Http2Upstream::resume_read(IOCtrlReason reason,
                                                      Downstream *downstream,
                                                      size_t consumed) {
  if (get_flow_control()) {
    if (auto rv = consume(downstream->get_stream_id(), consumed); !rv) {
      return rv;
    }

    auto &req = downstream->request();

    req.consume(consumed);
  }

  handler_->signal_write();
  return {};
}

std::expected<void, Error>
Http2Upstream::on_downstream_abort_request(Downstream *downstream,
                                           unsigned int status_code) {
  if (auto rv = error_reply(downstream, status_code); !rv) {
    return rv;
  }

  handler_->signal_write();
  return {};
}

std::expected<void, Error>
Http2Upstream::on_downstream_abort_request_with_https_redirect(
  Downstream *downstream) {
  if (auto rv = redirect_to_https(downstream); !rv) {
    return rv;
  }

  handler_->signal_write();
  return {};
}

std::expected<void, Error>
Http2Upstream::redirect_to_https(Downstream *downstream) {
  auto &req = downstream->request();
  if (req.regular_connect_method() || req.scheme != "http"sv) {
    return error_reply(downstream, 400);
  }

  auto maybe_authority = util::extract_host(req.authority);
  if (!maybe_authority) {
    return error_reply(downstream, 400);
  }

  auto &balloc = downstream->get_block_allocator();
  auto config = get_config();
  auto &httpconf = config->http;

  std::string_view loc;
  if (httpconf.redirect_https_port == "443"sv) {
    loc = concat_string_ref(balloc, "https://"sv, *maybe_authority, req.path);
  } else {
    loc = concat_string_ref(balloc, "https://"sv, *maybe_authority, ":"sv,
                            httpconf.redirect_https_port, req.path);
  }

  auto &resp = downstream->response();
  resp.http_status = 308;
  resp.fs.add_header_token("location"sv, loc, false,
                           NGHTTP2_HPACK_TOKEN_LOCATION);

  return send_reply(downstream, {});
}

std::expected<void, Error> Http2Upstream::consume(int64_t stream_id,
                                                  size_t len) {
  auto faddr = handler_->get_upstream_addr();

  if (faddr->alt_mode != UpstreamAltMode::NONE) {
    return {};
  }

  if (auto rv = nghttp2_conn_extend_max_stream_offset(conn_, stream_id, len);
      rv != 0) {
    Log{ERROR, this}
      << "nghttp2_conn_extend_max_stream_offset() returned error: "
      << nghttp2_strerror(rv);

    return std::unexpected{Error::HTTP2};
  }

  if (auto rv = nghttp2_conn_extend_max_offset(conn_, len); rv != 0) {
    Log{ERROR, this} << "nghttp2_conn_extend_max_offset() returned error: "
                     << nghttp2_strerror(rv);

    return std::unexpected{Error::HTTP2};
  }

  return {};
}

namespace {
std::string format_nva(std::span<const nghttp2_nv> nva) {
  std::string s;

  for (auto &nv : nva) {
    s += tty_http_hd();
    s += as_string_view(nv.name, nv.namelen);
    s += tty_rst();
    s += ": ";
    s += as_string_view(nv.value, nv.valuelen);
    s += '\n';
  }

  return s;
}
} // namespace

void Http2Upstream::log_response_headers(
  Downstream *downstream, const std::vector<nghttp2_nv> &nva) const {
  Log{INFO, this} << "HTTP response headers. stream_id="
                  << downstream->get_stream_id() << "\n"
                  << format_nva(nva);
}

std::expected<void, Error> Http2Upstream::on_timeout(Downstream *downstream) {
  if (log_enabled(INFO)) {
    Log{INFO, this} << "Stream timeout stream_id="
                    << downstream->get_stream_id();
  }

  shutdown_stream(downstream, NGHTTP2_INTERNAL_ERROR);

  handler_->signal_write();

  return {};
}

void Http2Upstream::on_handler_delete() {
  for (auto d = downstream_queue_.get_downstreams(); d; d = d->dlnext) {
    if (d->get_dispatch_state() == DispatchState::ACTIVE &&
        d->accesslog_ready()) {
      handler_->write_accesslog(d);
    }
  }
}

std::expected<void, Error>
Http2Upstream::on_downstream_reset(Downstream *downstream, bool no_retry) {
  if (downstream->get_dispatch_state() != DispatchState::ACTIVE) {
    // This is error condition when we failed push_request_headers()
    // in initiate_downstream().  Otherwise, we have
    // DispatchState::ACTIVE state, or we did not set
    // DownstreamConnection.
    downstream->pop_downstream_connection();
    handler_->signal_write();

    return {};
  }

  if (!downstream->request_submission_ready()) {
    if (downstream->get_response_state() == DownstreamState::MSG_COMPLETE) {
      // We have got all response body already.  Send it off.
      downstream->pop_downstream_connection();
      return {};
    }
    // pushed stream is handled here

    shutdown_stream(downstream, NGHTTP2_INTERNAL_ERROR);

    downstream->pop_downstream_connection();

    handler_->signal_write();

    return {};
  }

  downstream->pop_downstream_connection();

  downstream->add_retry();

  std::unique_ptr<DownstreamConnection> dconn;

  auto err = Error::INTERNAL;

  if (no_retry || downstream->no_more_retry()) {
    goto fail;
  }

  // downstream connection is clean; we can retry with new
  // downstream connection.

  for (;;) {
    auto maybe_dconn = handler_->get_downstream_connection(downstream);
    if (!maybe_dconn) {
      err = maybe_dconn.error();
      goto fail;
    }

    if (downstream->attach_downstream_connection(std::move(*maybe_dconn))) {
      break;
    }
  }

  if (auto rv = downstream->push_request_headers(); !rv) {
    err = rv.error();
    goto fail;
  }

  return {};

fail:
  if (!(err == Error::TLS_REQUIRED
          ? on_downstream_abort_request_with_https_redirect(downstream)
          : on_downstream_abort_request(downstream, 502))) {
    shutdown_stream(downstream, NGHTTP2_INTERNAL_ERROR);
  }
  downstream->pop_downstream_connection();

  handler_->signal_write();

  return {};
}

std::span<struct iovec>
Http2Upstream::response_riovec(std::span<struct iovec> iov) const {
  return wb_.riovec(iov);
}

std::span<const uint8_t> Http2Upstream::response_peek() const {
  return wb_.peek();
}

void Http2Upstream::response_drain(size_t n) { wb_.drain(n); }

bool Http2Upstream::response_empty() const { return wb_.rleft() == 0; }

DefaultMemchunks *Http2Upstream::get_response_buf() { return &wb_; }

size_t Http2Upstream::get_max_buffer_size() const { return max_buffer_size_; }

} // namespace shrpx
