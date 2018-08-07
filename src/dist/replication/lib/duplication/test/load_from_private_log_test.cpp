/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 *
 * -=- Robust Distributed System Nucleus (rDSN) -=-
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "dist/replication/lib/mutation_log_utils.h"
#include "dist/replication/lib/duplication/load_from_private_log.h"

#include "duplication_test_base.h"

namespace dsn {
namespace replication {

struct load_from_private_log_test : public replica_test_base
{
    load_from_private_log_test() : duplicator(create_test_duplicator())
    {
        utils::filesystem::remove_path(_log_dir);
        utils::filesystem::create_directory(_log_dir);
    }

    void test_find_log_file_to_start()
    {
        load_from_private_log load(_replica.get(), duplicator.get());

        std::vector<std::string> mutations;
        int max_log_file_mb = 1;

        mutation_log_ptr mlog = new mutation_log_private(
            _replica->dir(), max_log_file_mb, _replica->get_gpid(), nullptr, 1024, 512, 10000);
        EXPECT_EQ(mlog->open(nullptr, nullptr), ERR_OK);

        load.find_log_file_to_start({});
        ASSERT_FALSE(load._current);

        { // writing mutations to log which will generate multiple files
            for (int i = 0; i < 1000 * 50; i++) {
                std::string msg = "hello!";
                mutations.push_back(msg);
                mutation_ptr mu = create_test_mutation(2 + i, msg);
                mlog->append(mu, LPC_AIO_IMMEDIATE_CALLBACK, nullptr, nullptr, 0);
            }
            mlog->tracker()->wait_outstanding_tasks();
        }

        auto files = log_utils::list_all_files_or_die(_log_dir);

        load.set_start_decree(1);
        load.find_log_file_to_start(files);
        ASSERT_TRUE(load._current);
        ASSERT_EQ(load._current->index(), 1);

        load.set_start_decree(50);
        load.find_log_file_to_start(files);
        ASSERT_TRUE(load._current);
        ASSERT_EQ(load._current->index(), 1);

        std::map<int, log_file_ptr> log_file_map = log_utils::open_log_file_map(files);
        int last_idx = log_file_map.rbegin()->first;
        load.set_start_decree(1000 * 50 + 200);
        load.find_log_file_to_start(files);
        ASSERT_TRUE(load._current);
        ASSERT_EQ(load._current->index(), last_idx);
    }

    void test_start_duplication(int num_entries, int private_log_size_mb)
    {
        std::vector<std::string> mutations;

        mutation_log_ptr mlog = new mutation_log_private(
            _replica->dir(), private_log_size_mb, _replica->get_gpid(), nullptr, 1024, 512, 50000);
        EXPECT_EQ(mlog->open(nullptr, nullptr), ERR_OK);
        _replica->init_private_log(mlog);

        {
            for (int i = 1; i <= num_entries; i++) {
                std::string msg = "hello!";
                mutations.push_back(msg);
                mutation_ptr mu = create_test_mutation(i, msg);
                mlog->append(mu, LPC_AIO_IMMEDIATE_CALLBACK, nullptr, nullptr, 0);
            }

            // commit the last entry
            mutation_ptr mu = create_test_mutation(1 + num_entries, "hello!");
            mlog->append(mu, LPC_AIO_IMMEDIATE_CALLBACK, nullptr, nullptr, 0);
            mlog->tracker()->wait_outstanding_tasks();
        }

        load_and_wait_all_entries_loaded(num_entries, num_entries);
    }

    mutation_tuple_set load_and_wait_all_entries_loaded(int total, int last_decree)
    {
        load_from_private_log load(_replica.get(), duplicator.get());
        load.set_start_decree(1);

        mutation_tuple_set loaded_mutations;
        pipeline::do_when<decree, mutation_tuple_set> end_stage(
            [&loaded_mutations, &load, total, last_decree](decree &&d,
                                                           mutation_tuple_set &&mutations) {
                // we create one mutation_update per mutation
                // the mutations are started from 1
                for (mutation_tuple mut : mutations) {
                    loaded_mutations.emplace(mut);
                }

                if (loaded_mutations.size() < total || d < last_decree) {
                    load.run();
                }
            });

        duplicator->from(load).link(end_stage);
        duplicator->run_pipeline();
        duplicator->wait_all();

        return loaded_mutations;
    }

    std::unique_ptr<replica_duplicator> duplicator;
};

TEST_F(load_from_private_log_test, find_log_file_to_start) { test_find_log_file_to_start(); }

TEST_F(load_from_private_log_test, start_duplication_10000_4MB)
{
    test_start_duplication(10000, 4);
}

TEST_F(load_from_private_log_test, start_duplication_50000_4MB)
{
    test_start_duplication(50000, 4);
}

TEST_F(load_from_private_log_test, start_duplication_10000_1MB)
{
    test_start_duplication(10000, 1);
}

TEST_F(load_from_private_log_test, start_duplication_50000_1MB)
{
    test_start_duplication(50000, 1);
}

TEST_F(load_from_private_log_test, start_duplication_100000_1MB)
{
    test_start_duplication(100000, 1);
}

TEST_F(load_from_private_log_test, start_duplication_100000_4MB)
{
    test_start_duplication(100000, 4);
}

// Ensure replica_duplicator can correctly handle real-world log file
TEST_F(load_from_private_log_test, handle_real_private_log)
{
    struct test_data
    {
        std::string fname;
        int puts;
        int total;
    } tests[] = {
        // PUT, PUT, PUT, EMPTY, PUT, EMPTY, EMPTY
        {"log.1.0.handle_real_private_log", 4, 6},

        // EMPTY, PUT, EMPTY
        {"log.1.0.handle_real_private_log2", 1, 2},

        // EMPTY, EMPTY, EMPTY
        {"log.1.0.all_loaded_are_write_empties", 0, 2},
    };

    for (auto tt : tests) {
        ASSERT_TRUE(utils::filesystem::rename_path(tt.fname, _log_dir + "/log.1.0"));

        {
            /// load log.1.0
            mutation_log_ptr mlog = new mutation_log_private(
                _replica->dir(), 4, _replica->get_gpid(), nullptr, 1024, 512, 10000);
            _replica->init_private_log(mlog);
            mlog->update_max_commit_on_disk(1);
        }

        load_and_wait_all_entries_loaded(tt.puts, tt.total);
    }
}

} // namespace replication
} // namespace dsn
