#include "log.hpp"

#include <boost/log/trivial.hpp>

#include "actioncallback.hpp"

using porla::Actions::Log;

Log::Log(ISession &session)
    : m_session(session)
{
}

void Log::Invoke(const libtorrent::info_hash_t& hash, const toml::array& args, const std::shared_ptr<ActionCallback>& callback)
{
    if (args.empty()) return;
    BOOST_LOG_TRIVIAL(info) << *args[0].as_string();
    callback->Invoke(true);
}
