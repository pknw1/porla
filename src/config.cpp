#include "config.hpp"

#include <filesystem>
#include <iostream>

#include <boost/log/trivial.hpp>
#include <boost/program_options.hpp>
#include <libtorrent/extensions/ut_metadata.hpp>
#include <libtorrent/extensions/ut_pex.hpp>
#include <libtorrent/extensions/smart_ban.hpp>
#include <libtorrent/fingerprint.hpp>
#include <libtorrent/session.hpp>
#include <toml++/toml.h>

#include "data/migrate.hpp"
#include "data/models/sessionsettings.hpp"
#include "utils/secretkey.hpp"

namespace fs = std::filesystem;
namespace lt = libtorrent;
namespace po = boost::program_options;

using porla::Config;

static void ApplySettings(const toml::table& tbl, lt::settings_pack& settings);
static void ApplyPresetActions(std::vector<Config::PresetAction>& config_actions, const toml::array& actions_array);

std::unique_ptr<Config> Config::Load(const boost::program_options::variables_map& cmd)
{
    const static std::vector<fs::path> config_file_search_paths =
    {
        fs::current_path() / "porla.toml",

        // Check $XDG_CONFIG_HOME/porla/porla.toml
        std::getenv("XDG_CONFIG_HOME")
            ? fs::path(std::getenv("XDG_CONFIG_HOME")) / "porla" / "porla.toml"
            : fs::path(),

        // Check $HOME/.config/porla/porla.toml
        std::getenv("HOME")
            ? fs::path(std::getenv("HOME")) / ".config" / "porla" / "porla.toml"
            : fs::path(),

        // Check $HOME/.config/porla.toml
        std::getenv("HOME")
            ? fs::path(std::getenv("HOME")) / ".config" / "porla.toml"
            : fs::path(),

        "/etc/porla/porla.toml",
        "/etc/porla.toml"
    };

    auto cfg = std::unique_ptr<Config>(new Config());
    cfg->session_settings = lt::default_settings();

    // Check default locations for a config file.
    for (auto const& path : config_file_search_paths)
    {
        // Skip empty paths.
        if (path == fs::path()) continue;

        std::error_code ec;
        bool regular_file = fs::is_regular_file(path, ec);

        if (ec || !regular_file)
        {
            continue;
        }

        cfg->config_file = path;

        break;
    }

    if (auto val = std::getenv("PORLA_CONFIG_FILE"))           cfg->config_file     = val;
    if (auto val = std::getenv("PORLA_DB"))                    cfg->db_file         = val;
    if (auto val = std::getenv("PORLA_HTTP_BASE_PATH"))        cfg->http_base_path  = val;
    if (auto val = std::getenv("PORLA_HTTP_HOST"))             cfg->http_host       = val;
    if (auto val = std::getenv("PORLA_HTTP_METRICS_ENABLED"))
    {
        if (strcmp("true", val) == 0)  cfg->http_metrics_enabled = true;
        if (strcmp("false", val) == 0) cfg->http_metrics_enabled = false;
    }
    if (auto val = std::getenv("PORLA_HTTP_PORT"))             cfg->http_port       = std::stoi(val);
    if (auto val = std::getenv("PORLA_HTTP_WEBUI_ENABLED"))
    {
        if (strcmp("true", val) == 0)  cfg->http_webui_enabled = true;
        if (strcmp("false", val) == 0) cfg->http_webui_enabled = false;
    }
    if (auto val = std::getenv("PORLA_SECRET_KEY"))            cfg->secret_key      = val;
    if (auto val = std::getenv("PORLA_SESSION_SETTINGS_BASE"))
    {
        if (strcmp("default", val) == 0)               cfg->session_settings = lt::default_settings();
        if (strcmp("high_performance_seed", val) == 0) cfg->session_settings = lt::high_performance_seed();
        if (strcmp("min_memory_usage", val) == 0)      cfg->session_settings = lt::min_memory_usage();
    }
    if (auto val = std::getenv("PORLA_STATE_DIR"))             cfg->state_dir             = val;
    if (auto val = std::getenv("PORLA_TIMER_DHT_STATS"))       cfg->timer_dht_stats       = std::stoi(val);
    if (auto val = std::getenv("PORLA_TIMER_SESSION_STATS"))   cfg->timer_session_stats   = std::stoi(val);
    if (auto val = std::getenv("PORLA_TIMER_TORRENT_UPDATES")) cfg->timer_torrent_updates = std::stoi(val);

    if (cmd.count("config-file"))
    {
        cfg->config_file = cmd["config-file"].as<std::string>();

        if (!fs::is_regular_file(cfg->config_file.value()))
        {
            BOOST_LOG_TRIVIAL(warning)
                << "User-specified config file does not exist: "
                << fs::absolute(cfg->config_file.value());
        }
    }

    // Apply configuration from the config file before we apply the command line args.
    if (cfg->config_file && fs::is_regular_file(cfg->config_file.value()))
    {
        BOOST_LOG_TRIVIAL(debug) << "Reading config file at " << cfg->config_file.value();

        std::ifstream config_file_data(cfg->config_file.value(), std::ios::binary);

        try
        {
            const toml::table config_file_tbl = toml::parse(config_file_data);

            if (auto val = config_file_tbl["db"].value<std::string>())
                cfg->db_file = *val;

            if (auto val = config_file_tbl["http"]["base_path"].value<std::string>())
                cfg->http_base_path = *val;

            if (auto val = config_file_tbl["http"]["host"].value<std::string>())
                cfg->http_host = *val;

            if (auto val = config_file_tbl["http"]["metrics_enabled"].value<bool>())
                cfg->http_metrics_enabled = *val;

            if (auto val = config_file_tbl["http"]["port"].value<uint16_t>())
                cfg->http_port = *val;

            if (auto val = config_file_tbl["http"]["webui_enabled"].value<bool>())
                cfg->http_webui_enabled = *val;

            // Load presets
            if (auto const* presets_tbl = config_file_tbl["presets"].as_table())
            {
                for (auto const [key,value] : *presets_tbl)
                {
                    if (!value.is_table())
                    {
                        BOOST_LOG_TRIVIAL(warning) << "Preset '" << key << "' is not a TOML table";
                        continue;
                    }

                    const toml::table value_tbl = *value.as_table();

                    Preset p = {};

                    if (auto val = value_tbl["download_limit"].value<int>())
                        p.download_limit = *val;

                    if (auto val = value_tbl["max_connections"].value<int>())
                        p.max_connections = *val;

                    if (auto val = value_tbl["max_uploads"].value<int>())
                        p.max_uploads = *val;

                    if (auto val = value_tbl["save_path"].value<std::string>())
                        p.save_path = *val;

                    if (auto val = value_tbl["storage_mode"].value<std::string>())
                    {
                        if (strcmp(val->c_str(), "allocate") == 0) p.storage_mode = lt::storage_mode_allocate;
                        if (strcmp(val->c_str(), "sparse") == 0)   p.storage_mode = lt::storage_mode_sparse;
                    }

                    if (auto val = value_tbl["upload_limit"].value<int>())
                        p.upload_limit = *val;

                    // Set up actions
                    if (auto val = value_tbl["on_torrent_added"].as_array())
                        ApplyPresetActions(p.on_torrent_added, *val);

                    if (auto val = value_tbl["on_torrent_finished"].as_array())
                        ApplyPresetActions(p.on_torrent_finished, *val);

                    cfg->presets.insert({ key.data(), std::move(p) });
                }
            }

            if (auto val = config_file_tbl["secret_key"].value<std::string>())
                cfg->secret_key = *val;

            if (auto val = config_file_tbl["session_settings"]["extensions"].as_array())
            {
                std::vector<lt_plugin> extensions;

                for (auto const& item : *val)
                {
                    if (auto const item_value = item.value<std::string>())
                    {
                        if (*item_value == "smart_ban")
                            extensions.emplace_back(&lt::create_smart_ban_plugin);

                        if (*item_value == "ut_metadata")
                            extensions.emplace_back(&lt::create_ut_metadata_plugin);

                        if (*item_value == "ut_pex")
                            extensions.emplace_back(&lt::create_ut_pex_plugin);
                    }
                    else
                    {
                        BOOST_LOG_TRIVIAL(warning)
                            << "Item in session_extension array is not a string (" << item.type() << ")";
                    }
                }

                cfg->session_extensions = extensions;
            }

            if (auto val = config_file_tbl["session_settings"]["base"].value<std::string>())
            {
                if (*val == "default")               cfg->session_settings = lt::default_settings();
                if (*val == "high_performance_seed") cfg->session_settings = lt::high_performance_seed();
                if (*val == "min_memory_usage")      cfg->session_settings = lt::min_memory_usage();
            }

            if (auto session_settings_tbl = config_file_tbl["session_settings"].as_table())
                ApplySettings(*session_settings_tbl, cfg->session_settings);

            if (auto val = config_file_tbl["state_dir"].value<std::string>())
                cfg->state_dir = *val;

            if (auto val = config_file_tbl["timer"]["dht_stats"].value<int>())
                cfg->timer_dht_stats = *val;

            if (auto val = config_file_tbl["timer"]["session_stats"].value<int>())
                cfg->timer_session_stats = *val;

            if (auto val = config_file_tbl["timer"]["torrent_updates"].value<int>())
                cfg->timer_torrent_updates = *val;

            if (auto const* webhooks_array = config_file_tbl["webhooks"].as_array())
            {
                for (auto const& wh : *webhooks_array)
                {
                    auto const wh_tbl = *wh.as_table();

                    Webhook hook = {};

                    if (auto val = wh_tbl["on"].value<std::string>())
                        hook.on.insert(*val);

                    if (auto val = wh_tbl["on"].as_array())
                    {
                        for (auto const& event_name : *val)
                        {
                            if (auto event_name_str = event_name.value<std::string>())
                                hook.on.insert(*event_name_str);
                        }
                    }

                    hook.url = *wh_tbl["url"].value<std::string>();

                    if (auto headers_array = wh_tbl["headers"].as_array())
                    {
                        for (auto const& header_item : *headers_array)
                        {
                            auto const* header_tbl = header_item.as_table();

                            if (!header_tbl)
                            {
                                BOOST_LOG_TRIVIAL(warning) << "Webhook header item is not a TOML table";
                                continue;
                            }

                            if (header_tbl->size() != 1)
                            {
                                BOOST_LOG_TRIVIAL(warning) << "Webhook header item should only have a single value";
                                continue;
                            }

                            hook.headers.insert({
                                header_tbl->begin()->first.data(),
                                *header_tbl->begin()->second.value<std::string>()
                            });
                        }
                    }

                    if (auto val = wh_tbl["payload"].value<std::string>())
                        hook.payload = *val;

                    cfg->webhooks.push_back(std::move(hook));
                }
            }
        }
        catch (const toml::parse_error& err)
        {
            BOOST_LOG_TRIVIAL(error) << "Failed to parse config file '" << cfg->config_file.value() << "': " << err;
        }
    }

    if (cmd.count("db"))                    cfg->db_file               = cmd["db"].as<std::string>();
    if (cmd.count("http-base-path"))        cfg->http_base_path        = cmd["http-base-path"].as<std::string>();
    if (cmd.count("http-host"))             cfg->http_host             = cmd["http-host"].as<std::string>();
    if (cmd.count("http-metrics-enabled"))
    {
        cfg->http_metrics_enabled = cmd["http-metrics-enabled"].as<bool>();
    }
    if (cmd.count("http-port"))             cfg->http_port             = cmd["http-port"].as<uint16_t>();
    if (cmd.count("http-webui-enabled"))
    {
        cfg->http_webui_enabled = cmd["http-webui-enabled"].as<bool>();
    }
    if (cmd.count("secret-key"))            cfg->secret_key            = cmd["secret-key"].as<std::string>();
    if (cmd.count("session-settings-base"))
    {
        auto val = cmd["session-settings-base"].as<std::string>();

        if (val == "default")               cfg->session_settings = lt::default_settings();
        if (val == "high_performance_seed") cfg->session_settings = lt::high_performance_seed();
        if (val == "min_memory_usage")      cfg->session_settings = lt::min_memory_usage();
    }
    if (cmd.count("state-dir"))             cfg->state_dir             = cmd["state-dir"].as<std::string>();
    if (cmd.count("timer-dht-stats"))       cfg->timer_dht_stats       = cmd["timer-dht-stats"].as<int>();
    if (cmd.count("timer-session-stats"))   cfg->timer_session_stats   = cmd["timer-session-stats"].as<pid_t>();
    if (cmd.count("timer-torrent-updates")) cfg->timer_torrent_updates = cmd["timer-torrent-updates"].as<pid_t>();

    // If no db_file is set, default to a file in state_dir.
    if (!cfg->db_file.has_value())
    {
        cfg->db_file = cfg->state_dir.value_or(fs::current_path()) / "porla.sqlite";
    }

    if (sqlite3_open(cfg->db_file.value_or("porla.sqlite").c_str(), &cfg->db) != SQLITE_OK)
    {
        BOOST_LOG_TRIVIAL(fatal) << "Failed to open SQLite connection: " << sqlite3_errmsg(cfg->db);
        throw std::runtime_error("Failed to open SQLite connection");
    }

    if (sqlite3_exec(cfg->db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        BOOST_LOG_TRIVIAL(fatal) << "Failed to enable WAL journal mode: " << sqlite3_errmsg(cfg->db);
        throw std::runtime_error("Failed to enable WAL journal mode");
    }

    if (!porla::Data::Migrate(cfg->db))
    {
        BOOST_LOG_TRIVIAL(error) << "Failed to run migrations";
        throw std::runtime_error("Failed to apply migrations");
    }

    porla::Data::Models::SessionSettings::Apply(cfg->db, cfg->session_settings);

    // Apply static libtorrent settings here. These are always set after all other settings from
    // the config are applied, and cannot be overwritten by it.
    cfg->session_settings.set_int(
        lt::settings_pack::alert_mask,
        lt::alert::status_notification
        | lt::alert::storage_notification);

    cfg->session_settings.set_str(lt::settings_pack::peer_fingerprint, lt::generate_fingerprint("PO", 0, 1));
    cfg->session_settings.set_str(lt::settings_pack::user_agent, "porla/1.0");

    // If we get here without having a secret key, we must generate one. Also log a warning because
    // if the secret key changes, JWT's will not work if restarting.

    if (cfg->secret_key.empty())
    {
        BOOST_LOG_TRIVIAL(warning) << "No secret key set. Porla will generate one";
        BOOST_LOG_TRIVIAL(warning) << "Use './porla key:generate' to generate a secret key";

        cfg->secret_key = porla::Utils::SecretKey::New();
    }

    return std::move(cfg);
}

Config::~Config()
{
    BOOST_LOG_TRIVIAL(debug) << "Vacuuming database";

    if (sqlite3_exec(db, "VACUUM;", nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        BOOST_LOG_TRIVIAL(error) << "Failed to vacuum database: " << sqlite3_errmsg(db);
    }

    if (sqlite3_close(db) != SQLITE_OK)
    {
        BOOST_LOG_TRIVIAL(error) << "Failed to close SQLite connection: " << sqlite3_errmsg(db);
    }
}

static void ApplyPresetActions(std::vector<Config::PresetAction>& config_actions, const toml::array& actions_array)
{
    for (const auto& actions_item : actions_array)
    {
        if (!actions_item.is_array()) continue;

        const auto action_parameters = actions_item.as_array();

        // require at least one item in the array (the name of the action)
        if (action_parameters->empty()) continue;
        if (!action_parameters->at(0).is_string()) continue;

        std::string action_name = *action_parameters->at(0).value<std::string>();
        toml::array action_args;

        for (int i = 1; i < action_parameters->size(); i++)
        {
            action_args.push_back(action_parameters->at(i));
        }

        config_actions.emplace_back(Config::PresetAction{
            .action_name = action_name,
            .arguments   = action_args
        });
    }
}

static void ApplySettings(const toml::table& tbl, lt::settings_pack& settings)
{
    for (auto const& [key,value] : tbl)
    {
        const int type = lt::setting_by_name(key.data());

        if (type == -1)
        {
            continue;
        }

        if (porla::Data::Models::SessionSettings::BlockedKeys.contains(key.data()))
        {
            continue;
        }

        if ((type & lt::settings_pack::type_mask) == lt::settings_pack::bool_type_base)
        {
            if (!value.is_boolean())
            {
                BOOST_LOG_TRIVIAL(warning) << "Value for setting " << key << " is not a boolean";
                continue;
            }

            settings.set_bool(type, *value.value<bool>());
        }
        else if((type & lt::settings_pack::type_mask) == lt::settings_pack::int_type_base)
        {
            if (!value.is_integer())
            {
                BOOST_LOG_TRIVIAL(warning) << "Value for setting " << key << " is not an integer";
                continue;
            }

            settings.set_int(type, *value.value<int>());
        }
        else if((type & lt::settings_pack::type_mask) == lt::settings_pack::string_type_base)
        {
            if (!value.is_string())
            {
                BOOST_LOG_TRIVIAL(warning) << "Value for setting " << key << " is not a string";
                continue;
            }

            settings.set_str(type, *value.value<std::string>());
        }
    }
}
