The nghttp2 version 2 programmers' guide
========================================

This document describes the basic usage of the nghttp2 version 2
library and common pitfalls which programmers might encounter.

Initialization
--------------

The :type:`nghttp2_conn` represents a single HTTP/2 connection.  For a
client, use `nghttp2_conn_client_new` to create the object.  For a
server, use `nghttp2_conn_server_new`.

Both functions take the common parameters: :type:`nghttp2_callbacks`,
:type:`nghttp2_settings`, :type:`nghttp2_mem`, and an opaque pointer,
*user_data*.

The :type:`nghttp2_callbacks` stores the callbacks that are invoked
during the life cycle of an HTTP/2 connection.  Only
:member:`nghttp2_callbacks.rand` is required to be set.  The other
callbacks are all optional.  Not specifying any optional callbacks
makes the library rather useless.  Here is the minimal set of
callbacks that are useful:

- :member:`nghttp2_callbacks.recv_settings`: Called when a SETTINGS
  frame is received.
- :member:`nghttp2_callbacks.begin_headers`: Called when the local
  endpoint detects that HTTP header fields from the remote endpoint
  have started.
- :member:`nghttp2_callbacks.recv_header`: Called when an HTTP header
  field is received.
- :member:`nghttp2_callbacks.end_headers`: Called when the local
  endpoint detects that HTTP header fields from the remote endpoint
  have ended.
- :member:`nghttp2_callbacks.recv_data`: Called when the local
  endpoint receives the request or response body.
- :member:`nghttp2_callbacks.remote_end_stream`: Called when the local
  endpoint detects that the receiving side of the stream has closed.
- :member:`nghttp2_callbacks.stream_close`: Called when a stream is
  closed, that is, both sides of the stream have been closed.
- :member:`nghttp2_callbacks.write_stream_data_offset`: Called when
  the local endpoint finishes sending the given data.  This is a
  rather important callback when :type:`nghttp2_data_reader` is used
  to send the request or response body.  This notifies the application
  which portion of data can be freed.

The :type:`nghttp2_settings` stores the connection settings.  It
should be initialized by `nghttp2_settings_default`, and then the
application can specify its own values.
:member:`nghttp2_settings.initial_ts` should be set to the current
timestamp.  For a server,
:member:`nghttp2_settings.max_concurrent_streams_remote` should be set
to the maximum concurrent streams it can accept, say, 100.  To set up
the debug logging of the nghttp2 library, set
:type:`nghttp2_settings.log_write`.  It is recommended to set
:type:`nghttp2_settings.conn_id` to distinguish each connection in the
log output.

The :type:`nghttp2_mem` is a memory allocator.  All memory allocations
are done with this object.  If *mem* is ``NULL``, the default memory
allocator returned by `nghttp2_mem_default` is used.

After the connection is closed, call `nghttp2_conn_del` to deallocate
the resources.

Reading HTTP/2 stream data
--------------------------

To read HTTP/2 stream data, call `nghttp2_conn_read`.  It consumes all
input data.  If it returns a negative error code, the underlying
connection should be closed without calling any nghttp2 API for
:type:`nghttp2_conn`.

Writing HTTP/2 stream data
--------------------------

To write HTTP/2 stream data, call `nghttp2_conn_write`.  It is
generally recommended to pass a 16KiB buffer to the function so that
it can fill the maximum TLS record.  If it returns a negative error
code, the underlying connection should be closed without calling any
nghttp2 API for :type:`nghttp2_conn`.

If :macro:`NGHTTP2_ERR_CLOSING` is returned, it means one of the
following:

- The graceful shutdown has completed.
- A connection error has occurred and the connection should be
  closed.
- The application called `nghttp2_conn_terminate` and a GOAWAY frame
  was sent.

In any case, the connection can be closed.

Sending the HTTP message body
-----------------------------

To send the HTTP message body in HTTP requests and responses,
:type:`nghttp2_data_reader` is used.
:type:`nghttp2_data_reader.read_data` pulls data from the application.

The callback provides a writable :type:`nghttp2_vec` array of *veccnt*
elements.  If there is data to send, the application should populate
them with data and return the number of elements it fills.  If this is
the end of the message body, set :macro:`NGHTTP2_READ_DATA_FLAG_EOF`,
which signifies the end of the stream.  If, for some reason, the
application does not want to end the sending side of the stream at
this moment, for example, if it plans to send trailers later, also set
:macro:`NGHTTP2_READ_DATA_FLAG_NO_END_STREAM`.

If there is no data to send at this point and it is not the end of the
message body, return :macro:`NGHTTP2_ERR_WOULDBLOCK`.  When data
becomes available later, call `nghttp2_conn_resume_stream` so that
:type:`nghttp2_conn` can schedule this stream for transmission.

Returning 0 from the callback is only valid when
:macro:`NGHTTP2_READ_DATA_FLAG_EOF` is set.  The following cases are
treated as errors:

- 0 is returned without :macro:`NGHTTP2_READ_DATA_FLAG_EOF` set.
- The sum of the data filled in the :type:`nghttp2_vec` array is 0,
  and :macro:`NGHTTP2_READ_DATA_FLAG_EOF` is not set.

The memory region passed to the :type:`nghttp2_vec` array in this
callback must be retained until that portion of the data is written to
the underlying stream.  This is notified via
:member:`nghttp2_callbacks.write_stream_data_offset`.  The callback is
not called if the stream or the connection is closed before sending
data.

Stream life cycle
-----------------

The client can create a stream by calling
`nghttp2_conn_submit_request`.  For a server, a stream is created
implicitly when it receives the first HEADERS frame for the stream.

A stream is closed when both sides of the stream are closed.  That is,
sending all request or response messages, and receiving all response
or request messages.

To shut down (cancel, or reset) a stream abruptly, call
`nghttp2_conn_shutdown_stream`.  If it is called or a RST_STREAM frame
is received from the remote endpoint, the stream enters the closing
state and is eventually deleted.  If `nghttp2_conn_shutdown_stream` is
called inside a user callback, any further stream-based callbacks are
not called, except for :member:`nghttp2_callbacks.stream_close` and
:member:`nghttp2_callbacks.write_stream_data_offset`.  For example, if
the application calls `nghttp2_conn_shutdown_stream` inside
:member:`nghttp2_callbacks.recv_header`, any header fields that follow
the current field are not notified by the callback, and
:member:`nghttp2_callbacks.end_headers` is also not called.

Flow control
------------

`nghttp2_conn_extend_max_stream_offset` extends the maximum data
offset for the given stream by the given size.
`nghttp2_conn_extend_max_offset` extends the maximum data offset for
the connection by the given size.

In general, if the application receives N bytes of request or response
body via :member:`nghttp2_callbacks.recv_data`, it should call
`nghttp2_conn_extend_max_stream_offset` and
`nghttp2_conn_extend_max_offset` with N as the *datalen* parameter.

Shutting down the connection
----------------------------

To shut down the connection abruptly, call `nghttp2_conn_terminate`.
This schedules a GOAWAY frame, and after sending it,
`nghttp2_conn_write` returns :macro:`NGHTTP2_ERR_CLOSING`.  Then,
close the underlying connection.

To perform a graceful shutdown, a server first calls
`nghttp2_conn_submit_shutdown_notice`.  That tells the client that a
shutdown is imminent.  Then, after a couple of RTTs, call
`nghttp2_conn_shutdown`, which starts the graceful shutdown period.
In this period, all new streams are refused.  After all existing
streams have been processed, `nghttp2_conn_write` returns
:macro:`NGHTTP2_ERR_CLOSING`.  Then, close the underlying connection.

Timeout
-------

`nghttp2_conn_get_expiry` returns the next timepoint at which the
application should set the timer.  After the timer fires, call
`nghttp2_conn_handle_expiry`.  If it returns a negative error code,
close the underlying connection.  It handles the SETTINGS ACK timeout.
After `nghttp2_conn_handle_expiry`, schedule `nghttp2_conn_write`.  If
`nghttp2_conn_get_expiry` returns ``UINT64_MAX``, stop the timer.
