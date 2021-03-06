/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef RUNTIME_MONITOR_H
#define RUNTIME_MONITOR_H

extern "C" {
#include "wiredtiger.h"
}

#include "util/debug_utils.h"
#include "util/api_const.h"
#include "core/component.h"
#include "core/throttle.h"
#include "connection_manager.h"
#include "workload/database_operation.h"

namespace test_harness {
/* Static statistic get function. */
static void
get_stat(scoped_cursor &cursor, int stat_field, int64_t *valuep)
{
    const char *desc, *pvalue;
    cursor->set_key(cursor.get(), stat_field);
    testutil_check(cursor->search(cursor.get()));
    testutil_check(cursor->get_value(cursor.get(), &desc, &pvalue, valuep));
    testutil_check(cursor->reset(cursor.get()));
}

/*
 * The WiredTiger configuration API doesn't accept string statistic names when retrieving statistic
 * values. This function provides the required mapping to statistic id. We should consider
 * generating it programmatically in `stat.py` to avoid having to manually add a condition every
 * time we want to observe a new postrun statistic.
 */
inline int
get_stat_field(const std::string &name)
{
    if (name == "cache_hs_insert")
        return (WT_STAT_CONN_CACHE_HS_INSERT);
    else if (name == "cc_pages_removed")
        return (WT_STAT_CONN_CC_PAGES_REMOVED);
    testutil_die(EINVAL, "get_stat_field: Stat \"%s\" is unrecognized", name.c_str());
}

class runtime_statistic {
    public:
    explicit runtime_statistic(configuration *config)
    {
        _enabled = config->get_bool(ENABLED);
    }

    /* Check that the given statistic is within bounds. */
    virtual void check(scoped_cursor &cursor) = 0;

    /* Suppress warning about destructor being non-virtual. */
    virtual ~runtime_statistic() {}

    bool
    enabled() const
    {
        return (_enabled);
    }

    protected:
    bool _enabled = false;
};

class cache_limit_statistic : public runtime_statistic {
    public:
    explicit cache_limit_statistic(configuration *config) : runtime_statistic(config)
    {
        limit = config->get_int(LIMIT);
    }

    void
    check(scoped_cursor &cursor) override final
    {
        testutil_assert(cursor.get() != nullptr);
        int64_t cache_bytes_image, cache_bytes_other, cache_bytes_max;
        double use_percent;
        /* Three statistics are required to compute cache use percentage. */
        get_stat(cursor, WT_STAT_CONN_CACHE_BYTES_IMAGE, &cache_bytes_image);
        get_stat(cursor, WT_STAT_CONN_CACHE_BYTES_OTHER, &cache_bytes_other);
        get_stat(cursor, WT_STAT_CONN_CACHE_BYTES_MAX, &cache_bytes_max);
        /*
         * Assert that we never exceed our configured limit for cache usage. Add 0.0 to avoid
         * floating point conversion errors.
         */
        use_percent = ((cache_bytes_image + cache_bytes_other + 0.0) / cache_bytes_max) * 100;
        if (use_percent > limit) {
            const std::string error_string =
              "runtime_monitor: Cache usage exceeded during test! Limit: " + std::to_string(limit) +
              " usage: " + std::to_string(use_percent);
            testutil_die(-1, error_string.c_str());
        } else
            log_msg(LOG_TRACE, "Cache usage: " + std::to_string(use_percent));
    }

    private:
    int64_t limit;
};

static std::string
collection_name_to_file_name(const std::string &collection_name)
{
    /* Strip out the URI prefix. */
    const size_t colon_pos = collection_name.find(':');
    testutil_assert(colon_pos != std::string::npos);
    const auto stripped_name = collection_name.substr(colon_pos + 1);

    /* Now add the directory and file extension. */
    return (std::string(DEFAULT_DIR) + "/" + stripped_name + ".wt");
}

class db_size_statistic : public runtime_statistic {
    public:
    db_size_statistic(configuration *config, database &database)
        : runtime_statistic(config), _database(database)
    {
        _limit = config->get_int(LIMIT);
#ifdef _WIN32
        log_msg("Database size checking is not implemented on Windows", LOG_ERROR);
#endif
    }
    virtual ~db_size_statistic() = default;

    /* Don't need the stat cursor for this. */
    void
    check(scoped_cursor &) override final
    {
        const auto file_names = get_file_names();
#ifndef _WIN32
        size_t db_size = 0;
        for (const auto &name : file_names) {
            struct stat sb;
            if (stat(name.c_str(), &sb) == 0) {
                db_size += sb.st_size;
                log_msg(LOG_TRACE, name + " was " + std::to_string(sb.st_size) + " bytes");
            } else
                /* The only good reason for this to fail is if the file hasn't been created yet. */
                testutil_assert(errno == ENOENT);
        }
        log_msg(LOG_TRACE, "Current database size is " + std::to_string(db_size) + " bytes");
        if (db_size > _limit) {
            const std::string error_string =
              "runtime_monitor: Database size limit exceeded during test! Limit: " +
              std::to_string(_limit) + " db size: " + std::to_string(db_size);
            testutil_die(-1, error_string.c_str());
        }
#else
        static_cast<void>(file_names);
        static_cast<void>(_database);
        static_cast<void>(_limit);
#endif
    }

    private:
    std::vector<std::string>
    get_file_names()
    {
        std::vector<std::string> file_names;
        for (const auto &name : _database.get_collection_names())
            file_names.push_back(collection_name_to_file_name(name));

        /* Add WiredTiger internal tables. */
        file_names.push_back(std::string(DEFAULT_DIR) + "/" + WT_HS_FILE);
        file_names.push_back(std::string(DEFAULT_DIR) + "/" + WT_METAFILE);

        return (file_names);
    }

    database &_database;
    int64_t _limit;
};

class postrun_statistic_check {
    public:
    explicit postrun_statistic_check(configuration *config)
    {
        const auto config_stats = config->get_list(POSTRUN_STATISTICS);
        /*
         * Each stat in the configuration is a colon separated list in the following format:
         * <stat_name>:<min_limit>:<max_limit>
         */
        for (const auto &c : config_stats) {
            auto stat = split_string(c, ':');
            if (stat.size() != 3)
                testutil_die(EINVAL,
                  "runtime_monitor: Each postrun statistic must follow the format of "
                  "\"stat_name:min_limit:max_limit\". Invalid format \"%s\" provided.",
                  c.c_str());
            const int min_limit = std::stoi(stat.at(1)), max_limit = std::stoi(stat.at(2));
            if (min_limit > max_limit)
                testutil_die(EINVAL,
                  "runtime_monitor: The min limit of each postrun statistic must be less than or "
                  "equal to its max limit. Config=\"%s\" Min=%ld Max=%ld",
                  c.c_str(), min_limit, max_limit);
            _stats.emplace_back(std::move(stat.at(0)), min_limit, max_limit);
        }
    }
    virtual ~postrun_statistic_check() = default;

    void
    check(scoped_cursor &cursor) const
    {
        bool success = true;
        for (const auto &stat : _stats) {
            if (!check_stat(cursor, stat))
                success = false;
        }
        if (!success)
            testutil_die(-1,
              "runtime_monitor: One or more postrun statistics were outside of their specified "
              "limits.");
    }

    private:
    struct postrun_statistic {
        postrun_statistic(std::string &&name, const int64_t min_limit, const int64_t max_limit)
            : name(std::move(name)), field(get_stat_field(this->name)), min_limit(min_limit),
              max_limit(max_limit)
        {
        }
        const std::string name;
        const int field;
        const int64_t min_limit, max_limit;
    };
    bool
    check_stat(scoped_cursor &cursor, const postrun_statistic &stat) const
    {
        int64_t stat_value;

        testutil_assert(cursor.get() != nullptr);
        get_stat(cursor, stat.field, &stat_value);
        if (stat_value < stat.min_limit || stat_value > stat.max_limit) {
            const std::string error_string = "runtime_monitor: Postrun stat \"" + stat.name +
              "\" was outside of the specified limits. Min=" + std::to_string(stat.min_limit) +
              " Max=" + std::to_string(stat.max_limit) + " Actual=" + std::to_string(stat_value);
            log_msg(LOG_ERROR, error_string);
            return (false);
        }
        return (true);
    }

    std::vector<postrun_statistic> _stats;
};

/*
 * The runtime monitor class is designed to track various statistics or other runtime signals
 * relevant to the given workload.
 */
class runtime_monitor : public component {
    public:
    runtime_monitor(configuration *config, database &database)
        : component("runtime_monitor", config), _postrun_stats(config), _database(database)
    {
    }

    ~runtime_monitor()
    {
        for (auto &it : _stats)
            delete it;
        _stats.clear();
    }

    /* Delete the copy constructor and the assignment operator. */
    runtime_monitor(const runtime_monitor &) = delete;
    runtime_monitor &operator=(const runtime_monitor &) = delete;

    void
    load() override final
    {
        configuration *sub_config;
        std::string statistic_list;

        /* Load the general component things. */
        component::load();

        if (_enabled) {
            _session = connection_manager::instance().create_session();

            /* Open our statistic cursor. */
            _cursor = _session.open_scoped_cursor(STATISTICS_URI);

            /* Load known runtime statistics. */
            sub_config = _config->get_subconfig(STAT_CACHE_SIZE);
            _stats.push_back(new cache_limit_statistic(sub_config));
            delete sub_config;

            sub_config = _config->get_subconfig(STAT_DB_SIZE);
            _stats.push_back(new db_size_statistic(sub_config, _database));
            delete sub_config;
        }
    }

    void
    do_work() override final
    {
        for (const auto &it : _stats) {
            if (it->enabled())
                it->check(_cursor);
        }
    }

    void
    finish() override final
    {
        _postrun_stats.check(_cursor);
        component::finish();
    }

    private:
    scoped_session _session;
    scoped_cursor _cursor;
    std::vector<runtime_statistic *> _stats;
    postrun_statistic_check _postrun_stats;
    database &_database;
};
} // namespace test_harness

#endif
