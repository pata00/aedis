/* Copyright (c) 2018-2022 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#ifndef AEDIS_RESP3_EXEC_HPP
#define AEDIS_RESP3_EXEC_HPP

#include <asio/detail/assert.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <asio/coroutine.hpp>
#include <asio/compose.hpp>
#include <asio/steady_timer.hpp>
#include <asio/experimental/parallel_group.hpp>

#include <aedis/error.hpp>
#include <aedis/resp3/read.hpp>
#include <aedis/resp3/request.hpp>

#include <asio/yield.hpp>

namespace aedis::resp3::detail {

template <
   class AsyncStream,
   class Adapter,
   class DynamicBuffer
   >
struct exec_op {
   AsyncStream* socket = nullptr;
   request const* req = nullptr;
   Adapter adapter;
   DynamicBuffer dbuf{};
   std::size_t n_cmds = 0;
   std::size_t size = 0;
   asio::coroutine coro{};

   template <class Self>
   void operator()( Self& self
                  , asio::error_code ec = {}
                  , std::size_t n = 0)
   {
      reenter (coro) for (;;)
      {
         if (req) {
            yield
            asio::async_write(
               *socket,
               asio::buffer(req->payload()),
               std::move(self));
            AEDIS_CHECK_OP1();

            if (n_cmds == 0) {
               self.complete({}, n);
               return;
            }

            req = nullptr;
         }

         yield resp3::async_read(*socket, dbuf, adapter, std::move(self));
         AEDIS_CHECK_OP1();

         size += n;
         if (--n_cmds == 0) {
            self.complete(ec, size);
            return;
         }
      }
   }
};

template <
   class AsyncStream,
   class Adapter,
   class DynamicBuffer,
   class CompletionToken = asio::default_completion_token_t<typename AsyncStream::executor_type>
   >
auto async_exec(
   AsyncStream& socket,
   request const& req,
   Adapter adapter,
   DynamicBuffer dbuf,
   CompletionToken token = CompletionToken{})
{
   return asio::async_compose
      < CompletionToken
      , void(asio::error_code, std::size_t)
      >(detail::exec_op<AsyncStream, Adapter, DynamicBuffer>
         {&socket, &req, adapter, dbuf, req.size()}, token, socket);
}

template <
   class AsyncStream,
   class Timer,
   class Adapter,
   class DynamicBuffer
   >
struct exec_with_timeout_op {
   AsyncStream* socket = nullptr;
   Timer* timer = nullptr;
   request const* req = nullptr;
   Adapter adapter;
   DynamicBuffer dbuf{};
   asio::coroutine coro{};

   template <class Self>
   void operator()( Self& self
                  , std::array<std::size_t, 2> order = {}
                  , asio::error_code ec1 = {}
                  , std::size_t n = 0
                  , asio::error_code ec2 = {})
   {
      reenter (coro)
      {
         yield
         asio::experimental::make_parallel_group(
            [this](auto token) { return detail::async_exec(*socket, *req, adapter, dbuf, token);},
            [this](auto token) { return timer->async_wait(token);}
         ).async_wait(
            asio::experimental::wait_for_one(),
            std::move(self));

         if (is_cancelled(self)) {
            self.complete(asio::error::operation_aborted, 0);
            return;
         }

         switch (order[0]) {
            case 0: self.complete(ec1, n); break;
            case 1:
            {
               if (ec2) {
                  self.complete(ec2, 0);
               } else {
                  self.complete(aedis::error::exec_timeout, 0);
               }

            } break;

            default: ASIO_ASSERT(false);
         }
      }
   }
};

template <
   class AsyncStream,
   class Timer,
   class Adapter,
   class DynamicBuffer,
   class CompletionToken = asio::default_completion_token_t<typename AsyncStream::executor_type>
   >
auto async_exec(
   AsyncStream& socket,
   Timer& timer,
   request const& req,
   Adapter adapter,
   DynamicBuffer dbuf,
   CompletionToken token = CompletionToken{})
{
   return asio::async_compose
      < CompletionToken
      , void(asio::error_code, std::size_t)
      >(detail::exec_with_timeout_op<AsyncStream, Timer, Adapter, DynamicBuffer>
         {&socket, &timer, &req, adapter, dbuf}, token, socket, timer);
}

} // aedis::resp3::detail

#include <asio/unyield.hpp>
#endif // AEDIS_RESP3_EXEC_HPP
