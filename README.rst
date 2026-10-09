nghttp2 version 2 - HTTP/2 C Library
====================================

This is an implementation of the Hypertext Transfer Protocol version 2
(HTTP/2) in C.  This implementation is based on `RFC 9113
<https://datatracker.ietf.org/doc/html/rfc9113>`_

It is a complete rewrite of nghttp2 version 1, aiming for better
performance, security, and API ergonomics.

Documentation
-------------

`Online documentation <https://nghttp2.org/v2/>`_ is available.

Requirements
------------

Compiling the libnghttp2v2 library C source code requires a C11
compiler.

The following package is required to build the libnghttp2v2 library:

- pkg-config >= 0.20

If you need libnghttp2v2 (the C library) only, then the above packages
are all you need.  Use ``--enable-lib-only`` to ensure that only
libnghttp2v2 is built.  This avoids potential build errors related to
building bundled applications.

To build and run the application programs (``nghttp``, ``nghttpd``,
``nghttpx``, and ``h2load``) in the ``src`` directory, a C++23
compliant compiler is required.  The following packages are also
required:

- OpenSSL >= 1.1.1; or wolfSSL >= 5.7.0; or LibreSSL >= 3.8.1; or
  aws-lc >= 1.19.0; or BoringSSL
- libev >= 4.11
- zlib >= 1.2.3
- libc-ares >= 1.7.5

To enable the ``-a`` option (getting linked assets from the downloaded
resource) in ``nghttp``, the following package is required:

- libxml2 >= 2.6.26

To enable systemd support in nghttpx, the following package is
required:

- libsystemd-dev >= 209

To mitigate heap fragmentation in long-running server programs
(``nghttpd`` and ``nghttpx``), jemalloc is recommended:

- jemalloc

For BoringSSL or aws-lc builds, to enable :rfc:`8879` TLS Certificate
Compression in applications, the following library is required:

- libbrotli-dev >= 1.0.9

To enable mruby support for nghttpx, `mruby
<https://github.com/mruby/mruby>`_ is required.  We need to build
mruby with the C++ ABI explicitly turned on, and probably need other
mrgems; mruby is managed by git submodule under the third-party/mruby
directory.  Currently, mruby support for nghttpx is disabled by
default.  To enable mruby support, use the ``--with-mruby`` configure
option.  Note that at the time of this writing, the libmruby-dev and
mruby packages in Debian/Ubuntu are not usable for nghttp2, since they
do not enable the C++ ABI.  To build mruby, the following packages are
required:

- ruby
- bison

nghttpx supports `neverbleed <https://github.com/h2o/neverbleed>`_, a
privilege separation engine for OpenSSL.  In short, it minimizes the
risk of private key leakage when a serious bug like Heartbleed is
exploited.  neverbleed is disabled by default.  To enable it, use the
``--with-neverbleed`` configure option.

To enable experimental HTTP/3 support for h2load and nghttpx, the
following libraries are required:

- `quictls
  <https://github.com/quictls/openssl/tree/OpenSSL_1_1_1w+quic>`_; or
  wolfSSL; or LibreSSL (does not support 0RTT); or aws-lc; or
  `BoringSSL <https://boringssl.googlesource.com/boringssl/>`_ (commit
  3c6315e00ab02d7bc9b8922aff1f85d8f81ee130); or OpenSSL >= 3.5.0
- `ngtcp2 <https://github.com/ngtcp2/ngtcp2>`_ >= 1.23.0
- `nghttp3 <https://github.com/ngtcp2/nghttp3>`_ >= 1.17.0

Use the ``--enable-http3`` configure option to enable the HTTP/3
feature for h2load and nghttpx.

In order to build the optional eBPF program to direct an incoming QUIC
UDP datagram to the correct socket for nghttpx, the following
libraries are required:

- libbpf-dev >= 0.7.0

Use the ``--with-libbpf`` configure option to build the eBPF program.
libelf-dev is needed to build libbpf.

.. note::

   macOS users may need the ``--disable-threads`` configure option to
   disable multi-threading in nghttpd, nghttpx, and h2load to prevent
   them from crashing.  A patch is welcome to make multi-threading
   work on the macOS platform.

.. note::

   To compile the associated applications (nghttp, nghttpd, nghttpx,
   and h2load), you must use the ``--enable-app`` configure option and
   ensure that the requirements specified above are met.  Normally,
   the configure script checks the required dependencies to build
   these applications and enables ``--enable-app`` automatically, so
   you don't have to use it explicitly.  However, if you find that the
   applications were not built, using ``--enable-app`` may help
   identify the cause, such as a missing dependency.

.. note::

   In order to detect third-party libraries, pkg-config is used
   (however, we don't use pkg-config for some libraries, e.g., libev).
   By default, pkg-config searches for ``*.pc`` files in standard
   locations (e.g., /usr/lib/pkgconfig).  If it is necessary to use a
   ``*.pc`` file in a custom location, specify paths in the
   ``PKG_CONFIG_PATH`` environment variable and pass it to the
   configure script, like so:

   .. code-block:: text

       $ ./configure PKG_CONFIG_PATH=/path/to/pkgconfig

   For pkg-config-managed libraries, ``*_CFLAGS`` and ``*_LIBS``
   environment variables are defined (e.g., ``OPENSSL_CFLAGS``,
   ``OPENSSL_LIBS``).  Specifying a non-empty string for these
   variables completely overrides pkg-config.  In other words, if they
   are specified, pkg-config is not used for detection, and the user
   is responsible for specifying the correct values for these
   variables.  For a complete list of these variables, run
   ``./configure -h``.

Standards
---------

libnghttp2v2 implements the following HTTP/2 specification:

- `HTTP/2 <https://datatracker.ietf.org/doc/html/rfc9113>`_

Note that libnghttp2v2 does not implement server push, which is
largely regarded as useless.

The following HTTP/2 extensions are implemented:

- `Extensible Prioritization Scheme for HTTP
  <https://datatracker.ietf.org/doc/html/rfc9218>`_
- `Bootstrapping WebSockets with HTTP/2
  <https://datatracker.ietf.org/doc/html/rfc8441>`_

Applications
------------

There are 4 built-in applications:

- nghttp: HTTP/2 client with debugging capabilities
- nghttpd: HTTP/2 server
- nghttpx: HTTP/1/2/3 proxy
- h2load: HTTP/1/2/3 load testing tool

License
-------

The MIT License
