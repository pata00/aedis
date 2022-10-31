/* Copyright (c) 2018-2022 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#ifndef AEDIS_NET_HPP
#define AEDIS_NET_HPP

#include <array>

#include <boost/system.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/connect.hpp>
#include <asio/detail/assert.hpp>
#include <asio/experimental/parallel_group.hpp>

#include <asio/yield.hpp>

namespace aedis::detail {

template <class Executor>
using conn_timer_t = asio::basic_waitable_timer<std::chrono::steady_clock, asio::wait_traits<std::chrono::steady_clock>, Executor>;

template <
   class Stream,
   class EndpointSequence
   >
struct connect_op {
   Stream* socket;
   conn_timer_t<typename Stream::executor_type>* timer;
   EndpointSequence* endpoints;
   asio::coroutine coro{};

   template <class Self>
   void operator()( Self& self
                  , std::array<std::size_t, 2> order = {}
                  , asio::error_code ec1 = {}
                  , typename Stream::protocol_type::endpoint const& ep = {}
                  , asio::error_code ec2 = {})
   {
      reenter (coro)
      {
         yield
         asio::experimental::make_parallel_group(
            [this](auto token)
            {
               auto f = [](asio::error_code const&, auto const&) { return true; };
               return asio::async_connect(*socket, *endpoints, f, token);
            },
            [this](auto token) { return timer->async_wait(token);}
         ).async_wait(
            asio::experimental::wait_for_one(),
            std::move(self));

         if (is_cancelled(self)) {
            self.complete(asio::error::operation_aborted, {});
            return;
         }

         switch (order[0]) {
            case 0: self.complete(ec1, ep); return;
            case 1:
            {
               if (ec2) {
                  self.complete(ec2, {});
               } else {
                  self.complete(error::connect_timeout, ep);
               }
               return;
            }

            default: ASIO_ASSERT(false);
         }
      }
   }
};

template <class Resolver, class Timer>
struct resolve_op {
   Resolver* resv;
   Timer* timer;
   std::string_view host;
   std::string_view port;
   asio::coroutine coro{};

   template <class Self>
   void operator()( Self& self
                  , std::array<std::size_t, 2> order = {}
                  , asio::error_code ec1 = {}
                  , asio::ip::tcp::resolver::results_type res = {}
                  , asio::error_code ec2 = {})
   {
      reenter (coro)
      {
         yield
         asio::experimental::make_parallel_group(
            [this](auto token) { return resv->async_resolve(host.data(), port.data(), token);},
            [this](auto token) { return timer->async_wait(token);}
         ).async_wait(
            asio::experimental::wait_for_one(),
            std::move(self));

         if (is_cancelled(self)) {
            self.complete(asio::error::operation_aborted, {});
            return;
         }

         switch (order[0]) {
            case 0: self.complete(ec1, res); return;

            case 1:
            {
               if (ec2) {
                  self.complete(ec2, {});
               } else {
                  self.complete(error::resolve_timeout, {});
               }
               return;
            }

            default: ASIO_ASSERT(false);
         }
      }
   }
};

template <class Channel>
struct send_receive_op {
   Channel* channel;
   asio::coroutine coro{};

   template <class Self>
   void operator()( Self& self
                  , asio::error_code ec = {}
                  , std::size_t = 0)
   {
      reenter (coro)
      {
         yield
         channel->async_send(asio::error_code{}, 0, std::move(self));
         AEDIS_CHECK_OP1();

         yield
         channel->async_receive(std::move(self));
         AEDIS_CHECK_OP1();

         self.complete({}, 0);
      }
   }
};

template <
   class Stream,
   class EndpointSequence,
   class CompletionToken
   >
auto async_connect(
      Stream& socket,
      conn_timer_t<typename Stream::executor_type>& timer,
      EndpointSequence ep,
      CompletionToken&& token)
{
   return asio::async_compose
      < CompletionToken
      , void(asio::error_code, typename Stream::protocol_type::endpoint const&)
      >(connect_op<Stream, EndpointSequence>
            {&socket, &timer, &ep}, token, socket, timer);
}

template <
   class Resolver,
   class Timer,
   class CompletionToken =
      asio::default_completion_token_t<typename Resolver::executor_type>
   >
auto async_resolve(
      Resolver& resv,
      Timer& timer,
      std::string_view host,
      std::string_view port,
      CompletionToken&& token = CompletionToken{})
{
   return asio::async_compose
      < CompletionToken
      , void(asio::error_code, asio::ip::tcp::resolver::results_type)
      >(resolve_op<Resolver, Timer>{&resv, &timer, host, port}, token, resv, timer);
}

template <
   class Channel,
   class CompletionToken =
      asio::default_completion_token_t<typename Channel::executor_type>
   >
auto async_send_receive(Channel& channel, CompletionToken&& token = CompletionToken{})
{
   return asio::async_compose
      < CompletionToken
      , void(asio::error_code, std::size_t)
      >(send_receive_op<Channel>{&channel}, token, channel);
}
} // aedis::detail

#include <asio/unyield.hpp>
#endif // AEDIS_NET_HPP
