/* Copyright (c) 2019 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <map>
#include <set>
#include <vector>
#include <iostream>

#include <aedis/aedis.hpp>
#include <aedis/src.hpp>

namespace net = aedis::net;
using aedis::redis::command;
using aedis::redis::receiver_tuple;
using aedis::resp3::node;
using client_type = aedis::redis::client<net::ip::tcp::socket>;
using response_type = std::vector<node<std::string>>;

void print_flat_aggregate(response_type const& v)
{
   auto const m = element_multiplicity(v.front().data_type);
   for (auto i = 0lu; i < m * v.front().aggregate_size; ++i)
      std::cout << v[i + 1].value << " ";
   std::cout << "\n";
}

struct receiver : receiver_tuple<response_type> {
private:
   client_type* db_;

public:
   receiver(client_type& db) : db_{&db} {}

   void on_read(command cmd) override
   {
      switch (cmd) {
         case command::hello:
         {
            std::map<std::string, std::string> map
               { {"key1", "value1"}
               , {"key2", "value2"}
               , {"key3", "value3"}
               };

            std::vector<int> vec
               {1, 2, 3, 4, 5, 6};

            std::set<std::string> set
               {"one", "two", "three", "four"};

            // Sends the stl containers.
            db_->send_range(command::hset, "hset-key", std::cbegin(map), std::cend(map));
            db_->send_range(command::rpush, "rpush-key", std::cbegin(vec), std::cend(vec));
            db_->send_range(command::sadd, "sadd-key", std::cbegin(set), std::cend(set));

            // Retrieves the containers.
            db_->send(command::hgetall, "hset-key");
            db_->send(command::lrange, "rpush-key", 0, -1);
            db_->send(command::smembers, "sadd-key");
            db_->send(command::quit);
         } break;

         case command::lrange:
         case command::smembers:
         case command::hgetall:
         print_flat_aggregate(get<response_type>());
         break;

         default:;
      }

      get<response_type>().clear();
   }
};

int main()
{
   net::io_context ioc;
   client_type db{ioc.get_executor()};
   receiver recv{db};

   db.async_run(
      recv,
      {net::ip::make_address("127.0.0.1"), 6379},
      [](auto ec){ std::cout << ec.message() << std::endl;});

   ioc.run();
}