/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2013 Tatsuhiro Tsujikawa
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
#include "nghttp.h"

#include <sys/stat.h>
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
#include <getopt.h>
#include <sys/mman.h>

#include <cassert>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <tuple>
#include <print>

#include "ssl_compat.h"

#ifdef NGHTTP2_OPENSSL_IS_WOLFSSL
#  include <wolfssl/options.h>
#  include <wolfssl/openssl/err.h>
#else // !defined(NGHTTP2_OPENSSL_IS_WOLFSSL)
#  include <openssl/err.h>
#endif // !defined(NGHTTP2_OPENSSL_IS_WOLFSSL)

#ifdef HAVE_JANSSON
#  include <jansson.h>
#endif // defined(HAVE_JANSSON)

#include "app_helper.h"
#include "HtmlParser.h"
#include "util.h"
#include "base64.h"
#include "tls.h"
#include "template.h"

#ifndef O_BINARY
#  define O_BINARY (0)
#endif // !defined(O_BINARY)

namespace nghttp2 {

namespace {
Config config;
} // namespace

namespace {
void print_protocol_nego_error() {
  std::println(stderr, "[ERROR] HTTP/2 protocol was not selected. (nghttp2 "
                       "expects h2)");
}
} // namespace

namespace {
std::string strip_fragment(const char *raw_uri) {
  const char *end;
  for (end = raw_uri; *end && *end != '#'; ++end)
    ;
  return std::string(raw_uri, end);
}
} // namespace

Request::Request(const std::string &uri, const urlparse_url &u,
                 const nghttp2_data_reader *dr, int64_t data_length,
                 const nghttp2_pri &pri, int level)
  : uri{uri}, u{u}, pri{pri}, data_length{data_length}, dr{dr}, level{level} {
  http2::init_hdidx(res_hdidx);
  http2::init_hdidx(req_hdidx);
}

Request::~Request() { nghttp2_gzip_inflate_del(inflater); }

void Request::init_inflater() {
  int rv;
  // This is required with --disable-assert.
  (void)rv;
  rv = nghttp2_gzip_inflate_new(&inflater);
  assert(rv == 0);
}

std::string_view Request::get_real_scheme() const {
  return config.scheme_override.empty()
           ? util::get_uri_field(uri.c_str(), u, URLPARSE_SCHEMA)
           : std::string_view{config.scheme_override};
}

std::string_view Request::get_real_host() const {
  return config.host_override.empty()
           ? util::get_uri_field(uri.c_str(), u, URLPARSE_HOST)
           : std::string_view{config.host_override};
}

uint16_t Request::get_real_port() const {
  auto scheme = get_real_scheme();
  return config.host_override.empty() ? util::has_uri_field(u, URLPARSE_PORT)
                                          ? u.port
                                        : scheme == "https"sv ? 443
                                                              : 80
         : config.port_override == 0  ? scheme == "https"sv ? 443 : 80
                                      : config.port_override;
}

void Request::init_html_parser() {
  // We crawl HTML using overridden scheme, host, and port.
  auto scheme = get_real_scheme();
  auto host = get_real_host();
  auto port = get_real_port();
  auto ipv6_lit = util::contains(host, ':');

  auto base_uri = std::string{scheme};
  base_uri += "://";
  if (ipv6_lit) {
    base_uri += '[';
  }
  base_uri += host;
  if (ipv6_lit) {
    base_uri += ']';
  }
  if (!((scheme == "https"sv && port == 443) ||
        (scheme == "http"sv && port == 80))) {
    base_uri += ':';
    base_uri += util::utos(port);
  }
  base_uri += util::get_uri_field(uri.c_str(), u, URLPARSE_PATH);
  if (util::has_uri_field(u, URLPARSE_QUERY)) {
    base_uri += '?';
    base_uri += util::get_uri_field(uri.c_str(), u, URLPARSE_QUERY);
  }

  html_parser = std::make_unique<HtmlParser>(base_uri);
}

std::expected<void, Error>
Request::update_html_parser(std::span<const uint8_t> data, int fin) {
  if (!html_parser) {
    return {};
  }

  return html_parser->parse_chunk(data, fin);
}

std::string Request::make_reqpath() const {
  auto path =
    util::has_uri_field(u, URLPARSE_PATH)
      ? std::string{util::get_uri_field(uri.c_str(), u, URLPARSE_PATH)}
      : "/"s;
  if (util::has_uri_field(u, URLPARSE_QUERY)) {
    path += '?';
    path.append(uri.c_str() + u.field_data[URLPARSE_QUERY].off,
                u.field_data[URLPARSE_QUERY].len);
  }
  return path;
}

namespace {
// Perform special handling |host| if it is IPv6 literal and includes
// zone ID per RFC 6874.
std::string decode_host(std::string_view host) {
  auto zone_start = std::ranges::find(host, '%');
  if (zone_start == std::ranges::end(host) ||
      !util::ipv6_numeric_addr(
        std::string(std::ranges::begin(host), zone_start).c_str())) {
    return std::string{host};
  }
  // case: ::1%
  if (zone_start + 1 == std::ranges::end(host)) {
    return {host.data(), host.size() - 1};
  }
  // case: ::1%12 or ::1%1
  if (zone_start + 3 >= std::ranges::end(host)) {
    return std::string{host};
  }
  // If we see "%25", followed by more characters, then decode %25 as
  // '%'.
  auto zone_id_src = (*(zone_start + 1) == '2' && *(zone_start + 2) == '5')
                       ? zone_start + 3
                       : zone_start + 1;
  auto zone_id = util::percent_decode(zone_id_src, std::ranges::end(host));
  auto res = std::string(std::ranges::begin(host), zone_start + 1);
  res += zone_id;
  return res;
}
} // namespace

namespace {
nghttp2_pri resolve_pri(int res_type) {
  switch (res_type) {
  case REQ_CSS:
  case REQ_JS:
    return {
      .urgency = 0,
    };
  case REQ_UNBLOCK_JS:
    return {
      .urgency = 1,
    };
  case REQ_IMG:
    return {
      .urgency = NGHTTP2_DEFAULT_URGENCY,
      .inc = 1,
    };
  default:
    return {
      .urgency = NGHTTP2_DEFAULT_URGENCY,
    };
  }
}
} // namespace

bool Request::is_ipv6_literal_addr() const {
  if (util::has_uri_field(u, URLPARSE_HOST)) {
    return memchr(uri.c_str() + u.field_data[URLPARSE_HOST].off, ':',
                  u.field_data[URLPARSE_HOST].len);
  } else {
    return false;
  }
}

Headers::value_type *Request::get_res_header(int32_t token) {
  auto idx = res_hdidx[static_cast<size_t>(token)];
  if (idx == -1) {
    return nullptr;
  }
  return &res_nva[static_cast<size_t>(idx)];
}

Headers::value_type *Request::get_req_header(int32_t token) {
  auto idx = req_hdidx[static_cast<size_t>(token)];
  if (idx == -1) {
    return nullptr;
  }
  return &req_nva[static_cast<size_t>(idx)];
}

namespace {
std::chrono::steady_clock::time_point get_time() {
  return std::chrono::steady_clock::now();
}
} // namespace

void Request::record_request_start_time() {
  timing.state = RequestState::ON_REQUEST;
  timing.request_start_time = get_time();
}

void Request::record_response_start_time() {
  timing.state = RequestState::ON_RESPONSE;
  timing.response_start_time = get_time();
}

void Request::record_response_end_time() {
  timing.state = RequestState::ON_COMPLETE;
  timing.response_end_time = get_time();
}

ContinueTimer::ContinueTimer(struct ev_loop *loop, Request *req) : loop(loop) {
  ev_timer_init(
    &timer,
    [](struct ev_loop *loop, ev_timer *w, int revents) {
      auto client = static_cast<HttpClient *>(ev_userdata(loop));
      auto req = static_cast<Request *>(w->data);

      req->unblock_continue = true;
      req->continue_timer->stop();
      nghttp2_conn_resume_stream(client->conn, req->stream_id);

      client->signal_write();
    },
    1., 0.);
  timer.data = req;
}

ContinueTimer::~ContinueTimer() { stop(); }

void ContinueTimer::start() { ev_timer_start(loop, &timer); }

void ContinueTimer::stop() { ev_timer_stop(loop, &timer); }

void ContinueTimer::dispatch_continue() {
  // Only dispatch the timeout callback if it hasn't already been
  // called.
  if (ev_is_active(&timer)) {
    ev_feed_event(loop, &timer, 0);
  }
}

namespace {
std::expected<void, Error>
submit_request(HttpClient *client, const Headers &headers, Request *req) {
  auto scheme = util::get_uri_field(req->uri.c_str(), req->u, URLPARSE_SCHEMA);
  auto build_headers = Headers{{":method", req->dr ? "POST" : "GET"},
                               {":path", req->make_reqpath()},
                               {":scheme", std::string{scheme}},
                               {":authority", client->hostport},
                               {"priority", http2::encode_extpri(req->pri)},
                               {"accept", "*/*"},
                               {"accept-encoding", "gzip, deflate"},
                               {"user-agent", "nghttp2/" NGHTTP2_VERSION}};
  auto expect_continue = false;

  if (config.continuation) {
    for (auto i = 0UZ; i < 6; ++i) {
      build_headers.emplace_back("continuation-test-" + util::utos(i + 1),
                                 std::string(4_k, '-'));
    }
  }

  auto num_initial_headers = build_headers.size();

  if (req->dr) {
    if (!config.no_content_length) {
      build_headers.emplace_back("content-length",
                                 util::utos(as_unsigned(req->data_length)));
    }
    if (config.expect_continue) {
      expect_continue = true;
      build_headers.emplace_back("expect", "100-continue");
    }
  }

  for (auto &kv : headers) {
    size_t i;
    for (i = 0; i < num_initial_headers; ++i) {
      if (kv.name == build_headers[i].name) {
        build_headers[i].value = kv.value;
        break;
      }
    }
    if (i < num_initial_headers) {
      continue;
    }

    build_headers.emplace_back(kv.name, kv.value, kv.never_index);
  }

  std::ranges::stable_partition(
    build_headers, [](auto &&nv) { return nv.name.starts_with(':'); });

  auto nva = std::vector<nghttp2_nv>();
  nva.reserve(build_headers.size());

  for (auto &kv : build_headers) {
    nva.push_back(http2::make_field_nv(kv.name, kv.value,
                                       http2::never_index(kv.never_index)));
  }

  auto method = http2::get_header(build_headers, ":method"sv);
  assert(method);

  req->method = method->value;

  std::string trailer_names;
  if (!config.trailer.empty()) {
    trailer_names = config.trailer[0].name;

    for (size_t i = 1; i < config.trailer.size(); ++i) {
      trailer_names += ", ";
      trailer_names += config.trailer[i].name;
    }

    nva.push_back(http2::make_field_v("trailer"sv, trailer_names));
  }

  static constexpr auto continue_dr = nghttp2_data_reader{
    .read_data = [](nghttp2_conn *conn, int64_t stream_id, nghttp2_vec *vec,
                    size_t veccnt, uint32_t *pflags, void *conn_user_data,
                    void *stream_user_data) -> nghttp2_ssize {
      auto req = static_cast<Request *>(stream_user_data);

      if (!req->unblock_continue) {
        return NGHTTP2_ERR_WOULDBLOCK;
      }

      return req->dr->read_data(conn, stream_id, vec, veccnt, pflags,
                                conn_user_data, stream_user_data);
    },
  };

  auto stream_id =
    nghttp2_conn_submit_request(client->conn, nva.data(), nva.size(),
                                expect_continue ? &continue_dr : req->dr, req);
  if (stream_id < 0) {
    std::println(stderr,
                 "[ERROR] nghttp2_conn_submit_request() returned error: {}",
                 nghttp2_strerror(static_cast<int>(stream_id)));

    return std::unexpected{Error::HTTP2};
  }

  if (config.verbose) {
    print_http_request_headers(stream_id, nva);
  }

  req->stream_id = stream_id;
  req->record_request_start_time();
  client->request_done(req);

  req->req_nva = std::move(build_headers);

  if (expect_continue) {
    req->continue_timer = std::make_unique<ContinueTimer>(client->loop, req);
    req->continue_timer->start();
  }

  return {};
}
} // namespace

namespace {
void readcb(struct ev_loop *loop, ev_io *w, int revents) {
  auto client = static_cast<HttpClient *>(w->data);
  if (!client->do_read()) {
    client->disconnect();
  }
}
} // namespace

namespace {
void writecb(struct ev_loop *loop, ev_io *w, int revents) {
  auto client = static_cast<HttpClient *>(w->data);
  auto rv = client->do_write();
  if (!rv) {
    if (rv.error() == Error::CONNECT_FAIL) {
      client->connect_fail();
      return;
    }

    client->disconnect();
  }
}
} // namespace

namespace {
void timeoutcb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto client = static_cast<HttpClient *>(w->data);
  std::println(stderr, "[ERROR] Timeout");
  client->disconnect();
}
} // namespace

HttpClient::HttpClient(struct ev_loop *loop, SSL_CTX *ssl_ctx)
  : loop(loop), ssl_ctx(ssl_ctx) {
  ev_io_init(&wev, writecb, 0, EV_WRITE);
  ev_io_init(&rev, readcb, 0, EV_READ);

  wev.data = this;
  rev.data = this;

  ev_timer_init(&wt, timeoutcb, 0., config.timeout);
  ev_timer_init(&rt, timeoutcb, 0., config.timeout);

  wt.data = this;
  rt.data = this;

  ev_timer_init(
    &http2_timer,
    [](struct ev_loop *loop, ev_timer *w, int revents) {
      auto client = static_cast<HttpClient *>(w->data);

      if (auto rv = nghttp2_conn_handle_expiry(client->conn, util::timestamp());
          rv != 0) {
        std::println(stderr,
                     "[ERROR] nghttp2_conn_handle_expiry() returned error: {}",
                     nghttp2_strerror(rv));

        nghttp2_conn_terminate(client->conn,
                               nghttp2_err_infer_http2_error_code(rv));
      }

      client->signal_write();
    },
    0., 0.);
  http2_timer.data = this;
}

HttpClient::~HttpClient() {
  disconnect();

  if (addrs) {
    freeaddrinfo(addrs);
    addrs = nullptr;
    next_addr = nullptr;
  }
}

std::expected<void, Error> HttpClient::resolve_host(const std::string &host,
                                                    uint16_t port) {
  int rv;
  this->host = host;
  addrinfo hints{
    .ai_flags = AI_ADDRCONFIG,
    .ai_family = AF_UNSPEC,
    .ai_socktype = SOCK_STREAM,
  };
  rv = getaddrinfo(host.c_str(), util::utos(port).c_str(), &hints, &addrs);
  if (rv != 0) {
    std::println(stderr, "[ERROR] getaddrinfo() failed: {}", gai_strerror(rv));
    return std::unexpected{Error::LIBC};
  }
  if (addrs == nullptr) {
    std::println(stderr, "[ERROR] No address returned");
    return std::unexpected{Error::INTERNAL};
  }
  next_addr = addrs;
  return {};
}

namespace {
// Just returns 1 to continue handshake.
int verify_cb(int preverify_ok, X509_STORE_CTX *ctx) { return 1; }
} // namespace

std::expected<void, Error> HttpClient::initiate_connection() {
  int rv;

  cur_addr = nullptr;
  while (next_addr) {
    cur_addr = next_addr;
    next_addr = next_addr->ai_next;
    auto maybe_fd = util::create_nonblock_socket(cur_addr->ai_family);
    if (!maybe_fd) {
      continue;
    }

    fd = *maybe_fd;

    if (ssl_ctx) {
      // We are establishing TLS connection.
      ssl = SSL_new(ssl_ctx);
      if (!ssl) {
        std::println(stderr, "[ERROR] SSL_new() failed: {}",
                     ERR_error_string(ERR_get_error(), nullptr));
        return std::unexpected{Error::CRYPTO};
      }

      SSL_set_connect_state(ssl);

      // If the user overrode the :authority or host header, use that
      // value for the SNI extension
      const auto &host_string =
        config.host_override.empty() ? host : config.host_override;

      auto param = SSL_get0_param(ssl);
      X509_VERIFY_PARAM_set_hostflags(param, 0);
      X509_VERIFY_PARAM_set1_host(
        param, host_string.c_str(),
        static_cast<nghttp2_ssl_verify_host_length_type>(host_string.size()));
      SSL_set_verify(ssl, SSL_VERIFY_PEER, verify_cb);

      if (!util::numeric_host(host_string.c_str())) {
        SSL_set_tlsext_host_name(ssl, host_string.c_str());
      }
    }

    rv = connect(fd, cur_addr->ai_addr, cur_addr->ai_addrlen);

    if (rv != 0 && errno != EINPROGRESS) {
      if (ssl) {
        SSL_free(ssl);
        ssl = nullptr;
      }
      close(fd);
      fd = -1;
      continue;
    }
    break;
  }

  if (fd == -1) {
    return std::unexpected{Error::SYSCALL};
  }

  writefn = &HttpClient::connected;

  on_readfn = &HttpClient::on_read;
  on_writefn = &HttpClient::on_write;

  ev_io_set(&rev, fd, EV_READ);
  ev_io_set(&wev, fd, EV_WRITE);

  ev_io_start(loop, &wev);

  ev_timer_again(loop, &wt);

  return {};
}

void HttpClient::disconnect() {
  state = ClientState::IDLE;

  for (auto &req : reqvec) {
    if (req->continue_timer) {
      req->continue_timer->stop();
    }
  }

  ev_timer_stop(loop, &http2_timer);
  ev_timer_stop(loop, &rt);
  ev_timer_stop(loop, &wt);

  ev_io_stop(loop, &rev);
  ev_io_stop(loop, &wev);

  nghttp2_conn_del(conn);
  conn = nullptr;

  if (ssl) {
    SSL_set_shutdown(ssl, SSL_get_shutdown(ssl) | SSL_RECEIVED_SHUTDOWN);
    ERR_clear_error();
    SSL_shutdown(ssl);
    SSL_free(ssl);
    ssl = nullptr;
  }

  if (fd != -1) {
    shutdown(fd, SHUT_WR);
    close(fd);
    fd = -1;
  }
}

std::expected<void, Error> HttpClient::read_clear() {
  ev_timer_again(loop, &rt);

  std::array<uint8_t, 16_k> buf;

  for (;;) {
    ssize_t nread;
    while ((nread = read(fd, buf.data(), buf.size())) == -1 && errno == EINTR)
      ;
    if (nread == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return {};
      }
      return std::unexpected{Error::SYSCALL};
    }

    if (nread == 0) {
      return std::unexpected{Error::RECV_EOF};
    }

    if (auto rv = on_readfn(*this, std::span{buf}.first(as_unsigned(nread)));
        !rv) {
      return rv;
    }
  }

  return {};
}

std::expected<void, Error> HttpClient::write_clear() {
  ev_timer_again(loop, &rt);

  for (;;) {
    if (auto rv = on_writefn(*this); !rv) {
      return rv;
    }

    if (tx.data.empty()) {
      break;
    }

    ssize_t nwrite;
    while ((nwrite = write(fd, tx.data.data(), tx.data.size())) == -1 &&
           errno == EINTR)
      ;
    if (nwrite == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        ev_io_start(loop, &wev);
        ev_timer_again(loop, &wt);
        return {};
      }
      return std::unexpected{Error::SYSCALL};
    }

    tx.data = tx.data.subspan(as_unsigned(nwrite));
  }

  ev_io_stop(loop, &wev);
  ev_timer_stop(loop, &wt);

  return {};
}

void HttpClient::connect_fail() {
  if (state == ClientState::IDLE) {
    std::println(stderr, "[ERROR] Could not connect to the address {}",
                 util::numeric_name(cur_addr->ai_addr, cur_addr->ai_addrlen));
  }
  auto cur_state = state;
  disconnect();
  if (cur_state == ClientState::IDLE) {
    if (initiate_connection()) {
      std::println(stderr, "Trying next address {}",
                   util::numeric_name(cur_addr->ai_addr, cur_addr->ai_addrlen));
    }
  }
}

std::expected<void, Error> HttpClient::connected() {
  if (!util::check_socket_connected(fd)) {
    return std::unexpected{Error::CONNECT_FAIL};
  }

  if (config.verbose) {
    std::println("Connected");
  }

  state = ClientState::CONNECTED;

  ev_io_start(loop, &rev);
  ev_io_stop(loop, &wev);

  ev_timer_again(loop, &rt);
  ev_timer_stop(loop, &wt);

  if (ssl) {
    SSL_set_fd(ssl, fd);

    readfn = &HttpClient::tls_handshake;
    writefn = &HttpClient::tls_handshake;

    return do_write();
  }

  readfn = &HttpClient::read_clear;
  writefn = &HttpClient::write_clear;

  return connection_made();
}

std::expected<void, Error> HttpClient::do_read() { return readfn(*this); }
std::expected<void, Error> HttpClient::do_write() {
  auto rv = writefn(*this);
  if (!rv) {
    return rv;
  }

  reset_http2_timer();

  return {};
}

void HttpClient::reset_http2_timer() {
  if (!conn) {
    return;
  }

  auto expiry = nghttp2_conn_get_expiry(conn);
  if (expiry == UINT64_MAX) {
    if (ev_is_active(&http2_timer)) {
      ev_timer_stop(loop, &http2_timer);
    }

    return;
  }

  auto now = util::timestamp();

  if (expiry <= now) {
    ev_feed_event(loop, &http2_timer, EV_TIMER);

    return;
  }

  auto t = static_cast<ev_tstamp>(expiry - now) / NGHTTP2_SECONDS;

  http2_timer.repeat = t;
  ev_timer_again(loop, &http2_timer);
}

namespace {
void check_response_header(nghttp2_conn *conn, Request *req) {
  auto gzip = false;
  auto status_hd = req->get_res_header(NGHTTP2_HPACK_TOKEN__STATUS);

  assert(status_hd);

  // libnghttp2 guarantees that http2::parse_http_status_code
  // succeeds.
  auto status = *http2::parse_http_status_code(status_hd->value);
  req->status = status;

  for (auto &nv : req->res_nva) {
    if ("content-encoding" == nv.name) {
      gzip =
        util::strieq("gzip"sv, nv.value) || util::strieq("deflate"sv, nv.value);
      continue;
    }
  }

  if (req->status / 100 == 1) {
    if (req->continue_timer && (req->status == 100)) {
      req->continue_timer->dispatch_continue();
    }

    req->status = 0;
    req->res_nva.clear();
    http2::init_hdidx(req->res_hdidx);
    return;
  } else if (req->continue_timer) {
    // A final response stops any pending Expect/Continue handshake.
    req->continue_timer->stop();
  }

  req->expect_final_response = false;

  if (gzip) {
    if (!req->inflater) {
      req->init_inflater();
    }
  }
  if (config.get_assets && req->level == 0) {
    if (!req->html_parser) {
      req->init_html_parser();
    }
  }
}
} // namespace

namespace {
HttpClient *get_client(void *user_data) {
  return static_cast<HttpClient *>(user_data);
}
} // namespace

namespace {
int begin_headers(nghttp2_conn *conn, int64_t stream_id, void *conn_user_data,
                  void *stream_user_data) {
  auto req = static_cast<Request *>(stream_user_data);
  if (!req) {
    return 0;
  }

  if (config.verbose) {
    print_http_begin_response_headers(stream_id);
  }

  req->record_response_start_time();

  return 0;
}
} // namespace

namespace {
int recv_header(nghttp2_conn *conn, int64_t stream_id, int32_t token,
                nghttp2_rcbuf *name, nghttp2_rcbuf *value, uint8_t flags,
                void *conn_user_data, void *stream_user_data) {
  auto req = static_cast<Request *>(stream_user_data);
  if (!req) {
    return 0;
  }

  if (config.verbose) {
    print_http_header(stream_id, name, value, flags);
  }

  auto namebuf = nghttp2_rcbuf_get_buf(name);
  auto valuebuf = nghttp2_rcbuf_get_buf(value);

  if (req->header_buffer_size + namebuf.len + valuebuf.len > 64_k) {
    nghttp2_conn_shutdown_stream(conn, 0x00, stream_id, NGHTTP2_INTERNAL_ERROR);
    return 0;
  }

  req->header_buffer_size += namebuf.len + valuebuf.len;

  auto nameref = as_string_view(namebuf.base, namebuf.len);
  auto valueref = as_string_view(valuebuf.base, valuebuf.len);

  http2::index_header(req->res_hdidx, token, req->res_nva.size());
  http2::add_header(req->res_nva, nameref, valueref,
                    flags & NGHTTP2_NV_FLAG_NEVER_INDEX, token);

  return 0;
}
} // namespace

namespace {
int end_headers(nghttp2_conn *conn, int64_t stream_id, int fin,
                void *conn_user_data, void *stream_user_data) {
  auto req = static_cast<Request *>(stream_user_data);
  if (!req) {
    return 0;
  }

  if (config.verbose) {
    print_http_end_headers(stream_id);
  }

  if (req->expect_final_response) {
    check_response_header(conn, req);
  }

  return 0;
}
} // namespace

namespace {
void update_html_parser(HttpClient *client, Request *req,
                        std::span<const uint8_t> data, int fin) {
  if (!req->html_parser) {
    return;
  }
  (void)req->update_html_parser(data, fin);

  auto scheme = req->get_real_scheme();
  auto host = req->get_real_host();
  auto port = req->get_real_port();

  for (auto &p : req->html_parser->get_links()) {
    auto uri = strip_fragment(p.first.c_str());
    auto res_type = p.second;

    urlparse_url u;
    if (urlparse_parse_url(uri.c_str(), uri.size(), 0, &u) != 0) {
      continue;
    }

    if (!util::fieldeq(uri.c_str(), u, URLPARSE_SCHEMA, scheme) ||
        !util::fieldeq(uri.c_str(), u, URLPARSE_HOST, host)) {
      continue;
    }

    auto link_port = util::has_uri_field(u, URLPARSE_PORT) ? u.port
                     : scheme == "https"sv                 ? 443
                                                           : 80;

    if (port != link_port) {
      continue;
    }

    // No POST data for assets
    auto pri = resolve_pri(res_type);

    if (client->add_request(uri, nullptr, 0, pri, req->level + 1)) {
      (void)submit_request(client, config.headers, client->reqvec.back().get());
    }
  }
  req->html_parser->clear_links();
}
} // namespace

namespace {
int recv_data(nghttp2_conn *conn, int64_t stream_id, const uint8_t *data,
              size_t datalen, void *conn_user_data, void *stream_user_data) {
  auto client = get_client(conn_user_data);
  auto req = static_cast<Request *>(stream_user_data);
  if (!req) {
    return 0;
  }

  if (auto rv = nghttp2_conn_extend_max_stream_offset(conn, stream_id, datalen);
      rv != 0 && nghttp2_err_is_fatal(rv)) {
    return NGHTTP2_ERR_CALLBACK_FAILURE;
  }

  if (auto rv = nghttp2_conn_extend_max_offset(conn, datalen);
      rv != 0 && nghttp2_err_is_fatal(rv)) {
    return NGHTTP2_ERR_CALLBACK_FAILURE;
  }

  req->response_len += datalen;

  auto chunk = std::span{data, datalen};

  if (req->inflater) {
    constexpr size_t MAX_OUTLEN = 4_k;
    std::array<uint8_t, MAX_OUTLEN> rawout;

    while (!chunk.empty()) {
      auto out = std::span<uint8_t>{rawout};
      auto outlen = out.size();
      auto tlen = chunk.size();
      auto rv = nghttp2_gzip_inflate(req->inflater, out.data(), &outlen,
                                     chunk.data(), &tlen);
      if (rv != 0) {
        nghttp2_conn_shutdown_stream(conn, 0x00, stream_id,
                                     NGHTTP2_INTERNAL_ERROR);
        break;
      }

      out = out.first(outlen);

      if (!config.null_out) {
        fwrite(out.data(), 1, out.size(), stdout);
      }

      update_html_parser(client, req, out, 0);
      chunk = chunk.subspan(tlen);
    }

    return 0;
  }

  if (!config.null_out) {
    fwrite(chunk.data(), 1, chunk.size(), stdout);
  }

  update_html_parser(client, req, chunk, 0);

  return 0;
}
} // namespace

namespace {
int remote_end_stream(nghttp2_conn *conn, int64_t stream_id,
                      void *conn_user_data, void *stream_user_data) {
  auto client = get_client(conn_user_data);
  auto req = static_cast<Request *>(stream_user_data);
  if (!req) {
    return 0;
  }

  req->record_response_end_time();
  ++client->success;

  return 0;
}
} // namespace

namespace {
int stream_close(nghttp2_conn *conn, uint32_t flags, int64_t stream_id,
                 uint32_t error_code, void *conn_user_data,
                 void *stream_user_data) {
  auto client = get_client(conn_user_data);
  auto req = static_cast<Request *>(stream_user_data);
  if (!req) {
    return 0;
  }

  // If this request is using Expect/Continue, stop its ContinueTimer.
  if (req->continue_timer) {
    req->continue_timer->stop();
  }

  update_html_parser(client, req, {}, 1);
  ++client->complete;

  if (client->all_requests_processed()) {
    nghttp2_conn_terminate(conn, NGHTTP2_NO_ERROR);
  }

  return 0;
}
} // namespace

std::expected<void, Error> HttpClient::connection_made() {
  record_connect_end_time();

  if (ssl) {
    // Check ALPN result
    const unsigned char *next_proto = nullptr;
    unsigned int next_proto_len;

    SSL_get0_alpn_selected(ssl, &next_proto, &next_proto_len);
    if (next_proto) {
      auto proto = as_string_view(next_proto, next_proto_len);
      if (config.verbose) {
        std::println("The negotiated protocol: {}", proto);
      }
      if (!util::check_h2_is_selected(proto)) {
        next_proto = nullptr;
      }
    }
    if (!next_proto) {
      print_protocol_nego_error();
      return std::unexpected{Error::ALPN};
    }
  }

  static constexpr auto callbacks = nghttp2_callbacks{
    .rand = util::secure_random,
    .stream_close = stream_close,
    .begin_headers = begin_headers,
    .recv_header = recv_header,
    .end_headers = end_headers,
    .recv_data = recv_data,
    .remote_end_stream = remote_end_stream,
  };

  nghttp2_settings settings;
  nghttp2_settings_default(&settings);

  if (config.verbose) {
    settings.log_write = log_write;
  }

  util::secure_random(reinterpret_cast<uint8_t *>(&settings.conn_id),
                      sizeof(settings.conn_id));
  settings.initial_ts = util::timestamp();
  settings.max_concurrent_streams_local = config.peer_max_concurrent_streams;

  if (config.window_bits != -1) {
    settings.initial_max_stream_data = (1UZ << config.window_bits) - 1;
  }

  if (config.connection_window_bits != -1) {
    settings.initial_max_data = (1UZ << config.connection_window_bits) - 1;
  }

  if (config.header_table_size != -1) {
    settings.hpack_max_dtable_capacity =
      static_cast<size_t>(config.header_table_size);
  }

  if (config.encoder_header_table_size != -1) {
    settings.hpack_encoder_max_dtable_capacity =
      static_cast<size_t>(config.encoder_header_table_size);
  }

  if (auto rv =
        nghttp2_conn_client_new(&conn, &callbacks, &settings, nullptr, this);
      rv != 0) {
    return std::unexpected{Error::HTTP2};
  }

  for (auto &req : reqvec) {
    if (auto rv = submit_request(this, config.headers, req.get()); !rv) {
      return rv;
    }
  }

  signal_write();

  return {};
}

std::expected<void, Error> HttpClient::on_read(std::span<const uint8_t> data) {
  if (config.hexdump) {
    (void)util::hexdump(stdout, data.data(), data.size());
  }

  auto rv =
    nghttp2_conn_read(conn, data.data(), data.size(), util::timestamp());
  if (rv != 0) {
    std::println(stderr, "[ERROR] nghttp2_conn_read() returned error: {}",
                 nghttp2_strerror(rv));

    return std::unexpected{Error::HTTP2};
  }

  signal_write();

  return {};
}

std::expected<void, Error> HttpClient::on_write() {
  if (!tx.data.empty()) {
    return {};
  }

  auto nwrite =
    nghttp2_conn_write(conn, txbuf.data(), txbuf.size(), util::timestamp());
  if (nwrite < 0) {
    if (nwrite == NGHTTP2_ERR_CLOSING) {
      return std::unexpected{Error::DONE};
    }

    std::println(stderr, "[ERROR] nghttp2_conn_write() returned error: {}",
                 nghttp2_strerror(static_cast<int>(nwrite)));

    return std::unexpected{Error::HTTP2};
  }

  tx.data = std::span{txbuf}.first(as_unsigned(nwrite));

  return {};
}

std::expected<void, Error> HttpClient::tls_handshake() {
  ev_timer_again(loop, &rt);

  ERR_clear_error();

  auto rv = SSL_do_handshake(ssl);

  if (rv <= 0) {
    auto err = SSL_get_error(ssl, rv);
    switch (err) {
    case SSL_ERROR_WANT_READ:
      ev_io_stop(loop, &wev);
      ev_timer_stop(loop, &wt);
      return {};
    case SSL_ERROR_WANT_WRITE:
      ev_io_start(loop, &wev);
      ev_timer_again(loop, &wt);
      return {};
    default:
      return std::unexpected{Error::CRYPTO};
    }
  }

  ev_io_stop(loop, &wev);
  ev_timer_stop(loop, &wt);

  readfn = &HttpClient::read_tls;
  writefn = &HttpClient::write_tls;

  if (config.verify_peer) {
    auto verify_res = SSL_get_verify_result(ssl);
    if (verify_res != X509_V_OK) {
      std::println(stderr, "[WARNING] Certificate verification failed: {}",
                   X509_verify_cert_error_string(verify_res));
    }
  }

  return connection_made();
}

std::expected<void, Error> HttpClient::read_tls() {
  ev_timer_again(loop, &rt);

  ERR_clear_error();

  std::array<uint8_t, 16_k> buf;

  for (;;) {
    auto nread = SSL_read(ssl, buf.data(), static_cast<int>(buf.size()));
    if (nread <= 0) {
      auto err = SSL_get_error(ssl, nread);
      switch (err) {
      case SSL_ERROR_WANT_READ:
        return {};
      case SSL_ERROR_WANT_WRITE:
        // renegotiation started
      default:
        return std::unexpected{Error::CRYPTO};
      }
    }

    if (auto rv =
          on_readfn(*this, std::span{buf}.first(static_cast<size_t>(nread)));
        !rv) {
      return rv;
    }
  }
}

std::expected<void, Error> HttpClient::write_tls() {
  ev_timer_again(loop, &rt);

  ERR_clear_error();

  for (;;) {
    if (auto rv = on_writefn(*this); !rv) {
      return rv;
    }

    if (tx.data.empty()) {
      break;
    }

    auto nwrite =
      SSL_write(ssl, tx.data.data(), static_cast<int>(tx.data.size()));
    if (nwrite <= 0) {
      auto err = SSL_get_error(ssl, nwrite);
      switch (err) {
      case SSL_ERROR_WANT_WRITE:
        ev_io_start(loop, &wev);
        ev_timer_again(loop, &wt);
        return {};
      case SSL_ERROR_WANT_READ:
        // renegotiation started
      default:
        return std::unexpected{Error::CRYPTO};
      }
    }

    tx.data = tx.data.subspan(as_unsigned(nwrite));
  }

  ev_io_stop(loop, &wev);
  ev_timer_stop(loop, &wt);

  return {};
}

void HttpClient::signal_write() { ev_io_start(loop, &wev); }

bool HttpClient::all_requests_processed() const {
  return complete == reqvec.size();
}

void HttpClient::update_hostport() {
  if (reqvec.empty()) {
    return;
  }
  scheme =
    util::get_uri_field(reqvec[0]->uri.c_str(), reqvec[0]->u, URLPARSE_SCHEMA);
  auto host =
    util::get_uri_field(reqvec[0]->uri.c_str(), reqvec[0]->u, URLPARSE_HOST);
  if (reqvec[0]->is_ipv6_literal_addr()) {
    // we may have zone ID, which must start with "%25", or "%".  RFC
    // 6874 defines "%25" only, and just "%" is allowed for just
    // convenience to end-user input.
    auto end = std::ranges::find(host, '%');
    hostport = '[';
    hostport.append(std::ranges::begin(host), end);
    hostport += ']';
  } else {
    hostport = host;
  }
  if (util::has_uri_field(reqvec[0]->u, URLPARSE_PORT) &&
      reqvec[0]->u.port !=
        util::get_default_port(reqvec[0]->uri.c_str(), reqvec[0]->u)) {
    hostport += ':';
    hostport +=
      util::get_uri_field(reqvec[0]->uri.c_str(), reqvec[0]->u, URLPARSE_PORT);
  }
}

bool HttpClient::add_request(const std::string &uri,
                             const nghttp2_data_reader *dr, int64_t data_length,
                             const nghttp2_pri &pri, int level) {
  urlparse_url u;
  if (urlparse_parse_url(uri.c_str(), uri.size(), 0, &u) != 0) {
    return false;
  }
  if (path_cache.contains(uri)) {
    return false;
  }

  if (config.multiply == 1) {
    path_cache.insert(uri);
  }

  reqvec.push_back(
    std::make_unique<Request>(uri, u, dr, data_length, pri, level));
  return true;
}

void HttpClient::record_start_time() {
  timing.system_start_time = std::chrono::system_clock::now();
  timing.start_time = get_time();
}

void HttpClient::record_domain_lookup_end_time() {
  timing.domain_lookup_end_time = get_time();
}

void HttpClient::record_connect_end_time() {
  timing.connect_end_time = get_time();
}

void HttpClient::request_done(Request *req) {
  if (req->stream_id % 2 == 0) {
    return;
  }
}

#ifdef HAVE_JANSSON
void HttpClient::output_har(FILE *outfile) {
  static auto PAGE_ID = "page_0";

  auto root = json_object();
  auto log = json_object();
  json_object_set_new(root, "log", log);
  json_object_set_new(log, "version", json_string("1.2"));

  auto creator = json_object();
  json_object_set_new(log, "creator", creator);

  json_object_set_new(creator, "name", json_string("nghttp"));
  json_object_set_new(creator, "version", json_string(NGHTTP2_VERSION));

  auto pages = json_array();
  json_object_set_new(log, "pages", pages);

  auto page = json_object();
  json_array_append_new(pages, page);

  json_object_set_new(
    page, "startedDateTime",
    json_string(util::format_iso8601(timing.system_start_time).c_str()));
  json_object_set_new(page, "id", json_string(PAGE_ID));
  json_object_set_new(page, "title", json_string(""));

  json_object_set_new(page, "pageTimings", json_object());

  auto entries = json_array();
  json_object_set_new(log, "entries", entries);

  auto dns_delta =
    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                          timing.domain_lookup_end_time - timing.start_time)
                          .count()) /
    1000.0;
  auto connect_delta =
    static_cast<double>(
      std::chrono::duration_cast<std::chrono::microseconds>(
        timing.connect_end_time - timing.domain_lookup_end_time)
        .count()) /
    1000.0;

  for (size_t i = 0; i < reqvec.size(); ++i) {
    auto &req = reqvec[i];

    if (req->timing.state != RequestState::ON_COMPLETE) {
      continue;
    }

    auto entry = json_object();
    json_array_append_new(entries, entry);

    auto &req_timing = req->timing;
    auto request_time =
      (i == 0)
        ? timing.system_start_time
        : timing.system_start_time +
            std::chrono::duration_cast<std::chrono::system_clock::duration>(
              req_timing.request_start_time - timing.start_time);

    auto wait_delta =
      static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(
          req_timing.response_start_time - req_timing.request_start_time)
          .count()) /
      1000.0;
    auto receive_delta =
      static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(
          req_timing.response_end_time - req_timing.response_start_time)
          .count()) /
      1000.0;

    auto time_sum =
      static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(
          (i == 0)
            ? (req_timing.response_end_time - timing.start_time)
            : (req_timing.response_end_time - req_timing.request_start_time))
          .count()) /
      1000.0;

    json_object_set_new(
      entry, "startedDateTime",
      json_string(util::format_iso8601(request_time).c_str()));
    json_object_set_new(entry, "time", json_real(time_sum));

    auto pushed = req->stream_id % 2 == 0;

    json_object_set_new(entry, "comment",
                        json_string(pushed ? "Pushed Object" : ""));

    auto request = json_object();
    json_object_set_new(entry, "request", request);

    auto req_headers = json_array();
    json_object_set_new(request, "headers", req_headers);

    for (auto &nv : req->req_nva) {
      auto hd = json_object();
      json_array_append_new(req_headers, hd);

      json_object_set_new(hd, "name", json_string(nv.name.c_str()));
      json_object_set_new(hd, "value", json_string(nv.value.c_str()));
    }

    json_object_set_new(request, "method", json_string(req->method.c_str()));
    json_object_set_new(request, "url", json_string(req->uri.c_str()));
    json_object_set_new(request, "httpVersion", json_string("HTTP/2.0"));
    json_object_set_new(request, "cookies", json_array());
    json_object_set_new(request, "queryString", json_array());
    json_object_set_new(request, "headersSize", json_integer(-1));
    json_object_set_new(request, "bodySize", json_integer(-1));

    auto response = json_object();
    json_object_set_new(entry, "response", response);

    auto res_headers = json_array();
    json_object_set_new(response, "headers", res_headers);

    for (auto &nv : req->res_nva) {
      auto hd = json_object();
      json_array_append_new(res_headers, hd);

      json_object_set_new(hd, "name", json_string(nv.name.c_str()));
      json_object_set_new(hd, "value", json_string(nv.value.c_str()));
    }

    json_object_set_new(response, "status", json_integer(req->status));
    json_object_set_new(response, "statusText", json_string(""));
    json_object_set_new(response, "httpVersion", json_string("HTTP/2.0"));
    json_object_set_new(response, "cookies", json_array());

    auto content = json_object();
    json_object_set_new(response, "content", content);

    json_object_set_new(content, "size", json_integer(req->response_len));

    auto content_type_ptr = http2::get_header(req->res_nva, "content-type"sv);

    const char *content_type = "";
    if (content_type_ptr) {
      content_type = content_type_ptr->value.c_str();
    }

    json_object_set_new(content, "mimeType", json_string(content_type));

    json_object_set_new(response, "redirectURL", json_string(""));
    json_object_set_new(response, "headersSize", json_integer(-1));
    json_object_set_new(response, "bodySize", json_integer(-1));
    json_object_set_new(entry, "cache", json_object());

    auto timings = json_object();
    json_object_set_new(entry, "timings", timings);

    auto dns_timing = (i == 0) ? dns_delta : 0;
    auto connect_timing = (i == 0) ? connect_delta : 0;

    json_object_set_new(timings, "dns", json_real(dns_timing));
    json_object_set_new(timings, "connect", json_real(connect_timing));

    json_object_set_new(timings, "blocked", json_real(0.0));
    json_object_set_new(timings, "send", json_real(0.0));
    json_object_set_new(timings, "wait", json_real(wait_delta));
    json_object_set_new(timings, "receive", json_real(receive_delta));

    json_object_set_new(entry, "pageref", json_string(PAGE_ID));
    json_object_set_new(
      entry, "connection",
      json_string(util::utos(as_unsigned(req->stream_id)).c_str()));
  }

  json_dumpf(root, outfile, JSON_PRESERVE_ORDER | JSON_INDENT(2));
  json_decref(root);
}
#endif // defined(HAVE_JANSSON)

struct RequestResult {
  std::chrono::microseconds time;
};

namespace {
void print_stats(const HttpClient &client) {
  std::println("***** Statistics *****");

  std::vector<Request *> reqs;
  reqs.reserve(client.reqvec.size());
  for (const auto &req : client.reqvec) {
    if (req->timing.state == RequestState::ON_COMPLETE) {
      reqs.push_back(req.get());
    }
  }

  std::ranges::sort(reqs, [](const Request *lhs, const Request *rhs) {
    const auto &ltiming = lhs->timing;
    const auto &rtiming = rhs->timing;
    return ltiming.response_end_time < rtiming.response_end_time ||
           (ltiming.response_end_time == rtiming.response_end_time &&
            ltiming.request_start_time < rtiming.request_start_time);
  });

  std::println(R"(
Request timing:
  responseEnd: the  time  when  last  byte of  response  was  received
               relative to connectEnd
 requestStart: the time  just before  first byte  of request  was sent
               relative  to connectEnd.
      process: responseEnd - requestStart
         code: HTTP status code
         size: number  of  bytes  received as  response  body  without
               inflation.
          URI: request URI

see http://www.w3.org/TR/resource-timing/#processing-model

sorted by 'complete'

id  responseEnd requestStart  process code size request path)");

  const auto &base = client.timing.connect_end_time;
  for (const auto &req : reqs) {
    auto response_end = std::chrono::duration_cast<std::chrono::microseconds>(
      req->timing.response_end_time - base);
    auto request_start = std::chrono::duration_cast<std::chrono::microseconds>(
      req->timing.request_start_time - base);
    auto total = std::chrono::duration_cast<std::chrono::microseconds>(
      req->timing.response_end_time - req->timing.request_start_time);

    std::println("{:3} {:>11}  {:>11} {:>8} {:4} {:>4} {}", req->stream_id,
                 "+" + util::format_duration(response_end),
                 "+" + util::format_duration(request_start),
                 util::format_duration(total), req->status,
                 util::utos_unit(as_unsigned(req->response_len)),
                 req->make_reqpath());
  }
}
} // namespace

namespace {
std::expected<void, Error> communicate(
  const std::string &scheme, const std::string &host, uint16_t port,
  std::vector<
    std::tuple<std::string, const nghttp2_data_reader *, int64_t, nghttp2_pri>>
    requests) {
  auto loop = EV_DEFAULT;
  SSL_CTX *ssl_ctx = nullptr;

  auto ssl_ctx_d = defer([&ssl_ctx] {
    if (ssl_ctx) {
      SSL_CTX_free(ssl_ctx);
    }
  });

  if (scheme == "https") {
    ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!ssl_ctx) {
      std::println(stderr, "[ERROR] Failed to create SSL_CTX: {}",
                   ERR_error_string(ERR_get_error(), nullptr));
      return std::unexpected{Error::CRYPTO};
    }

    auto ssl_opts = static_cast<nghttp2_ssl_op_type>(
      (SSL_OP_ALL & ~SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS) | SSL_OP_NO_SSLv2 |
      SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION |
      SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION);

#ifdef SSL_OP_ENABLE_KTLS
    if (config.ktls) {
      ssl_opts |= SSL_OP_ENABLE_KTLS;
    }
#endif // defined(SSL_OP_ENABLE_KTLS)

    SSL_CTX_set_options(ssl_ctx, ssl_opts);
    SSL_CTX_set_mode(ssl_ctx, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_mode(ssl_ctx, SSL_MODE_RELEASE_BUFFERS);

    if (SSL_CTX_set_default_verify_paths(ssl_ctx) != 1) {
      std::println(
        stderr, "[WARNING] Could not load system trusted CA certificates: {}",
        ERR_error_string(ERR_get_error(), nullptr));
    }

    if (auto rv = nghttp2::tls::ssl_ctx_set_proto_versions(
          ssl_ctx, nghttp2::tls::NGHTTP2_TLS_MIN_VERSION,
          nghttp2::tls::NGHTTP2_TLS_MAX_VERSION);
        !rv) {
      std::println(stderr, "[ERROR] Could not set TLS versions");
      return rv;
    }

    if (SSL_CTX_set_cipher_list(ssl_ctx, tls::DEFAULT_CIPHER_LIST.data()) ==
        0) {
      std::println(stderr, "[ERROR] {}",
                   ERR_error_string(ERR_get_error(), nullptr));
      return std::unexpected{Error::CRYPTO};
    }

#ifdef NGHTTP2_OPENSSL_IS_WOLFSSL
    if (SSL_CTX_set_ciphersuites(ssl_ctx,
                                 tls::DEFAULT_TLS13_CIPHER_LIST.data()) == 0) {
      std::println(stderr, "[ERROR] {}",
                   ERR_error_string(ERR_get_error(), nullptr));
      return std::unexpected{Error::CRYPTO};
    }
#endif // defined(NGHTTP2_OPENSSL_IS_WOLFSSL)

    if (!config.keyfile.empty()) {
      if (SSL_CTX_use_PrivateKey_file(ssl_ctx, config.keyfile.c_str(),
                                      SSL_FILETYPE_PEM) != 1) {
        std::println(stderr, "[ERROR] {}",
                     ERR_error_string(ERR_get_error(), nullptr));
        return std::unexpected{Error::CRYPTO};
      }
    }
    if (!config.certfile.empty()) {
      if (SSL_CTX_use_certificate_chain_file(ssl_ctx,
                                             config.certfile.c_str()) != 1) {
        std::println(stderr, "[ERROR] {}",
                     ERR_error_string(ERR_get_error(), nullptr));
        return std::unexpected{Error::CRYPTO};
      }
    }

    SSL_CTX_set_alpn_protos(
      ssl_ctx, reinterpret_cast<const uint8_t *>(NGHTTP2_H2_ALPN.data()),
      NGHTTP2_H2_ALPN.size());

#if defined(NGHTTP2_OPENSSL_IS_BORINGSSL) && defined(HAVE_LIBBROTLI)
    if (!SSL_CTX_add_cert_compression_alg(
          ssl_ctx, nghttp2::tls::CERTIFICATE_COMPRESSION_ALGO_BROTLI,
          nghttp2::tls::cert_compress, nghttp2::tls::cert_decompress)) {
      std::println(stderr, "[ERROR] SSL_CTX_add_cert_compression_alg failed.");
      return std::unexpected{Error::CRYPTO};
    }
#endif // defined(NGHTTP2_OPENSSL_IS_BORINGSSL) &&
       // defined(HAVE_LIBBROTLI)

    if (auto rv = tls::setup_keylog_callback(ssl_ctx); !rv) {
      std::println(stderr, "[ERROR] Failed to setup keylog");

      return rv;
    }
  }
  {
    HttpClient client{loop, ssl_ctx};

    for (auto &req : requests) {
      for (int i = 0; i < config.multiply; ++i) {
        client.add_request(std::get<0>(req), std::get<1>(req), std::get<2>(req),
                           std::get<3>(req));
      }
    }
    client.update_hostport();

    client.record_start_time();

    if (auto rv = client.resolve_host(host, port); !rv) {
      return rv;
    }

    client.record_domain_lookup_end_time();

    if (auto rv = client.initiate_connection(); !rv) {
      std::println(stderr, "[ERROR] Could not connect to {}, port {}", host,
                   port);
      return rv;
    }

    ev_set_userdata(loop, &client);
    ev_run(loop, 0);
    ev_set_userdata(loop, nullptr);

#ifdef HAVE_JANSSON
    if (!config.harfile.empty()) {
      FILE *outfile;
      if (config.harfile == "-") {
        outfile = stdout;
      } else {
        outfile = fopen(config.harfile.c_str(), "wb");
      }

      if (outfile) {
        client.output_har(outfile);

        if (outfile != stdout) {
          fclose(outfile);
        }
      } else {
        std::println(stderr,
                     "Cannot open file {}. har file could not be created.",
                     config.harfile);
      }
    }
#endif // defined(HAVE_JANSSON)

    if (client.success != client.reqvec.size()) {
      std::println(stderr,
                   "Some requests were not processed. total={}, processed={}",
                   client.reqvec.size(), client.success);
    }
    if (config.stat) {
      print_stats(client);
    }
  }

  return {};
}
} // namespace

namespace {
void *data_map;
size_t data_maplen;
} // namespace

namespace {
nghttp2_ssize file_read_data(nghttp2_conn *conn, int64_t stream_id,
                             nghttp2_vec *vec, size_t veccnt, uint32_t *pflags,
                             void *conn_user_data, void *stream_user_data) {
  auto client = get_client(conn_user_data);

  if (!config.trailer.empty()) {
    std::vector<nghttp2_nv> nva;
    nva.reserve(config.trailer.size());

    for (auto &kv : config.trailer) {
      nva.push_back(http2::make_field_nv(kv.name, kv.value,
                                         http2::never_index(kv.never_index)));
    }

    if (auto rv = nghttp2_conn_submit_trailers(client->conn, stream_id,
                                               nva.data(), nva.size());
        rv != 0) {
      std::println(stderr,
                   "[ERROR] nghttp2_conn_submit_trailers() returned error: {}",
                   nghttp2_strerror(rv));

      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    if (config.verbose) {
      print_http_trailers(stream_id, nva);
    }
  }

  *pflags = NGHTTP2_READ_DATA_FLAG_EOF;
  vec[0] = {
    .base = reinterpret_cast<uint8_t *>(data_map),
    .len = data_maplen,
  };

  return 1;
}
} // namespace

namespace {
int run(char **uris, int n) {
  std::string prev_scheme;
  std::string prev_host;
  uint16_t prev_port = 0;
  int failures = 0;
  int data_fd = -1;
  static constexpr auto dr = nghttp2_data_reader{
    .read_data = file_read_data,
  };
  struct stat data_stat;

  if (!config.datafile.empty()) {
    if (config.datafile == "-") {
      // copy the contents of STDIN to a temporary file
      char tempfn[] = "/tmp/nghttp.temp.XXXXXX";
      data_fd = mkstemp(tempfn);
      if (data_fd == -1) {
        std::println(stderr,
                     "[ERROR] Could not create a temporary file in /tmp");
        return 1;
      }
      if (unlink(tempfn) != 0) {
        std::println(stderr, "[WARNING] failed to unlink temporary file: {}",
                     tempfn);
      }
      while (1) {
        std::array<char, 1_k> buf;
        ssize_t rret, wret;
        while ((rret = read(0, buf.data(), buf.size())) == -1 && errno == EINTR)
          ;
        if (rret == 0)
          break;
        if (rret == -1) {
          std::println(stderr, "[ERROR] I/O error while reading from STDIN");
          return 1;
        }
        while ((wret = write(data_fd, buf.data(), as_unsigned(rret))) == -1 &&
               errno == EINTR)
          ;
        if (wret != rret) {
          std::println(stderr,
                       "[ERROR] I/O error while writing to temporary file");
          return 1;
        }
      }
      if (fstat(data_fd, &data_stat) == -1) {
        close(data_fd);
        std::println(stderr, "[ERROR] Could not stat temporary file");
        return 1;
      }
    } else {
      data_fd = open(config.datafile.c_str(), O_RDONLY | O_BINARY);
      if (data_fd == -1) {
        std::println(stderr, "[ERROR] Could not open file {}", config.datafile);
        return 1;
      }
      if (fstat(data_fd, &data_stat) == -1) {
        close(data_fd);
        std::println(stderr, "[ERROR] Could not stat file {}", config.datafile);
        return 1;
      }
    }

    data_map = mmap(nullptr, static_cast<size_t>(data_stat.st_size), PROT_READ,
                    MAP_SHARED, data_fd, 0);
    if (data_map == MAP_FAILED) {
      std::println(stderr, "[ERROR] Could not map file");
      return 1;
    }

    data_maplen = static_cast<size_t>(data_stat.st_size);
  }
  std::vector<
    std::tuple<std::string, const nghttp2_data_reader *, int64_t, nghttp2_pri>>
    requests;

  auto next_pri_idx = 0UZ;

  for (int i = 0; i < n; ++i) {
    urlparse_url u;
    auto uri = strip_fragment(uris[i]);
    if (urlparse_parse_url(uri.c_str(), uri.size(), 0, &u) != 0) {
      ++next_pri_idx;
      std::println(stderr, "[ERROR] Could not parse URI {}", uri);
      continue;
    }
    if (!util::has_uri_field(u, URLPARSE_SCHEMA)) {
      ++next_pri_idx;
      std::println(stderr, "[ERROR] URI {} does not have scheme part", uri);
      continue;
    }
    auto port = util::has_uri_field(u, URLPARSE_PORT)
                  ? u.port
                  : util::get_default_port(uri.c_str(), u);
    auto host = decode_host(util::get_uri_field(uri.c_str(), u, URLPARSE_HOST));
    if (!util::fieldeq(uri.c_str(), u, URLPARSE_SCHEMA, prev_scheme.c_str()) ||
        host != prev_host || port != prev_port) {
      if (!requests.empty()) {
        if (!communicate(prev_scheme, prev_host, prev_port,
                         std::move(requests))) {
          ++failures;
        }
        requests.clear();
      }
      prev_scheme = util::get_uri_field(uri.c_str(), u, URLPARSE_SCHEMA);
      prev_host = std::move(host);
      prev_port = port;
    }
    requests.emplace_back(uri, data_fd == -1 ? nullptr : &dr, data_stat.st_size,
                          config.pris[next_pri_idx++]);
  }
  if (!requests.empty()) {
    if (!communicate(prev_scheme, prev_host, prev_port, std::move(requests))) {
      ++failures;
    }
  }

  if (data_fd != -1) {
    munmap(data_map, data_maplen);
    data_map = nullptr;
    data_maplen = 0;
  }

  return failures;
}
} // namespace

namespace {
void print_version() { std::println("nghttp nghttp2/" NGHTTP2_VERSION); }
} // namespace

namespace {
void print_usage(std::ostream &out) {
  out << R"(Usage: nghttp [OPTIONS]... <URI>...
HTTP/2 client)"
      << std::endl;
}
} // namespace

namespace {
void print_help(std::ostream &out) {
  print_usage(out);
  out << R"(
  <URI>       Specify URI to access.
Options:
  -v, --verbose
              Print   debug   information   such  as   reception   and
              transmission of frames and name/value pairs.  Specifying
              this option multiple times increases verbosity.
  -n, --null-out
              Discard downloaded data.
  -O, --remote-name
              Save  download  data  in  the  current  directory.   The
              filename is  derived from  URI.  If  URI ends  with '/',
              'index.html'  is used  as a  filename.  Not  implemented
              yet.
  -t, --timeout=<DURATION>
              Timeout each request after <DURATION>.  Set 0 to disable
              timeout.
  -w, --window-bits=<N>
              Sets the stream level initial window size to 2**<N>-1.
  -W, --connection-window-bits=<N>
              Sets  the  connection  level   initial  window  size  to
              2**<N>-1.
  -a, --get-assets
              Download assets  such as stylesheets, images  and script
              files linked  from the downloaded resource.   Only links
              whose  origins are  the same  with the  linking resource
              will be downloaded.   nghttp prioritizes resources using
              HTTP/2 dependency  based priority.  The  priority order,
              from highest to lowest,  is html itself, css, javascript
              and images.
  -s, --stat  Print statistics.
  -H, --header=<HEADER>
              Add a header to the requests.  Example: -H':method: PUT'
  --trailer=<HEADER>
              Add a trailer header to the requests.  <HEADER> must not
              include pseudo header field  (header field name starting
              with ':').  To  send trailer, one must use  -d option to
              send request body.  Example: --trailer 'foo: bar'.
  --cert=<CERT>
              Use  the specified  client certificate  file.  The  file
              must be in PEM format.
  --key=<KEY> Use the  client private key  file.  The file must  be in
              PEM format.
  -d, --data=<PATH>
              Post FILE to server. If '-'  is given, data will be read
              from stdin.
  -m, --multiply=<N>
              Request each URI <N> times.  By default, same URI is not
              requested twice.  This option disables it too.
  --extpri=<PRI>
              Sets RFC 9218 priority of  given URI.  <PRI> must be the
              wire format  of priority  header field  (e.g., "u=3,i").
              This  option  can  be  used  multiple  times,  and  N-th
              --extpri option sets priority of N-th URI in the command
              line.  If  the number  of this option  is less  than the
              number of  URI, the last  option value is  repeated.  If
              there  is   no  --extpri  option,  urgency   is  3,  and
              incremental is false.
  -M, --peer-max-concurrent-streams=<N>
              Use  <N>  as  SETTINGS_MAX_CONCURRENT_STREAMS  value  of
              remote endpoint as if it  is received in SETTINGS frame.
              Default: 100
  -c, --header-table-size=<SIZE>
              Specify decoder header table size.
  --encoder-header-table-size=<SIZE>
              Specify encoder header table size.  The decoder (server)
              specifies  the maximum  dynamic table  size it  accepts.
              Then the negotiated dynamic table size is the minimum of
              this option value and the value which server specified.
  -r, --har=<PATH>
              Output HTTP  transactions <PATH> in HAR  format.  If '-'
              is given, data is written to stdout.
  --continuation
              Send large header to test CONTINUATION.
  --no-content-length
              Don't send content-length header field.
  --hexdump   Display the  incoming traffic in  hexadecimal (Canonical
              hex+ASCII display).  If SSL/TLS  is used, decrypted data
              are used.
  --expect-continue
              Perform an Expect/Continue handshake:  wait to send DATA
              (up to  a short  timeout)  until the server sends  a 100
              Continue interim response. This option is ignored unless
              combined with the -d option.
  -y, --no-verify-peer
              Suppress  warning  on  server  certificate  verification
              failure.
  --ktls      Enable ktls.
  --version   Display version information and exit.
  -h, --help  Display this help and exit.

--

  The <SIZE> argument is an integer and an optional unit (e.g., 10K is
  10 * 1024).  Units are K, M and G (powers of 1024).

  The <DURATION> argument is an integer and an optional unit (e.g., 1s
  is 1 second and 500ms is 500 milliseconds).  Units are h, m, s or ms
  (hours, minutes, seconds and milliseconds, respectively).  If a unit
  is omitted, a second is used as unit.)"
      << std::endl;
}
} // namespace

int main(int argc, char **argv) {
  while (1) {
    static int flag = 0;
    constexpr static option long_options[] = {
      {"verbose", no_argument, nullptr, 'v'},
      {"null-out", no_argument, nullptr, 'n'},
      {"remote-name", no_argument, nullptr, 'O'},
      {"timeout", required_argument, nullptr, 't'},
      {"window-bits", required_argument, nullptr, 'w'},
      {"connection-window-bits", required_argument, nullptr, 'W'},
      {"get-assets", no_argument, nullptr, 'a'},
      {"stat", no_argument, nullptr, 's'},
      {"help", no_argument, nullptr, 'h'},
      {"header", required_argument, nullptr, 'H'},
      {"data", required_argument, nullptr, 'd'},
      {"multiply", required_argument, nullptr, 'm'},
      {"weight", required_argument, nullptr, 'p'},
      {"peer-max-concurrent-streams", required_argument, nullptr, 'M'},
      {"header-table-size", required_argument, nullptr, 'c'},
      {"har", required_argument, nullptr, 'r'},
      {"no-verify-peer", no_argument, nullptr, 'y'},
      {"cert", required_argument, &flag, 1},
      {"key", required_argument, &flag, 2},
      {"continuation", no_argument, &flag, 4},
      {"version", no_argument, &flag, 5},
      {"no-content-length", no_argument, &flag, 6},
      {"no-dep", no_argument, &flag, 7},
      {"trailer", required_argument, &flag, 9},
      {"hexdump", no_argument, &flag, 10},
      {"expect-continue", no_argument, &flag, 13},
      {"encoder-header-table-size", required_argument, &flag, 14},
      {"ktls", no_argument, &flag, 15},
      {"no-rfc7540-pri", no_argument, &flag, 16},
      {"extpri", required_argument, &flag, 17},
      {nullptr, 0, nullptr, 0}};
    int option_index = 0;
    int c = getopt_long(argc, argv, "M:Oac:d:m:np:r:hH:vst:w:yW:", long_options,
                        &option_index);
    if (c == -1) {
      break;
    }
    switch (c) {
    case 'M': {
      // peer-max-concurrent-streams option
      auto n = util::parse_uint(optarg);
      if (!n) {
        std::println(stderr, "-M: Bad option value: {}", optarg);
        exit(EXIT_FAILURE);
      }
      config.peer_max_concurrent_streams = static_cast<size_t>(*n);
      break;
    }
    case 'O':
      config.remote_name = true;
      break;
    case 'h':
      print_help(std::cout);
      exit(EXIT_SUCCESS);
    case 'n':
      config.null_out = true;
      break;
    case 'p':
      std::println(stderr, "[WARNING]: --weight option has been deprecated.");
      break;
    case 'r':
#ifdef HAVE_JANSSON
      config.harfile = optarg;
#else  // !defined(HAVE_JANSSON)
      std::println(stderr, "[WARNING]: -r, --har option is ignored because the "
                           "binary was not compiled with libjansson.");
#endif // !defined(HAVE_JANSSON)
      break;
    case 'v':
      ++config.verbose;
      break;
    case 't': {
      auto d = util::parse_duration_with_unit(optarg);
      if (!d) {
        std::println(stderr, "-t: bad timeout value: {}", optarg);
        exit(EXIT_FAILURE);
      }
      config.timeout = *d;
      break;
    }
    case 'w':
    case 'W': {
      auto n = util::parse_uint(optarg);
      if (!n || *n > 30) {
        std::println(stderr,
                     "-{}: specify the integer in the range [0, 30], inclusive",
                     static_cast<char>(c));
        exit(EXIT_FAILURE);
      }
      if (c == 'w') {
        config.window_bits = static_cast<int>(*n);
      } else {
        config.connection_window_bits = static_cast<int>(*n);
      }
      break;
    }
    case 'H': {
      char *header = optarg;
      // Skip first possible ':' in the header name
      auto name_end = strchr(optarg + 1, ':');
      if (!name_end || (header[0] == ':' && header + 1 == name_end)) {
        std::println(stderr, "-H: invalid header: {}", optarg);
        exit(EXIT_FAILURE);
      }
      *name_end = 0;
      auto value = name_end + 1;
      while (isspace(*value)) {
        value++;
      }
      if (*value == 0) {
        // This could also be a valid case for suppressing a header
        // similar to curl
        std::println(stderr, "-H: invalid header - value missing: {}", optarg);
        exit(EXIT_FAILURE);
      }
      util::tolower(header, name_end, header);
      config.headers.emplace_back(header, value, false);
      break;
    }
    case 'a':
#ifdef HAVE_LIBXML2
      config.get_assets = true;
#else  // !defined(HAVE_LIBXML2)
      std::println(stderr, "[WARNING]: -a, --get-assets option is ignored "
                           "because the binary was not compiled with libxml2.");
#endif // !defined(HAVE_LIBXML2)
      break;
    case 's':
      config.stat = true;
      break;
    case 'd':
      config.datafile = optarg;
      break;
    case 'm': {
      auto n = util::parse_uint(optarg);
      if (!n) {
        std::println(stderr, "-m: Bad option value: {}", optarg);
        exit(EXIT_FAILURE);
      }
      config.multiply = static_cast<int>(*n);
      break;
    }
    case 'c': {
      auto n = util::parse_uint_with_unit(optarg);
      if (!n) {
        std::println(stderr, "-c: Bad option value: {}", optarg);
        exit(EXIT_FAILURE);
      }
      if (*n > std::numeric_limits<uint32_t>::max()) {
        std::println(
          stderr, "-c: Value too large.  It should be less than or equal to {}",
          std::numeric_limits<uint32_t>::max());
        exit(EXIT_FAILURE);
      }
      config.header_table_size = static_cast<int64_t>(*n);
      break;
    }
    case 'y':
      config.verify_peer = false;
      break;
    case '?':
      util::show_candidates(argv[optind - 1], long_options);
      exit(EXIT_FAILURE);
    case 0:
      switch (flag) {
      case 1:
        // cert option
        config.certfile = optarg;
        break;
      case 2:
        // key option
        config.keyfile = optarg;
        break;
      case 4:
        // continuation option
        config.continuation = true;
        break;
      case 5:
        // version option
        print_version();
        exit(EXIT_SUCCESS);
      case 6:
        // no-content-length option
        config.no_content_length = true;
        break;
      case 7:
        // no-dep option
        std::println(stderr, "[WARNING]: --no-dep option has been deprecated.");
        break;
      case 9: {
        // trailer option
        auto header = optarg;
        auto name_end = strchr(optarg, ':');
        if (!name_end) {
          std::println(stderr, "--trailer: invalid header: {}", optarg);
          exit(EXIT_FAILURE);
        }
        *name_end = 0;
        auto value = name_end + 1;
        while (isspace(*value)) {
          value++;
        }
        if (*value == 0) {
          // This could also be a valid case for suppressing a header
          // similar to curl
          std::println(stderr, "--trailer: invalid header - value missing: {}",
                       optarg);
          exit(EXIT_FAILURE);
        }
        util::tolower(header, name_end, header);
        config.trailer.emplace_back(header, value, false);
        break;
      }
      case 10:
        // hexdump option
        config.hexdump = true;
        break;
      case 13:
        // expect-continue option
        config.expect_continue = true;
        break;
      case 14: {
        // encoder-header-table-size option
        auto n = util::parse_uint_with_unit(optarg);
        if (!n) {
          std::println(stderr,
                       "--encoder-header-table-size: Bad option value: {}",
                       optarg);
          exit(EXIT_FAILURE);
        }
        if (*n > std::numeric_limits<uint32_t>::max()) {
          std::println(stderr,
                       "--encoder-header-table-size: Value too large.  It "
                       "should be less than or equal to {}",
                       std::numeric_limits<uint32_t>::max());
          exit(EXIT_FAILURE);
        }
        config.encoder_header_table_size = static_cast<int64_t>(*n);
        break;
      }
      case 15:
        // ktls option
        config.ktls = true;
        break;
      case 16:
        // no-rfc7540-pri option
        std::println(stderr,
                     "[WARNING]: --no-rfc7540-pri option has been deprecated.");
        break;
      case 17: {
        // extpri option
        nghttp2_pri pri{
          .urgency = NGHTTP2_DEFAULT_URGENCY,
        };

        if (nghttp2_pri_parse_priority(
              &pri, reinterpret_cast<const uint8_t *>(optarg),
              strlen(optarg)) != 0) {
          std::println(stderr, "--extpri: Bad option value: {}", optarg);
          exit(EXIT_FAILURE);
        }

        config.pris.emplace_back(std::move(pri));

        break;
      }
      }
      break;
    default:
      break;
    }
  }

  nghttp2_pri pri_to_fill{
    .urgency = NGHTTP2_DEFAULT_URGENCY,
  };

  if (!config.pris.empty()) {
    pri_to_fill = config.pris.back();
  }
  config.pris.insert(std::ranges::end(config.pris),
                     static_cast<size_t>(argc - optind), pri_to_fill);

  // Find scheme overridden by extra header fields.
  auto scheme_it = std::ranges::find_if(
    config.headers, [](const Header &nv) { return nv.name == ":scheme"; });
  if (scheme_it != std::ranges::end(config.headers)) {
    config.scheme_override = (*scheme_it).value;
  }

  // Find host and port overridden by extra header fields.
  auto authority_it = std::ranges::find_if(
    config.headers, [](const Header &nv) { return nv.name == ":authority"; });
  if (authority_it == std::ranges::end(config.headers)) {
    authority_it = std::ranges::find_if(
      config.headers, [](const Header &nv) { return nv.name == "host"; });
  }

  if (authority_it != std::ranges::end(config.headers)) {
    // authority_it may looks like "host:port".
    auto uri = "https://" + (*authority_it).value;
    urlparse_url u;
    if (urlparse_parse_url(uri.c_str(), uri.size(), 0, &u) != 0) {
      std::println(stderr, "[ERROR] Could not parse authority in {}: {}",
                   (*authority_it).name, (*authority_it).value);
      exit(EXIT_FAILURE);
    }

    config.host_override = util::get_uri_field(uri.c_str(), u, URLPARSE_HOST);
    if (util::has_uri_field(u, URLPARSE_PORT)) {
      config.port_override = u.port;
    }
  }

  struct sigaction act{};
  act.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &act, nullptr);

  return run(argv + optind, argc - optind);
}

} // namespace nghttp2

int main(int argc, char **argv) {
  return nghttp2::run_app(nghttp2::main, argc, argv);
}
