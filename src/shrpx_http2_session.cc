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
#include "shrpx_http2_session.h"

#include <netinet/tcp.h>
#ifdef HAVE_UNISTD_H
#  include <unistd.h>
#endif // defined(HAVE_UNISTD_H)

#include <vector>

#include "ssl_compat.h"

#ifdef NGHTTP2_OPENSSL_IS_WOLFSSL
#  include <wolfssl/options.h>
#  include <wolfssl/openssl/err.h>
#else // !defined(NGHTTP2_OPENSSL_IS_WOLFSSL)
#  include <openssl/err.h>
#endif // !defined(NGHTTP2_OPENSSL_IS_WOLFSSL)

#include "shrpx_upstream.h"
#include "shrpx_downstream.h"
#include "shrpx_config.h"
#include "shrpx_error.h"
#include "shrpx_http2_downstream_connection.h"
#include "shrpx_client_handler.h"
#include "shrpx_tls.h"
#include "shrpx_http.h"
#include "shrpx_worker.h"
#include "shrpx_connect_blocker.h"
#include "shrpx_log.h"
#include "http2.h"
#include "util.h"
#include "base64.h"
#include "tls.h"
#include "app_helper.h"

using namespace nghttp2;

namespace shrpx {

constexpr ev_tstamp CONNCHK_TIMEOUT = 5.;
constexpr ev_tstamp CONNCHK_PING_TIMEOUT = 1.;

namespace {
void connchk_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto http2session = static_cast<Http2Session *>(w->data);

  ev_timer_stop(loop, w);

  switch (http2session->get_connection_check_state()) {
  case ConnectionCheck::STARTED:
    // ping timeout; disconnect
    if (log_enabled(INFO)) {
      Log{INFO, http2session} << "ping timeout";
    }

    delete http2session;

    return;
  default:
    if (log_enabled(INFO)) {
      Log{INFO, http2session} << "connection check required";
    }
    http2session->set_connection_check_state(ConnectionCheck::REQUIRED);
  }
}
} // namespace

namespace {
void timeoutcb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto conn = static_cast<Connection *>(w->data);
  auto http2session = static_cast<Http2Session *>(conn->data);

  if (w == &conn->rt && !conn->expired_rt()) {
    return;
  }

  if (log_enabled(INFO)) {
    Log{INFO, http2session} << "Timeout";
  }

  http2session->on_timeout();

  delete http2session;
}
} // namespace

namespace {
void readcb(struct ev_loop *loop, ev_io *w, int revents) {
  auto conn = static_cast<Connection *>(w->data);
  auto http2session = static_cast<Http2Session *>(conn->data);
  if (!http2session->do_read() || !http2session->connection_alive()) {
    delete http2session;

    return;
  }
}
} // namespace

namespace {
void writecb(struct ev_loop *loop, ev_io *w, int revents) {
  auto conn = static_cast<Connection *>(w->data);
  auto http2session = static_cast<Http2Session *>(conn->data);
  if (!http2session->do_write()) {
    delete http2session;

    return;
  }
  http2session->reset_connection_check_timer_if_not_checking();
}
} // namespace

namespace {
void initiate_connection_cb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto http2session = static_cast<Http2Session *>(w->data);
  ev_timer_stop(loop, w);
  if (!http2session->initiate_connection()) {
    if (log_enabled(INFO)) {
      Log{INFO, http2session} << "Could not initiate backend connection";
    }

    delete http2session;

    return;
  }
}
} // namespace

namespace {
void prepare_cb(struct ev_loop *loop, ev_prepare *w, int revents) {
  auto http2session = static_cast<Http2Session *>(w->data);
  http2session->check_retire();
}
} // namespace

Http2Session::Http2Session(struct ev_loop *loop, SSL_CTX *ssl_ctx,
                           Worker *worker,
                           const std::shared_ptr<DownstreamAddrGroup> &group,
                           DownstreamAddr *addr)
  : conn_(loop, -1, nullptr, worker->get_mcpool(),
          group->shared_addr->timeout.write, group->shared_addr->timeout.read,
          {}, {}, writecb, readcb, timeoutcb, this,
          get_config()->tls.dyn_rec.warmup_threshold,
          get_config()->tls.dyn_rec.idle_timeout, Proto::HTTP2),
    wb_(worker->get_mcpool()),
    worker_(worker),
    ssl_ctx_(ssl_ctx),
    group_(group),
    addr_(addr) {
  read_ = write_ = &Http2Session::noop;

  on_read_ = &Http2Session::read_noop;
  on_write_ = &Http2Session::write_noop;

  // We will reuse this many times, so use repeat timeout value.  The
  // timeout value is set later.
  ev_timer_init(&connchk_timer_, connchk_timeout_cb, 0., 0.);

  connchk_timer_.data = this;

  ev_timer_init(
    &http2_timer_,
    [](struct ev_loop *loop, ev_timer *w, int revents) {
      auto http2session = static_cast<Http2Session *>(w->data);
      http2session->handle_http2_timeout();
    },
    0., 0.);
  http2_timer_.data = this;

  ev_timer_init(&initiate_connection_timer_, initiate_connection_cb, 0., 0.);
  initiate_connection_timer_.data = this;

  ev_prepare_init(&prep_, prepare_cb);
  prep_.data = this;
  ev_prepare_start(loop, &prep_);
}

Http2Session::~Http2Session() {
  exclude_from_scheduling();
  disconnect(should_hard_fail());
}

void Http2Session::disconnect(bool hard) {
  if (log_enabled(INFO)) {
    Log{INFO, this} << "Disconnecting";
  }
  nghttp2_conn_del(h2conn_);
  h2conn_ = nullptr;

  wb_.reset();

  if (dns_query_) {
    auto dns_tracker = worker_->get_dns_tracker();
    dns_tracker->cancel(dns_query_.get());
  }

  conn_.rlimit.stopw();
  conn_.wlimit.stopw();

  ev_prepare_stop(conn_.loop, &prep_);

  ev_timer_stop(conn_.loop, &initiate_connection_timer_);
  ev_timer_stop(conn_.loop, &http2_timer_);
  ev_timer_stop(conn_.loop, &connchk_timer_);

  read_ = write_ = &Http2Session::noop;

  on_read_ = &Http2Session::read_noop;
  on_write_ = &Http2Session::write_noop;

  conn_.disconnect();

  if (proxy_htp_) {
    proxy_htp_.reset();
  }

  connection_check_state_ = ConnectionCheck::NONE;
  state_ = Http2SessionState::DISCONNECTED;

  // When deleting Http2DownstreamConnection, it calls this object's
  // remove_downstream_connection().  The multiple
  // Http2DownstreamConnection objects belong to the same
  // ClientHandler object if upstream is h2.  So be careful when you
  // delete ClientHandler here.
  //
  // We allow creating new pending Http2DownstreamConnection with this
  // object.  Upstream::on_downstream_reset() may add
  // Http2DownstreamConnection to another Http2Session.

  for (auto dc = dconns_.head; dc;) {
    auto next = dc->dlnext;
    auto downstream = dc->get_downstream();
    auto upstream = downstream->get_upstream();

    // Failure is allowed only for HTTP/1 upstream where upstream is
    // not shared by multiple Downstreams.
    if (!upstream->on_downstream_reset(downstream, hard)) {
      delete upstream->get_client_handler();
    }

    // dc was deleted
    dc = next;
  }

  auto streams = std::move(streams_);
  for (auto s = streams.head; s;) {
    auto next = s->dlnext;
    delete s;
    s = next;
  }
}

std::expected<void, Error> Http2Session::resolve_name() {
  auto dns_query = std::make_unique<DNSQuery>(
    addr_->host, [this](DNSResolverStatus status, const Address *result) {
      if (status == DNSResolverStatus::OK) {
        *resolved_addr_ = *result;
        resolved_addr_->port(addr_->port);
      }

      if (!this->initiate_connection()) {
        delete this;
      }
    });
  resolved_addr_ = std::make_unique<Address>();
  auto dns_tracker = worker_->get_dns_tracker();
  switch (dns_tracker->resolve(resolved_addr_.get(), dns_query.get())) {
  case DNSResolverStatus::ERROR:
    return std::unexpected{Error::DNS};
  case DNSResolverStatus::RUNNING:
    dns_query_ = std::move(dns_query);
    state_ = Http2SessionState::RESOLVING_NAME;
    return {};
  case DNSResolverStatus::OK:
    resolved_addr_->port(addr_->port);
    return {};
  default:
    assert(0);
    abort();
  }
}

namespace {
int htp_hdrs_completecb(llhttp_t *htp);
} // namespace

constexpr llhttp_settings_t htp_hooks = {
  .on_headers_complete = htp_hdrs_completecb,
};

std::expected<void, Error> Http2Session::initiate_connection() {
  int rv = 0;

  auto worker_blocker = worker_->get_connect_blocker();

  if (state_ == Http2SessionState::DISCONNECTED ||
      state_ == Http2SessionState::RESOLVING_NAME) {
    if (worker_blocker->blocked()) {
      if (log_enabled(INFO)) {
        Log{INFO, this}
          << "Worker wide backend connection was blocked temporarily";
      }
      return std::unexpected{Error::INTERNAL};
    }
  }

  auto &downstreamconf = *get_config()->conn.downstream;

  const auto &proxy = get_config()->downstream_http_proxy;
  if (!proxy.host.empty() && state_ == Http2SessionState::DISCONNECTED) {
    if (log_enabled(INFO)) {
      Log{INFO, this} << "Connecting to the proxy " << proxy.host << ":"
                      << proxy.port;
    }

    auto maybe_fd = util::create_nonblock_socket(proxy.addr.family());
    if (!maybe_fd) {
      auto error = errno;
      Log{WARN, this} << "Backend proxy socket() failed; addr="
                      << util::to_numeric_addr(&proxy.addr)
                      << ", errno=" << error;

      worker_blocker->on_failure();
      return std::unexpected{maybe_fd.error()};
    }

    conn_.fd = *maybe_fd;

    rv = connect(conn_.fd, proxy.addr.as_sockaddr(), proxy.addr.size());
    if (rv != 0 && errno != EINPROGRESS) {
      auto error = errno;
      Log{WARN, this} << "Backend proxy connect() failed; addr="
                      << util::to_numeric_addr(&proxy.addr)
                      << ", errno=" << error;

      worker_blocker->on_failure();

      return std::unexpected{Error::SYSCALL};
    }

    raddr_ = &proxy.addr;

    worker_blocker->on_success();

    ev_io_set(&conn_.rev, conn_.fd, EV_READ);
    ev_io_set(&conn_.wev, conn_.fd, EV_WRITE);

    conn_.wlimit.startw();

    conn_.wt.repeat = downstreamconf.timeout.connect;
    ev_timer_again(conn_.loop, &conn_.wt);

    write_ = &Http2Session::connected;

    on_read_ = &Http2Session::downstream_read_proxy;
    on_write_ = &Http2Session::downstream_connect_proxy;

    proxy_htp_ = std::make_unique<llhttp_t>();
    llhttp_init(proxy_htp_.get(), HTTP_RESPONSE, &htp_hooks);
    proxy_htp_->data = this;

    state_ = Http2SessionState::PROXY_CONNECTING;

    return {};
  }

  if (state_ == Http2SessionState::DISCONNECTED ||
      state_ == Http2SessionState::PROXY_CONNECTED ||
      state_ == Http2SessionState::RESOLVING_NAME) {
    if (log_enabled(INFO)) {
      if (state_ != Http2SessionState::RESOLVING_NAME) {
        Log{INFO, this} << "Connecting to downstream server";
      }
    }
    if (addr_->tls) {
      assert(ssl_ctx_);

      if (state_ != Http2SessionState::RESOLVING_NAME) {
        auto maybe_ssl = tls::create_ssl(ssl_ctx_);
        if (!maybe_ssl) {
          return std::unexpected{maybe_ssl.error()};
        }

        auto ssl = *maybe_ssl;

        tls::setup_downstream_http2_alpn(ssl);

        conn_.set_ssl(ssl);
        conn_.tls.client_session_cache = &addr_->tls_session_cache;

        auto sni_name = addr_->sni.empty() ? addr_->host : addr_->sni;

        if (!util::numeric_host(sni_name.data())) {
          // TLS extensions: SNI. There is no documentation about the return
          // code for this function (actually this is macro wrapping SSL_ctrl
          // at the time of this writing).
          SSL_set_tlsext_host_name(conn_.tls.ssl, sni_name.data());
        }

        auto maybe_tls_session =
          tls::reuse_tls_session(addr_->tls_session_cache);
        if (maybe_tls_session) {
          auto tls_session = *maybe_tls_session;
          SSL_set_session(conn_.tls.ssl, tls_session);
          SSL_SESSION_free(tls_session);
        }
      }

      if (state_ == Http2SessionState::DISCONNECTED) {
        if (addr_->dns) {
          if (auto rv = resolve_name(); !rv) {
            downstream_failure(addr_, nullptr);
            return rv;
          }
          if (state_ == Http2SessionState::RESOLVING_NAME) {
            return {};
          }
          raddr_ = resolved_addr_.get();
        } else {
          raddr_ = &addr_->addr;
        }
      }

      if (state_ == Http2SessionState::RESOLVING_NAME) {
        if (dns_query_->status == DNSResolverStatus::ERROR) {
          downstream_failure(addr_, nullptr);
          return std::unexpected{Error::DNS};
        }
        assert(dns_query_->status == DNSResolverStatus::OK);
        state_ = Http2SessionState::DISCONNECTED;
        dns_query_.reset();
        raddr_ = resolved_addr_.get();
      }

      // If state_ == Http2SessionState::PROXY_CONNECTED, we have
      // connected to the proxy using conn_.fd and tunnel has been
      // established.
      if (state_ == Http2SessionState::DISCONNECTED) {
        assert(conn_.fd == -1);

        auto maybe_fd = util::create_nonblock_socket(raddr_->family());
        if (!maybe_fd) {
          auto error = errno;
          Log{WARN, this} << "socket() failed; addr="
                          << util::to_numeric_addr(raddr_)
                          << ", errno=" << error;

          worker_blocker->on_failure();
          return std::unexpected{maybe_fd.error()};
        }

        conn_.fd = *maybe_fd;

        worker_blocker->on_success();

        rv = connect(conn_.fd,
                     // TODO maybe not thread-safe?
                     raddr_->as_sockaddr(), raddr_->size());
        if (rv != 0 && errno != EINPROGRESS) {
          auto error = errno;
          Log{WARN, this} << "connect() failed; addr="
                          << util::to_numeric_addr(raddr_)
                          << ", errno=" << error;

          downstream_failure(addr_, raddr_);
          return std::unexpected{Error::SYSCALL};
        }

        ev_io_set(&conn_.rev, conn_.fd, EV_READ);
        ev_io_set(&conn_.wev, conn_.fd, EV_WRITE);
      }

      conn_.prepare_client_handshake();
    } else {
      if (state_ == Http2SessionState::DISCONNECTED) {
        // Without TLS and proxy.
        if (addr_->dns) {
          if (auto rv = resolve_name(); !rv) {
            downstream_failure(addr_, nullptr);
            return rv;
          }
          if (state_ == Http2SessionState::RESOLVING_NAME) {
            return {};
          }
          raddr_ = resolved_addr_.get();
        } else {
          raddr_ = &addr_->addr;
        }
      }

      if (state_ == Http2SessionState::RESOLVING_NAME) {
        if (dns_query_->status == DNSResolverStatus::ERROR) {
          downstream_failure(addr_, nullptr);
          return std::unexpected{Error::DNS};
        }
        assert(dns_query_->status == DNSResolverStatus::OK);
        state_ = Http2SessionState::DISCONNECTED;
        dns_query_.reset();
        raddr_ = resolved_addr_.get();
      }

      if (state_ == Http2SessionState::DISCONNECTED) {
        // Without TLS and proxy.
        assert(conn_.fd == -1);

        auto maybe_fd = util::create_nonblock_socket(raddr_->family());
        if (!maybe_fd) {
          auto error = errno;
          Log{WARN, this} << "socket() failed; addr="
                          << util::to_numeric_addr(raddr_)
                          << ", errno=" << error;

          worker_blocker->on_failure();
          return std::unexpected{maybe_fd.error()};
        }

        conn_.fd = *maybe_fd;

        worker_blocker->on_success();

        rv = connect(conn_.fd, raddr_->as_sockaddr(), raddr_->size());
        if (rv != 0 && errno != EINPROGRESS) {
          auto error = errno;
          Log{WARN, this} << "connect() failed; addr="
                          << util::to_numeric_addr(raddr_)
                          << ", errno=" << error;

          downstream_failure(addr_, raddr_);
          return std::unexpected{Error::SYSCALL};
        }

        ev_io_set(&conn_.rev, conn_.fd, EV_READ);
        ev_io_set(&conn_.wev, conn_.fd, EV_WRITE);
      }
    }

    // We have been already connected when no TLS and proxy is used.
    if (state_ == Http2SessionState::PROXY_CONNECTED) {
      on_read_ = &Http2Session::read_noop;
      on_write_ = &Http2Session::write_noop;

      if (auto rv = connected(); !rv) {
        return rv;
      }
    }

    write_ = &Http2Session::connected;

    state_ = Http2SessionState::CONNECTING;
    conn_.wlimit.startw();

    conn_.wt.repeat = downstreamconf.timeout.connect;
    ev_timer_again(conn_.loop, &conn_.wt);

    return {};
  }

  // Unreachable
  assert(0);

  return {};
}

namespace {
int htp_hdrs_completecb(llhttp_t *htp) {
  auto http2session = static_cast<Http2Session *>(htp->data);

  // We only read HTTP header part.  If tunneling succeeds, response
  // body is a different protocol (HTTP/2 in this case), we don't read
  // them here.

  // We just check status code here
  if (htp->status_code / 100 == 2) {
    if (log_enabled(INFO)) {
      Log{INFO, http2session} << "Tunneling success";
    }
    http2session->set_state(Http2SessionState::PROXY_CONNECTED);

    return HPE_PAUSED;
  }

  Log{WARN, http2session} << "Tunneling failed: " << htp->status_code;
  http2session->set_state(Http2SessionState::PROXY_FAILED);

  return HPE_PAUSED;
}
} // namespace

std::expected<void, Error>
Http2Session::downstream_read_proxy(std::span<const uint8_t> data) {
  auto htperr = llhttp_execute(
    proxy_htp_.get(), reinterpret_cast<const char *>(data.data()), data.size());
  if (htperr == HPE_PAUSED) {
    switch (state_) {
    case Http2SessionState::PROXY_CONNECTED:
      // Initiate SSL/TLS handshake through established tunnel.
      if (auto rv = initiate_connection(); !rv) {
        return rv;
      }
      return {};
    case Http2SessionState::PROXY_FAILED:
      return std::unexpected{Error::INTERNAL};
    default:
      break;
    }
    // should not be here
    assert(0);
  }

  if (htperr != HPE_OK) {
    return std::unexpected{Error::HTTP1};
  }

  return {};
}

std::expected<void, Error> Http2Session::downstream_connect_proxy() {
  if (log_enabled(INFO)) {
    Log{INFO, this} << "Connected to the proxy";
  }

  std::string req = "CONNECT ";
  req.append(addr_->hostport.data(), addr_->hostport.size());
  if (addr_->port == 80 || addr_->port == 443) {
    req += ':';
    req += util::utos(addr_->port);
  }
  req += " HTTP/1.1\r\nHost: ";
  req += addr_->host;
  req += "\r\n";
  const auto &proxy = get_config()->downstream_http_proxy;
  if (!proxy.userinfo.empty()) {
    req += "Proxy-Authorization: Basic ";
    req += base64::encode(proxy.userinfo);
    req += "\r\n";
  }
  req += "\r\n";
  if (log_enabled(INFO)) {
    Log{INFO, this} << "HTTP proxy request headers\n" << req;
  }
  wb_.append(req);

  on_write_ = &Http2Session::write_noop;

  signal_write();
  return {};
}

void Http2Session::add_downstream_connection(Http2DownstreamConnection *dconn) {
  dconns_.append(dconn);
  ++addr_->num_dconn;
}

void Http2Session::remove_downstream_connection(
  Http2DownstreamConnection *dconn) {
  --addr_->num_dconn;
  dconns_.remove(dconn);
  dconn->detach_stream_data();

  if (log_enabled(INFO)) {
    Log{INFO, this} << "Remove downstream";
  }

  if (freelist_zone_ == FreelistZone::NONE && !max_concurrency_reached()) {
    if (log_enabled(INFO)) {
      Log{INFO, this} << "Append to http2_extra_freelist, addr=" << addr_
                      << ", freelist.size="
                      << addr_->http2_extra_freelist.size();
    }

    add_to_extra_freelist();
  }
}

void Http2Session::remove_stream_data(StreamData *sd) {
  streams_.remove(sd);
  if (sd->dconn) {
    sd->dconn->detach_stream_data();
  }
  delete sd;
}

std::expected<void, Error>
Http2Session::submit_request(Http2DownstreamConnection *dconn,
                             const nghttp2_nv *nva, size_t nvlen,
                             const nghttp2_data_reader *dr) {
  assert(state_ == Http2SessionState::CONNECTED);
  auto sd = std::make_unique<StreamData>();
  sd->dlnext = sd->dlprev = nullptr;
  auto stream_id =
    nghttp2_conn_submit_request(h2conn_, nva, nvlen, dr, sd.get());
  if (stream_id < 0) {
    Log{FATAL, this} << "nghttp2_conn_submit_request() failed: "
                     << nghttp2_strerror((int)stream_id);
    return std::unexpected{Error::HTTP2};
  }

  dconn->attach_stream_data(sd.get());
  dconn->get_downstream()->set_downstream_stream_id(stream_id);
  streams_.append(sd.release());

  return {};
}

void Http2Session::shutdown_stream(int64_t stream_id, uint32_t error_code) {
  assert(state_ == Http2SessionState::CONNECTED);
  if (log_enabled(INFO)) {
    Log{INFO, this} << "RST_STREAM stream_id=" << stream_id
                    << " with error_code=" << error_code;
  }

  nghttp2_conn_shutdown_stream(h2conn_, 0x00, stream_id, error_code);
}

nghttp2_conn *Http2Session::get_h2conn() const { return h2conn_; }

std::expected<void, Error>
Http2Session::resume_data(Http2DownstreamConnection *dconn) {
  assert(state_ == Http2SessionState::CONNECTED);
  auto downstream = dconn->get_downstream();
  auto rv =
    nghttp2_conn_resume_stream(h2conn_, downstream->get_downstream_stream_id());
  if (rv != 0) {
    if (rv != NGHTTP2_ERR_INVALID_ARGUMENT) {
      Log{FATAL, this} << "nghttp2_conn_resume_stream() failed: "
                       << nghttp2_strerror(rv);

      return std::unexpected{Error::HTTP2};
    }
  }

  return {};
}

namespace {
void call_downstream_readcb(Http2Session *http2session,
                            Downstream *downstream) {
  auto upstream = downstream->get_upstream();
  if (!upstream) {
    return;
  }
  if (!upstream->downstream_read(downstream->get_downstream_connection())) {
    delete upstream->get_client_handler();
  }
}
} // namespace

namespace {
int stream_close(nghttp2_conn *conn, uint32_t flags, int64_t stream_id,
                 uint32_t error_code, void *conn_user_data,
                 void *stream_user_data) {
  auto http2session = static_cast<Http2Session *>(conn_user_data);

  if (!(flags & NGHTTP2_STREAM_CLOSE_FLAG_ERROR_CODE_SET)) {
    error_code = NGHTTP2_NO_ERROR;
  }

  if (log_enabled(INFO)) {
    Log{INFO, http2session} << "Stream stream_id=" << stream_id
                            << " is being closed with error code "
                            << error_code;
  }
  auto sd = static_cast<StreamData *>(stream_user_data);
  if (!sd) {
    // We might get this close callback when pushed streams are
    // closed.
    return 0;
  }
  auto dconn = sd->dconn;
  if (dconn) {
    auto downstream = dconn->get_downstream();

    if (downstream->get_upgraded() &&
        downstream->get_response_state() == DownstreamState::HEADER_COMPLETE) {
      // For tunneled connection, we have to submit RST_STREAM to
      // upstream *after* whole response body is sent. We just set
      // MSG_COMPLETE here. Upstream will take care of that.
      if (!downstream->get_upstream()->on_downstream_body_complete(
            downstream)) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
      }
      downstream->set_response_state(DownstreamState::MSG_COMPLETE);
    } else if (error_code == NGHTTP2_NO_ERROR) {
      switch (downstream->get_response_state()) {
      case DownstreamState::MSG_COMPLETE:
      case DownstreamState::MSG_BAD_HEADER:
        break;
      default:
        downstream->set_response_state(DownstreamState::MSG_RESET);
      }
    } else if (downstream->get_response_state() !=
               DownstreamState::MSG_BAD_HEADER) {
      downstream->set_response_state(DownstreamState::MSG_RESET);
    }
    if (downstream->get_response_state() == DownstreamState::MSG_RESET) {
      downstream->set_response_rst_stream_error_code(error_code);
    }
    call_downstream_readcb(http2session, downstream);

    // dconn may be deleted
  }
  // The life time of StreamData ends here
  http2session->remove_stream_data(sd);
  return 0;
}
} // namespace

namespace {
int recv_header(nghttp2_conn *conn, int64_t stream_id, int32_t token,
                nghttp2_rcbuf *name, nghttp2_rcbuf *value, uint8_t flags,
                void *conn_user_data, void *stream_user_data) {
  auto sd = static_cast<StreamData *>(stream_user_data);
  if (!sd || !sd->dconn) {
    return 0;
  }
  auto downstream = sd->dconn->get_downstream();

  auto namebuf = nghttp2_rcbuf_get_buf(name);
  auto valuebuf = nghttp2_rcbuf_get_buf(value);

  auto &resp = downstream->response();
  auto &httpconf = get_config()->http;

  if (resp.fs.buffer_size() + namebuf.len + valuebuf.len >
        httpconf.response_header_field_buffer ||
      resp.fs.num_fields() >= httpconf.max_response_header_fields) {
    if (log_enabled(INFO)) {
      Log{INFO, downstream}
        << "Too large or many header field size="
        << resp.fs.buffer_size() + namebuf.len + valuebuf.len
        << ", num=" << resp.fs.num_fields() + 1;
    }

    nghttp2_conn_shutdown_stream(conn, 0x00, stream_id, NGHTTP2_INTERNAL_ERROR);

    return 0;
  }

  auto nameref = as_string_view(namebuf.base, namebuf.len);
  auto valueref = as_string_view(valuebuf.base, valuebuf.len);
  auto never_index = flags & NGHTTP2_NV_FLAG_NEVER_INDEX;

  downstream->add_rcbuf(name);
  downstream->add_rcbuf(value);

  resp.fs.add_header_token(nameref, valueref, never_index, token);
  return 0;
}
} // namespace

namespace {
int recv_trailer(nghttp2_conn *conn, int64_t stream_id, int32_t token,
                 nghttp2_rcbuf *name, nghttp2_rcbuf *value, uint8_t flags,
                 void *conn_user_data, void *stream_user_data) {
  auto sd = static_cast<StreamData *>(stream_user_data);
  if (!sd || !sd->dconn) {
    return 0;
  }
  auto downstream = sd->dconn->get_downstream();

  auto namebuf = nghttp2_rcbuf_get_buf(name);
  auto valuebuf = nghttp2_rcbuf_get_buf(value);

  auto &resp = downstream->response();
  auto &httpconf = get_config()->http;

  if (resp.fs.buffer_size() + namebuf.len + valuebuf.len >
        httpconf.response_header_field_buffer ||
      resp.fs.num_fields() >= httpconf.max_response_header_fields) {
    if (log_enabled(INFO)) {
      Log{INFO, downstream}
        << "Too large or many header field size="
        << resp.fs.buffer_size() + namebuf.len + valuebuf.len
        << ", num=" << resp.fs.num_fields() + 1;
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

  resp.fs.add_trailer_token(nameref, valueref, never_index, token);
  return 0;
}
} // namespace

namespace {
int begin_headers(nghttp2_conn *conn, int64_t stream_id, void *conn_user_data,
                  void *stream_user_data) {
  auto http2session = static_cast<Http2Session *>(conn_user_data);

  auto sd = static_cast<StreamData *>(stream_user_data);
  if (!sd || !sd->dconn) {
    http2session->shutdown_stream(stream_id, NGHTTP2_INTERNAL_ERROR);

    return 0;
  }

  return 0;
}
} // namespace

namespace {
std::expected<void, Error> on_response_headers(Http2Session *http2session,
                                               Downstream *downstream,
                                               int64_t stream_id, bool fin) {
  auto upstream = downstream->get_upstream();
  auto handler = upstream->get_client_handler();
  const auto &req = downstream->request();
  auto &resp = downstream->response();

  auto &nva = resp.fs.headers();

  auto config = get_config();
  auto &loggingconf = config->logging;

  downstream->set_expect_final_response(false);

  auto status = resp.fs.header(NGHTTP2_HPACK_TOKEN__STATUS);
  // libnghttp2 guarantees this exists and can be parsed
  assert(status);
  auto status_code = *http2::parse_http_status_code(status->value);

  resp.http_status = as_unsigned(status_code);
  resp.http_major = 2;
  resp.http_minor = 0;

  downstream->set_downstream_addr_group(
    http2session->get_downstream_addr_group());
  downstream->set_addr(http2session->get_addr());

  if (log_enabled(INFO)) {
    std::string ss;
    for (auto &nv : nva) {
      ss += tty_http_hd();
      ss += nv.name;
      ss += tty_rst();
      ss += ": ";
      ss += nv.value;
      ss += '\n';
    }
    Log{INFO, http2session} << "HTTP response headers. stream_id=" << stream_id
                            << "\n"
                            << ss;
  }

  if (downstream->get_non_final_response()) {
    if (log_enabled(INFO)) {
      Log{INFO, http2session} << "This is non-final response.";
    }

    downstream->set_expect_final_response(true);
    // After Upstream::on_downstream_header_complete, Dowstream's
    // response headers are erased.
    if (!upstream->on_downstream_header_complete(downstream)) {
      http2session->shutdown_stream(stream_id, NGHTTP2_PROTOCOL_ERROR);

      downstream->set_response_state(DownstreamState::MSG_RESET);
    }

    return {};
  }

  downstream->set_response_state(DownstreamState::HEADER_COMPLETE);
  downstream->check_upgrade_fulfilled_http2();

  if (downstream->get_upgraded()) {
    resp.connection_close = true;
    // On upgrade success, both ends can send data
    if (auto rv = upstream->resume_read(SHRPX_NO_BUFFER, downstream, 0); !rv) {
      return rv;
    }
    downstream->set_request_state(DownstreamState::HEADER_COMPLETE);
    if (log_enabled(INFO)) {
      Log{INFO, http2session} << "HTTP upgrade success. stream_id="
                              << stream_id;
    }
  } else {
    auto content_length = resp.fs.header(NGHTTP2_HPACK_TOKEN_CONTENT_LENGTH);
    if (content_length) {
      // libnghttp2 guarantees this can be parsed
      resp.fs.content_length =
        static_cast<int64_t>(*util::parse_uint(content_length->value));
    }

    if (resp.fs.content_length == -1 && downstream->expect_response_body()) {
      // Here we have response body but Content-Length is not known in
      // advance.
      if (req.http_major <= 0 || (req.http_major == 1 && req.http_minor == 0)) {
        // We simply close connection for pre-HTTP/1.1 in this case.
        resp.connection_close = true;
      } else {
        // Otherwise, use chunked encoding to keep upstream connection
        // open.  In HTTP2, we are supposed not to receive
        // transfer-encoding.
        resp.fs.add_header_token("transfer-encoding"sv, "chunked"sv, false,
                                 NGHTTP2_HPACK_TOKEN_TRANSFER_ENCODING);
        downstream->set_chunked_response(true);
      }
    }
  }

  if (fin) {
    resp.headers_only = true;
  }

  if (loggingconf.access.write_early && downstream->accesslog_ready()) {
    handler->write_accesslog(downstream);
    downstream->set_accesslog_written(true);
  }

  if (!upstream->on_downstream_header_complete(downstream)) {
    // Handling early return (in other words, response was hijacked by
    // mruby scripting).
    if (downstream->get_response_state() == DownstreamState::MSG_COMPLETE) {
      http2session->shutdown_stream(stream_id, NGHTTP2_CANCEL);
    } else {
      http2session->shutdown_stream(stream_id, NGHTTP2_INTERNAL_ERROR);

      downstream->set_response_state(DownstreamState::MSG_RESET);
    }
  }

  return {};
}
} // namespace

namespace {
int end_headers(nghttp2_conn *conn, int64_t stream_id, int fin,
                void *conn_user_data, void *stream_user_data) {
  auto http2session = static_cast<Http2Session *>(conn_user_data);
  auto sd = static_cast<StreamData *>(stream_user_data);
  if (!sd || !sd->dconn) {
    return 0;
  }
  auto downstream = sd->dconn->get_downstream();

  if (!on_response_headers(http2session, downstream, stream_id, fin)) {
    return NGHTTP2_ERR_CALLBACK_FAILURE;
  }

  downstream->reset_downstream_rtimer();

  // This may delete downstream
  call_downstream_readcb(http2session, downstream);

  return 0;
}
} // namespace

namespace {
int remote_end_stream(nghttp2_conn *conn, int64_t stream_id,
                      void *conn_user_data, void *stream_user_data) {
  auto sd = static_cast<StreamData *>(stream_user_data);
  if (!sd || !sd->dconn) {
    return 0;
  }
  auto downstream = sd->dconn->get_downstream();

  downstream->disable_downstream_rtimer();

  if (downstream->get_response_state() == DownstreamState::HEADER_COMPLETE) {
    downstream->set_response_state(DownstreamState::MSG_COMPLETE);

    auto upstream = downstream->get_upstream();

    if (!upstream->on_downstream_body_complete(downstream)) {
      downstream->set_response_state(DownstreamState::MSG_RESET);
    }
  }

  return 0;
}
} // namespace

namespace {
int recv_settings(nghttp2_conn *conn, const nghttp2_proto_settings *settings,
                  void *conn_user_data) {
  auto http2session = static_cast<Http2Session *>(conn_user_data);

  if (!http2session->on_settings_received(settings)) {
    return NGHTTP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
int recv_ping_ack(nghttp2_conn *conn, const nghttp2_ping_data *data,
                  nghttp2_duration rtt, void *conn_user_data) {
  auto http2session = static_cast<Http2Session *>(conn_user_data);

  if (log_enabled(INFO)) {
    Log{INFO} << "PING ACK received";
  }

  if (!http2session->connection_alive()) {
    return NGHTTP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
int http2_shutdown(nghttp2_conn *conn, int64_t last_stream_id,
                   uint32_t error_code, void *conn_user_data) {
  auto http2session = static_cast<Http2Session *>(conn_user_data);

  if (log_enabled(INFO)) {
    Log{INFO, http2session}
      << "GOAWAY received: last-stream-id=" << last_stream_id
      << ", error_code=" << error_code;
  }

  return 0;
}
} // namespace

namespace {
int recv_data(nghttp2_conn *conn, int64_t stream_id, const uint8_t *data,
              size_t datalen, void *conn_user_data, void *stream_user_data) {
  auto http2session = static_cast<Http2Session *>(conn_user_data);
  auto sd = static_cast<StreamData *>(stream_user_data);
  if (!sd || !sd->dconn) {
    http2session->shutdown_stream(stream_id, NGHTTP2_INTERNAL_ERROR);

    if (!http2session->consume(stream_id, datalen)) {
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    return 0;
  }
  auto downstream = sd->dconn->get_downstream();
  if (!downstream->expect_response_body()) {
    http2session->shutdown_stream(stream_id, NGHTTP2_INTERNAL_ERROR);

    if (!http2session->consume(stream_id, datalen)) {
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    return 0;
  }

  downstream->reset_downstream_rtimer();

  auto &resp = downstream->response();

  resp.recv_body_length += datalen;
  resp.unconsumed_body_length += datalen;

  auto upstream = downstream->get_upstream();
  if (auto rv = upstream->on_downstream_body(downstream, {data, datalen}, true);
      !rv) {
    http2session->shutdown_stream(stream_id, NGHTTP2_INTERNAL_ERROR);

    if (!http2session->consume(stream_id, datalen)) {
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    downstream->set_response_state(DownstreamState::MSG_RESET);
  }

  call_downstream_readcb(http2session, downstream);
  return 0;
}
} // namespace

namespace {
int write_stream_data_offset(nghttp2_conn *conn, int64_t stream_id,
                             uint64_t offset, size_t len, void *conn_user_data,
                             void *stream_user_data) {
  auto sd = static_cast<StreamData *>(stream_user_data);
  if (!sd || !sd->dconn) {
    return 0;
  }

  auto downstream = sd->dconn->get_downstream();
  auto body = downstream->get_request_buf();

  body->drain(len);

  if (body->rleft()) {
    downstream->reset_downstream_wtimer();
  } else {
    downstream->disable_downstream_wtimer();
  }

  downstream->reset_downstream_rtimer();

  auto upstream = downstream->get_upstream();

  // This is important because it will handle flow control stuff.
  if (!upstream->resume_read(SHRPX_NO_BUFFER, downstream, len)) {
    // In this case, downstream may be deleted.
    nghttp2_conn_shutdown_stream(conn, 0x00, stream_id, NGHTTP2_INTERNAL_ERROR);
    return 0;
  }

  // Here sd->dconn could be nullptr, because Upstream::resume_read()
  // may delete downstream which will delete dconn.  Is this still
  // really true?

  return 0;
}
} // namespace

std::expected<void, Error> Http2Session::connection_made() {
  state_ = Http2SessionState::CONNECTED;

  on_write_ = &Http2Session::downstream_write;
  on_read_ = &Http2Session::downstream_read;

  if (addr_->tls) {
    const unsigned char *next_proto = nullptr;
    unsigned int next_proto_len = 0;

    SSL_get0_alpn_selected(conn_.tls.ssl, &next_proto, &next_proto_len);

    if (!next_proto) {
      downstream_failure(addr_, raddr_);
      return std::unexpected{Error::ALPN};
    }

    auto proto = as_string_view(next_proto, next_proto_len);
    if (log_enabled(INFO)) {
      Log{INFO, this} << "Negotiated next protocol: " << proto;
    }
    if (!util::check_h2_is_selected(proto)) {
      downstream_failure(addr_, raddr_);
      return std::unexpected{Error::ALPN};
    }
  }

  auto config = get_config();
  auto &http2conf = config->http2;

  static constexpr auto callbacks = nghttp2_callbacks{
    .rand = util::secure_random,
    .recv_settings = recv_settings,
    .stream_close = stream_close,
    .write_stream_data_offset = write_stream_data_offset,
    .begin_headers = begin_headers,
    .recv_header = recv_header,
    .end_headers = end_headers,
    .recv_trailer = recv_trailer,
    .recv_data = recv_data,
    .remote_end_stream = remote_end_stream,
    .recv_ping_ack = recv_ping_ack,
    .shutdown = http2_shutdown,
  };

  nghttp2_settings settings;
  nghttp2_settings_default(&settings);

  util::secure_random(reinterpret_cast<uint8_t *>(&settings.conn_id),
                      sizeof(settings.conn_id));
  settings.initial_ts = util::timestamp();
  settings.settings_timeout = static_cast<nghttp2_duration>(
    std::chrono::floor<std::chrono::nanoseconds>(
      util::duration_from(http2conf.downstream.timeout.settings))
      .count());
  settings.initial_max_stream_data =
    as_unsigned(http2conf.downstream.window_size);
  settings.initial_max_data =
    as_unsigned(http2conf.downstream.connection_window_size);
  settings.hpack_max_dtable_capacity =
    http2conf.downstream.decoder_dynamic_table_size;

  if (auto rv =
        nghttp2_conn_client_new(&h2conn_, &callbacks, &settings, NULL, this);
      rv != 0) {
    return std::unexpected{Error::HTTP2};
  }

  reset_connection_check_timer(CONNCHK_TIMEOUT);

  if (auto rv = submit_pending_requests(); !rv) {
    return rv;
  }

  signal_write();
  return {};
}

std::expected<void, Error> Http2Session::do_read() { return read_(*this); }
std::expected<void, Error> Http2Session::do_write() { return write_(*this); }

std::expected<void, Error>
Http2Session::on_read(std::span<const uint8_t> data) {
  return on_read_(*this, data);
}

std::expected<void, Error> Http2Session::on_write() {
  if (auto rv = on_write_(*this); !rv) {
    return rv;
  }

  reset_http2_timer();

  return {};
}

void Http2Session::reset_http2_timer() {
  if (!h2conn_) {
    return;
  }

  auto expiry = nghttp2_conn_get_expiry(h2conn_);
  if (expiry == UINT64_MAX) {
    if (ev_is_active(&http2_timer_)) {
      ev_timer_stop(conn_.loop, &http2_timer_);
    }

    return;
  }

  auto now = util::timestamp();

  if (expiry <= now) {
    ev_feed_event(conn_.loop, &http2_timer_, EV_TIMER);

    return;
  }

  auto t = static_cast<ev_tstamp>(expiry - now) / NGHTTP2_SECONDS;

  http2_timer_.repeat = t;
  ev_timer_again(conn_.loop, &http2_timer_);
}

void Http2Session::handle_http2_timeout() {
  ev_timer_stop(conn_.loop, &http2_timer_);

  auto rv = nghttp2_conn_handle_expiry(h2conn_, util::timestamp());
  if (rv != 0) {
    Log{ERROR, this} << "nghttp2_conn_handle_expiry() returned error: "
                     << nghttp2_strerror(rv);

    downstream_failure(addr_, raddr_);
    terminate_session(nghttp2_err_infer_http2_error_code(rv));
  }

  signal_write();
}

std::expected<void, Error>
Http2Session::downstream_read(std::span<const uint8_t> data) {
  auto rv =
    nghttp2_conn_read(h2conn_, data.data(), data.size(), util::timestamp());
  if (rv != 0) {
    Log{ERROR, this} << "nghttp2_conn_read() returned error: "
                     << nghttp2_strerror(rv);

    return std::unexpected{Error::HTTP2};
  }

  signal_write();
  return {};
}

std::expected<void, Error> Http2Session::downstream_write() {
  if (wb_.rleft()) {
    return {};
  }

  return wb_.append_or_error(
    16_k, [this](std::span<uint8_t> dest) -> std::expected<size_t, Error> {
      auto nwrite = nghttp2_conn_write(h2conn_, dest.data(), dest.size(),
                                       util::timestamp());
      if (nwrite < 0) {
        if (nwrite != NGHTTP2_ERR_CLOSING) {
          Log{ERROR, this} << "nghttp2_conn_write() returned error: "
                           << nghttp2_strerror(static_cast<int>(nwrite));
        }

        return std::unexpected{Error::HTTP2};
      }

      return as_unsigned(nwrite);
    });
}

void Http2Session::signal_write() {
  switch (state_) {
  case Http2SessionState::DISCONNECTED:
    if (!ev_is_active(&initiate_connection_timer_)) {
      if (log_enabled(INFO)) {
        Log{INFO} << "Start connecting to backend server";
      }
      // Since the timer is set to 0., these will feed 2 events.  We
      // will stop the timer in the initiate_connection_timer_ to void
      // 2nd event.
      ev_timer_start(conn_.loop, &initiate_connection_timer_);
      ev_feed_event(conn_.loop, &initiate_connection_timer_, 0);
    }
    break;
  case Http2SessionState::CONNECTED:
    conn_.wlimit.startw();
    break;
  default:
    break;
  }
}

struct ev_loop *Http2Session::get_loop() const { return conn_.loop; }

ev_io *Http2Session::get_wev() { return &conn_.wev; }

Http2SessionState Http2Session::get_state() const { return state_; }

void Http2Session::set_state(Http2SessionState state) { state_ = state; }

void Http2Session::terminate_session(uint32_t error_code) {
  nghttp2_conn_terminate(h2conn_, error_code);
}

SSL *Http2Session::get_ssl() const { return conn_.tls.ssl; }

std::expected<void, Error> Http2Session::consume(int64_t stream_id,
                                                 size_t len) {
  if (!h2conn_) {
    return {};
  }

  if (auto rv = nghttp2_conn_extend_max_stream_offset(h2conn_, stream_id, len);
      rv != 0) {
    Log{WARN, this}
      << "nghttp2_conn_extend_max_stream_offset() returned error: "
      << nghttp2_strerror(rv);

    return std::unexpected{Error::HTTP2};
  }

  if (auto rv = nghttp2_conn_extend_max_offset(h2conn_, len); rv != 0) {
    Log{WARN, this} << "nghttp2_conn_extend_max_offset() returned error: "
                    << nghttp2_strerror(rv);

    return std::unexpected{Error::HTTP2};
  }

  return {};
}

bool Http2Session::can_push_request(const Downstream *downstream) const {
  auto &req = downstream->request();
  return state_ == Http2SessionState::CONNECTED &&
         connection_check_state_ == ConnectionCheck::NONE &&
         (req.connect_proto == ConnectProto::NONE || settings_recved_);
}

void Http2Session::start_checking_connection() {
  if (state_ != Http2SessionState::CONNECTED ||
      connection_check_state_ != ConnectionCheck::REQUIRED) {
    return;
  }
  connection_check_state_ = ConnectionCheck::STARTED;

  Log{INFO, this} << "Start checking connection";
  // If connection is down, we may get error when writing data.  Issue
  // ping frame to see whether connection is alive.
  nghttp2_ping_data data;
  util::secure_random(data.data, sizeof(data.data));

  if (auto rv = nghttp2_conn_submit_ping(h2conn_, &data); rv != 0) {
    Log{WARN, this} << "nghttp2_conn_submit_ping() returned error: "
                    << nghttp2_strerror(rv);
  }

  // set ping timeout and start timer again
  reset_connection_check_timer(CONNCHK_PING_TIMEOUT);

  signal_write();
}

void Http2Session::reset_connection_check_timer(ev_tstamp t) {
  connchk_timer_.repeat = t;
  ev_timer_again(conn_.loop, &connchk_timer_);
}

void Http2Session::reset_connection_check_timer_if_not_checking() {
  if (connection_check_state_ != ConnectionCheck::NONE) {
    return;
  }

  reset_connection_check_timer(CONNCHK_TIMEOUT);
}

std::expected<void, Error> Http2Session::connection_alive() {
  reset_connection_check_timer(CONNCHK_TIMEOUT);

  if (connection_check_state_ == ConnectionCheck::NONE) {
    return {};
  }

  if (log_enabled(INFO)) {
    Log{INFO, this} << "Connection alive";
  }

  connection_check_state_ = ConnectionCheck::NONE;

  return submit_pending_requests();
}

std::expected<void, Error> Http2Session::submit_pending_requests() {
  for (auto dconn = dconns_.head; dconn; dconn = dconn->dlnext) {
    auto downstream = dconn->get_downstream();

    if (!downstream->get_request_pending() ||
        !downstream->request_submission_ready()) {
      continue;
    }

    auto &req = downstream->request();
    if (req.connect_proto != ConnectProto::NONE && !settings_recved_) {
      continue;
    }

    auto upstream = downstream->get_upstream();

    if (!dconn->push_request_headers()) {
      if (log_enabled(INFO)) {
        Log{INFO, this} << "backend request failed";
      }

      if (auto rv = upstream->on_downstream_abort_request(downstream, 400);
          !rv) {
        return rv;
      }

      continue;
    }

    if (auto rv = upstream->resume_read(SHRPX_NO_BUFFER, downstream, 0); !rv) {
      return rv;
    }
  }

  return {};
}

void Http2Session::set_connection_check_state(ConnectionCheck state) {
  connection_check_state_ = state;
}

ConnectionCheck Http2Session::get_connection_check_state() const {
  return connection_check_state_;
}

std::expected<void, Error> Http2Session::connected() {
  auto sock_error = util::get_socket_error(conn_.fd);
  if (sock_error != 0) {
    Log{WARN, this} << "Backend connect failed; addr="
                    << util::to_numeric_addr(raddr_)
                    << ": errno=" << sock_error;

    downstream_failure(addr_, raddr_);

    return std::unexpected{Error::CONNECT_FAIL};
  }

  if (log_enabled(INFO)) {
    Log{INFO, this} << "Connection established";
  }

  // Reset timeout for write.  Previously, we set timeout for connect.
  conn_.wt.repeat = group_->shared_addr->timeout.write;
  ev_timer_again(conn_.loop, &conn_.wt);

  conn_.rlimit.startw();
  conn_.again_rt();

  read_ = &Http2Session::read_clear;
  write_ = &Http2Session::write_clear;

  if (state_ == Http2SessionState::PROXY_CONNECTING) {
    return do_write();
  }

  if (conn_.tls.ssl) {
    read_ = &Http2Session::tls_handshake;
    write_ = &Http2Session::tls_handshake;

    return do_write();
  }

  if (auto rv = connection_made(); !rv) {
    state_ = Http2SessionState::CONNECT_FAILING;
    return rv;
  }

  return {};
}

std::expected<void, Error> Http2Session::read_clear() {
  conn_.last_read = std::chrono::steady_clock::now();

  std::array<uint8_t, 16_k> rawbuf;
  auto buf = std::span{rawbuf};

  for (;;) {
    auto maybe_data = conn_.read_clear(buf);
    if (!maybe_data) {
      return std::unexpected{maybe_data.error()};
    }

    auto data = *maybe_data;
    if (data.empty()) {
      return write_clear();
    }

    if (auto rv = on_read(data); !rv) {
      return rv;
    }
  }
}

std::expected<void, Error> Http2Session::write_clear() {
  conn_.last_read = std::chrono::steady_clock::now();

  std::array<struct iovec, MAX_WR_IOVCNT> iovbuf;

  for (;;) {
    auto iov = wb_.riovec(iovbuf);
    if (iov.empty()) {
      if (auto rv = on_write(); !rv) {
        return rv;
      }

      iov = wb_.riovec(iovbuf);
      if (iov.empty()) {
        break;
      }
    }

    auto maybe_nwrite = conn_.writev_clear(iov);
    if (!maybe_nwrite) {
      // We may have pending data in receive buffer which may contain
      // part of response body.  So keep reading.  Invoke read event
      // to get read(2) error just in case.
      ev_feed_event(conn_.loop, &conn_.rev, EV_READ);
      write_ = &Http2Session::write_void;
      break;
    }

    auto nwrite = *maybe_nwrite;
    if (nwrite == 0) {
      return {};
    }

    wb_.drain(nwrite);
  }

  conn_.wlimit.stopw();
  ev_timer_stop(conn_.loop, &conn_.wt);

  return {};
}

std::expected<void, Error> Http2Session::tls_handshake() {
  conn_.last_read = std::chrono::steady_clock::now();

  ERR_clear_error();

  if (auto rv = conn_.tls_handshake(); !rv) {
    if (rv.error() == Error::TLS_HANDSHAKE_INPROGRESS) {
      return {};
    }

    downstream_failure(addr_, raddr_);

    return rv;
  }

  if (log_enabled(INFO)) {
    Log{INFO, this} << "SSL/TLS handshake completed";
  }

  if (!get_config()->tls.insecure) {
    if (auto rv = tls::check_cert(conn_.tls.ssl, addr_, raddr_); !rv) {
      downstream_failure(addr_, raddr_);

      return rv;
    }
  }

  read_ = &Http2Session::read_tls;
  write_ = &Http2Session::write_tls;

  if (auto rv = connection_made(); !rv) {
    state_ = Http2SessionState::CONNECT_FAILING;
    return rv;
  }

  return {};
}

std::expected<void, Error> Http2Session::read_tls() {
  conn_.last_read = std::chrono::steady_clock::now();

  std::array<uint8_t, 16_k> rawbuf;
  auto buf = std::span{rawbuf};

  ERR_clear_error();

  for (;;) {
    auto maybe_data = conn_.read_tls(buf);
    if (!maybe_data) {
      return std::unexpected{maybe_data.error()};
    }

    auto data = *maybe_data;
    if (data.empty()) {
      return write_tls();
    }

    if (auto rv = on_read(data); !rv) {
      return rv;
    }
  }
}

std::expected<void, Error> Http2Session::write_tls() {
  conn_.last_read = std::chrono::steady_clock::now();

  ERR_clear_error();

  for (;;) {
    auto data = wb_.peek();
    if (data.empty()) {
      if (auto rv = on_write(); !rv) {
        return rv;
      }

      data = wb_.peek();
      if (data.empty()) {
        conn_.start_tls_write_idle();
        break;
      }
    }

    auto maybe_nwrite = conn_.write_tls(data);
    if (!maybe_nwrite) {
      // We may have pending data in receive buffer which may contain
      // part of response body.  So keep reading.  Invoke read event
      // to get read(2) error just in case.
      ev_feed_event(conn_.loop, &conn_.rev, EV_READ);
      write_ = &Http2Session::write_void;
      break;
    }

    auto nwrite = *maybe_nwrite;
    if (nwrite == 0) {
      return {};
    }

    wb_.drain(nwrite);
  }

  conn_.wlimit.stopw();
  ev_timer_stop(conn_.loop, &conn_.wt);

  return {};
}

std::expected<void, Error> Http2Session::write_void() {
  conn_.wlimit.stopw();
  return {};
}

bool Http2Session::should_hard_fail() const {
  switch (state_) {
  case Http2SessionState::PROXY_CONNECTING:
  case Http2SessionState::PROXY_FAILED:
    return true;
  case Http2SessionState::DISCONNECTED: {
    const auto &proxy = get_config()->downstream_http_proxy;
    return !proxy.host.empty();
  }
  default:
    return false;
  }
}

DownstreamAddr *Http2Session::get_addr() const { return addr_; }

size_t Http2Session::get_num_dconns() const { return dconns_.size(); }

bool Http2Session::max_concurrency_reached(size_t extra) const {
  if (!h2conn_) {
    return dconns_.size() + extra >= 100;
  }

  // If session does not allow further requests, it effectively means
  // that maximum concurrency is reached.
  return nghttp2_conn_get_streams_left(h2conn_) == 0 ||
         dconns_.size() + extra >=
           nghttp2_conn_get_remote_settings(h2conn_)->max_concurrent_streams;
}

const std::shared_ptr<DownstreamAddrGroup> &
Http2Session::get_downstream_addr_group() const {
  return group_;
}

void Http2Session::add_to_extra_freelist() {
  if (freelist_zone_ != FreelistZone::NONE) {
    return;
  }

  if (log_enabled(INFO)) {
    Log{INFO, this} << "Append to http2_extra_freelist, addr=" << addr_
                    << ", freelist.size=" << addr_->http2_extra_freelist.size();
  }

  freelist_zone_ = FreelistZone::EXTRA;
  addr_->http2_extra_freelist.append(this);
}

void Http2Session::remove_from_freelist() {
  switch (freelist_zone_) {
  case FreelistZone::NONE:
    return;
  case FreelistZone::EXTRA:
    if (log_enabled(INFO)) {
      Log{INFO, this} << "Remove from http2_extra_freelist, addr=" << addr_
                      << ", freelist.size="
                      << addr_->http2_extra_freelist.size();
    }
    addr_->http2_extra_freelist.remove(this);
    break;
  case FreelistZone::GONE:
    return;
  }

  freelist_zone_ = FreelistZone::NONE;
}

void Http2Session::exclude_from_scheduling() {
  remove_from_freelist();
  freelist_zone_ = FreelistZone::GONE;
}

DefaultMemchunks *Http2Session::get_request_buf() { return &wb_; }

void Http2Session::on_timeout() {
  switch (state_) {
  case Http2SessionState::PROXY_CONNECTING: {
    auto worker_blocker = worker_->get_connect_blocker();
    worker_blocker->on_failure();
    break;
  }
  case Http2SessionState::CONNECTING:
    Log{WARN, this} << "Connect time out; addr="
                    << util::to_numeric_addr(raddr_);

    downstream_failure(addr_, raddr_);
    break;
  default:
    break;
  }
}

void Http2Session::check_retire() {
  if (!group_->retired) {
    return;
  }

  ev_prepare_stop(conn_.loop, &prep_);

  if (!h2conn_) {
    return;
  }

  nghttp2_conn_shutdown(h2conn_);

  signal_write();
}

const Address *Http2Session::get_raddr() const { return raddr_; }

std::expected<void, Error>
Http2Session::on_settings_received(const nghttp2_proto_settings *settings) {
  // TODO This effectively disallows nghttpx to change its behaviour
  // based on the 2nd SETTINGS.
  if (settings_recved_) {
    return {};
  }

  settings_recved_ = true;
  allow_connect_proto_ = settings->enable_connect_protocol;

  return submit_pending_requests();
}

bool Http2Session::get_allow_connect_proto() const {
  return allow_connect_proto_;
}

} // namespace shrpx
