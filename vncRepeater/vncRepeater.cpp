// vncRepeater.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"

#include <map>

#include "config.h"

#include "util.h"
#include "vncRepeater.h"
#include "service.h"

using namespace std;

constexpr char rfbProtocolVersion[] = "RFB 000.000\n";

bool config::traceToConsole = true;

// should be configurable eventually
uint16_t config::serverPort = 5500;
uint16_t config::viewerPort = 5901;

// a connection has a socket and an ID that was read from it immediately after connecting.
// two matched Connection objects form a ConnectionPair which proxies data from one to the other
class Connection
{
public:
	asio::ip::tcp::socket socket_;

	asio::ip::tcp::endpoint localEndpoint_; // persist endpoints so can be accessed even when socket_ is closed.
	asio::ip::tcp::endpoint remoteEndpoint_;

	string id;

	string extra;
	string rfbVersion;

	Connection(asio::io_service& ioService)
		: socket_(ioService)
	{}

	Connection(const Connection& r) = delete;
	Connection& operator=(const Connection& r) = delete;

	Connection(Connection&& r) = default;
	Connection& operator=(Connection&& r) = default;
	
	void onConnected()
	{
		configureSocket(socket_);
		localEndpoint_ = socket_.local_endpoint();
		remoteEndpoint_ = socket_.remote_endpoint();
	}
};

void error(const std::error_code& ec, const Connection& connection, const char* category, const char* msg = "")
{
	ostringstream stream;
	stream
		<< category
		<< "\t" << connection.remoteEndpoint_
		<< "\t" << ((connection.localEndpoint_.port() == config::viewerPort) ? "Viewer" : "Server")
		<< "\t" << "ID:" << connection.id << " (" << connection.extra << ")"
		<< "\t" << msg;

	if (ec) {
		stream << "\t" << ec << " (" << ec.message() << ")";
	}

	string text = stream.str();

	trace(text.c_str());
}

void info(const Connection& connection, const char* category, const char* msg = "")
{
	error({}, connection, category, msg);
}

// when first connected, an IncomingConnection reads the extra info and parses it
// then the Connection can be moved out from within
class IncomingConnection
{
public:
	asio::steady_timer timeout_;
	Connection connection_;

	array<char, 250> infoBuffer_;
	array<char, 12> rfbBuffer_;

	IncomingConnection(asio::io_service& ioService)
		: connection_(ioService)
		, timeout_(ioService)
	{
		infoBuffer_.fill(0);
		rfbBuffer_.fill(0);
	}

	void parseRfbVersion()
	{
		string rfbVersion;
		rfbVersion.assign(rfbBuffer_.begin(), rfbBuffer_.end());
		string prefix = rfbVersion.substr(0, 4);
		if (prefix != "RFB ") {
			return;
		}
		connection_.rfbVersion = move(rfbVersion);
	}

	void parseInfo()
	{
		string id;
		string extra;

		const char* pBegin = infoBuffer_.data();
		const char* pEnd = pBegin + infoBuffer_.size();

		const char* pInfo = pBegin;
		
		if (pInfo[0] != 'I' || pInfo[1] != 'D' || pInfo[2] != ':') {
			extra = string(pBegin, pEnd - pBegin);
		}
		else {
			pInfo = pInfo + 3;

			if (*pInfo == ':') {
				++pInfo;
			}

			const char* pID = pInfo;

			while (pInfo < pEnd && *pInfo != ';') {
				++pInfo;
			}


			id = string(pID, pInfo - pID);
			for (char& c : id) {
				if (c >= 'A' && c <= 'Z') {
					c += ('a' - 'A');
				}
			}

			// skip the ; for extra
			if (pInfo < pEnd) {
				++pInfo;
			}

			extra = string(pInfo, pEnd - pInfo);
		}

		connection_.id = move(id);
		connection_.extra = move(extra);
	}
};

// most activity occurs within the ConnectionPair, which proxies data between the two Connections
// a single buffer is used for the data, and a BufferedHandlerAllocator eliminates any allocations
// to hold callbacks. 
class ConnectionPair
	: public std::enable_shared_from_this<ConnectionPair>
{
public:
	asio::strand strand_;

	Connection first_;
	Connection second_;

	string rfbServerVersion_;

	ConnectionPair(asio::io_service& ioService, Connection&& first)
		: strand_(ioService)
		, first_(move(first))
		, second_(ioService)
	{}

	void run()
	{
		strand_.post([self = shared_from_this()]() {
			self->readFirst();
		});
	}

	void postAttach(shared_ptr<IncomingConnection> pIncomingConnection)
	{
		strand_.post([self = shared_from_this(), pIncomingConnection]() {
			self->second_ = move(pIncomingConnection->connection_);

			self->flushRfbVersion();

			self->readSecond();
		});
	}

protected:

	array<uint8_t, config::bufferSize> bufferFirst_;
	array<uint8_t, config::bufferSize> bufferSecond_;

	BufferedHandlerAllocator handlerFirst_;
	BufferedHandlerAllocator handlerSecond_;

	void shutdown(Connection& closing, Connection& lingering)
	{
		std::error_code dontCare;

		if (closing.socket_.is_open()) {
			closing.socket_.shutdown(asio::socket_base::shutdown_both, dontCare);
		}
		if (lingering.socket_.is_open()) {
			lingering.socket_.shutdown(asio::socket_base::shutdown_receive, dontCare);
		}
	}

	void shutdownFirst()
	{
		shutdown(first_, second_);
	}

	void shutdownSecond()
	{
		shutdown(second_, first_);
	}

	void readFirst()
	{
		auto self = shared_from_this();

		self->first_.socket_.async_read_some(asio::buffer(self->bufferFirst_), self->strand_.wrap(MakeBufferedHandler(self->handlerFirst_, [self](const std::error_code& ec, size_t bytesTransferred) {
			if (ec) {
				error(ec, self->first_, "readFirst");
				self->shutdownFirst();
				return;
			}

			if (!bytesTransferred) {
				error(asio::error::eof, self->first_, "readFirst", "0 byte op");
				self->shutdownFirst();
				return;
			}

			if (!self->second_.socket_.is_open()) {
				error(asio::error::not_connected, self->first_, "readFirst", "other side not open");
				self->shutdownFirst();
				return;
			}

			async_write(self->second_.socket_, asio::buffer(self->bufferFirst_, bytesTransferred), self->strand_.wrap(MakeBufferedHandler(self->handlerFirst_, [self](const std::error_code& ec, size_t bytesTransferred) {
				if (ec) {
					error(ec, self->second_, "readFirst-write");
					self->shutdownSecond();
					return;
				}

				if (!bytesTransferred) {
					error(asio::error::eof, self->second_, "readFirst-write", "0 byte op");
					self->shutdownSecond();
					return;
				}

				self->readFirst();
			})));
		})));
	}

	void readSecond()
	{
		auto self = shared_from_this();

		self->second_.socket_.async_read_some(asio::buffer(self->bufferSecond_), self->strand_.wrap(MakeBufferedHandler(self->handlerSecond_, [self](const std::error_code& ec, size_t bytesTransferred) {
			if (ec) {
				error(ec, self->second_, "readSecond");
				self->shutdownSecond();
				return;
			}

			if (!bytesTransferred) {
				error(asio::error::eof, self->second_, "readSecond", "0 byte op");
				self->shutdownSecond();
				return;
			}

			async_write(self->first_.socket_, asio::buffer(self->bufferSecond_, bytesTransferred), self->strand_.wrap(MakeBufferedHandler(self->handlerSecond_, [self](const std::error_code& ec, size_t bytesTransferred) {
				if (ec) {
					error(ec, self->first_, "readSecond-write");
					self->shutdownFirst();
					return;
				}

				if (!bytesTransferred) {
					error(asio::error::eof, self->first_, "readSecond-write", "0 byte op");
					self->shutdownFirst();
					return;
				}

				self->readSecond();
			})));
		})));
	}

	// the rfbVersion has to be held and echoed to the other connection once the match is made
	void flushRfbVersion()
	{
		auto& rfbConnection = first_.rfbVersion.empty() ? second_ : first_;
		auto& otherConnection = first_.rfbVersion.empty() ? first_ : second_;
		
		async_write(otherConnection.socket_, asio::buffer(rfbConnection.rfbVersion), strand_.wrap([self = shared_from_this(), &rfbConnection, &otherConnection](const std::error_code& ec, size_t bytesTransferred) {
			if (ec) {
				error(ec, otherConnection, "flushRfbVersion");
				self->shutdown(otherConnection, rfbConnection);
				return;
			}

			if (!bytesTransferred) {
				error(asio::error::eof, otherConnection, "flushRfbVersion", "0 byte op");
				self->shutdown(otherConnection, rfbConnection);
				return;
			}

			// read should already be in progress
		}));
	}
};

// Connection objects are matched by ID; multiple can wait on a single ID as well.
// upon a match, a ConnectionPair is created and run.
// a multimap is used to handle multiple waiters; while this generally ends up having
// stable ordering for multiple values on one key, that is not necessarily guaranteed.
class ConnectionBroker
{
public:
	asio::strand strand_;

	ConnectionBroker(asio::io_service& ioService)
		: strand_(ioService)
	{}

	void postPendingViewer(shared_ptr<IncomingConnection> pIncomingConnection) {
		strand_.post([this, pIncomingConnection]() {
			handleNewConnection(pIncomingConnection, waitingServers_, waitingViewers_);
		});
	}

	void postPendingServer(shared_ptr<IncomingConnection> pIncomingConnection) {
		strand_.post([this, pIncomingConnection]() {
			handleNewConnection(pIncomingConnection, waitingViewers_, waitingServers_);
		});
	}

protected:

	void handleNewConnection(shared_ptr<IncomingConnection> pIncomingConnection, multimap<string, weak_ptr<ConnectionPair>>& fromWaiting, multimap<string, weak_ptr<ConnectionPair>>& toWaiting)
	{
		auto matches = fromWaiting.equal_range(pIncomingConnection->connection_.id);

		for (auto it = matches.first; it != matches.second;) {
			auto pConnection = it->second.lock();

			if (!pConnection) {
				fromWaiting.erase(it++);
				continue;
			}

			fromWaiting.erase(it);

			info(pIncomingConnection->connection_, "handleNewConnection", "matched");

			pConnection->postAttach(pIncomingConnection);
			return;
		}

		// otherwise, add a new waiting ConnectionPair
		{
			auto id = pIncomingConnection->connection_.id;

			auto pConnection = make_shared<ConnectionPair>(strand_.get_io_service(), move(pIncomingConnection->connection_));
			toWaiting.emplace(id, pConnection);
			
			info(pConnection->first_, "handleNewConnection", "waiting");

			pConnection->run();
		}
	}

	multimap<string, weak_ptr<ConnectionPair>> waitingServers_;
	multimap<string, weak_ptr<ConnectionPair>> waitingViewers_;
};

// handle incoming server and viewer connections, and pass them off to the ConnectionBroker once initialized
class Server
{
public:
	asio::io_service ioService_;

	asio::strand serverStrand_;
	asio::strand viewerStrand_;

	asio::ip::tcp::acceptor serverAcceptor_;
	asio::ip::tcp::acceptor viewerAcceptor_;

	ConnectionBroker broker_;

	Server()
		: Server(config::serverPort, config::viewerPort)
	{}

	Server(uint16_t serverPort, uint16_t viewerPort)
		: ioService_()
		, serverStrand_(ioService_)
		, viewerStrand_(ioService_)
		, serverAcceptor_(ioService_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), serverPort))
		, viewerAcceptor_(ioService_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), viewerPort))
		, broker_(ioService_)
	{}

	void acceptNewServer()
	{
		auto pIncomingConnection = std::make_shared<IncomingConnection>(ioService_);
		serverAcceptor_.async_accept(pIncomingConnection->connection_.socket_, serverStrand_.wrap([this, pIncomingConnection](const std::error_code& ec) {
			if (!ioService_.stopped()) {
				acceptNewServer();
			}

			if (ec) {
				error(ec, pIncomingConnection->connection_, "acceptNewServer");
				return;
			}

			pIncomingConnection->connection_.onConnected();

			info(pIncomingConnection->connection_, "acceptNewServer", "accepted");

			// setup init timeout
			pIncomingConnection->timeout_.expires_from_now(std::chrono::seconds(config::rfbInitTimeout));
			pIncomingConnection->timeout_.async_wait(serverStrand_.wrap([this, pIncomingConnection](const std::error_code& ec) {
				if (ec) {
					if (ec != asio::error::operation_aborted) {
						error(ec, pIncomingConnection->connection_, "acceptNewServer-timeout");
					}
					// probably cancelled
					return;
				}

				std::error_code dontCare;
				pIncomingConnection->connection_.socket_.shutdown(asio::socket_base::shutdown_both, dontCare);
			}));

			// read connection info
			async_read(pIncomingConnection->connection_.socket_, asio::buffer(pIncomingConnection->infoBuffer_), serverStrand_.wrap([this, pIncomingConnection](const std::error_code& ec, size_t bytesTransferred) {

				if (ec) {
					error(ec, pIncomingConnection->connection_, "acceptNewServer-readInfo");
					return;
				}

				pIncomingConnection->parseInfo();

				if (pIncomingConnection->connection_.id.empty()) {
					error(asio::error::invalid_argument, pIncomingConnection->connection_, "acceptNewServer-readInfo", "no ID");

					std::error_code dontCare;
					pIncomingConnection->timeout_.cancel(dontCare);
					return;
				}

				info(pIncomingConnection->connection_, "acceptNewServer", "established");

				// read protocol version
				async_read(pIncomingConnection->connection_.socket_, asio::buffer(pIncomingConnection->rfbBuffer_), serverStrand_.wrap([this, pIncomingConnection](const std::error_code& ec, size_t bytesTransferred) {

					std::error_code dontCare;
					pIncomingConnection->timeout_.cancel(dontCare);

					if (ec) {
						error(ec, pIncomingConnection->connection_, "acceptNewServer-readProtocol");
						return;
					}

					pIncomingConnection->parseRfbVersion();

					broker_.postPendingServer(pIncomingConnection);
				}));
			}));
		}));
	}

	void acceptNewViewer()
	{
		auto pIncomingConnection = std::make_shared<IncomingConnection>(ioService_);
		viewerAcceptor_.async_accept(pIncomingConnection->connection_.socket_, viewerStrand_.wrap([this, pIncomingConnection](const std::error_code& ec) {
			if (!ioService_.stopped()) {
				acceptNewViewer();
			}

			if (ec) {
				error(ec, pIncomingConnection->connection_, "acceptNewViewer");
				return;
			}

			pIncomingConnection->connection_.onConnected();

			info(pIncomingConnection->connection_, "acceptNewViewer", "accepted");

			// setup init timeout
			pIncomingConnection->timeout_.expires_from_now(std::chrono::seconds(config::rfbInitTimeout));
			pIncomingConnection->timeout_.async_wait(viewerStrand_.wrap([this, pIncomingConnection](const std::error_code& ec) {
				if (ec) {
					if (ec != asio::error::operation_aborted) {
						error(ec, pIncomingConnection->connection_, "acceptNewViewer-timeout");
					}
					// probably cancelled
					return;
				}

				std::error_code dontCare;
				pIncomingConnection->connection_.socket_.shutdown(asio::socket_base::shutdown_both, dontCare);
			}));

			// send protocol version
			async_write(pIncomingConnection->connection_.socket_, asio::buffer(rfbProtocolVersion, _countof(rfbProtocolVersion) - 1), viewerStrand_.wrap([this, pIncomingConnection](const std::error_code& ec, size_t bytesTransferred) {
				if (ec) {
					error(ec, pIncomingConnection->connection_, "acceptNewViewer-writeProtocol");
					return;
				}

				// read connection info
				async_read(pIncomingConnection->connection_.socket_, asio::buffer(pIncomingConnection->infoBuffer_), viewerStrand_.wrap([this, pIncomingConnection](const std::error_code& ec, size_t bytesTransferred) {

					std::error_code dontCare;
					pIncomingConnection->timeout_.cancel(dontCare);

					if (ec) {
						error(ec, pIncomingConnection->connection_, "acceptNewViewer-readInfo");
						return;
					}

					pIncomingConnection->parseInfo();

					if (pIncomingConnection->connection_.id.empty()) {
						error(asio::error::invalid_argument, pIncomingConnection->connection_, "acceptNewViewer-readInfo", "no ID");
						return;
					}

					info(pIncomingConnection->connection_, "acceptNewViewer", "established");

					broker_.postPendingViewer(pIncomingConnection);
				}));
			}));
		}));
	}
};

int InitService()
{
	config::traceToConsole = false;
	resetCurrentDirectory();
	return 0;
}

Server theServer;

int RunApplication()
{
	auto coreCount = std::thread::hardware_concurrency();
	auto coreStep = 1U;

	if (coreCount == 3) {
		coreCount = 2;
	}
	if (coreCount < 1) {
		coreCount = 1;
	}
	if (coreCount > 16) {
		coreCount = 16;
	}

	// adjust coreStep to only run on even cores.
	// this really does not need that many cores anyway!

	if (coreCount >= 4) {
		coreStep = 2;
	}

	{
		ostringstream header;
		header << "vncRepeater" << "\r\n";

		for (auto core = 0U; core < coreCount; core += coreStep) {
			header << "\t... running on core " << core << "\r\n";
		}

		trace(header.str().c_str());
	}
	

	theServer.acceptNewServer();
	theServer.acceptNewViewer();

	vector<thread> threads;


	for (auto core = 0U; core < coreCount; core += coreStep) {
		threads.push_back(thread([core]() {
			::SetThreadIdealProcessor(::GetCurrentThread(), core);

			while (!theServer.ioService_.stopped()) {
				auto ran = theServer.ioService_.run();
			}
		}));
	}

	vector<HANDLE> threadHandles;
	for (auto& thread : threads) {
		threadHandles.push_back(thread.native_handle());
	}

	int ret = ::WaitForMultipleObjects((DWORD)threadHandles.size(), &threadHandles[0], TRUE, INFINITE);

	for (auto& thread : threads) {
		thread.join();
	}

	return 0;
}

int StopApplication()
{
	if (!theServer.ioService_.stopped()) {
		theServer.ioService_.stop();
		trace("requested stop");
		return 1;
	}
	return 0;
}

int main()
{
	::SetConsoleCtrlHandler(&ConsoleCtrlHandler, TRUE);
	
	int ret = BeginService();

	if (ERROR_FAILED_SERVICE_CONTROLLER_CONNECT == ret) {
		ret = RunApplication();
	}

	return ret;
}

