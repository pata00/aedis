
# Documentation

[TOC]

## Overview

Aedis is a high-level [Redis](https://redis.io/) client library
built on top of
[Asio](https://www.boost.org/doc/libs/release/doc/html/boost_asio.html).
Some of its distinctive features are

* Support for the latest version of the Redis communication protocol [RESP3](https://github.com/redis/redis-specifications/blob/master/protocol/RESP3.md).
* Support for STL containers, TLS and Redis sentinel.
* Serialization and deserialization of your own data types.
* Healthy checks, back pressure, cancellation and low latency.

In addition to that, Aedis hides most of the low-level Asio code away
from the user, which in the majority of the cases, will interact with
only three library entities

* `aedis::connection`: A connection to the Redis server.
* `aedis::resp3::request`: A container of Redis commands.
* `aedis::adapt()`: A function that adapts data structures to receive Redis responses.

The example below shows for example how to read Redis hashes in an
`std::map` using a coroutine, a short-lived connection and
cancellation

```cpp
net::awaitable<std::map<std::string, std::string>> retrieve_hashes(endpoint ep)
{
   connection conn{co_await net::this_coro::executor};

   request req;
   req.push("HGETALL", "hset-key");
   req.push("QUIT");

   std::tuple<
      std::map<std::string, std::string>,
      aedis::ignore> response;

   boost::system::error_code ec1, ec2;
   co_await (conn.async_run(ep, {}, redir(ec1)) || conn.async_exec(req, adapt(response), redir(ec2)));
   co_return std::move(std::get<0>(response));
}

```

In the next section we will see more details about connections,
requests and responses.

<a name="connection"></a>

### Connection

The `aedis::connection` is a class that provides async-only
communication with a Redis server by means of three member
functions

* `aedis::connection::async_run`: Establishes a connection and
  completes only when it is lost.
* `aedis::connection::async_exec`: Executes commands.
* `aedis::connection::async_receive`: Receives server-side pushes.

In general, these operations will be running concurrently in user
application, where, for example

1. **Connect**: One coroutine will call `async_run` in a loop
   to reconnect whenever a connection is lost.
2. **Execute**: Multiple coroutines will call `async_exec` independently
   and without coordination (e.g. queuing).
3. **Receive**: One coroutine will loop on `async_receive` to receive
   server-side pushes (required only if the app expects server pushes).

Each of the operations above can be performed without regards to the
other as they are independent from each other.  Let us see with
more detail each point above.

#### Connect

In general, applications will connect to a Redis server and hang
around for as long as possible, until the connection is lost for some
reason.  When that happens, simple setups will want to wait for a
short period of time and try to connect again.  The code snippet below
shows how this can be achieved with a coroutine (see echo_server.cpp)

```cpp
net::awaitable<void> reconnect(std::shared_ptr<connection> conn, endpoint ep)
{
   net::steady_timer timer{co_await net::this_coro::executor};
   for (boost::system::error_code ec;;) {

      // Establiches a connection and hangs around until it is lost.
      co_await conn->async_run(ep, {}, redir(ec));
      conn->reset_stream();

      // Waits some time before trying to restablish the connection.
      timer.expires_after(std::chrono::seconds{1});
      co_await timer.async_wait();
   }
}
```

Other common scenarios are, for example, performing a failover with
sentinels and re-subscribing to pubsub channels, both are covered in
the `subscriber_sentinel.cpp` example.

#### Execute

The basic idea about `async_exec` was stated above already: execute Redis
commands. One of the most important things about it though is that it
can be called multiple times without coordination, for example, in a
HTTP or Websocket server where each session calls it to communicate
with the database. The benefits of this feature are manifold

* Having only connection to the database increases the performance
  of [pipelines](https://redis.io/topics/pipelining).
* Having sessions independent from each other makes backend code simpler.

The code below illustrates this concepts in a TCP `echo_server.cpp`

```cpp
awaitable_type echo_server_session(tcp_socket socket, std::shared_ptr<connection> db)
{
   request req;
   std::tuple<std::string> response;

   for (std::string buffer;;) {
      // Reads a user message.
      auto n = co_await net::async_read_until(socket, net::dynamic_buffer(buffer, 1024), "\n");

      // Echos it through Redis.
      req.push("PING", buffer);
      co_await db->async_exec(req, adapt(response));

      // Writes is back to the user.
      co_await net::async_write(socket, net::buffer(std::get<0>(response)));

      // Cleanup
      std::get<0>(response).clear();
      req.clear();
      buffer.erase(0, n);
   }
}
```

Notice also how the session above provides back-pressure as the
coroutine won't read the next message from the socket until a cycle is
complete.

#### Receive

Point number 3. above is only necessary for servers that expect server
pushes, like, for example, when using Redis pubsub. The example below
was taken from subscriber.cpp

```cpp
net::awaitable<void> push_receiver(std::shared_ptr<connection> conn)
{
   for (std::vector<node<std::string>> resp;;) {
      co_await conn->async_receive(adapt(resp));
      print_push(resp);
      resp.clear();
   }
}
```

In general, it is advisable to all apps to keep a coroutine calling
`async_receive` as an unread push will cause the connection to stall
and eventually timeout.  Notice that the same connection that is being
used to send requests can be also used to receive server-side pushes.

#### Cancellation

Aedis supports both implicit and explicit cancellation of connection
operations. Explicit cancellation is support by means of the
`aedis::connection::cancel` member function. Implicit cancellation,
like those that may happen when using Asio awaitable operators && and
|| will be discussed with more detail below.

```cpp
co_await (conn.async_run(...) && conn.async_exec(...))
```

* Useful when implementing reconnection on applications that
  use pubsub.
* Makes the channel re-subscribe operation simpler when the
  connection is reestablished.

```cpp
co_await (conn.async_run(...) || conn.async_exec(...))
```

* Useful for short-lived connections that are meant to be closed after
  a command has been executed.

```cpp
co_await (conn.async_exec(...) || time.async_wait(...))
```

* Provides a way to limit how long the execution of a single request
  should last.
* Alternatively, for a connection-wide timeout set
  `aedis::connection::timeouts::ping_interval` to a proper value. This
  will work because all requests use the same queue and is also more
  efficient as only one timer will be used.

* The cancellation will be ignored if the request has already
  been written to the socket.

```cpp
co_await (conn.async_run(...) || time.async_wait(...))
```

* Set a limit on how long the connection should live (see also
  `aedis::connection::timeouts`)

```cpp
co_await (conn.async_exec(...) || conn.async_exec(...) || ... || conn.async_exec(...))
```

* This is supported but is considered an antipattern. Unless
  the user has set the `aedis::resp3::request::config::coalesce` to
  `false`, and he shouldn't, the connection will automatically merge
  the individual requests into a single payload anyway.

#### Timeouts

Aedis high-level API provides built-in support for many of the
timeouts users usually need. For example, the
`aedis::connection::async_run` member function performs the following
operations on behalf of the user

* Resolves Redis address.
* Connects to the resolved endpoint.
* TLS handshake (for TLS endpoints).
* RESP3 handshake and authentication and role check.
* Periodic healthy checks with the PING command.
* Keeps reading from the socket to handle server pushes and command responses.
* Keeps writing requests as it becomes possible e.g. after last response has arrived.

To control the timeout-behaviour of these operations users must
create a `aedis::connection::timeouts` object and pass it to as
argument to the `aedis::connection::async_run` (if
the suggested defaults are not suitable). 

<a name="requests"></a>

### Requests

Redis requests are composed of one of more Redis commands (in
Redis documentation they are called
[pipelines](https://redis.io/topics/pipelining)). For example

```cpp
request req;

// Command with variable length of arguments.
req.push("SET", "key", "some value", value, "EX", "2");

// Pushes a list.
std::list<std::string> list
   {"channel1", "channel2", "channel3"};
req.push_range("SUBSCRIBE", list);

// Same as above but as an iterator range.
req.push_range("SUBSCRIBE", std::cbegin(list), std::cend(list));

// Pushes a map.
std::map<std::string, mystruct> map
   { {"key1", "value1"}
   , {"key2", "value2"}
   , {"key3", "value3"}};
req.push_range("HSET", "key", map);
```

Sending a request to Redis is performed with `aedis::connection::async_exec` as already stated.

<a name="serialization"></a>

#### Serialization

The `push` and `push_range` functions above work with integers
e.g. `int` and `std::string` out of the box. To send your own
data type defined a `to_bulk` function like this

```cpp
// Example struct.
struct mystruct {...};

// Serialize your data structure here.
void to_bulk(std::string& to, mystruct const& obj)
{
   std::string dummy = "Dummy serializaiton string.";
   aedis::resp3::to_bulk(to, dummy);
}
```

Once `to_bulk` is defined and visible over ADL `mystruct` can
be passed to the `request`

```cpp
request req;

std::map<std::string, mystruct> map {...};

req.push_range("HSET", "key", map);
```

Example serialization.cpp shows how store json string in Redis.

<a name="responses"></a>

### Responses

Aedis uses the following strategy to support Redis responses

* **Static**: For `aedis::resp3::request` whose sizes are known at compile time
  std::tuple is supported.
* **Dynamic**: Otherwise use `std::vector<aedis::resp3::node<std::string>>`.

For example, below is a request with a compile time size

```cpp
request req;
req.push("PING");
req.push("INCR", "key");
req.push("QUIT");
```

to read the response to this request users can use the following tuple

```cpp
// Replace a tuple element with aedis::ignore to ignore the response
// to a specific command.
std::tuple<std::string, int, std::string>
```

The pattern is obvious, the tuple must have the same size as the
request (exceptions below) and each element must be able to store
the response to the command it refers to. The following table provides
the response types of some commands

Command  | RESP3 type                          | Documentation
---------|-------------------------------------|--------------
lpush    | Number                              | https://redis.io/commands/lpush
lrange   | Array                               | https://redis.io/commands/lrange
set      | Simple-string, null or blob-string  | https://redis.io/commands/set
get      | Blob-string                         | https://redis.io/commands/get
smembers | Set                                 | https://redis.io/commands/smembers
hgetall  | Map                                 | https://redis.io/commands/hgetall

To map these RESP3 types into a C++ data structure use the table below

RESP3 type     | Possible C++ type                                            | Type
---------------|--------------------------------------------------------------|------------------
Simple-string  | `std::string`                                              | Simple
Simple-error   | `std::string`                                              | Simple
Blob-string    | `std::string`, `std::vector`                               | Simple
Blob-error     | `std::string`, `std::vector`                               | Simple
Number         | `long long`, `int`, `std::size_t`, `std::string`           | Simple
Double         | `double`, `std::string`                                    | Simple
Null           | `std::optional<T>`                                         | Simple
Array          | `std::vector`, `std::list`, `std::array`, `std::deque`     | Aggregate
Map            | `std::vector`, `std::map`, `std::unordered_map`            | Aggregate
Set            | `std::vector`, `std::set`, `std::unordered_set`            | Aggregate
Push           | `std::vector`, `std::map`, `std::unordered_map`            | Aggregate

For example

```cpp
request req;
req.push("HELLO", 3);
req.push_range("RPUSH", "key1", vec);
req.push_range("HSET", "key2", map);
req.push("LRANGE", "key3", 0, -1);
req.push("HGETALL", "key4");
req.push("QUIT");

std::tuple<
   aedis::ignore,  // hello
   int,            // rpush
   int,            // hset
   std::vector<T>, // lrange
   std::map<U, V>, // hgetall
   std::string     // quit
> resp;

co_await db->async_exec(req, adapt(resp));
```

The tag `aedis::ignore` can be used to ignore individual elements in
the responses. If the intention is to ignore the response to all
commands altogether use `adapt()` without arguments instead

```cpp
co_await db->async_exec(req, adapt());
```

Responses that contain nested aggregates or heterogeneous data
types will be given special treatment later in [The general case](#the-general-case).  As
of this writing, not all RESP3 types are used by the Redis server,
which means in practice users will be concerned with a reduced
subset of the RESP3 specification.

#### Push

The only commands that are excluded from the rules from last section
are those that have RESP3 push types as response, those are

* `"SUBSCRIBE"`
* `"PSUBSCRIBE"`
* `"UNSUBSCRIBE"`

For example, this request

```cpp
request req;
req.push("PING");
req.push("SUBSCRIBE", "channel");
req.push("QUIT");
```

must be read in this tuple `std::tuple<std::string, std::string>`,
that has size two.

#### Null

It is not uncommon for apps to access keys that do not exist or
that have already expired in the Redis server, to deal with these
cases Aedis provides support for `std::optional`. To use it,
wrap your type around `std::optional` like this

```cpp
std::tuple<
   std::optional<A>,
   std::optional<B>,
   ...
   > response;

co_await db->async_exec(req, adapt(response));
```

Everything else stays pretty much the same.

#### Transactions

To read responses to transactions we have to observe that Redis will
queue its commands and send their responses to the user as elements
of an array, after the `EXEC` command comes.  For example, to read
the response to the this request

```cpp
db.send("MULTI");
db.send("GET", "key1");
db.send("LRANGE", "key2", 0, -1);
db.send("HGETALL", "key3");
db.send("EXEC");
```

use the following response type

```cpp
using aedis::ignore;

using exec_resp_type = 
   std::tuple<
      std::optional<std::string>, // get
      std::optional<std::vector<std::string>>, // lrange
      std::optional<std::map<std::string, std::string>> // hgetall
   >;

std::tuple<
   aedis::ignore,     // multi
   aedis::ignore,     // get
   aedis::ignore,     // lrange
   aedis::ignore,     // hgetall
   exec_resp_type, // exec
> resp;

co_await db->async_exec(req, adapt(resp));
```

Note that we are not ignoring the response to the commands themselves
above as commands in a transaction will always get `"QUEUED"` as
response. For a complete example see containers.cpp.

#### Deserialization

As mentioned in \ref serialization, it is common to
serialize data before sending it to Redis e.g.  to json strings.
For performance and convenience reasons, we may also want to
deserialize it directly in its final data structure. Aedis
supports this use case by calling a user provided `from_bulk`
function while parsing the response. For example

```cpp
void from_bulk(mystruct& obj, char const* p, std::size_t size, boost::system::error_code& ec)
{
   // Deserializes p into obj.
}
```

After that, you can start receiving data efficiently in the desired
types e.g. `mystruct`, `std::map<std::string, mystruct>` etc.

<a name="the-general-case"></a>

#### The general case

There are cases where responses to Redis
commands won't fit in the model presented above, some examples are

* Commands (like `set`) whose responses don't have a fixed
RESP3 type. Expecting an `int` and receiving a blob-string
will result in error.
* RESP3 aggregates that contain nested aggregates can't be read in STL containers.
* Transactions with a dynamic number of commands can't be read in a `std::tuple`.

To deal with these cases Aedis provides the `aedis::resp3::node` type
abstraction, that is the most general form of an element in a
response, be it a simple RESP3 type or the element of an aggregate. It
is defined like this

```cpp
template <class String>
struct node {
   // The RESP3 type of the data in this node.
   type data_type;

   // The number of elements of an aggregate (or 1 for simple data).
   std::size_t aggregate_size;

   // The depth of this node in the response tree.
   std::size_t depth;

   // The actual data. For aggregate types this is always empty.
   String value;
};
```

Any response to a Redis command can be received in a
`std::vector<node<std::string>>`.  The vector can be seen as a
pre-order view of the response tree.  Using it is not different than
using other types

```cpp
// Receives any RESP3 simple or aggregate data type.
std::vector<node<std::string>> resp;
co_await db->async_exec(req, adapt(resp));
```

For example, suppose we want to retrieve a hash data structure
from Redis with `HGETALL`, some of the options are

* `std::vector<node<std::string>`: Works always.
* `std::vector<std::string>`: Efficient and flat, all elements as string.
* `std::map<std::string, std::string>`: Efficient if you need the data as a `std::map`.
* `std::map<U, V>`: Efficient if you are storing serialized data. Avoids temporaries and requires `from_bulk` for `U` and `V`.

In addition to the above users can also use unordered versions of the
containers. The same reasoning also applies to sets e.g. `SMEMBERS`.

### Examples

To conclude this overview users are invited to skim over the
examples below

* intro.cpp: The Aedis hello-world program. It sends one command to Redis and quits the connection.
* intro_tls.cpp: Same as intro.cpp but over TLS.
* intro_sync.cpp: Synchronous version of intro.cpp.
* containers.cpp: Shows how to send and receive stl containers and how to use transactions.
* serialization.cpp: Shows how to serialize types using Boost.Json.
* subscriber.cpp: Shows how to implement pubsub that reconnects and resubscribes when the connection is lost.
* subscriber_sentinel.cpp: Same as subscriber.cpp but with failover with sentinels.
* echo_server.cpp: A simple TCP echo server.
* chat_room.cpp: A simple chat room.

## Why Aedis

The main reason for why I started writing Aedis was to have a client
compatible with the Asio asynchronous model. As I made progresses I could
also address what I considered weaknesses in other libraries.  Due to
time constraints I won't be able to give a detailed comparison with
each client listed in the
[official](https://redis.io/docs/clients/#cpp) list of clients,
instead I will focus on the most popular C++ client on github in number of
stars, namely

* https://github.com/sewenew/redis-plus-plus

Before we start it is important to mentioning some of the things
redis-plus-plus does not support

* RESP3. Without RESP3 is impossible to support some important Redis features like client side caching, among other things.
* Coroutines.
* Reading responses directly in user data structures to avoid creating temporaries.
* Proper error handling with support for error-code.
* Healthy checks.

The remaining points will be addressed individually.  Let us first
have a look at what sending a command a pipeline and a transaction
look like

```cpp
auto redis = Redis("tcp://127.0.0.1:6379");

// Send commands
redis.set("key", "val");
auto val = redis.get("key"); // val is of type OptionalString.
if (val)
    std::cout << *val << std::endl;

// Sending pipelines
auto pipe = redis.pipeline();
auto pipe_replies = pipe.set("key", "value")
                        .get("key")
                        .rename("key", "new-key")
                        .rpush("list", {"a", "b", "c"})
                        .lrange("list", 0, -1)
                        .exec();

// Parse reply with reply type and index.
auto set_cmd_result = pipe_replies.get<bool>(0);
// ...

// Sending a transaction
auto tx = redis.transaction();
auto tx_replies = tx.incr("num0")
                    .incr("num1")
                    .mget({"num0", "num1"})
                    .exec();

auto incr_result0 = tx_replies.get<long long>(0);
// ...
```

Some of the problems with this API are

* Heterogeneous treatment of commands, pipelines and transaction. This makes auto-pipelining impossible.
* Any Api that sends individual commands has a very restricted scope of usability and should be avoided for performance reasons.
* The API imposes exceptions on users, no error-code overload is provided.
* No way to reuse the buffer for new calls to e.g. redis.get in order to avoid further dynamic memory allocations.
* Error handling of resolve and connection not clear.

According to the documentation, pipelines in redis-plus-plus have
the following characteristics

> NOTE: By default, creating a Pipeline object is NOT cheap, since
> it creates a new connection.

This is clearly a downside in the API as pipelines should be the
default way of communicating and not an exception, paying such a
high price for each pipeline imposes a severe cost in performance.
Transactions also suffer from the very same problem.

> NOTE: Creating a Transaction object is NOT cheap, since it
> creates a new connection.

In Aedis there is no difference between sending one command, a
pipeline or a transaction because requests are decoupled
from the IO objects.

> redis-plus-plus also supports async interface, however, async
> support for Transaction and Subscriber is still on the way.
> 
> The async interface depends on third-party event library, and so
> far, only libuv is supported.

Async code in redis-plus-plus looks like the following

```cpp
auto async_redis = AsyncRedis(opts, pool_opts);

Future<string> ping_res = async_redis.ping();

cout << ping_res.get() << endl;
```
As the reader can see, the async interface is based on futures
which is also known to have a bad performance.  The biggest
problem however with this async design is that it makes it
impossible to write asynchronous programs correctly since it
starts an async operation on every command sent instead of
enqueueing a message and triggering a write when it can be sent.
It is also not clear how are pipelines realised with this design
(if at all).

### Echo server benchmark

This document benchmarks the performance of TCP echo servers I
implemented in different languages using different Redis clients.  The
main motivations for choosing an echo server are

   * Simple to implement and does not require expertise level in most languages.
   * I/O bound: Echo servers have very low CPU consumption in general
     and  therefore are excelent to  measure how a program handles concurrent requests.
   * It simulates very well a typical backend in regard to concurrency.

I also imposed some constraints on the implementations

   * It should be simple enough and not require writing too much code.
   * Favor the use standard idioms and avoid optimizations that require expert level.
   * Avoid the use of complex things like connection and thread pool.

To reproduce these results run one of the echo-server programs in one
terminal and the
[echo-server-client](https://github.com/mzimbres/aedis/blob/42880e788bec6020dd018194075a211ad9f339e8/benchmarks/cpp/asio/echo_server_client.cpp)
in another.

#### Without Redis

First I tested a pure TCP echo server, i.e. one that sends the messages
directly to the client without interacting with Redis. The result can
be seen below

![](https://mzimbres.github.io/aedis/tcp-echo-direct.png)

The tests were performed with a 1000 concurrent TCP connections on the
localhost where latency is 0.07ms on average on my machine. On higher
latency networks the difference among libraries is expected to
decrease. 

   * I expected Libuv to have similar performance to Asio and Tokio.
   * I did expect nodejs to come a little behind given it is is
     javascript code. Otherwise I did expect it to have similar
     performance to libuv since it is the framework behind it.
   * Go did surprise me: faster than nodejs and liuv!

The code used in the benchmarks can be found at

   * [Asio](https://github.com/mzimbres/aedis/blob/3fb018ccc6138d310ac8b73540391cdd8f2fdad6/benchmarks/cpp/asio/echo_server_direct.cpp): A variation of [this](https://github.com/chriskohlhoff/asio/blob/4915cfd8a1653c157a1480162ae5601318553eb8/asio/src/examples/cpp20/coroutines/echo_server.cpp) Asio example.
   * [Libuv](https://github.com/mzimbres/aedis/tree/835a1decf477b09317f391eddd0727213cdbe12b/benchmarks/c/libuv): Taken from [here](https://github.com/libuv/libuv/blob/06948c6ee502862524f233af4e2c3e4ca876f5f6/docs/code/tcp-echo-server/main.c) Libuv example .
   * [Tokio](https://github.com/mzimbres/aedis/tree/3fb018ccc6138d310ac8b73540391cdd8f2fdad6/benchmarks/rust/echo_server_direct): Taken from [here](https://docs.rs/tokio/latest/tokio/).
   * [Nodejs](https://github.com/mzimbres/aedis/tree/3fb018ccc6138d310ac8b73540391cdd8f2fdad6/benchmarks/nodejs/echo_server_direct)
   * [Go](https://github.com/mzimbres/aedis/blob/3fb018ccc6138d310ac8b73540391cdd8f2fdad6/benchmarks/go/echo_server_direct.go)

#### With Redis

This is similar to the echo server described above but messages are
echoed by Redis and not by the echo-server itself, which acts
as a proxy between the client and the Redis server. The results
can be seen below

![](https://mzimbres.github.io/aedis/tcp-echo-over-redis.png)

The tests were performed on a network where latency is 35ms on
average, otherwise it uses the same number of TCP connections
as the previous example.

As the reader can see, the Libuv and the Rust test are not depicted
in the graph, the reasons are

   * [redis-rs](https://github.com/redis-rs/redis-rs): This client
     comes so far behind that it can't even be represented together
     with the other benchmarks without making them look insignificant.
     I don't know for sure why it is so slow, I suppose it has
     something to do with its lack of proper
     [pipelining](https://redis.io/docs/manual/pipelining/) support.
     In fact, the more TCP connections I lauch the worse its
     performance gets.

   * Libuv: I left it out because it would require too much work to
     write it and make it have a good performance. More specifically,
     I would have to use hiredis and implement support for pipelines
     manually.

The code used in the benchmarks can be found at

   * [Aedis](https://github.com/mzimbres/aedis): [code](https://github.com/mzimbres/aedis/blob/3fb018ccc6138d310ac8b73540391cdd8f2fdad6/examples/echo_server.cpp)
   * [node-redis](https://github.com/redis/node-redis): [code](https://github.com/mzimbres/aedis/tree/3fb018ccc6138d310ac8b73540391cdd8f2fdad6/benchmarks/nodejs/echo_server_over_redis)
   * [go-redis](https://github.com/go-redis/redis): [code](https://github.com/mzimbres/aedis/blob/3fb018ccc6138d310ac8b73540391cdd8f2fdad6/benchmarks/go/echo_server_over_redis.go)

<a name="api-reference"></a>

## Reference

* [High-Level](#high-level-api): Covers the topics discussed in this document.
* [Low-Level](#low-level-api): Covers low-level building blocks. Provided mostly for developers, most users won't need any information provided here.

## Installation

Download the latest release on
https://github.com/mzimbres/aedis/releases.  Aedis is a header only
library, so you can starting using it right away by adding the
`include` subdirectory to your project and including

```cpp
#include <aedis/src.hpp>

```
in no more than one source file in your applications. The 
requirements for using Aedis are

- Boost 1.79 or greater.
- C++17 minimum.
- Redis 6 or higher (must support RESP3).
- Optionally also redis-cli and Redis Sentinel.

The following compilers are supported

- Tested with gcc: 10, 11, 12.
- Tested with clang: 11, 13, 14.

## Acknowledgement

Acknowledgement to people that helped shape Aedis in one way or
another.

* Richard Hodges ([madmongo1](https://github.com/madmongo1)): For very helpful support with Asio, the design of asynchronous programs, etc.
* Vinícius dos Santos Oliveira ([vinipsmaker](https://github.com/vinipsmaker)): For useful discussion about how Aedis consumes buffers in the read operation.
* Petr Dannhofer ([Eddie-cz](https://github.com/Eddie-cz)): For helping me understand how the `AUTH` and `HELLO` command can influence each other.
* Mohammad Nejati ([ashtum](https://github.com/ashtum)): For pointing out scenarios where calls to `async_exec` should fail when the connection is lost.
* Klemens Morgenstern ([klemens-morgenstern](https://github.com/klemens-morgenstern)): For useful discussion about timeouts, cancellation, synchronous interfaces and general help with Asio.

## Changelog

### master

* Renames `fail_on_connection_lost` to
  `aedis::resp3::request::config::cancel_on_connection_lost`. Now, it will
  only cause connections to be canceled when `async_run` completes.

* Introduces `aedis::resp3::request::config::cancel_if_not_connected` which will
  cause a request to be canceled if `async_exec` is called before a
  connection has been established.

* Introduces new request flag `aedis::resp3::request::config::retry` that if
  set to true will cause the request to not be canceled when it was
  sent to Redis but remained unresponded after `async_run` completed.
  It provides a way to avoid executing commands twice.

* Removes the `aedis::connection::async_run` overload that takes
  request and adapter as parameters.

* Adds a second parameter to the `aedis::connection::async_run`
  completion signature that contains the number of requests that have
  been canceled on its completion.

* Changes the way `aedis::adapt()` behaves with
  `std::vector<aedis::resp3::node<T>>`. Receiving RESP3 simple errors,
  blob errors or null won't causes an error but will be treated as
  normal response.  It is the user responsibility to check the content
  in the vector.

* Fixes a bug in `connection::cancel(operation::exec)`. Now this
  call will only cancel non-written requests.

* Implements per-operation implicit cancellation support for
  `aedis::connection::async_exec`. The following call will `co_await (conn.async_exec(...) || timer.async_wait(...))`
  will cancel the request as long as it has not been written.

* Changes `aedis::connection::async_run` completion signature to
  `f(error_code)`. This is how is was in the past, the second
  parameter was not helpful.

* Renames `operation::receive_push` to `aedis::operation::receive`.

### v1.1.0...1

* Removes `coalesce_requests` from the `aedis::connection::config`, it
  became a request property now, see `aedis::resp3::request::config::coalesce`.

* Removes `max_read_size` from the `aedis::connection::config`. The maximum
  read size can be specified now as a parameter of the
  `aedis::adapt()` function.

* Removes `aedis::sync` class, see intro_sync.cpp for how to perform
  synchronous and thread safe calls. This is possible in Boost. 1.80
  only as it requires `boost::asio::deferred`. 

* Moves from `boost::optional` to `std::optional`. This is part of
  moving to C++17.

* Changes the behaviour of the second `aedis::connection::async_run` overload
  so that it always returns an error when the connection is lost.

* Adds TLS support, see intro_tls.cpp.

* Adds an example that shows how to resolve addresses over sentinels,
  see subscriber_sentinel.cpp.

* Adds a `aedis::connection::timeouts::resp3_handshake_timeout`. This is
  timeout used to send the `HELLO` command.

* Adds `aedis::endpoint` where in addition to host and port, users can
  optionally provide username, password and the expected server role
  (see `aedis::error::unexpected_server_role`).

* `aedis::connection::async_run` checks whether the server role received in
  the hello command is equal to the expected server role specified in
  `aedis::endpoint`. To skip this check let the role variable empty.

* Removes reconnect functionality from `aedis::connection`. It
  is possible in simple reconnection strategies but bloats the class
  in more complex scenarios, for example, with sentinel,
  authentication and TLS. This is trivial to implement in a separate
  coroutine. As a result the enum `event` and `async_receive_event`
  have been removed from the class too.

* Fixes a bug in `connection::async_receive_push` that prevented
  passing any response adapter other that `adapt(std::vector<node>)`.

* Changes the behaviour of `aedis::adapt()` that caused RESP3 errors
  to be ignored. One consequence of it is that `connection::async_run`
  would not exit with failure in servers that required authentication.

* Changes the behaviour of `connection::async_run` that would cause it
  to complete with success when an error in the
  `connection::async_exec` occurred.

* Ports the buildsystem from autotools to CMake.

### v1.0.0

* Adds experimental cmake support for windows users.

* Adds new class `aedis::sync` that wraps an `aedis::connection` in
  a thread-safe and synchronous API.  All free functions from the
  `sync.hpp` are now member functions of `aedis::sync`.

* Split `aedis::connection::async_receive_event` in two functions, one
  to receive events and another for server side pushes, see
  `aedis::connection::async_receive_push`.

* Removes collision between `aedis::adapter::adapt` and
  `aedis::adapt`.

* Adds `connection::operation` enum to replace `cancel_*` member
  functions with a single cancel function that gets the operations
  that should be cancelled as argument.

* Bugfix: a bug on reconnect from a state where the `connection` object
  had unsent commands. It could cause `async_exec` to never
  complete under certain conditions.

* Bugfix: Documentation of `adapt()` functions were missing from
  Doxygen.

### v0.3.0

* Adds `experimental::exec` and `receive_event` functions to offer a
  thread safe and synchronous way of executing requests across
  threads. See `intro_sync.cpp` and `subscriber_sync.cpp` for
  examples.

* `connection::async_read_push` was renamed to `async_receive_event`.

* `connection::async_receive_event` is now being used to communicate
  internal events to the user, such as resolve, connect, push etc. For
  examples see subscriber.cpp and `connection::event`.

* The `aedis` directory has been moved to `include` to look more
  similar to Boost libraries. Users should now replace `-I/aedis-path`
  with `-I/aedis-path/include` in the compiler flags.

* The `AUTH` and `HELLO` commands are now sent automatically. This change was
  necessary to implement reconnection. The username and password
  used in `AUTH` should be provided by the user on
  `connection::config`.

* Adds support for reconnection. See `connection::enable_reconnect`.

* Fixes a bug in the `connection::async_run(host, port)` overload
  that was causing crashes on reconnection.

* Fixes the executor usage in the connection class. Before theses
  changes it was imposing `any_io_executor` on users.

* `connection::async_receiver_event` is not cancelled anymore when
  `connection::async_run` exits. This change makes user code simpler.

* `connection::async_exec` with host and port overload has been
  removed. Use the other `connection::async_run` overload.

* The host and port parameters from `connection::async_run` have been
  move to `connection::config` to better support authentication and
  failover.

* Many simplifications in the `chat_room` example.

* Fixes build in clang the compilers and makes some improvements in
  the documentation.

### v0.2.0...1

* Fixes a bug that happens on very high load. (v0.2.1) 
* Major rewrite of the high-level API. There is no more need to use the low-level API anymore.
* No more callbacks: Sending requests follows the ASIO asynchronous model.
* Support for reconnection: Pending requests are not canceled when a connection is lost and are re-sent when a new one is established.
* The library is not sending HELLO-3 on user behalf anymore. This is important to support AUTH properly.

### v0.1.0...2

* Adds reconnect coroutine in the `echo_server` example. (v0.1.2)
* Corrects `client::async_wait_for_data` with `make_parallel_group` to launch operation. (v0.1.2)
* Improvements in the documentation. (v0.1.2)
* Avoids dynamic memory allocation in the client class after reconnection. (v0.1.2)
* Improves the documentation and adds some features to the high-level client. (v.0.1.1)
* Improvements in the design and documentation.

### v0.0.1

* First release to collect design feedback.

