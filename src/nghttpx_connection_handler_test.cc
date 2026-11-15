/*
 * nghttp2
 *
 * Copyright (c) 2026 nghttp2 contributors
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
#include "nghttpx_connection_handler_test.h"

#include "nghttpx_connection_handler.h"

namespace nghttpx {

namespace {
const MunitTest tests[]{
#ifdef ENABLE_HTTP3
  munit_void_test(
    test_nghttpx_connection_handler_match_quic_lingering_worker_process_worker_id),
#endif // defined(ENABLE_HTTP3)
  munit_test_end(),
};
} // namespace

const MunitSuite connection_handler_suite{
  .prefix = "/connection_handler",
  .tests = tests,
};

#ifdef ENABLE_HTTP3
void test_nghttpx_connection_handler_match_quic_lingering_worker_process_worker_id() {
  auto loop = ev_loop_new(0);

  assert_not_null(loop);

  {
    std::mt19937_64 gen;
    ConnectionHandler handler{loop, gen};
    auto match = [&handler](const WorkerID &wid) {
      return handler.match_quic_lingering_worker_process_worker_id(wid);
    };
    static constexpr auto first = WorkerID{
      .server = 1,
      .worker_process = 2,
    };
    static constexpr auto second = WorkerID{
      .server = 1,
      .worker_process = 3,
    };
    static constexpr auto second_thread = WorkerID{
      .server = 1,
      .worker_process = 3,
      .thread = 1,
    };

    assert_err(Error::ENTITY_NOT_FOUND, match(first));

    handler.set_quic_lingering_worker_processes({
      QUICLingeringWorkerProcess{{first}, -1},
      QUICLingeringWorkerProcess{{second, second_thread}, -1},
    });

    auto first_match = match(first);

    assert_ok(first_match);
    assert_true(first == (*first_match)->worker_ids[0]);

    // Skip the first process and accept index zero in the second one.
    auto second_match = match(second);

    assert_ok(second_match);
    assert_true(second == (*second_match)->worker_ids[0]);
    assert_ok_eq(*second_match, match(second_thread));

    assert_err(Error::ENTITY_NOT_FOUND, match(WorkerID{
                                          .server = 9,
                                          .worker_process = 3,
                                        }));
    assert_err(Error::ENTITY_NOT_FOUND, match(WorkerID{
                                          .server = 1,
                                          .worker_process = 9,
                                        }));
    assert_err(Error::ENTITY_NOT_FOUND, match(WorkerID{
                                          .server = 1,
                                          .worker_process = 3,
                                          .thread = 2,
                                        }));
  }

  ev_loop_destroy(loop);
}
#endif // defined(ENABLE_HTTP3)

} // namespace nghttpx
