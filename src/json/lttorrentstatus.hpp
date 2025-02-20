#pragma once

#include <libtorrent/torrent_status.hpp>
#include <nlohmann/json.hpp>

#include "ltinfohash.hpp"

namespace libtorrent
{
    static void to_json(nlohmann::json& j, const libtorrent::torrent_status& ts)
    {
        j = {
            {"info_hash", ts.info_hashes},
            {"name", ts.name},
            {"progress", ts.progress},
            {"save_path", ts.save_path},
            {"total", ts.total},
            {"total_done", ts.total_done},
            {"total_wanted", ts.total_wanted},
        };
    }
}
