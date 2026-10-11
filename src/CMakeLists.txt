file(GLOB c_sources *.c)
set_source_files_properties(${c_sources} PROPERTIES
  COMPILE_FLAGS "${WARNCFLAGS}")
file(GLOB cxx_sources *.cc)
set_source_files_properties(${cxx_sources} PROPERTIES
  COMPILE_FLAGS "${WARNCXXFLAGS} ${CXX1XCXXFLAGS}")

include_directories(
  "${CMAKE_CURRENT_SOURCE_DIR}/includes"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third-party/urlparse"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third-party/llhttp/include"

  ${JEMALLOC_INCLUDE_DIRS}
  ${LIBXML2_INCLUDE_DIRS}
  ${LIBEV_INCLUDE_DIRS}
  ${LIBNGHTTP3_INCLUDE_DIRS}
  ${LIBNGTCP2_INCLUDE_DIRS}
  ${LIBNGTCP2_CRYPTO_QUICTLS_INCLUDE_DIRS}
  ${LIBNGTCP2_CRYPTO_LIBRESSL_INCLUDE_DIRS}
  ${LIBNGTCP2_CRYPTO_WOLFSSL_INCLUDE_DIRS}
  ${LIBNGTCP2_CRYPTO_OSSL_INCLUDE_DIRS}
  ${OPENSSL_INCLUDE_DIRS}
  ${WOLFSSL_INCLUDE_DIRS}
  ${LIBCARES_INCLUDE_DIRS}
  ${JANSSON_INCLUDE_DIRS}
  ${ZLIB_INCLUDE_DIRS}
  ${LIBBPF_INCLUDE_DIRS}
  ${LIBBROTLIENC_INCLUDE_DIRS}
  ${LIBBROTLIDEC_INCLUDE_DIRS}
)

# XXX per-target?
link_libraries(
  nghttp2v2
  ${JEMALLOC_LIBRARIES}
  ${LIBXML2_LIBRARIES}
  ${LIBEV_LIBRARIES}
  ${LIBNGHTTP3_LIBRARIES}
  ${LIBNGTCP2_LIBRARIES}
  ${LIBNGTCP2_CRYPTO_QUICTLS_LIBRARIES}
  ${LIBNGTCP2_CRYPTO_LIBRESSL_LIBRARIES}
  ${LIBNGTCP2_CRYPTO_WOLFSSL_LIBRARIES}
  ${LIBNGTCP2_CRYPTO_OSSL_LIBRARIES}
  ${OPENSSL_LIBRARIES}
  ${WOLFSSL_LIBRARIES}
  ${LIBCARES_LIBRARIES}
  ${JANSSON_LIBRARIES}
  ${ZLIB_LIBRARIES}
  ${APP_LIBRARIES}
  ${LIBBPF_LIBRARIES}
  ${LIBBROTLIENC_LIBRARIES}
  ${LIBBROTLIDEC_LIBRARIES}
)

if(ENABLE_APP)
  set(HELPER_OBJECTS
    util.cc
    http2.cc timegm.c app_helper.cc nghttp2_gzip.c
    network.cc
  )

  # nghttp client
  set(NGHTTP_SOURCES
    ${HELPER_OBJECTS}
    nghttp.cc
    tls.cc
  )
  if(HAVE_LIBXML2)
    list(APPEND NGHTTP_SOURCES HtmlParser.cc)
  endif()

  # nghttpd
  set(NGHTTPD_SOURCES
    ${HELPER_OBJECTS}
    nghttpd.cc
    tls.cc
    HttpServer.cc
  )

  # h2load
  set(H2LOAD_SOURCES
    util.cc
    http2.cc h2load.cc
    timegm.c
    app_helper.cc
    tls.cc
    network.cc
    h2load_http2_session.cc
    h2load_http1_session.cc
  )
  if(ENABLE_HTTP3)
    list(APPEND H2LOAD_SOURCES
      h2load_http3_session.cc
      h2load_quic.cc
    )
  endif()

  # Common libnhttpx sources (used for nghttpx and unit tests)
  set(NGHTTPX_SRCS
    util.cc http2.cc timegm.c
    app_helper.cc
    tls.cc
    network.cc
    nghttpx_config.cc
    nghttpx_accept_handler.cc
    nghttpx_connection_handler.cc
    nghttpx_client_handler.cc
    nghttpx_http2_upstream.cc
    nghttpx_https_upstream.cc
    nghttpx_downstream.cc
    nghttpx_downstream_connection.cc
    nghttpx_http_downstream_connection.cc
    nghttpx_http2_downstream_connection.cc
    nghttpx_http2_session.cc
    nghttpx_downstream_queue.cc
    nghttpx_log.cc
    nghttpx_http.cc
    nghttpx_io_control.cc
    nghttpx_tls.cc
    nghttpx_worker.cc
    nghttpx_log_config.cc
    nghttpx_connect_blocker.cc
    nghttpx_live_check.cc
    nghttpx_downstream_connection_pool.cc
    nghttpx_rate_limit.cc
    nghttpx_connection.cc
    nghttpx_memcached_dispatcher.cc
    nghttpx_memcached_connection.cc
    nghttpx_worker_process.cc
    nghttpx_signal.cc
    nghttpx_router.cc
    nghttpx_api_downstream_connection.cc
    nghttpx_health_monitor_downstream_connection.cc
    nghttpx_null_downstream_connection.cc
    nghttpx_dns_resolver.cc
    nghttpx_dual_dns_resolver.cc
    nghttpx_dns_tracker.cc
    xsi_strerror.c
  )
  if(HAVE_MRUBY)
    list(APPEND NGHTTPX_SRCS
      nghttpx_mruby.cc
      nghttpx_mruby_module.cc
      nghttpx_mruby_module_env.cc
      nghttpx_mruby_module_request.cc
      nghttpx_mruby_module_response.cc
    )
  endif()
  if(ENABLE_HTTP3)
    list(APPEND NGHTTPX_SRCS
     nghttpx_quic.cc
     nghttpx_quic_listener.cc
     nghttpx_quic_connection_handler.cc
     nghttpx_http3_upstream.cc
     http3.cc
     siphash.cc
    )
  endif()
  add_library(nghttpx_static STATIC ${NGHTTPX_SRCS})
  set_target_properties(nghttpx_static PROPERTIES ARCHIVE_OUTPUT_NAME nghttpx)

  set(NGHTTPX-bin_SOURCES
    nghttpx.cc
  )

  if(HAVE_SYSTEMD)
    target_link_libraries(nghttpx_static ${SYSTEMD_LIBRARIES})
    target_compile_definitions(nghttpx_static PUBLIC HAVE_LIBSYSTEMD)
    target_include_directories(nghttpx_static PUBLIC ${SYSTEMD_INCLUDE_DIRS})
  endif()

  if(HAVE_MRUBY)
    target_link_libraries(nghttpx_static mruby-lib)
    target_include_directories(nghttpx_static PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/../third-party/mruby/build/include)
  endif()

  if(HAVE_NEVERBLEED)
    target_link_libraries(nghttpx_static neverbleed)
  endif()

  if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME AND BUILD_TESTING)
    set(NGHTTPX_UNITTEST_SOURCES
      nghttpx-unittest.cc
      nghttpx_tls_test.cc
      nghttpx_downstream_test.cc
      nghttpx_config_test.cc
      nghttpx_worker_test.cc
      nghttpx_http_test.cc
      nghttpx_router_test.cc
      http2_test.cc
      util_test.cc
      nghttp2_gzip_test.c
      nghttp2_gzip.c
      buffer_test.cc
      memchunk_test.cc
      template_test.cc
      base64_test.cc
      network_test.cc
      allocator_test.cc
      ${CMAKE_SOURCE_DIR}/tests/munit/munit.c
    )
    if(ENABLE_HTTP3)
      list(APPEND NGHTTPX_UNITTEST_SOURCES siphash_test.cc)
    endif()
    add_executable(nghttpx-unittest EXCLUDE_FROM_ALL
      ${NGHTTPX_UNITTEST_SOURCES}
      $<TARGET_OBJECTS:llhttp>
      $<TARGET_OBJECTS:urlparse>
    )
    target_include_directories(nghttpx-unittest PRIVATE
      ${CMAKE_SOURCE_DIR}/tests/munit
    )
    target_compile_definitions(nghttpx-unittest
      PRIVATE "-DNGHTTP2_SRC_DIR=\"${CMAKE_SOURCE_DIR}/src\""
    )
    target_link_libraries(nghttpx-unittest nghttpx_static)
    if(HAVE_MRUBY)
      target_link_libraries(nghttpx-unittest mruby-lib)
    endif()
    if(HAVE_NEVERBLEED)
      target_link_libraries(nghttpx-unittest neverbleed)
    endif()

    add_test(nghttpx-unittest nghttpx-unittest)
    add_dependencies(check nghttpx-unittest)
  endif()

  add_executable(nghttp   ${NGHTTP_SOURCES}   $<TARGET_OBJECTS:llhttp>
    $<TARGET_OBJECTS:urlparse>
  )
  add_executable(nghttpd  ${NGHTTPD_SOURCES}  $<TARGET_OBJECTS:llhttp>
    $<TARGET_OBJECTS:urlparse>
  )
  add_executable(nghttpx  ${NGHTTPX-bin_SOURCES} $<TARGET_OBJECTS:llhttp>
    $<TARGET_OBJECTS:urlparse>
  )
  target_compile_definitions(nghttpx PRIVATE
    "-DPKGDATADIR=\"${PKGDATADIR}\""
    "-DPKGLIBDIR=\"${PKGLIBDIR}\""
  )
  target_link_libraries(nghttpx nghttpx_static)
  add_executable(h2load   ${H2LOAD_SOURCES}   $<TARGET_OBJECTS:llhttp>
    $<TARGET_OBJECTS:urlparse>
  )

  # install(TARGETS nghttp nghttpd nghttpx h2load)
endif()
