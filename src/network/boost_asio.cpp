/*
 * Copyright (C) 2012  Maxim Noah Khailo
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "network/boost_asio.hpp"
#include "util/thread.hpp"
#include "util/mencode.hpp"
#include "util/string.hpp"
#include "util/dbc.hpp"

#include <stdexcept>
#include <sstream>
#include <functional>
#include <boost/lexical_cast.hpp>
#include <boost/bind.hpp>

using boost::lexical_cast;
namespace u = fire::util;
namespace ba = boost::asio;
using namespace boost::asio::ip;

namespace so = boost::asio::detail::socket_option; 
typedef so::integer<IPPROTO_IP, IP_TTL> time_to_live; 
#ifdef __APPLE__
typedef so::boolean<SOL_SOCKET, SO_REUSEPORT> reuse_port;
#endif

namespace fire
{
    namespace network
    {
        namespace
        {
            const size_t BLOCK_SLEEP = 10;
            const int RETRIES = 3;

            asio_params::connect_mode determine_connection_mode(const queue_options& o)
            {
                asio_params::connect_mode m = asio_params::connect;

                if(o.count("bnd")) m = asio_params::bind;
                else if(o.count("con")) m = asio_params::connect;
                else if(o.count("dcon")) m = asio_params::delayed_connect;

                return m;
            }

        }

        asio_params parse_params(const address_components& c)
        {
            const auto& o = c.options;

            asio_params p;
            p.mode = determine_connection_mode(o);
            p.uri = c.address;
            p.host = c.host;
            p.port = c.port;
            p.local_port = get_opt(o, "local_port", std::string(""));
            p.block = get_opt(o, "block", 0);
            p.wait = get_opt(o, "wait", 0);
            p.track_incoming = get_opt(o, "track_incoming", 0);

            return p;
        }

        tcp_connection::tcp_connection(
                ba::io_service& io, 
                byte_queue& in,
                tcp_connection_ptr_queue& last_in,
                std::mutex& in_mutex,
                bool track,
                bool con) :
            _io(io),
            _in_queue(in),
            _in_mutex(in_mutex),
            _last_in_socket(last_in),
            _track{track},
            _socket{new tcp::socket{io}},
            _state{ con ? connected : disconnected},
            _writing{false},
            _retries{RETRIES}
        {
            INVARIANT(_socket);
        }

        tcp_connection::~tcp_connection()
        {
            close();
        }

        void tcp_connection::close()
        {
            _io.post(boost::bind(&tcp_connection::do_close, this));
        }

        void tcp_connection::do_close()
        {
            INVARIANT(_socket);
            u::mutex_scoped_lock l(_mutex);
            std::cerr << "tcp_connection closed " << _socket->local_endpoint() << " + " << _socket->remote_endpoint() << " error: " << _error.message() << std::endl;
            _socket->close();
            _state = disconnected;
            _writing = false;
        }

        bool tcp_connection::is_connected() const
        {
            u::mutex_scoped_lock l(_mutex);
            return _state == connected;
        }

        bool tcp_connection::is_disconnected() const
        {
            u::mutex_scoped_lock l(_mutex);
            INVARIANT(_socket);
            return _state == disconnected || !_socket->is_open();
        }

        bool tcp_connection::is_connecting() const
        {
            u::mutex_scoped_lock l(_mutex);
            return _state == disconnected;
        }

        tcp_connection::con_state tcp_connection::state() const
        {
            return _state;
        }

        void tcp_connection::bind(const std::string& port)
        {
            u::mutex_scoped_lock l(_mutex);
            INVARIANT(_socket);
            REQUIRE(_state == disconnected);

            boost::system::error_code error;

            _socket->open(tcp::v4(), error);
            _socket->set_option(tcp::socket::keep_alive(true),error);
            _socket->set_option(tcp::socket::reuse_address(true),error);
#ifdef __APPLE__
            _socket->set_option(reuse_port(true),error);
#endif
            auto p = boost::lexical_cast<short unsigned int>(port);
            _socket->bind(tcp::endpoint(tcp::v4(), p), error);

            if(error)
            {
                std::cerr << "error binding to port " << port << ": " << error.message() << std::endl;
                _error = error;
                _state = disconnected;
            }
        }

        void tcp_connection::connect(tcp::endpoint endpoint)
        {
            u::mutex_scoped_lock l(_mutex);
            INVARIANT(_socket);

            if(_state != disconnected || !_socket->is_open()) return;

            _state = connecting;

            _socket->async_connect(endpoint,
                    boost::bind(&tcp_connection::handle_connect, this,
                        ba::placeholders::error, endpoint));
        }

        void tcp_connection::start_read()
        {
            INVARIANT(_socket);
            if(!_socket->is_open()) return;

            //read message header
            ba::async_read_until(*_socket, _in_buffer, ':',
                    boost::bind(&tcp_connection::handle_header, this,
                        ba::placeholders::error,
                        ba::placeholders::bytes_transferred));
        }

        void tcp_connection::handle_connect(
                const boost::system::error_code& error,
                tcp::endpoint endpoint)
        {
            INVARIANT(_socket);
            ENSURE(_state == connecting);

            //if no error then we are connected
            if (!error) 
            {
                u::mutex_scoped_lock l(_mutex);
                _state = connected;
                std::cerr << "new out tcp_connection " << _socket->local_endpoint() << " -> " << _socket->remote_endpoint() << ": " << error.message() << std::endl;
                start_read();

                //if we have called send already before we connected,
                //send the data
                if(!_out_queue.empty()) 
                {
                    std::cerr << "already have " << _out_queue.size() << " stuff in queue...sending" << std::endl;
                    do_send(false);
                }
            }
            else 
            {
                if(_retries > 0)
                {
                    std::cerr << "retrying (" << (RETRIES - _retries) << "/" << RETRIES << ")..." << std::endl;
                    {
                        u::mutex_scoped_lock l(_mutex);
                        _retries--;

                        _state = disconnected;
                        u::sleep_thread(2000);
                    }
                    connect(endpoint);
                }
                else
                {
                    u::mutex_scoped_lock l(_mutex);
                    _error = error;
                    _state = disconnected;
                }
            }

            if(error)
                std::cerr << "error connecting to `" << _remote_address << "' : " << error.message() << std::endl;
        }

        u::bytes encode_wire(const u::bytes data)
        {
            auto m = u::encode(data);
            u::bytes encoded;
            encoded.resize(m.size() + 1);
            encoded[0] = '!';
            std::copy(m.begin(), m.end(), encoded.begin() + 1);
            return encoded;
        }

        bool tcp_connection::send(const u::bytes& b, bool block)
        {
            //add message to queue
            _out_queue.push(b);

            //do send if we are connected
            if(is_connected())
                _io.post(boost::bind(&tcp_connection::do_send, this, false));

            //if we are blocking, block until all messages are sent
            while(block && !_out_queue.empty() && is_disconnected()) u::sleep_thread(BLOCK_SLEEP);

            return is_connected();
        }

        void tcp_connection::do_send(bool force)
        {
            ENSURE(_socket);

            //check to see if a write is in progress
            if(!force && _writing) return;

            REQUIRE_FALSE(_out_queue.empty());

            _writing = true;

            //encode bytes to wire format
            _out_buffer = encode_wire(_out_queue.front());

            ba::async_write(*_socket,
                    ba::buffer(&_out_buffer[0], _out_buffer.size()),
                        boost::bind(&tcp_connection::handle_write, this,
                            ba::placeholders::error));

            ENSURE(_writing);
        }

        void tcp_connection::handle_write(const boost::system::error_code& error)
        {
            if(error) { _error = error; close(); return; }

            INVARIANT(_socket);
            REQUIRE_FALSE(_out_queue.empty());

            //remove sent message
            _out_queue.pop_front();
            std::cerr << "sent message, " << _out_queue.size() << " remaining..." << std::endl;

            //if we are done sending finish the async write chain
            if(_out_queue.empty()) 
            {
                _writing = false;
                return;
            }

            //otherwise do another async write
            do_send(true);
            ENSURE(_writing);
        }

        void tcp_connection::handle_header(const boost::system::error_code& error, size_t transferred)
        {
            INVARIANT(_socket);

            if(error) { _error = error; close(); return; }

            //read header 
            size_t o_size = _in_buffer.size();
            std::istream in(&_in_buffer);
            std::string size_buf;
            size_buf.reserve(64);
            int c = in.get();
            size_t garbage = 1;

            while(c != '!' && in.good()) 
            { 
                c = in.get(); 
                garbage++;
            }
            if(!in.good()) { start_read(); return;}


            c = in.get();
            size_t rc = 1;
            while(c != ':' && in.good())
            {
                size_buf.push_back(c);
                c = in.get();
                rc++;
            }
            if(!in.good()) { start_read(); return;}

            CHECK_EQUAL(_in_buffer.size(), o_size - rc - garbage);

            size_t size = 0; 
            try { size = lexical_cast<size_t>(size_buf); } catch (...){}

            if(size == 0) 
            {
                start_read();
                return;
            }

            //read body
            ba::async_read(*_socket,
                    _in_buffer,
                    ba::transfer_at_least(size - _in_buffer.size()),
                    boost::bind(&tcp_connection::handle_body, this,
                        ba::placeholders::error,
                        ba::placeholders::bytes_transferred,
                        size));
        }

        void tcp_connection::handle_body(const boost::system::error_code& error, size_t transferred, size_t size)
        {
            INVARIANT(_socket);
            REQUIRE_GREATER(size, 0);
            if(error && error == ba::error::eof) { _error = error; close(); return; }
            if(_in_buffer.size() < size) 
            {
                ba::async_read(*_socket,
                        _in_buffer,
                        ba::transfer_at_least(size - _in_buffer.size()),
                        boost::bind(&tcp_connection::handle_body, this,
                            ba::placeholders::error,
                            ba::placeholders::bytes_transferred,
                            size));
                return;
            }

            u::bytes data;
            data.resize(size);
            std::istream in(&_in_buffer);
            in.read(&data[0], size);

            //add message to in queue
            {
                u::mutex_scoped_lock l(_in_mutex);
                _in_queue.push(data);
                if(_track) _last_in_socket.push(this);
            }

            //read next message
            start_read();
        }

        tcp::socket& tcp_connection::socket()
        {
            INVARIANT(_socket);
            return *_socket;
        }

        const std::string& tcp_connection::remote_address() const
        {
            return _remote_address;
        }

        void tcp_connection::remote_address(const std::string& a)
        {
            REQUIRE_FALSE(a.empty());
            _remote_address = a;
        }

        void tcp_connection::update_remote_address()
        {
            INVARIANT(_socket);

            auto remote = _socket->remote_endpoint();
            auto ip = remote.address().to_string();
            auto port = boost::lexical_cast<std::string>(remote.port());

            _remote_address = make_tcp_address(ip, port);

            ENSURE_FALSE(_remote_address.empty());
        }

        void run_thread(tcp_queue*);
        tcp_queue::tcp_queue(const asio_params& p) : 
            _p(p), _done{false},
            _io{new ba::io_service}
        {
            switch(_p.mode)
            {
                case asio_params::bind: accept(); break;
                case asio_params::connect: connect(); break;
                case asio_params::delayed_connect: delayed_connect(); break;
                default: CHECK(false && "missed case");
            }

            if(_p.mode != asio_params::delayed_connect)
                _run_thread.reset(new std::thread{run_thread, this});

            INVARIANT(_io);
            INVARIANT(_p.mode == asio_params::delayed_connect || _run_thread);
        }

        tcp_queue::~tcp_queue() 
        {
            INVARIANT(_io);
            _io->stop();
            _done = true;
            if(_p.wait > 0) u::sleep_thread(_p.wait);
            if(_out) _out->close();
            if(_run_thread) _run_thread->join();
        }

        bool tcp_queue::send(const u::bytes& b)
        {
            INVARIANT(_io);
            REQUIRE(_p.mode != asio_params::bind);
            CHECK(_out);

            if(_out->is_disconnected() && _p.mode == asio_params::connect) 
                connect();

            return _out->send(b, _p.block);
        }

        bool tcp_queue::recieve(u::bytes& b)
        {
            //if we are blocking, block until we get message
            while(_p.block && !_in_queue.empty()) u::sleep_thread(BLOCK_SLEEP);

            //return true if we got message
            return _in_queue.pop(b);
        }

        void tcp_queue::connect(const std::string& host, const std::string& port)
        {
            REQUIRE(_p.mode == asio_params::delayed_connect);
            REQUIRE_FALSE(host.empty());
            REQUIRE_FALSE(port.empty());
            INVARIANT(_io);
            REQUIRE(!_out || _out->state() == tcp_connection::disconnected);

            _p.uri = make_tcp_address(host, port);
            _p.host = host;
            _p.port = port;
            _p.mode = asio_params::connect;
            connect();

            //start up engine
            _run_thread.reset(new std::thread{run_thread, this});
            ENSURE(_run_thread);
        }

        void tcp_queue::delayed_connect()
        try
        {
            INVARIANT(_io);
            REQUIRE(!_out);

            _out.reset(new tcp_connection{*_io, _in_queue, _last_in_socket, _mutex});
            if(!_p.local_port.empty()) _out->bind(_p.local_port);

            ENSURE(_out);
        }
        catch(std::exception& e)
        {
            std::cerr << "error in delayed connecting `" << _p.local_port << "' : " << e.what() << std::endl;
        }
        catch(...)
        {
            std::cerr << "error in delayed connecting `" << _p.local_port << "' : unknown error." << std::endl;
        }

        void tcp_queue::connect()
        try
        {
            INVARIANT(_io);
            REQUIRE(!_out || _out->state() == tcp_connection::disconnected);

            //init resolver if it does not exist
            if(!_resolver) _resolver.reset(new tcp::resolver{*_io}); 

            tcp::resolver::query query{_p.host, _p.port}; 
            auto ei = _resolver->resolve(query);
            auto endpoint = *ei;

            if(!_out) delayed_connect();

            _out->remote_address(_p.uri);
            _out->connect(endpoint);

            ENSURE(_out);
            ENSURE(_resolver);
        }
        catch(std::exception& e)
        {
            std::cerr << "error connecting to `" << _p.host << ":" << _p.port << "' : " << e.what() << std::endl;
        }
        catch(...)
        {
            std::cerr << "error connecting to `" << _p.host << ":" << _p.port << "' : unknown error." << std::endl;
        }

        void tcp_queue::accept()
        {
            INVARIANT(_io);

            if(!_acceptor) 
            {
                _acceptor.reset(new tcp::acceptor{*_io});

                auto port = boost::lexical_cast<short unsigned int>(_p.port);
                tcp::endpoint endpoint{tcp::v4(), port}; 

                _acceptor->open(endpoint.protocol());
                _acceptor->set_option(tcp::acceptor::keep_alive(true));
                _acceptor->set_option(tcp::acceptor::reuse_address(true));
#ifdef __APPLE__
                _acceptor->set_option(reuse_port(true));
#endif
                _acceptor->bind(endpoint);
                _acceptor->listen();
            }

            //prepare incoming tcp_connection
            tcp_connection_ptr new_connection{new tcp_connection{*_io, _in_queue, _last_in_socket, _mutex, _p.track_incoming, true}};
            _acceptor->async_accept(new_connection->socket(),
                    bind(&tcp_queue::handle_accept, this, new_connection,
                        ba::placeholders::error));

            ENSURE(_acceptor);
        }

        void tcp_queue::handle_accept(tcp_connection_ptr nc, const boost::system::error_code& error)
        {
            REQUIRE(nc);
            INVARIANT(_acceptor);

            if(error) 
            {
                std::cerr << "error accept " << nc->socket().local_endpoint() << " -> " << nc->socket().remote_endpoint() << ": " << error.message() << std::endl;
                return;
            }
            std::cerr << "new in tcp_connection " << nc->socket().remote_endpoint() << " " << error.message() << std::endl;

            _in_connections.push_back(nc);
            nc->update_remote_address();
            nc->start_read();

            //prepare next incoming tcp_connection
            tcp_connection_ptr new_connection{new tcp_connection{*_io, _in_queue, _last_in_socket, _mutex, _p.track_incoming, true}};
            _acceptor->async_accept(new_connection->socket(),
                    boost::bind(&tcp_queue::handle_accept, this, new_connection,
                        ba::placeholders::error));

            ENSURE(new_connection);
        }

        tcp_connection* tcp_queue::get_socket() const
        {
            tcp_connection* p = nullptr;
            switch(_p.mode)
            {
                case asio_params::bind: if(_p.track_incoming) _last_in_socket.pop(p); break;
                case asio_params::connect: p = _out.get(); break;
                default:
                    CHECK(false && "missed case");
            }
            return p;
        }

        tcp_queue_ptr create_bst_message_queue(const address_components& c)
        {
            auto p = parse_params(c);
            return tcp_queue_ptr{new tcp_queue{p}};
        }

        void run_thread(tcp_queue* q)
        {
            CHECK(q);
            CHECK(q->_io);
            while(!q->_done) 
            {
                q->_io->run_one();
                u::sleep_thread(BLOCK_SLEEP);
            }
        }

        std::string make_tcp_address(const std::string& host, const std::string& port, const std::string& local_port)
        {
            return local_port.empty() ? 
                "tcp://" + host + ":" + port : 
                "tcp://" + host + ":" + port + ",local_port=" + local_port;
        }

        tcp_queue_ptr create_message_queue(
                const std::string& address, 
                const queue_options& defaults)
        {
            auto c = parse_address(address, defaults); 
            tcp_queue_ptr p = create_bst_message_queue(c);
            ENSURE(p);
            return p;
        }

        socket_info get_socket_info(tcp_connection& c)
        {
            socket_info r;
            auto local = c.socket().local_endpoint();
            auto remote = c.socket().remote_endpoint();
            r.local_address = local.address().to_string();
            r.local_port = boost::lexical_cast<std::string>(local.port());
            r.remote_address = remote.address().to_string();
            r.remote_port = boost::lexical_cast<std::string>(remote.port());

            return r;
        }
    }
}