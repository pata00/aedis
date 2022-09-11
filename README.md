
# Documentation

[TOC]

## Overview

Aedis is a high-level [Redis](https://redis.io/) client library
built on top of
[Asio](https://www.boost.org/doc/libs/release/doc/html/boost_asio.html).
Some of its distinctive features are

* Support for the latest version of the Redis communication protocol [RESP3](https://github.com/redis/redis-specifications/blob/master/protocol/RESP3.md).
* Support for STL containers and serialization of your own data types.
* Support for TLS and Redis sentinel.
* Healthy checks, back pressure and low latency.

If you are unfamiliar with Redis, the best place to start is https://redis.io, in short 

* Redis is an In-memory data structure server with support for strings, hashes, lists, sets, sorted sets, streams, and more.
* Keeps the dataset in memory for fast access, but can also persist all writes to permanent storage to survive reboots and system failures.
* Replication with automatic failover for both standalone and clustered deployments.

Inpacient users can skim over the examples before proceeding to the next sections

* intro.cpp: This is the Aedis hello-world program. It sends one command to Redis and quits the connection.
* intro_sync.cpp: Synchronous version of intro.cpp.
* containers.cpp: Shows how to send and receive stl containers. Also shows how to use transactions.
* serialization.cpp: Shows how to serialize types using Boost.Json.
* subscriber.cpp: Shows how to implement pubsub that reconnects and resubscribes when the connection is lost.
* subscriber_sentinel.cpp: Subscriber that resolves the master address with sentinels.
* subscriber_sync.cpp: Synchronous version of subscriber.cpp.
* echo_server.cpp: A simple TCP echo server that uses coroutines.
* chat_room.cpp: A simple chat room that uses coroutines.

## Tutorial

The support for synchronous communication with Redis is based on
the asynchronous interface, therefore we will start with the later
here.

### Async

The basic Aedis functionality resolves around calling these three
functions

* `connection::async_run`: Stablishes a connection the Redis
server and performs healthy checks.
* `connection::async_exec`: Send commands and receive responses.
* `connection::async_receive_push`: Receives server side pushes.

For example, in the simplest cases when no reconnection is required,
calling `connection::async_run` can be as simple as

```cpp
auto main() -> int
{
   using connection = aedis::connection<net::ip::tcp::socket>;

   try {
      net::io_context ioc;
      connection conn{ioc};
      endpoint ep{"127.0.0.1", "6379"};

      conn.async_run([](auto const& ec) {
         std::clog << ec.message() << std::endl;
      });

      // Spawn other operations that use the conn object.
      net::co_spawn(ioc, receive_pushes(conn), net::detached);
      net::co_spawn(ioc, send_commands(conn), net::detached);
      ...

      ioc.run();
   } catch (...) {
      std::cerr << "Error" << std::endl;
   }
}
```

`async_run` completes only when the connection is lost. When that
happens other operations like `async_exec` or `async_receive_push`
won't be affected and will be processed once a new connection is
stablished, by calling `async_run` again.  The code snippet below is
an example implementation of `receive_pushes(conn)`

```cpp
net::awaitable<void> receive_pushes(connection& db)
{
   for (std::vector<node<std::string>> resp;;) {
      co_await db->async_receive_push(adapt(resp));
      // Process the push in resp.
      resp.clear();
   }
}
```

`send_commands` on the other hand could look like

```cpp
net::awaitable<void> send_commands(connection& db)
{
   request req;
   std::tuple<std::string> resp;
   req.push("PING", "some message");
   co_await db->async_exec(req, adapt(resp));
}
```

#### Reconnection

In the majority of cases users will want to reconnect after a
connection is lost, since the `run`, `exec` and `receive_push`
operations are independent from one another, reconnection can be
implemented as a simple loop arount `async_run`

```cpp
net::awaitable<void> reconnect(std::shared_ptr<connection> db)
{
   net::steady_timer timer{co_await net::this_coro::executor};
   endpoint ep{"127.0.0.1", "6379"};
   for (;;) {
      boost::system::error_code ec;
      co_await db->async_run(ep, req, adapt(), net::redirect_error(net::use_awaitable, ec));
      std::cout << ec.message() << std::endl;
      db->reset_stream();
      timer.expires_after(std::chrono::seconds{1});
      co_await timer.async_wait(net::use_awaitable);
   }
}
```

More complex scenarios like performing a failover with sentinels
can be seen in the example subscriber_sentinel.cpp.

### Sync

Synchronous communication is supported in Aedis by the wrapper class `aedis::sync`.

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
req.push_range2("SUBSCRIBE", std::cbegin(list), std::cend(list));

// Pushes a map.
std::map<std::string, mystruct> map
   { {"key1", "value1"}
   , {"key2", "value2"}
   , {"key3", "value3"}};
req.push_range("HSET", "key", map);
```

Sending a request to Redis is then peformed with the following function

```cpp
co_await db->async_exec(req, adapt(resp));
```

#### Serialization

The `push` and `push_range` functions above work with integers
e.g. `int` and `std::string` out of the box. To send your own
data type defined a `to_bulk` function like this

```cpp
struct mystruct {
   // Example struct.
};

void to_bulk(std::string& to, mystruct const& obj)
{
   std::string dummy = "Dummy serializaiton string.";
   aedis::resp3::to_bulk(to, dummy);
}
```

Once `to_bulk` is defined and accessible over ADL `mystruct` can
be passed to the `request`

```cpp
request req;

std::map<std::string, mystruct> map {...};

req.push_range("HSET", "key", map);
```

Example serialization.cpp shows how store json string in Redis.

### Responses

To read responses effectively, users must know their RESP3 type,
this can be found in the Redis documentation for each command
(https://redis.io/commands). For example

Command  | RESP3 type                          | Documentation
---------|-------------------------------------|--------------
lpush    | Number                              | https://redis.io/commands/lpush
lrange   | Array                               | https://redis.io/commands/lrange
set      | Simple-string, null or blob-string  | https://redis.io/commands/set
get      | Blob-string                         | https://redis.io/commands/get
smembers | Set                                 | https://redis.io/commands/smembers
hgetall  | Map                                 | https://redis.io/commands/hgetall

Once the RESP3 type of a given response is known we can choose a
proper C++ data structure to receive it in. Fortunately, this is a
simple task for most types. The table below summarises the options

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

The tag @c aedis::ignore can be used to ignore individual
elements in the responses. If the intention is to ignore the
response to all commands in the request use @c adapt()

```cpp
co_await db->async_exec(req, adapt());
```

Responses that contain nested aggregates or heterogeneous data
types will be given special treatment later in \ref gen-case.  As
of this writing, not all RESP3 types are used by the Redis server,
which means in practice users will be concerned with a reduced
subset of the RESP3 specification.

#### Null

It is not uncommon for apps to access keys that do not exist or
that have already expired in the Redis server, to deal with these
cases Aedis provides support for `std::optional`. To use it,
wrap your type around `std::optional` like this

```cpp
std::optional<std::unordered_map<T, U>> resp;
co_await db->async_exec(req, adapt(resp));
```

Everything else stays the same.

#### Transactions

To read the response to transactions we have to observe that Redis
queues the commands as they arrive and sends the responses back to
the user as an array, in the response to the @c exec command.
For example, to read the response to the this request

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
   ignore,     // multi
   ignore,     // get
   ignore,     // lrange
   ignore,     // hgetall
   exec_resp_type, // exec
> resp;

co_await db->async_exec(req, adapt(resp));
```

Note that above we are not ignoring the response to the commands
themselves but whether they have been successfully queued. For a
complete example see containers.cpp.

#### Deserialization

As mentioned in \ref requests-serialization, it is common to
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

#### The general case

There are cases where responses to Redis
commands won't fit in the model presented above, some examples are

* Commands (like `set`) whose responses don't have a fixed
RESP3 type. Expecting an `int` and receiving a blob-string
will result in error.
* RESP3 aggregates that contain nested aggregates can't be read in STL containers.
* Transactions with a dynamic number of commands can't be read in a `std::tuple`.

To deal with these cases Aedis provides the `resp3::node`
type, that is the most general form of an element in a response,
be it a simple RESP3 type or an aggregate. It is defined like this

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
pre-order view of the response tree.  Using it is no different than
using other types

```cpp
// Receives any RESP3 simple data type.
node<std::string> resp;
co_await db->async_exec(req, adapt(resp));

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

In addition to the above users can also use unordered versions of the containers. The same reasoning also applies to sets e.g. `SMEMBERS`.

## Installation

Download the latest Aedis release from github 

```cpp
$ wget https://github.com/mzimbres/aedis/releases/download/v1.0.0/aedis-1.0.0.tar.gz
```

and unpack in your prefered location. Aedis is a header only
library, so you can starting using it. For that include the
following header 

```cpp
#include <aedis/src.hpp>

```
in no more than one source file in your applications (see
intro.cpp for example). To build the examples, run the tests etc.
cmake is also supported

```cpp
$ BOOST_ROOT=/opt/boost_1_79_0/ cmake
$ make
$ make test
```

Notice you have to specify the compiler flags manually.

These are the requirements for using Aedis

- Boost 1.78 or greater.
- C++17. Some examples require C++20 with coroutine support.
- Redis 6 or higher. Optionally also redis-cli and Redis Sentinel.

The following compilers are supported

- Tested with gcc: 10, 11, 12.
- Tested with clang: 11, 13, 14.


## Why Aedis

At the time of this writing there are seventeen Redis clients
listed in the [official](https://redis.io/docs/clients/#cpp) list.
With so many clients available it is not unlikely that users are
asking themselves why yet another one.  In this section I will try
to compare Aedis with the most popular clients and why we need
Aedis. Notice however that this is ongoing work as comparing
client objectively is difficult and time consuming.

The most popular client at the moment of this writing ranked by
github stars is

* https://github.com/sewenew/redis-plus-plus

Before we start it is worth mentioning some of the things it does
not support

* RESP3. Without RESP3 is impossible to support some important Redis features like client side caching, among other things.
* Coroutines.
* Reading responses directly in user data structures avoiding temporaries.
* Error handling with error-code and exception overloads.
* Healthy checks.

The remaining points will be addressed individually.

### redis-plus-plus

Let us first have a look at what sending a command a pipeline and a
transaction look like

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

This is clearly a downside of the API as pipelines should be the
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
It is also not clear how are pipelines realised with the design
(if at all).

## Build status

Branch          | GH Actions | codecov.io |
:-------------: | ---------- | ---------- |
[`master`](https://github.com/mzimbres/aedis/tree/master) | [![CI](https://github.com/mzimbres/aedis/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/mzimbres/aedis/actions/workflows/ci.yml) | [![codecov](https://codecov.io/gh/mzimbres/aedis/branch/master/graph/badge.svg)](https://codecov.io/gh/mzimbres/aedis/branch/master)

## Reference

See [Reference](#any)

## Acknowledgement

Some people that were helpful in the development of Aedis

* Richard Hodges ([madmongo1](https://github.com/madmongo1)): For helping me with Asio and the design of asynchronous programs in general.
* Vinícius dos Santos Oliveira ([vinipsmaker](https://github.com/vinipsmaker)): For useful discussion about how Aedis consumes buffers in the read operation (among other things).
* Petr Dannhofer ([Eddie-cz](https://github.com/Eddie-cz)): For helping me understand how the `AUTH` and `HELLO` command can influence each other.

