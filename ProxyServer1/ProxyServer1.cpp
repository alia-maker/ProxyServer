

#include <cstdlib>
#include "pch.h"
#include <iostream>
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/noncopyable.hpp>



template <size_t alloc_size>
class handler_allocator 
{
public:
	handler_allocator() = default;
	~handler_allocator() = default;

	void *allocate(size_t size)
	{
		if (!is_use && (size <= buffer.size))
		{
			is_use = true;
			return buffer.address();
		}
		return ::operator new(size);
	}

	void deallocate(void *pointer)
	{
		if (buffer.address() == pointer)
		{
			is_use = false;
			return;
		}
		::operator delete(pointer);
	}

private:
	bool is_use{ false };
	boost::aligned_storage<alloc_size> buffer{};
};
template <typename Allocator, typename F>
class handler_caller
{
public:
	using handler_type = handler_caller<Allocator, F>;

	handler_caller(Allocator &allocator, const F &h) : allocator(allocator),
		handler(h)
	{
	}

	handler_caller(const handler_type &o) : allocator(o.allocator),
		handler(o.handler)
	{
	}

	~handler_caller() = default;

	friend void *asio_handler_allocate(std::size_t size, handler_type *ctx)
	{
		return ctx->allocator.allocate(size);
	}

	friend void asio_handler_deallocate(void *ptr, std::size_t, handler_type *ctx)
	{
		ctx->allocator.deallocate(ptr);
	}

	template <typename H>
	friend void asio_handler_invoke(const H function, handler_type *context)
	{
		using boost::asio::asio_handler_invoke;
		asio_handler_invoke(function, boost::addressof(context->handler));
	}

	template <typename... Args>
	void operator()(const Args &... args)
	{
		handler(args...);
	}

private:
	Allocator &allocator;
	F handler;
};

template <typename Allocator, typename F>
inline handler_caller<Allocator, F>

make_custom_handler(Allocator &allocator, const F &f)
{
	return handler_caller<Allocator, F>(allocator, f);
}



class session : public std::enable_shared_from_this<session>, private boost::noncopyable
{
public:

	session(boost::asio::io_service& ios)
		: ios(ios),
		strand_lock(ios),
		local_sock(ios),
		sock(ios)
	{
	}

  boost::asio::ip::tcp::socket& socket()
  {
	  return local_sock;
  }

  void start(const boost::asio::ip::tcp::endpoint &endpoint)
  {

	  sock.async_connect(endpoint, 
		  strand_lock.wrap(
		  boost::bind(&session::handle_connection, shared_from_this(), boost::asio::placeholders::error)));

  }

private:
	void handle_connection(const boost::system::error_code &ec)
	{
		if (!ec)
		{
			sock.async_read_some(
				boost::asio::buffer(buffer, def_buffer_size),
				make_custom_handler(
					socket_allocator,
					strand_lock.wrap(
						boost::bind(
							&session::handle_upstream_read,
							shared_from_this(),
							boost::asio::placeholders::error,
							boost::asio::placeholders::bytes_transferred))));
		

			local_sock.async_read_some(
				boost::asio::buffer(local_buffer, def_buffer_size),
				make_custom_handler(
					local_socket_allocator,
					strand_lock.wrap(
						boost::bind(
							&session::handle_local_read,
							shared_from_this(),
							boost::asio::placeholders::error,
							boost::asio::placeholders::bytes_transferred))));
		}
		else
		{
			close();
		}
	}

	void handle_upstream_read(const boost::system::error_code &ec, const std::size_t bytes_transferred)
	{
		if (!ec)
		{
			boost::asio::async_write(
				local_sock,
				boost::asio::buffer(buffer, bytes_transferred),
				make_custom_handler(
					local_socket_allocator,
					strand_lock.wrap(
						boost::bind(
							&session::handle_local_write,
							shared_from_this(),
							boost::asio::placeholders::error))));
		}
		else
		{
			close();
		}
	}

	void handle_local_write(const boost::system::error_code &ec)
	{
		if (!ec)
		{
			sock.async_read_some(
				boost::asio::buffer(buffer, def_buffer_size),
				make_custom_handler(
					socket_allocator,
					strand_lock.wrap(
						boost::bind(
							&session::handle_upstream_read,
							shared_from_this(),
							boost::asio::placeholders::error,
							boost::asio::placeholders::bytes_transferred))));
		}
		else
		{
			close();
		}
	}

	void handle_local_read(const boost::system::error_code &ec, const std::size_t bytes_transferred)
	{
		if (!ec)
		{
			auto buffer_data = boost::asio::buffer(local_buffer, def_buffer_size);

			boost::asio::async_write(
				sock,
				boost::asio::buffer(local_buffer, bytes_transferred),
				make_custom_handler(
					socket_allocator,
					strand_lock.wrap(
						boost::bind(
							&session::handle_upstream_write,
							shared_from_this(),
							boost::asio::placeholders::error))));
			if (bytes_transferred > 5)
			{
					if (local_buffer[0] == 'Q')
					{
						for (int i = 5; i < bytes_transferred; i++) 
							std::cout << local_buffer[i];
						
					}
			}

			std::cout << std::endl;
		}
		else
		{
			close();
		}
	}


	void handle_upstream_write(const boost::system::error_code &ec)
	{
		if (!ec)
		{

			local_sock.async_read_some(
				boost::asio::buffer(local_buffer, def_buffer_size),
				make_custom_handler(
					local_socket_allocator,
					strand_lock.wrap(
						boost::bind(
							&session::handle_local_read,
							shared_from_this(),
							boost::asio::placeholders::error,
							boost::asio::placeholders::bytes_transferred))));
		}
		else
		{
			close();
		}
	}

	void close()
	{
		std::lock_guard<std::mutex> lock(mtx);

		auto close_socket = [](boost::asio::ip::tcp::socket &socket) {
			if (socket.is_open())
			{
				boost::system::error_code ec;
				socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
				socket.close(ec);
			}
		};

		close_socket(sock);
		close_socket(local_sock);
	}


  static const int def_buffer_size = 1024;
  static const int max_length = 1024;
  char data_[max_length];
  boost::asio::io_service &ios;
  std::array<uint8_t, def_buffer_size> buffer;
  std::array<uint8_t, def_buffer_size> local_buffer;
  boost::asio::ip::tcp::socket sock;
  boost::asio::ip::tcp::socket local_sock;
  boost::asio::io_service::strand strand_lock;

  handler_allocator<def_buffer_size> socket_allocator;
  handler_allocator<def_buffer_size> local_socket_allocator;
  std::mutex mtx;

};



class server: private boost::noncopyable
{
public:
	server(boost::asio::io_service& io_service, uint16_t proxy_port, std::string upstream_ip, uint16_t upstream_port)
		: io_service_(io_service),
		acceptor_(io_service, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), proxy_port)),
		endpoint(boost::asio::ip::address::from_string(upstream_ip), upstream_port)
	{
		std::cout << "Server is started"<<std::endl;

		start_accept();

	}


private:

	void start_accept()
	{
			auto current_session = std::make_shared <session>(io_service_);
			acceptor_.async_accept(
				current_session->socket(),
				[this, current_session](const boost::system::error_code &ec) {
					if (!ec)
					{
						current_session->start(endpoint);
						std::cout << "Start new session";
						start_accept();
					}
					else
					{
						std::cerr << ec.message() << std::endl;
					}
				});
	}

	/*void handle_accept(session* new_session,
		const boost::system::error_code& error)
	{
		if (!error)
		{
			new_session->start(upstream_endpoint);
		}
		else
		{
			delete new_session;
		}

		start_accept();
	}*/



	boost::asio::io_service& io_service_;
	boost::asio::ip::tcp::acceptor acceptor_;
	boost::asio::ip::tcp::endpoint endpoint;
	
};





void connect_handler(const boost::system::error_code & ec)
{
	if (!ec.failed()) {
		std::cout << "Connection is successfull!\n";
	}
	// here we know we connected successfully
	// if ec indicates success 
}

using boost::asio::ip::tcp;

int main(int argc, char* argv[])
{
	try
	{
		std::cout << "Enter enter target base hostname:";
		std::string hostname;
		std::cin >> hostname;
		if (hostname=="localhost")
			hostname = "127.0.0.1";
		std::cout << "Enter target base port:";
		uint16_t port;
		std::cin >> port;


		std::cout << "Enter local port:";
		uint16_t local_port;
		std::cin >> local_port;

		boost::asio::io_service io_service;
		//server s(io_service, 5000, std::string("127.0.0.1"), 5432);
		server s(io_service, local_port, hostname, port);

		io_service.run();
	}
	catch (std::exception& e)
	{
	std::cerr << "Exception: " << e.what() << "\n";
	}

 
  return 0;
}