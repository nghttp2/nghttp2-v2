.. program:: h2load

h2load - HTTP/2 benchmarking tool - HOW-TO
==========================================

:doc:`h2load.1` is benchmarking tool for HTTP/2 and HTTP/1.1.  It
supports SSL/TLS and clear text for all supported protocols.

Compiling from source
---------------------

h2load is compiled alongside nghttp2 and requires that the
``--enable-app`` flag is passed to ``./configure`` and `required
dependencies <https://github.com/nghttp2/nghttp2#requirements>`_ are
available during compilation. For details on compiling, see `nghttp2:
Building from Git
<https://github.com/nghttp2/nghttp2#building-from-git>`_.

Basic Usage
-----------

In order to set benchmark settings, specify following 3 options.

:option:`-n`
    The number of total requests.  Default: 1

:option:`-c`
    The number of concurrent clients.  Default: 1

:option:`-m`
   The max concurrent streams to issue per client.  Default: 1

For SSL/TLS connection, the protocol will be negotiated via ALPN.  You
can set specific protocols in :option:`--alpn-list` option.  For
cleartext connection, the default protocol is HTTP/2.  To change the
protocol in cleartext connection, use :option:`--no-tls-proto` option.
For convenience, :option:`--h1` option forces HTTP/1.1 for both
cleartext and SSL/TLS connections.

Here is a command-line to perform benchmark to URI \https://localhost
using total 100000 requests, 100 concurrent clients and 10 max
concurrent streams:

.. code-block:: text

    $ h2load -n100000 -c100 -m10 https://localhost

The benchmarking result looks like this:

.. code-block:: text

    finished in 10.04s, 1022070.00 req/s, 1002.99MB/s
    requests: 10220700 total, 10230700 started, 10220700 done, 10220700 succeeded, 0 failed, 0 errored, 0 timeout
    status codes: 10220700 2xx, 0 3xx, 0 4xx, 0 5xx
    traffic: 9.79GB (10517133900) total, 107.25MB (112458300) headers (space savings 93.08%), 9.52GB (10220700000) data
		     min         max         median      p95         p99         mean        sd         +/- sd
    request     :     2.77ms     29.70ms      9.75ms      9.86ms     11.78ms      9.76ms       722us    97.90%
    connect     :    17.58ms     90.77ms     43.50ms     88.58ms     90.77ms     48.64ms     22.90ms    54.00%
    TTFB        :    30.04ms    108.93ms     61.32ms    108.51ms    108.93ms     67.34ms     27.07ms    54.00%
    req/s       :   10168.16    10269.11    10224.41    10259.08    10269.11    10219.57       27.99    47.00%

See the h2load manual page :ref:`h2load-1-output` section for the
explanation of the above numbers.

Timing-based load-testing
-------------------------

This method performs load-testing in terms of a given duration instead
of a pre-defined number of requests. The new option
:option:`--duration` specifies how long the load-testing takes. For
example, ``--duration=10`` makes h2load perform load-testing against a
server for 10 seconds. You can also specify a “warming-up” period
with :option:`--warm-up-time`. If :option:`--duration` is used,
:option:`-n` option is ignored.

The following command performs load-testing for 10 seconds after 5
seconds warming up period:

.. code-block:: text

    $ h2load -c100 -m100 --duration=10 --warm-up-time=5 https://localhost

Flow Control
------------

HTTP/2 has flow control and it may affect benchmarking results.  By
default, h2load uses large enough flow control window, which
effectively disables flow control.  To adjust receiver flow control
window size, there are following options:

:option:`-w`
   Sets  the stream  level  initial  window size  to
   (2**<N>)-1.

:option:`-W`
   Sets the connection level  initial window size to
   (2**<N>)-1.

Multi-Threading
---------------

Sometimes benchmarking client itself becomes a bottleneck.  To remedy
this situation, use :option:`-t` option to specify the number of native
thread to use.

:option:`-t`
    The number of native threads. Default: 1

Selecting protocol for clear text
---------------------------------

By default, if \http:// URI is given, HTTP/2 protocol is used.  To
change the protocol to use for clear text, use :option:`-p` option.

Multiple URIs
-------------

If multiple URIs are specified, they are used in round robin manner.

.. note::

    Please note that h2load uses scheme, host and port in the first URI
    and ignores those parts in the rest of the URIs.

UNIX domain socket
------------------

To request against UNIX domain socket, use :option:`--base-uri`, and
specify ``unix:`` followed by the path to UNIX domain socket.  For
example, if UNIX domain socket is ``/tmp/nghttpx.sock``, use
``--base-uri=unix:/tmp/nghttpx.sock``.  h2load uses scheme, host and
port in the first URI in command-line or input file.

HTTP/3
------

h2load supports HTTP/3 if it is built with HTTP/3 enabled.  HTTP/3
support is experimental.

In order to send HTTP/3 request, use :option:`--h3`, or specify ``h3``
to :option:`--alpn-list`.
