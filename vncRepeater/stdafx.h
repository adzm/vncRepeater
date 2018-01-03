// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WINSOCK_DEPRECATED_NO_WARNINGS


#define WINVER 0x0600
#define _WIN32_WINNT 0x0600

#include <SDKDDKVer.h>

#include <stdio.h>
#include <tchar.h>

#include <cstdint>

#include <memory>

#include <vector>
#include <thread>
#include <mutex>
#include <string>
#include <sstream>
#include <chrono>

#include "asio.hpp"

// for SIO_KEEPALIVE_VALS and etc
#include <mstcpip.h>
#include <ShlObj.h>
#include <Shlwapi.h>