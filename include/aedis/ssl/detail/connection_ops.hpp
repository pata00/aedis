/* Copyright (c) 2018-2022 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#ifndef AEDIS_SSL_CONNECTION_OPS_HPP
#define AEDIS_SSL_CONNECTION_OPS_HPP

#include <array>

#include <asio/detail/assert.hpp>
#include <boost/system.hpp>
#include <asio/experimental/parallel_group.hpp>
#include <asio/yield.hpp>

namespace aedis::ssl::detail
{

template <class Stream>
struct handshake_op {
   Stream* stream;
   aedis::detail::conn_timer_t<typename Stream::executor_type>* timer;
   asio::coroutine coro{};

   template <class Self>
   void operator()( Self& self
                  , std::array<std::size_t, 2> order = {}
                  , asio::error_code ec1 = {}
                  , asio::error_code ec2 = {})
   {
      reenter (coro)
      {
         yield
         asio::experimental::make_parallel_group(
            [this](auto token)
            {
               return stream->async_handshake(asio::ssl::stream_base::client, token);
            },
            [this](auto token) { return timer->async_wait(token);}
         ).async_wait(
            asio::experimental::wait_for_one(),
            std::move(self));

         if (is_cancelled(self)) {
            self.complete(asio::error::operation_aborted);
            return;
         }

         switch (order[0]) {
            case 0: self.complete(ec1); return;
            case 1:
            {
               ASIO_ASSERT_MSG(!ec2, "handshake_op: Incompatible state.");
               self.complete(error::ssl_handshake_timeout);
               return;
            }

            default: ASIO_ASSERT(false);
         }
      }
   }
};

template <
   class Stream,
   class CompletionToken
   >
auto async_handshake(
      Stream& stream,
      aedis::detail::conn_timer_t<typename Stream::executor_type>& timer,
      CompletionToken&& token)
{
   return asio::async_compose
      < CompletionToken
      , void(asio::error_code)
      >(handshake_op<Stream>{&stream, &timer}, token, stream, timer);
}

template <class Conn, class Timer>
struct ssl_connect_with_timeout_op {
   Conn* conn = nullptr;
   asio::ip::tcp::resolver::results_type const* endpoints = nullptr;
   typename Conn::timeouts ts;
   Timer* timer = nullptr;
   asio::coroutine coro{};

   template <class Self>
   void operator()( Self& self
                  , asio::error_code ec = {}
                  , asio::ip::tcp::endpoint const& = {})
   {
      reenter (coro)
      {
         timer->expires_after(ts.connect_timeout);
         yield
         aedis::detail::async_connect(
            conn->lowest_layer(), *timer, *endpoints, std::move(self));
         AEDIS_CHECK_OP0();

         timer->expires_after(ts.handshake_timeout);
         yield
         async_handshake(conn->next_layer(), *timer, std::move(self));
         AEDIS_CHECK_OP0();
         self.complete({});
      }
   }
};

} // aedis::ssl::detail
 
#include <asio/unyield.hpp>
#endif // AEDIS_SSL_CONNECTION_OPS_HPP
