/* Copyright (c) 2018-2022 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#include <iostream>
#include <boost/asio.hpp>
#include <boost/system/errc.hpp>
#include <boost/asio/experimental/as_tuple.hpp>

#define BOOST_TEST_MODULE low level
#include <boost/test/included/unit_test.hpp>

#include <aedis.hpp>
#include <aedis/src.hpp>

namespace net = boost::asio;

using aedis::resp3::request;
using aedis::adapt;
using aedis::endpoint;
using aedis::operation;
using connection = aedis::connection<>;
using error_code = boost::system::error_code;
using net::experimental::as_tuple;

BOOST_AUTO_TEST_CASE(push_filtered_out)
{
   net::io_context ioc;
   auto conn = std::make_shared<connection>(ioc);

   request req;
   req.push("PING");
   req.push("SUBSCRIBE", "channel");
   req.push("QUIT");

   std::tuple<std::string, std::string> resp;
   conn->async_exec(req, adapt(resp), [](auto ec, auto){
      BOOST_TEST(!ec);
   });

   conn->async_receive(adapt(), [](auto ec, auto){
      BOOST_TEST(!ec);
   });

   conn->async_run({"127.0.0.1", "6379"}, {}, [conn](auto ec){
      BOOST_CHECK_EQUAL(ec, net::error::misc_errors::eof);
   });

   ioc.run();

   BOOST_CHECK_EQUAL(std::get<0>(resp), "PONG");
   BOOST_CHECK_EQUAL(std::get<1>(resp), "OK");
}

// Checks whether we get idle timeout when no push reader is set.
void test_missing_push_reader1(bool coalesce)
{
   net::io_context ioc;
   auto conn = std::make_shared<connection>(ioc);

   request req{{false, coalesce}};
   req.get_config().cancel_on_connection_lost = true;
   req.push("SUBSCRIBE", "channel");

   conn->async_exec(req, adapt(), [](auto ec, auto){
      BOOST_TEST(!ec);
   });

   conn->async_run({"127.0.0.1", "6379"}, {}, [conn](auto ec){
      BOOST_CHECK_EQUAL(ec, aedis::error::idle_timeout);
   });

   ioc.run();
}

void test_missing_push_reader2(request const& req)
{
   net::io_context ioc;
   auto conn = std::make_shared<connection>(ioc);

   conn->async_exec(req, adapt(), [](auto ec, auto){
      BOOST_TEST(!ec);
   });

   conn->async_run({"127.0.0.1", "6379"}, {}, [](auto ec){
      BOOST_CHECK_EQUAL(ec, aedis::error::idle_timeout);
   });

   ioc.run();
}

#ifdef BOOST_ASIO_HAS_CO_AWAIT
net::awaitable<void> push_consumer1(std::shared_ptr<connection> conn, bool& push_received)
{
   {
      auto [ec, ev] = co_await conn->async_receive(adapt(), as_tuple(net::use_awaitable));
      BOOST_TEST(!ec);
   }

   {
      auto [ec, ev] = co_await conn->async_receive(adapt(), as_tuple(net::use_awaitable));
      BOOST_CHECK_EQUAL(ec, boost::asio::experimental::channel_errc::channel_cancelled);
   }

   push_received = true;
}

struct adapter_error {
   void
   operator()(
      std::size_t, aedis::resp3::node<boost::string_view> const&, boost::system::error_code& ec)
   {
      ec = aedis::error::incompatible_size;
   }

   [[nodiscard]]
   auto get_supported_response_size() const noexcept
      { return static_cast<std::size_t>(-1);}

   [[nodiscard]]
   auto get_max_read_size(std::size_t) const noexcept
      { return static_cast<std::size_t>(-1);}
};

BOOST_AUTO_TEST_CASE(test_push_adapter)
{
   std::cout << boost::unit_test::framework::current_test_case().p_name << std::endl;
   net::io_context ioc;
   auto conn = std::make_shared<connection>(ioc);

   request req;
   req.push("PING");
   req.push("SUBSCRIBE", "channel");
   req.push("PING");

   conn->async_receive(adapter_error{}, [](auto ec, auto) {
      BOOST_CHECK_EQUAL(ec, aedis::error::incompatible_size);
   });

   conn->async_exec(req, adapt(), [](auto ec, auto){
      BOOST_CHECK_EQUAL(ec, boost::asio::experimental::error::channel_errors::channel_cancelled);
   });

   conn->async_run({"127.0.0.1", "6379"}, {}, [](auto ec){
      BOOST_CHECK_EQUAL(ec, boost::system::errc::errc_t::operation_canceled);
   });

   ioc.run();

   // TODO: Reset the ioc reconnect and send a quit to ensure
   // reconnection is possible after an error.
}

void test_push_is_received1(bool coalesce)
{
   net::io_context ioc;
   auto conn = std::make_shared<connection>(ioc);

   request req{{false, coalesce}};
   req.push("SUBSCRIBE", "channel");
   req.push("QUIT");

   conn->async_exec(req, adapt(), [](auto ec, auto){
      BOOST_TEST(!ec);
   });

   conn->async_run({"127.0.0.1", "6379"}, {}, [conn](auto ec){
      BOOST_CHECK_EQUAL(ec, net::error::misc_errors::eof);
      conn->cancel(operation::receive);
   });

   bool push_received = false;
   net::co_spawn(
      ioc.get_executor(),
      push_consumer1(conn, push_received),
      net::detached);

   ioc.run();

   BOOST_TEST(push_received);
}

void test_push_is_received2(bool coalesce)
{
   request req1{{false, coalesce}};
   req1.push("PING", "Message1");

   request req2{{false, coalesce}};
   req2.push("SUBSCRIBE", "channel");

   request req3{{false, coalesce}};
   req3.push("PING", "Message2");
   req3.push("QUIT");

   net::io_context ioc;

   auto conn = std::make_shared<connection>(ioc);

   auto handler =[](auto ec, auto...)
   {
      BOOST_TEST(!ec);
   };

   conn->async_exec(req1, adapt(), handler);
   conn->async_exec(req2, adapt(), handler);
   conn->async_exec(req3, adapt(), handler);

   endpoint ep{"127.0.0.1", "6379"};
   conn->async_run(ep, {}, [conn](auto ec) {
      BOOST_CHECK_EQUAL(ec, net::error::misc_errors::eof);
      conn->cancel(operation::receive);
   });

   bool push_received = false;
   net::co_spawn(
      ioc.get_executor(),
      push_consumer1(conn, push_received),
      net::detached);

   ioc.run();

   BOOST_TEST(push_received);
}

net::awaitable<void> push_consumer3(std::shared_ptr<connection> conn)
{
   for (;;)
      co_await conn->async_receive(adapt(), net::use_awaitable);
}

// Test many subscribe requests.
void test_push_many_subscribes(bool coalesce)
{
   request req0{{false, coalesce}};
   req0.push("HELLO", 3);

   request req1{{false, coalesce}};
   req1.push("PING", "Message1");

   request req2{{false, coalesce}};
   req2.push("SUBSCRIBE", "channel");

   request req3{{false, coalesce}};
   req3.push("QUIT");

   auto handler =[](auto ec, auto...)
   {
      BOOST_TEST(!ec);
   };

   net::io_context ioc;
   auto conn = std::make_shared<connection>(ioc);
   conn->async_exec(req0, adapt(), handler);
   conn->async_exec(req1, adapt(), handler);
   conn->async_exec(req2, adapt(), handler);
   conn->async_exec(req2, adapt(), handler);
   conn->async_exec(req1, adapt(), handler);
   conn->async_exec(req2, adapt(), handler);
   conn->async_exec(req1, adapt(), handler);
   conn->async_exec(req2, adapt(), handler);
   conn->async_exec(req2, adapt(), handler);
   conn->async_exec(req1, adapt(), handler);
   conn->async_exec(req2, adapt(), handler);
   conn->async_exec(req3, adapt(), handler);

   endpoint ep{"127.0.0.1", "6379"};
   conn->async_run(ep, {}, [conn](auto ec) {
      BOOST_CHECK_EQUAL(ec, net::error::misc_errors::eof);
      conn->cancel(operation::receive);
   });

   net::co_spawn(ioc.get_executor(), push_consumer3(conn), net::detached);
   ioc.run();
}

BOOST_AUTO_TEST_CASE(push_received1)
{
   std::cout << boost::unit_test::framework::current_test_case().p_name << std::endl;
   test_push_is_received1(true);
   test_push_is_received1(false);
}

BOOST_AUTO_TEST_CASE(push_received2)
{
   std::cout << boost::unit_test::framework::current_test_case().p_name << std::endl;
   test_push_is_received2(true);
   test_push_is_received2(false);
}

BOOST_AUTO_TEST_CASE(many_subscribers)
{
   std::cout << boost::unit_test::framework::current_test_case().p_name << std::endl;
   test_push_many_subscribes(true);
   test_push_many_subscribes(false);
}
#endif

BOOST_AUTO_TEST_CASE(missing_reader1_coalesce)
{
   std::cout << boost::unit_test::framework::current_test_case().p_name << std::endl;
   test_missing_push_reader1(true);
}

BOOST_AUTO_TEST_CASE(missing_reader1_no_coalesce)
{
   std::cout << boost::unit_test::framework::current_test_case().p_name << std::endl;
   test_missing_push_reader1(false);
}

BOOST_AUTO_TEST_CASE(missing_reader2a)
{
   std::cout << boost::unit_test::framework::current_test_case().p_name << std::endl;
   request req1{{false}};
   req1.push("PING", "Message");
   req1.push("SUBSCRIBE"); // Wrong command synthax.

   req1.get_config().coalesce = true;
   test_missing_push_reader2(req1);

   req1.get_config().coalesce = false;
   test_missing_push_reader2(req1);
}

BOOST_AUTO_TEST_CASE(missing_reader2b)
{
   std::cout << boost::unit_test::framework::current_test_case().p_name << std::endl;
   request req2{{false}};
   req2.push("SUBSCRIBE"); // Wrong command syntax.

   req2.get_config().coalesce = true;
   test_missing_push_reader2(req2);

   req2.get_config().coalesce = false;
   test_missing_push_reader2(req2);
}

