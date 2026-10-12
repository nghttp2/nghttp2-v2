The migration guide from nghttp2 version 1
==========================================

This document provides general guidance to migrate from `nghttp2
version 1 <https://github.com/nghttp2/nghttp2>`_ (nghttp2 v1).
Because nghttp2 version 2 (nghttp2 v2) does not provide all
functionalities that its predecessor offers, existing nghttp2 v1
applications should decide whether to migrate to v2 by removing those
functionalities or to keep using v1.

The primary object initialization
---------------------------------

The primary object is now called :type:`nghttp2_conn` instead of
``nghttp2_session``.

The set of callbacks is now exposed as :type:`nghttp2_callbacks`, a
plain C struct, rather than the opaque object
``nghttp2_session_callbacks``.  Zero-initialize the struct and set the
required :member:`nghttp2_callbacks.rand` callback before creating a
connection.  Consider the designated initializers if available.

Connection options are now exposed as :type:`nghttp2_settings`, a
plain C struct, rather than the opaque object ``nghttp2_option``.
Initialize it with `nghttp2_settings_default`, set
:member:`nghttp2_settings.initial_ts` to the current timestamp, and
then override any application-specific settings before creating the
connection.

Stream identifier
-----------------

The stream ID is now of type ``int64_t``, similar to more modern HTTP
versions like HTTP/3. The value range is still subject to HTTP/2
limits.

Callbacks
---------

Callbacks are now simplified and organized for connection and HTTP
events rather than frame-based events.

For HTTP header field events, subscribe to:

- :member:`nghttp2_callbacks.begin_headers`
- :member:`nghttp2_callbacks.recv_header`
- :member:`nghttp2_callbacks.end_headers`

For HTTP message body events, subscribe to:

- :member:`nghttp2_callbacks.recv_data`

For HTTP trailer field events, subscribe to:

- :member:`nghttp2_callbacks.begin_trailers`
- :member:`nghttp2_callbacks.recv_trailer`
- :member:`nghttp2_callbacks.end_trailers`

For end-of-stream events, subscribe to:

- :member:`nghttp2_callbacks.remote_end_stream` when the receiving
  side of the stream closes.
- :member:`nghttp2_callbacks.local_end_stream` when the sending side
  of the stream closes

For the stream closure event, subscribe to:

- :member:`nghttp2_callbacks.stream_close`

SETTINGS acknowledgement timeout
--------------------------------

In v1, the application uses frame-based callbacks to detect SETTINGS
transmission and its ACK reception.

In v2, `nghttp2_conn_get_expiry` returns the next library-managed
expiry deadline, currently the SETTINGS acknowledgement timeout, or
``UINT64_MAX`` if no deadline exists.  The application sets or stops
its timer accordingly.  When the timer fires, call
`nghttp2_conn_handle_expiry`; if it returns a negative error code,
close the connection.

PING frames
-----------

In v1, the reception of PING with ACK is detected by a frame-based
callback.

In v2, there is a dedicated callback,
:member:`nghttp2_callbacks.recv_ping_ack`, which includes the
calculated RTT.

Resetting a stream
------------------

In v2, resetting a stream is done via `nghttp2_conn_shutdown_stream`.

Transmission of SETTINGS
------------------------

In v2, the application can send SETTINGS just once.  The values of
SETTINGS are synthesized by :type:`nghttp2_settings`.

Flow control
------------

In v1, flow control is managed automatically by the library.  It can
optionally be disabled, allowing the application to manually increase
the maximum data offset for the remote endpoint to send more data.

In v2, automatic management is removed.  The application must increase
the maximum offset by calling `nghttp2_conn_extend_max_stream_offset`
and `nghttp2_conn_extend_max_offset`.

Reading HTTP/2 stream data
--------------------------

In v1, there are two ways to read data: reading via callback, or
passing a buffer filled with read data to the API function.

In v2, `nghttp2_conn_read` is solely used.

Writing HTTP/2 stream data
--------------------------

In v1, there are two ways to write data: writing via callback, or
getting a pointer to the internal frame buffer and its length to send.

In v2, `nghttp2_conn_write` is solely used.  It takes a buffer
provided by the application (usually 16384 bytes, matching the TLS
record size to optimize transfer efficiency).  It also writes multiple
frames at once, so the application no longer needs to copy each frame
into its own buffer as it did in v1.

Abruptly closing the connection
-------------------------------

Call `nghttp2_conn_terminate` to initiate an abrupt shutdown.  It
schedules a GOAWAY frame; continue calling `nghttp2_conn_write`, and
close the underlying connection after it returns
:macro:`NGHTTP2_ERR_CLOSING` or any other error.

The end of the connection
-------------------------

In v1, you used ``nghttp2_session_want_read`` and
``nghttp2_session_want_write`` to know when to close the connection.

In v2, simply close the connection when `nghttp2_conn_read` or
`nghttp2_conn_write` returns an error.

Removed features
----------------

In v2, the following features are removed:

- Server push
- Padding added to HEADERS and DATA frames
- Sending multiple SETTINGS frames
- Pausing reception of HTTP fields and data chunks
- Sending the custom extension frames
- And possibly more

Sending HTTP message data
-------------------------

In v1, the application writes data into a buffer offered by the
library.  Optionally, the application can write data directly into its
own buffer, with both approaches limited by the current flow control
credit.

In v2, the application passes references to a data vector to the
library via the callback :member:`nghttp2_data_reader.read_data`.
After the library writes a portion of the passed data, it calls
:member:`nghttp2_callbacks.write_stream_data_offset` for that data.
The application must keep the data intact until notified by the
callback.  If the stream or connection closes before queued data is
written, the callback is not invoked for that data, so the application
must release it while handling the stream closure.  If the connection
closes first, :member:`nghttp2_callbacks.stream_close` is not invoked
for its remaining streams; release all of their retained data as part
of connection teardown.

Sending the custom SETTINGS
---------------------------

In v1, the application can send the custom SETTINGS via
``nghttp2_submit_settings``.

In v2, the application can specify them in
:member:`nghttp2_settings.extra_settings` and its length in
:member:`nghttp2_settings.extra_settingslen`.

Timestamp
---------

In v2, many functions require the current timestamp to take
countermeasures against certain attacks.  The timestamps must be
:type:`nghttp2_tstamp` values, which have nanosecond resolution.

The built-in logger
-------------------

nghttp2 v2 offers the built-in logger.  See
:member:`nghttp2_settings.log_write`.
