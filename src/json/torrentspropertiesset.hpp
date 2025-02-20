#pragma once

#include <nlohmann/json.hpp>

#include "ltinfohash.hpp"
#include "lttorrentflags.hpp"
#include "utils.hpp"
#include "../methods/torrentspropertiesset_reqres.hpp"

using json = nlohmann::json;

namespace porla::Methods
{
    NLOHMANN_JSONIFY_ALL_THINGS(
        TorrentsPropertiesSetReq,
        info_hash,
        download_limit,
        max_connections,
        max_uploads,
        set_flags,
        unset_flags,
        upload_limit);
}
