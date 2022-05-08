/* Copyright (c) 2018-2022 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#include <vector>
#include <iostream>

#include <boost/asio/signal_set.hpp>

#include <aedis/aedis.hpp>
#include <aedis/src.hpp>

#include "user_session.hpp"

namespace net = boost::asio;
using aedis::resp3::node;
using aedis::adapter::adapt;
using aedis::adapter::adapter_t;
using aedis::redis::command;
using aedis::generic::client;
using aedis::user_session;
using aedis::user_session_base;

using client_type = client<net::ip::tcp::socket, command>;
using response_type = std::vector<node<std::string>>;

class receiver {
public:
   receiver(std::shared_ptr<client_type> db)
   : adapter_{adapt(resp_)}
   , db_{db}
   {}

   void on_resp3(command cmd, node<boost::string_view> const& nd, boost::system::error_code& ec)
   {
      adapter_(nd, ec);
   }

   void on_read(command cmd, std::size_t)
   {
      switch (cmd) {
         case command::hello:
         db_->send(command::subscribe, "channel");
         break;

         case command::incr:
         std::cout << "Messages so far: " << resp_.front().value << std::endl;
         break;

         case command::invalid:
         {
            for (auto& session: sessions_)
               session->deliver(resp_.at(3).value);
         } break;

         default:;
      }

      resp_.clear();
   }

   auto add(std::shared_ptr<user_session_base> session)
      { sessions_.push_back(session); }

private:
   response_type resp_;
   adapter_t<response_type> adapter_;
   std::shared_ptr<client_type> db_;
   std::vector<std::shared_ptr<user_session_base>> sessions_;
};

net::awaitable<void>
listener(
    std::shared_ptr<net::ip::tcp::acceptor> acc,
    std::shared_ptr<client_type> db,
    std::shared_ptr<receiver> recv)
{
   auto on_user_msg = [db](std::string const& msg)
   {
      db->send(command::publish, "channel", msg);
      db->send(command::incr, "message-counter");
   };

   for (;;) {
      auto socket = co_await acc->async_accept(net::use_awaitable);
      auto session = std::make_shared<user_session>(std::move(socket));
      session->start(on_user_msg);
      recv->add(session);
   }
}

int main()
{
   try {
      net::io_context ioc{1};

      auto db = std::make_shared<client_type>(ioc.get_executor());
      auto recv = std::make_shared<receiver>(db);
      db->set_receiver(recv);
      db->async_run([](auto ec){ std::cout << ec.message() << std::endl;});

      auto endpoint = net::ip::tcp::endpoint{net::ip::tcp::v4(), 55555};
      auto acc = std::make_shared<net::ip::tcp::acceptor>(ioc.get_executor(), endpoint);
      co_spawn(ioc, listener(acc, db, recv), net::detached);

      net::signal_set signals(ioc.get_executor(), SIGINT, SIGTERM);
      signals.async_wait([&] (auto, int) { ioc.stop(); });

      ioc.run();
   } catch (std::exception const& e) {
      std::cerr << e.what() << std::endl;
   }
}
