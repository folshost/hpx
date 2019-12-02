//  Copyright (c) 2019 Hartmut Kaiser
//
//  SPDX-License-Identifier: BSL-1.0
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// This tests whether noexcept function used a non-type template arguments are
// properly supported by the compiler.

template <typename F, F* f>
struct action;

template <typename R, typename... Args, R (*f)(Args...)>
struct action
{
};

template <typename R, typename... Args, R (*f)(Args...) noexcept>
struct action
{
};

void foo() noexcept {}

int main()
{
    action<decltype(&foo), &foo> t;
    return 0;
}
