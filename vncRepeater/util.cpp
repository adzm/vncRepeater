#include "stdafx.h"
#include "util.h"
#include "config.h"

#pragma comment(lib, "shlwapi.lib")

using namespace std;

// set SO_NODELAY and enable keepalive
void configureSocket(asio::ip::tcp::socket& socket)
{
	socket.set_option(asio::ip::tcp::no_delay(true));

	socket.set_option(asio::socket_base::keep_alive(true));

	DWORD bytes_returned = 0;
	tcp_keepalive keepalive_requested = { 0 };
	tcp_keepalive keepalive_returned = { 0 };

	keepalive_requested.onoff = 1;
	keepalive_requested.keepalivetime = config::keepAliveTime;
	keepalive_requested.keepaliveinterval = config::keepAliveInterval;
	// 10 probes always used by default in Vista+; not changeable. 

	if (0 != WSAIoctl(socket.native(), SIO_KEEPALIVE_VALS,
		&keepalive_requested, sizeof(keepalive_requested),
		&keepalive_returned, sizeof(keepalive_returned),
		&bytes_returned, NULL, NULL))
	{
		int lastError = WSAGetLastError();
		// but still continue even if we couldn't set this
	}
}

bool resetCurrentDirectory()
{
	wchar_t wszPath[MAX_PATH] = { 0 };
	if (SUCCEEDED(::SHGetFolderPath(NULL, CSIDL_COMMON_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, wszPath))) {
		::PathAppend(wszPath, L"vncRepeater\\");
		::CreateDirectory(wszPath, nullptr);
		::SetCurrentDirectory(wszPath);
		return true;
	}

	return false;
}


namespace {
	SYSTEMTIME querySystemTime()
	{
		SYSTEMTIME st = { 0 };
		::GetSystemTime(&st);
		return st;
	}

	mutex logMutex;

	HANDLE internalCreateLogFile(SYSTEMTIME& st)
	{
		wchar_t filePath[_MAX_PATH] = { 0 };

		swprintf_s(filePath,
			L"vncRepeater_%04hu%02hu%02hu"
			L"T%02hu%02hu%02huZ"
			L"_%06lu.log"
			, st.wYear, st.wMonth, st.wDay
			, st.wHour, st.wMinute, st.wSecond
			, ::GetCurrentProcessId()
		);

		HANDLE h = ::CreateFile(filePath, GENERIC_WRITE, FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) {
			resetCurrentDirectory();
			h = ::CreateFile(filePath, GENERIC_WRITE, FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE) {
				h = nullptr;
			}
		}

		if (h) {
			::SetFilePointer(h, 0, nullptr, FILE_END);
		}
		return h;
	}

	HANDLE openLogFile()
	{
		static SYSTEMTIME lastTime = { 0 };
		static HANDLE logFile = nullptr;

		auto st = querySystemTime();
		if (!logFile || st.wYear != lastTime.wYear || st.wMonth != lastTime.wMonth || st.wDay != lastTime.wDay) {

			unique_lock<mutex> lock(logMutex);

			if (!logFile || st.wYear != lastTime.wYear || st.wMonth != lastTime.wMonth || st.wDay != lastTime.wDay) {
				if (logFile) {
					::CloseHandle(logFile);
					logFile = nullptr;
				}

				logFile = internalCreateLogFile(st);
				lastTime = st;
			}
		}

		return logFile;
	}
}

void trace(const char* msg)
{
	auto st = querySystemTime();

	char prefix[128] = { 0 };

	sprintf_s(prefix,
		"%04hu%02hu%02hu"
		"T%02hu%02hu%02huZ"
		":\t"
		, st.wYear, st.wMonth, st.wDay
		, st.wHour, st.wMinute, st.wSecond
	);

	ostringstream stream;
	stream << prefix
		<< msg
		<< "\r\n";

	string text = stream.str();

	if (auto logFile = openLogFile())
	{
		unique_lock<mutex> lock(logMutex);
		DWORD bytesWritten = 0;
		::WriteFile(logFile, &text[0], (DWORD)text.size(), &bytesWritten, nullptr);
	}

	// echo to console?
	if (config::traceToConsole) {
		puts(text.c_str());
	}
}

