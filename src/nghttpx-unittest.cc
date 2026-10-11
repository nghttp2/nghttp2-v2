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
#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif // defined(HAVE_CONFIG_H)

#include "munit.h"

// include test cases' include files here
#include "nghttpx_tls_test.h"
#include "nghttpx_downstream_test.h"
#include "nghttpx_config_test.h"
#include "nghttpx_worker_test.h"
#include "http2_test.h"
#include "util_test.h"
#include "nghttp2_gzip_test.h"
#include "buffer_test.h"
#include "memchunk_test.h"
#include "template_test.h"
#include "nghttpx_http_test.h"
#include "base64_test.h"
#include "nghttpx_config.h"
#include "tls.h"
#include "nghttpx_router_test.h"
#include "nghttpx_log.h"
#include "network_test.h"
#ifdef ENABLE_HTTP3
#  include "siphash_test.h"
#endif // defined(ENABLE_HTTP3)
#include "allocator_test.h"

int main(int argc, char *argv[]) {
  nghttpx::create_config();

  const MunitSuite suites[] = {
    nghttpx::tls_suite,
    nghttpx::downstream_suite,
    nghttpx::config_suite,
    nghttpx::worker_suite,
    nghttpx::http_suite,
    nghttpx::router_suite,
    nghttpx::http2_suite,
    nghttpx::util_suite,
    gzip_suite,
    buffer_suite,
    memchunk_suite,
    template_suite,
    base64_suite,
    network_suite,
#ifdef ENABLE_HTTP3
    siphash_suite,
#endif // defined(ENABLE_HTTP3)
    allocator_suite,
    {},
  };
  const MunitSuite suite = {
    .prefix = "",
    .suites = suites,
    .iterations = 1,
  };

  return munit_suite_main(&suite, nullptr, argc, argv);
}
