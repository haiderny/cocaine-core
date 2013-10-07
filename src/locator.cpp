/*
    Copyright (c) 2011-2013 Andrey Sibiryov <me@kobology.ru>
    Copyright (c) 2013 Andrey Goryachev <andrey.goryachev@gmail.com>
    Copyright (c) 2011-2013 Other contributors as noted in the AUTHORS file.

    This file is part of Cocaine.

    Cocaine is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    Cocaine is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "cocaine/detail/locator.hpp"

#include "cocaine/api/gateway.hpp"

#include "cocaine/asio/reactor.hpp"
#include "cocaine/asio/resolver.hpp"
#include "cocaine/asio/socket.hpp"
#include "cocaine/asio/tcp.hpp"
#include "cocaine/asio/timeout.hpp"
#include "cocaine/asio/udp.hpp"

#include "cocaine/context.hpp"

#include "cocaine/detail/actor.hpp"
#include "cocaine/detail/group.hpp"

#include "cocaine/logging.hpp"

#include "cocaine/rpc/channel.hpp"

#include "cocaine/traits/tuple.hpp"

#include <random>

using namespace cocaine;

using namespace std::placeholders;

struct locator_t::synchronize_slot_t:
    public slot_concept_t
{
    synchronize_slot_t(locator_t& self);

    virtual
    void
    operator()(const msgpack::object& unpacked, const api::stream_ptr_t& upstream);

    void
    update();

    void
    shutdown();

private:
    bool
    dump(const api::stream_ptr_t& upstream);

    static
    void
    close(const api::stream_ptr_t& upstream);

private:
    msgpack::sbuffer m_buffer;
    msgpack::packer<msgpack::sbuffer> m_packer;

    locator_t& m_self;

    std::vector<api::stream_ptr_t> m_upstreams;
};

namespace {

#if defined(__clang__) || defined(HAVE_GCC46)
typedef std::default_random_engine random_generator_t;
typedef std::uniform_int_distribution<unsigned int> uniform_uint;
#else
typedef std::minstd_rand0 random_generator_t;
typedef std::uniform_int<unsigned int> uniform_uint;
#endif

struct group_index_t {
    group_index_t();
    group_index_t(const std::map<std::string, unsigned int>& group);

    void
    add(size_t service_index);

    void
    remove(size_t service_index);

    const std::vector<std::string>&
    services() const {
        return m_services;
    }

    const std::vector<unsigned int>&
    weights() const {
        return m_weights;
    }

    const std::vector<unsigned int>&
    used_weights() const {
        return m_used_weights;
    }

    unsigned int
    sum() const {
        return m_sum;
    }

private:
    std::vector<std::string> m_services;
    std::vector<unsigned int> m_weights;
    std::vector<unsigned int> m_used_weights; // = original weight or 0 if there is no such service in the Locator
    unsigned int m_sum;
};

} // namespace

class locator_t::router_t {
    public:
        typedef std::vector<
            std::pair<std::string, locator_t::resolve_result_type>
        > services_vector_t;

    public:
        router_t(logging::log_t& log);

        void
        add_local(const std::string& name);

        void
        remove_local(const std::string& name);

        std::pair<services_vector_t, services_vector_t> // added, removed
        update_remote(const std::string& uuid, const synchronize_result_type& dump);

        std::map<std::string, resolve_result_type> // services of the removed node
        remove_remote(const std::string& uuid);

        bool
        has(const std::string& name) const;

        void
        add_group(const std::string& name, const std::map<std::string, unsigned int>& group);

        void
        remove_group(const std::string& name);

        std::string
        select_service(const std::string& name) const;

    private:
        void
        add(const std::string& uuid, const std::string& name, const resolve_result_type& info);

        void
        remove(const std::string& uuid, const std::string& name);

    private:
        typedef std::map<
            std::string,
            std::map<std::string, resolve_result_type>
        > inverted_index_t;

        // Service -> UUIDs of the remote nodes that have this service.
        std::map<std::string, std::set<std::string>> m_services;

        // Inverse index: UUID -> {(service, info)}.
        inverted_index_t m_inverted;

        struct groups_t {
            groups_t(logging::log_t& log, const locator_t::router_t& router);

            void
            add_group(const std::string& name, const std::map<std::string, unsigned int>& group);

            void
            remove_group(const std::string& name);

            void
            add_service(const std::string& name);

            void
            remove_service(const std::string& name);

            std::string
            select_service(const std::string& group_name) const;

        private:
            // Maps group name to services.
            std::map<std::string, group_index_t> m_groups;

            typedef std::map<
                std::string,
                std::map<std::string, size_t>
            > inverted_index_t;

            // Inverse index: {service: {group: index in services vector}}.
            inverted_index_t m_inverted;

            logging::log_t& m_log;
            const locator_t::router_t& m_router;

            mutable random_generator_t m_generator;
        };

        groups_t m_groups;

        // Router interlocking.
        mutable std::mutex m_mutex;
};

#include "synchronization.inl"
#include "routing.inl"

locator_t::locator_t(context_t& context, io::reactor_t& reactor):
    dispatch_t(context, "service/locator"),
    m_context(context),
    m_log(new logging::log_t(context, "service/locator")),
    m_reactor(reactor),
    m_router(new router_t(*m_log.get()))
{
    COCAINE_LOG_INFO(m_log, "this node's id is '%s'", m_context.config.network.uuid);

    try {
        auto groups = api::storage(context, "core")->find("groups", std::vector<std::string>({"group"}));

        if(groups.empty()) {
            COCAINE_LOG_INFO(m_log, "there is no routing groups to load");
        } else {
            for(auto it = groups.begin(); it != groups.end(); ++it) {
                m_router->add_group(*it, group_t(context, *it).to_map());
            }
        }
    } catch(const std::exception& e) {
        // Unable to read groups from storage.
        // Remove already loaded groups to move in a predictable state.
        m_router.reset(new router_t(*m_log.get()));

        COCAINE_LOG_INFO(m_log, "unable to read groups from storage: %s", e.what());
    }

    on<io::locator::resolve>("resolve", std::bind(&locator_t::resolve, this, _1));
    on<io::locator::reports>("reports", std::bind(&locator_t::reports, this));
    on<io::locator::refresh>("refresh", std::bind(&locator_t::refresh, this, _1));

    if(m_context.config.network.ports) {
        uint16_t min, max;

        std::tie(min, max) = m_context.config.network.ports.get();

        COCAINE_LOG_INFO(m_log, "%u locator ports available, %u through %u", max - min, min, max);

        while(min != max) {
            m_ports.push(--max);
        }
    }
}

locator_t::~locator_t() {
    if(m_services.empty()) {
        return;
    }

    COCAINE_LOG_WARNING(
        m_log,
        "disposing of %llu orphan %s",
        m_services.size(),
        m_services.size() == 1 ? "service" : "services"
    );

    while(!m_services.empty()) {
        m_services.back().second->terminate();
        m_services.pop_back();
    }
}

void
locator_t::connect() {
    using namespace boost::asio::ip;

    io::udp::endpoint endpoint = {
        address::from_string(m_context.config.network.group.get()),
        0
    };

    if(m_context.config.network.gateway) {
        const io::udp::endpoint bindpoint = { address::from_string("0.0.0.0"), 10054 };

        m_sink.reset(new io::socket<io::udp>());

        if(::bind(m_sink->fd(), bindpoint.data(), bindpoint.size()) != 0) {
            throw std::system_error(errno, std::system_category(), "unable to bind an announce socket");
        }

        COCAINE_LOG_INFO(m_log, "joining multicast group '%s' on '%s'", endpoint.address(), bindpoint);

        group_req request;

        std::memset(&request, 0, sizeof(request));

        request.gr_interface = 0;

        std::memcpy(&request.gr_group, endpoint.data(), endpoint.size());

        if(::setsockopt(m_sink->fd(), IPPROTO_IP, MCAST_JOIN_GROUP, &request, sizeof(request)) != 0) {
            throw std::system_error(errno, std::system_category(), "unable to join a multicast group");
        }

        m_sink_watcher.reset(new ev::io(m_reactor.native()));
        m_sink_watcher->set<locator_t, &locator_t::on_announce_event>(this);
        m_sink_watcher->start(m_sink->fd(), ev::READ);

        m_gateway = m_context.get<api::gateway_t>(
            m_context.config.network.gateway.get().type,
            m_context,
            "service/locator",
            m_context.config.network.gateway.get().args
        );
    }

    endpoint.port(10054);

    COCAINE_LOG_INFO(m_log, "announcing the node on '%s'", endpoint);

    // NOTE: Connect this UDP socket so that we could send announces via write() instead of sendto().
    m_announce.reset(new io::socket<io::udp>(endpoint));

    const int loop = 0;
    const int life = IP_DEFAULT_MULTICAST_TTL;

    // NOTE: I don't think these calls might fail at all.
    ::setsockopt(m_announce->fd(), IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    ::setsockopt(m_announce->fd(), IPPROTO_IP, IP_MULTICAST_TTL,  &life, sizeof(life));

    m_announce_timer.reset(new ev::timer(m_reactor.native()));
    m_announce_timer->set<locator_t, &locator_t::on_announce_timer>(this);
    m_announce_timer->start(0.0f, 5.0f);

    m_synchronizer = std::make_shared<synchronize_slot_t>(*this);

    on<io::locator::synchronize>(m_synchronizer);
}

void
locator_t::disconnect() {
    // Disable the synchronize method.
    forget<io::locator::synchronize>();

    // Disconnect all the clients.
    m_synchronizer->shutdown();
    m_synchronizer.reset();

    m_announce_timer.reset();
    m_announce.reset();

    if(m_context.config.network.gateway) {
        // Purge the routing tables.
        m_gateway.reset();

        // Disconnect all the routed peers.
        m_remotes.clear();

        m_sink_watcher.reset();
        m_sink.reset();
    }
}

namespace {

struct match {
    template<class T>
    bool
    operator()(const T& service) const {
        return name == service.first;
    }

    const std::string name;
};

}

void
locator_t::attach(const std::string& name, std::unique_ptr<actor_t>&& service) {
    uint16_t port = 0;

    {
        std::lock_guard<std::mutex> guard(m_services_mutex);

        auto existing = std::find_if(m_services.cbegin(), m_services.cend(), match {
            name
        });

        BOOST_VERIFY(existing == m_services.end());

        if(m_context.config.network.ports) {
            if(m_ports.empty()) {
                throw cocaine::error_t("no ports left for allocation");
            }

            port = m_ports.top();

            // NOTE: Remove the taken port from the free pool. If, for any reason, this port is
            // unavailable for binding, it's okay to keep it removed forever.
            m_ports.pop();
        }

        const std::vector<io::tcp::endpoint> endpoints = {
            { boost::asio::ip::address::from_string(m_context.config.network.endpoint), port }
        };

        service->run(endpoints);

        COCAINE_LOG_INFO(m_log, "service '%s' published on port %d", name, service->location().front().port());

        m_services.emplace_back(name, std::move(service));
    }

    m_router->add_local(name);

    if(m_synchronizer) {
        m_synchronizer->update();
    }
}

auto
locator_t::detach(const std::string& name) -> std::unique_ptr<actor_t> {
    std::unique_ptr<actor_t> service;
    
    {
        std::lock_guard<std::mutex> guard(m_services_mutex);

        auto it = std::find_if(m_services.begin(), m_services.end(), match {
            name
        });

        BOOST_VERIFY(it != m_services.end());

        const std::vector<io::tcp::endpoint> endpoints = it->second->location();

        it->second->terminate();

        if(m_context.config.network.ports) {
            m_ports.push(endpoints.front().port());
        }

        COCAINE_LOG_INFO(m_log, "service '%s' withdrawn from port %d", name, endpoints.front().port());

        // Release the service's actor ownership.
        service = std::move(it->second);

        m_services.erase(it);
    }

    m_router->remove_local(name);

    if(m_synchronizer) {
        m_synchronizer->update();
    }

    return service;
}

auto
locator_t::resolve(const std::string& name) const -> resolve_result_type {
    std::string target = m_router->select_service(name);

    {
        std::lock_guard<std::mutex> guard(m_services_mutex);

        auto local = std::find_if(m_services.begin(), m_services.end(), match {
            target
        });

        if(local != m_services.end()) {
            COCAINE_LOG_DEBUG(m_log, "providing '%s' using local node", name);

            // TODO: Might be a good idea to return an endpoint suitable for the interface
            // which the client used to connect to the Locator.
            return local->second->metadata();
        }
    }

    if(m_gateway) {
        return m_gateway->resolve(target);
    } else {
        throw cocaine::error_t("the specified service is not available");
    }
}

auto
locator_t::dump() const -> synchronize_result_type {
    std::lock_guard<std::mutex> guard(m_services_mutex);

    synchronize_result_type result;

    for(auto it = m_services.begin(); it != m_services.end(); ++it) {
        result[it->first] = it->second->metadata();
    }

    return result;
}

auto
locator_t::reports() const -> reports_result_type {
    std::lock_guard<std::mutex> guard(m_services_mutex);

    reports_result_type result;

    for(auto it = m_services.begin(); it != m_services.end(); ++it) {
        io::locator::reports::usage_report_type report;

        // Get the usage counters from the service's actor.
        const auto source = it->second->counters();

        for(auto channel = source.footprints.begin(); channel != source.footprints.end(); ++channel) {
            auto& endpoint = channel->first;
            auto  consumed = channel->second;

            report.insert({
                io::locator::endpoint_tuple_type(endpoint.address().to_string(), endpoint.port()),
                consumed
            });
        }

        result[it->first] = make_tuple(source.channels, report);
    }

    return result;
}

void
locator_t::refresh(const std::string& name) {
    try {
        m_router->add_group(name, group_t(m_context, name).to_map());
    } catch(const storage_error_t& e) {
        COCAINE_LOG_INFO(m_log, "unable to read group '%s' from storage - %s", name, e.what());

        // Unable to read the group from storage. Assume that it was deleted.
        m_router->remove_group(name);
    }
}

void
locator_t::on_announce_event(ev::io&, int) {
    char buffers[1024];
    std::error_code ec;

    const ssize_t size = m_sink->read(buffers, sizeof(buffers), ec);

    if(size <= 0) {
        if(ec) {
            COCAINE_LOG_ERROR(m_log, "unable to receive an announce - [%d] %s", ec.value(), ec.message());
        }

        return;
    }

    msgpack::unpacked unpacked;
    key_type key;

    try {
        msgpack::unpack(&unpacked, buffers, size);
        unpacked.get() >> key;
    } catch(const msgpack::unpack_error& e) {
        COCAINE_LOG_ERROR(m_log, "unable to decode an announce");
        return;
    } catch(const msgpack::type_error& e) {
        COCAINE_LOG_ERROR(m_log, "unable to decode an announce");
        return;
    }

    if(m_remotes.find(key) == m_remotes.end()) {
        std::string uuid;
        std::string hostname;
        uint16_t    port;

        std::tie(uuid, hostname, port) = key;

        COCAINE_LOG_INFO(m_log, "discovered node '%s' on '%s:%d'", uuid, hostname, port);

        std::vector<io::tcp::endpoint> endpoints;

        try {
            endpoints = io::resolver<io::tcp>::query(hostname, port);
        } catch(const std::system_error& e) {
            COCAINE_LOG_ERROR(m_log, "unable to resolve node '%s' endpoints - [%d] %s", uuid, e.code().value(),
                e.code().message());

            return;
        }

        std::shared_ptr<io::channel<io::socket<io::tcp>>> channel;

        for(auto it = endpoints.begin(); it != endpoints.end(); ++it) {
            try {
                channel = std::make_shared<io::channel<io::socket<io::tcp>>>(
                    m_reactor,
                    std::make_shared<io::socket<io::tcp>>(*it)
                );
            } catch(const std::system_error& e) {
                COCAINE_LOG_WARNING(m_log, "unable to connect to node '%s' via endpoint '%s' - [%d] %s", uuid, *it,
                    e.code().value(), e.code().message());

                continue;
            }

            break;
        }

        if(!channel) {
            COCAINE_LOG_ERROR(m_log, "unable to connect to node '%s'", hostname);
            return;
        }

        channel->rd->bind(
            std::bind(&locator_t::on_message, this, key, _1),
            std::bind(&locator_t::on_failure, this, key, _1)
        );

        channel->wr->bind(
            std::bind(&locator_t::on_failure, this, key, _1)
        );

        auto timeout = std::make_shared<io::timeout_t>(m_reactor);

        timeout->bind(
            std::bind(&locator_t::on_timeout, this, key)
        );

        m_remotes[key] = remote_t {
            channel,
            timeout
        };

        channel->wr->write<io::locator::synchronize>(0UL);
    }

    COCAINE_LOG_DEBUG(m_log, "resetting the heartbeat timeout for node '%s'", std::get<0>(key));

    m_remotes[key].timeout->stop();
    m_remotes[key].timeout->start(60.0f);
}

void
locator_t::on_announce_timer(ev::timer&, int) {
    msgpack::sbuffer buffer;
    msgpack::packer<msgpack::sbuffer> packer(buffer);

    packer << key_type(
        m_context.config.network.uuid,
        m_context.config.network.hostname,
        m_context.config.network.locator
    );

    std::error_code ec;

    if(m_announce->write(buffer.data(), buffer.size(), ec) != static_cast<ssize_t>(buffer.size())) {
        if(ec) {
            COCAINE_LOG_ERROR(m_log, "unable to announce the node - [%d] %s", ec.value(), ec.message());
        } else {
            COCAINE_LOG_ERROR(m_log, "unable to announce the node");
        }
    }
}

namespace {

template<class Container>
struct deferred_erase_action {
    typedef Container container_type;
    typedef typename container_type::key_type key_type;

    void
    operator()() {
        target.erase(key);
    }

    container_type& target;
    const key_type  key;
};

}

void
locator_t::on_message(const key_type& key, const io::message_t& message) {
    std::string uuid;

    std::tie(uuid, std::ignore, std::ignore) = key;

    switch(message.id()) {
    case io::event_traits<io::rpc::chunk>::id: {
        std::string chunk;

        message.as<io::rpc::chunk>(chunk);

        msgpack::unpacked unpacked;
        msgpack::unpack(&unpacked, chunk.data(), chunk.size());

        auto dump = unpacked.get().as<synchronize_result_type>();
        auto diff = m_router->update_remote(uuid, dump);

        for(auto it = diff.second.begin(); it != diff.second.end(); ++it) {
            m_gateway->cleanup(uuid, it->first);
        }

        for(auto it = diff.first.begin(); it != diff.first.end(); ++it) {
            m_gateway->consume(uuid, it->first, it->second);
        }
    } break;

    case io::event_traits<io::rpc::error>::id:
    case io::event_traits<io::rpc::choke>::id: {
        COCAINE_LOG_INFO(m_log, "node '%s' has been shut down", uuid);

        auto removed = m_router->remove_remote(uuid);

        for(auto it = removed.begin(); it != removed.end(); ++it) {
            m_gateway->cleanup(uuid, it->first);
        }

        // NOTE: It is dangerous to remove the channel while the message is still being
        // processed, so we defer it via reactor_t::post().
        m_reactor.post(deferred_erase_action<decltype(m_remotes)> {
            m_remotes,
            key
        });
    } break;

    default:
        COCAINE_LOG_ERROR(m_log, "dropped unknown type %d synchronization message", message.id());
    }
}

void
locator_t::on_failure(const key_type& key, const std::error_code& ec) {
    std::string uuid;

    std::tie(uuid, std::ignore, std::ignore) = key;

    if(ec) {
        COCAINE_LOG_WARNING(m_log, "node '%s' has unexpectedly disconnected - [%d] %s", uuid, ec.value(), ec.message());
    } else {
        COCAINE_LOG_WARNING(m_log, "node '%s' has unexpectedly disconnected", uuid);
    }

    auto removed = m_router->remove_remote(uuid);

    for(auto it = removed.begin(); it != removed.end(); ++it) {
        m_gateway->cleanup(uuid, it->first);
    }

    // NOTE: Safe to do since errors are queued up.
    m_remotes.erase(key);
}

void
locator_t::on_timeout(const key_type& key) {
    std::string uuid;

    std::tie(uuid, std::ignore, std::ignore) = key;

    COCAINE_LOG_WARNING(m_log, "node '%s' has timed out", uuid);

    auto removed = m_router->remove_remote(uuid);

    for(auto it = removed.begin(); it != removed.end(); ++it) {
        m_gateway->cleanup(uuid, it->first);
    }

    // NOTE: Safe to do since timeouts are not related to I/O.
    m_remotes.erase(key);
}
