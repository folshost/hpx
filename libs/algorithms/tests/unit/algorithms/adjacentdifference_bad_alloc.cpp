//  Copyright (c) 2015 Daniel Bourgeois
//
//  SPDX-License-Identifier: BSL-1.0
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/hpx.hpp>
#include <hpx/hpx_init.hpp>
#include <hpx/include/parallel_adjacent_difference.hpp>
#include <hpx/testing.hpp>

#include <cstddef>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "test_utils.hpp"

//////////////////////////////////////////////////////////////////////////////
template <typename ExPolicy, typename IteratorTag>
void test_adjacent_difference_bad_alloc(ExPolicy policy, IteratorTag)
{
    static_assert(
        hpx::parallel::execution::is_execution_policy<ExPolicy>::value,
        "hpx::parallel::execution::is_execution_policy<ExPolicy>::value");

    typedef std::vector<std::size_t>::iterator base_iterator;
    typedef test::decorated_iterator<base_iterator, IteratorTag>
        decorated_iterator;

    std::vector<std::size_t> c(10007);
    std::vector<std::size_t> d(10007);

    bool caught_bad_alloc = false;
    try
    {
        hpx::parallel::adjacent_difference(policy,
            decorated_iterator(std::begin(c)), decorated_iterator(std::end(c)),
            std::begin(d), [](std::size_t lhs, std::size_t rhs) -> std::size_t {
                throw std::bad_alloc();
                return lhs - rhs;
            });
    }
    catch (std::bad_alloc const&)
    {
        caught_bad_alloc = true;
    }
    catch (...)
    {
        HPX_TEST(false);
    }

    HPX_TEST(caught_bad_alloc);
}

template <typename ExPolicy, typename IteratorTag>
void test_adjacent_difference_bad_alloc_async(ExPolicy p, IteratorTag)
{
    typedef std::vector<std::size_t>::iterator base_iterator;
    typedef test::decorated_iterator<base_iterator, IteratorTag>
        decorated_iterator;

    std::vector<std::size_t> c(10007);
    std::vector<std::size_t> d(10007);

    bool caught_bad_alloc = false;
    bool returned_from_algorithm = false;

    try
    {
        hpx::future<base_iterator> f = hpx::parallel::adjacent_difference(p,
            decorated_iterator(std::begin(c)), decorated_iterator(std::end(c)),
            std::begin(d), [](std::size_t lhs, std::size_t rhs) -> std::size_t {
                throw std::bad_alloc();
                return lhs - rhs;
            });
        returned_from_algorithm = true;

        f.get();

        HPX_TEST(false);
    }
    catch (std::bad_alloc const&)
    {
        caught_bad_alloc = true;
    }
    catch (...)
    {
        HPX_TEST(false);
    }

    HPX_TEST(caught_bad_alloc);
    HPX_TEST(returned_from_algorithm);
}

template <typename IteratorTag>
void test_adjacent_difference_bad_alloc()
{
    using namespace hpx::parallel;

    // If the execution policy object is of type vector_execution_policy,
    // std::terminate shall be called. therefore we do not test exceptions
    // with a vector execution policy
    test_adjacent_difference_bad_alloc(execution::seq, IteratorTag());
    test_adjacent_difference_bad_alloc(execution::par, IteratorTag());

    test_adjacent_difference_bad_alloc_async(
        execution::seq(execution::task), IteratorTag());
    test_adjacent_difference_bad_alloc_async(
        execution::par(execution::task), IteratorTag());
}

void adjacent_difference_bad_alloc_test()
{
    test_adjacent_difference_bad_alloc<std::random_access_iterator_tag>();
    test_adjacent_difference_bad_alloc<std::forward_iterator_tag>();
}

int hpx_main(hpx::program_options::variables_map& vm)
{
    unsigned int seed = (unsigned int) std::time(nullptr);
    if (vm.count("seed"))
        seed = vm["seed"].as<unsigned int>();

    std::cout << "using seed: " << seed << std::endl;
    std::srand(seed);

    adjacent_difference_bad_alloc_test();
    return hpx::finalize();
}

int main(int argc, char* argv[])
{
    // add command line option which controls the random number generator seed
    using namespace hpx::program_options;
    options_description desc_commandline(
        "Usage: " HPX_APPLICATION_STRING " [options]");

    desc_commandline.add_options()("seed,s", value<unsigned int>(),
        "the random number generator seed to use for this run");

    // By default this test should run on all available cores
    std::vector<std::string> const cfg = {"hpx.os_threads=all"};

    // Initialize and run HPX
    HPX_TEST_EQ_MSG(hpx::init(desc_commandline, argc, argv, cfg), 0,
        "HPX main exited with non-zero status");

    return hpx::util::report_errors();
}
