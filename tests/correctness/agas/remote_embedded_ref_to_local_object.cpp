//  Copyright (c) 2011 Bryce Adelstein-Lelbach
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/hpx_init.hpp>
#include <hpx/include/iostreams.hpp>
#include <hpx/util/lightweight_test.hpp>
#include <hpx/runtime/applier/applier.hpp>
#include <hpx/runtime/agas/interface.hpp>

#include <boost/date_time/posix_time/posix_time.hpp>

#include <tests/correctness/agas/components/simple_refcnt_checker.hpp>
#include <tests/correctness/agas/components/managed_refcnt_checker.hpp>

using boost::program_options::variables_map;
using boost::program_options::options_description;
using boost::program_options::value;

using hpx::init;
using hpx::finalize;
using hpx::find_here;

using boost::posix_time::milliseconds;

using hpx::naming::id_type;
using hpx::naming::get_management_type_name;

using hpx::components::component_type;
using hpx::components::get_component_type;

using hpx::applier::get_applier;

using hpx::agas::garbage_collect;

using hpx::test::simple_refcnt_monitor;
using hpx::test::managed_refcnt_monitor;

using hpx::util::report_errors;

using hpx::cout;
using hpx::flush;

///////////////////////////////////////////////////////////////////////////////
template <
    typename Client
>
void hpx_test_main(
    variables_map& vm
    )
{
    boost::uint64_t const delay = vm["delay"].as<boost::uint64_t>();

    {
        /// AGAS reference-counting test 5 (from #126):
        ///
        ///     Create two components, one locally and one one remotely.
        ///     Have the remote component store a reference to the local
        ///     component. Let the original references to both components go
        ///     out of scope. Both components should be deleted.

        std::vector<id_type> remote_localities;

        typedef typename Client::server_type server_type;

        component_type ctype = get_component_type<server_type>();

        if (!get_applier().get_remote_prefixes(remote_localities, ctype))
            throw std::logic_error("this test cannot be run on one locality");

        Client monitor_remote(remote_localities[0]);
        Client monitor_local(find_here());

        cout << "id_remote: " << monitor_remote.get_gid() << " "
             << get_management_type_name
                    (monitor_remote.get_gid().get_management_type()) << "\n"
             << "id_local:  " << monitor_local.get_gid() << " "
             << get_management_type_name
                    (monitor_local.get_gid().get_management_type()) << "\n"
             << flush;

        {
            // Have the remote object store a reference to the local object.
            monitor_remote.take_reference(monitor_local.get_gid());

            // Detach the references.
            id_type id_remote = monitor_remote.detach()
                  , id_local = monitor_local.detach();

            // Both components should still be alive.
            HPX_TEST_EQ(false, monitor_remote.ready(milliseconds(delay)));
            HPX_TEST_EQ(false, monitor_local.ready(milliseconds(delay)));
        }
        
        // Flush pending reference counting operations.
        garbage_collect();

        // Both components should be out of scope now.
        HPX_TEST_EQ(true, monitor_remote.ready(milliseconds(delay)));
        HPX_TEST_EQ(true, monitor_local.ready(milliseconds(delay)));
    }
}

///////////////////////////////////////////////////////////////////////////////
int hpx_main(
    variables_map& vm
    )
{
    {
        cout << std::string(80, '#') << "\n"
             << "simple component test\n"
             << std::string(80, '#') << "\n" << flush;

        hpx_test_main<simple_refcnt_monitor>(vm);

        cout << std::string(80, '#') << "\n"
             << "managed component test\n"
             << std::string(80, '#') << "\n" << flush;

        hpx_test_main<managed_refcnt_monitor>(vm);
    }

    finalize();
    return report_errors();
}

///////////////////////////////////////////////////////////////////////////////
int main(
    int argc
  , char* argv[]
    )
{
    // Configure application-specific options.
    options_description cmdline("usage: " HPX_APPLICATION_STRING " [options]");

    cmdline.add_options()
        ( "delay"
        , value<boost::uint64_t>()->default_value(1000)
        , "number of milliseconds to wait for object destruction")
        ;

    // Initialize and run HPX.
    return init(cmdline, argc, argv);
}

