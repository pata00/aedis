/* Copyright (c) 2018-2022 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#include <map>
#include <set>
#include <vector>
#include <string>
#include <iostream>

#include <aedis/resp3/type.hpp>
#include <aedis/resp3/node.hpp>

// Some functions to make the examples less repetitive.

namespace net = boost::asio;
using aedis::resp3::node;
using aedis::adapter::adapt;
using aedis::adapter::adapter_t;
using aedis::redis::command;
using aedis::generic::client;

void print_and_clear_aggregate(std::vector<aedis::resp3::node<std::string>>& v)
{
   auto const m = aedis::resp3::element_multiplicity(v.front().data_type);
   for (auto i = 0lu; i < m * v.front().aggregate_size; ++i)
      std::cout << v[i + 1].value << " ";
   std::cout << "\n";
   v.clear();
}

void print_and_clear(std::set<std::string>& cont)
{
   for (auto const& e: cont) std::cout << e << " ";
   std::cout << "\n";
   cont.clear();
}

void print_and_clear(std::map<std::string, std::string>& cont)
{
   for (auto const& e: cont)
      std::cout << e.first << ": " << e.second << "\n";
   cont.clear();
}
