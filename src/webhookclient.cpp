#include "webhookclient.hpp"

#include <boost/asio/ssl/context.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/log/trivial.hpp>
#include <libjsonnet++.h>

#include "json/ltinfohash.hpp"
#include "json/lttorrentstatus.hpp"
#include "session.hpp"
#include "uri.hpp"

using porla::WebhookClient;
using porla::WebhookClientOptions;

struct WebhookClient::RequestState
{
    explicit RequestState(boost::asio::io_context& io)
        : m_stream(io)
        , m_ssl_ctx(boost::asio::ssl::context::tls_client)
        , m_ssl_stream(io, m_ssl_ctx)
    {
        m_ssl_ctx.set_verify_mode(
            boost::asio::ssl::verify_peer
            | boost::asio::ssl::context::verify_fail_if_no_peer_cert);
        m_ssl_ctx.set_default_verify_paths();
    }

    boost::beast::flat_buffer m_buffer;
    boost::beast::http::response<boost::beast::http::string_body> m_res;
    boost::beast::tcp_stream m_stream;
    boost::asio::ssl::context m_ssl_ctx;
    boost::beast::ssl_stream<boost::beast::tcp_stream> m_ssl_stream;
    porla::Uri m_uri;
    porla::Config::Webhook m_webhook;
    std::string m_payload;
};

WebhookClient::WebhookClient(boost::asio::io_context& io, const WebhookClientOptions& opts)
    : m_io(io)
    , m_resolver(io)
    , m_session(opts.session)
    , m_webhooks(opts.webhooks)
{
    m_torrentAddedConnection = m_session.OnTorrentAdded([this](auto && s) { OnTorrentAdded(s); });
    m_torrentFinishedConnection = m_session.OnTorrentFinished([this](auto && s) { OnTorrentFinished(s); });
    m_torrentPausedConnection = m_session.OnTorrentPaused([this](auto && s) { OnTorrentPaused(s); });
    m_torrentRemovedConnection = m_session.OnTorrentRemoved([this](auto && s) { OnTorrentRemoved(s); });
    m_torrentResumedConnection = m_session.OnTorrentResumed([this](auto && s) { OnTorrentResumed(s); });
}

WebhookClient::~WebhookClient()
{
    m_torrentAddedConnection.disconnect();
    m_torrentFinishedConnection.disconnect();
    m_torrentPausedConnection.disconnect();
    m_torrentRemovedConnection.disconnect();
    m_torrentResumedConnection.disconnect();
}

void WebhookClient::OnTorrentAdded(const libtorrent::torrent_status& ts)
{
    SendEvent("torrent_added", {
        {"torrent", ts}
    });
}

void WebhookClient::OnTorrentFinished(const libtorrent::torrent_status &ts)
{
    SendEvent("torrent_finished", {
        {"torrent", ts}
    });
}

void WebhookClient::OnTorrentPaused(const libtorrent::torrent_status &ts)
{
    SendEvent("torrent_paused", {
        {"torrent", ts}
    });
}

void WebhookClient::OnTorrentRemoved(const libtorrent::info_hash_t &ih)
{
    SendEvent("torrent_removed", {
        {"torrent", {
            {"info_hash", ih}
        }}
    });
}

void WebhookClient::OnTorrentResumed(const libtorrent::torrent_status &ts)
{
    SendEvent("torrent_resumed", {
        {"torrent", ts}
    });
}

void WebhookClient::OnAsyncConnect(boost::system::error_code ec,
                                   const boost::asio::ip::tcp::resolver::results_type::endpoint_type& type,
                                   std::shared_ptr<RequestState> state)
{
    if (ec)
    {
        BOOST_LOG_TRIVIAL(error) << "Webhook connect error: " << ec.message();
        return;
    }

    if (state->m_uri.scheme == "https")
    {
        if (!SSL_set_tlsext_host_name(state->m_ssl_stream.native_handle(), state->m_uri.host.c_str()))
        {
            ec = {static_cast<int>(::ERR_get_error()), boost::asio::error::get_ssl_category()};
            BOOST_LOG_TRIVIAL(error) << "Failed to set tlsext hostname: " << ec.message();
            return;
        }

        state->m_ssl_stream.async_handshake(
            boost::asio::ssl::stream_base::client,
            boost::beast::bind_front_handler(
                [this, state](auto && PH1) { OnAsyncHandshake(PH1, state); }));
    }
    else
    {
        SendRequest(state);
    }
}

void WebhookClient::OnAsyncHandshake(boost::system::error_code ec, std::shared_ptr<RequestState> state)
{
    if (ec)
    {
        BOOST_LOG_TRIVIAL(error) << "Webhook SSL handshake error: " << ec.message();
        return;
    }

    SendRequest(std::move(state));
}

void WebhookClient::OnAsyncRead(boost::system::error_code ec, std::size_t size, std::shared_ptr<RequestState> state)
{
    if (ec)
    {
        BOOST_LOG_TRIVIAL(info) << "Webhook read error: " << ec.message();
        return;
    }

    // TODO: Check expected status codes
}

void WebhookClient::OnAsyncResolve(boost::system::error_code ec,
                                   const boost::asio::ip::tcp::resolver::results_type &results,
                                   std::shared_ptr<RequestState> state)
{
    if (ec)
    {
        BOOST_LOG_TRIVIAL(error) << "Webhook resolve error: " << ec.message();
        return;
    }

    auto& stream = state->m_uri.scheme == "https"
        ? boost::beast::get_lowest_layer(state->m_ssl_stream)
        : state->m_stream;

    stream.async_connect(
        results,
        boost::beast::bind_front_handler(
            [this, state](auto && PH1, auto && PH2)
            {
                OnAsyncConnect(PH1, PH2, state);
            }));
}

void WebhookClient::OnAsyncWrite(boost::system::error_code ec, std::size_t size, std::shared_ptr<RequestState> state)
{
    if (ec)
    {
        BOOST_LOG_TRIVIAL(error) << "Webhook write error: " << ec.message();
        return;
    }

    if (state->m_uri.scheme == "https")
    {
        boost::beast::http::async_read(
            state->m_ssl_stream,
            state->m_buffer,
            state->m_res,
            boost::beast::bind_front_handler(
                [this, state](auto && PH1, auto && PH2)
                {
                    OnAsyncRead(
                        std::forward<decltype(PH1)>(PH1),
                        std::forward<decltype(PH2)>(PH2),
                        state);
                }));
    }
    else
    {
        boost::beast::http::async_read(
            state->m_stream,
            state->m_buffer,
            state->m_res,
            boost::beast::bind_front_handler(
                [this, state](auto && PH1, auto && PH2)
                {
                    OnAsyncRead(
                        std::forward<decltype(PH1)>(PH1),
                        std::forward<decltype(PH2)>(PH2),
                        state);
                }));
    }
}

void WebhookClient::SendEvent(const std::string& eventName, const std::map<std::string, nlohmann::json>& ext_vars)
{
    jsonnet::Jsonnet jn;
    if (!jn.init())
    {
        BOOST_LOG_TRIVIAL(error) << "Failed to initialize jsonnet: " << jn.lastError();
    }

    for (auto const& [key,value] : ext_vars)
    {
        jn.bindExtCodeVar(key, value.dump());
    }

    jn.bindExtCodeVar("event_name", nlohmann::json(eventName).dump());

    for (auto const& wh : m_webhooks)
    {
        if (!wh.on.contains(eventName)) continue;

        auto state = std::make_shared<RequestState>(m_io);
        state->m_webhook = wh;

        if (wh.payload.has_value())
        {
            if (!jn.evaluateSnippet("payload", wh.payload.value(), &state->m_payload))
            {
                BOOST_LOG_TRIVIAL(error) << "Failed to evaluate jsonnet snippet: " << jn.lastError();
            }
        }

        if (!porla::Uri::Parse(wh.url, state->m_uri))
        {
            BOOST_LOG_TRIVIAL(error) << "Invalid url: " << wh.url;
            continue;
        }

        BOOST_LOG_TRIVIAL(debug) << "Calling webhook " << wh.url;

        m_resolver.async_resolve(
            state->m_uri.host,
            std::to_string(state->m_uri.port),
            boost::beast::bind_front_handler(
                [this, state](auto && PH1, auto && PH2) { OnAsyncResolve(PH1, PH2, state); }));
    }
}

void WebhookClient::SendRequest(std::shared_ptr<RequestState> state)
{
    auto req = std::make_shared<boost::beast::http::request<boost::beast::http::string_body>>();
    req->method(state->m_payload.empty() ? boost::beast::http::verb::get : boost::beast::http::verb::post);
    req->target(state->m_uri.path);
    req->version(11);

    req->set(boost::beast::http::field::content_type, "application/json");

    for (auto const& [key,value] : state->m_webhook.headers)
    {
        req->set(key, value);
    }

    // Set these headers after user-specified headers. These cannot be overridden.

    req->set(boost::beast::http::field::host, state->m_uri.host + ":" + std::to_string(state->m_uri.port));
    req->set(boost::beast::http::field::user_agent, "porla/1.0");
    req->body() = state->m_payload;
    req->prepare_payload();

    if (state->m_uri.scheme == "https")
    {
        boost::beast::http::async_write(
            state->m_ssl_stream,
            *req,
            boost::beast::bind_front_handler(
                [this, state, req](auto &&PH1, auto &&PH2)
                { OnAsyncWrite(PH1, PH2, state); }));
    }
    else
    {
        boost::beast::http::async_write(
            state->m_stream,
            *req,
            boost::beast::bind_front_handler(
                [this, state, req](auto &&PH1, auto &&PH2)
                { OnAsyncWrite(PH1, PH2, state); }));
    }
}
