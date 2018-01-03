#pragma once

namespace config
{
	extern bool traceToConsole;

	constexpr int rfbInitTimeout = 5;
	constexpr size_t keepAliveTime = 1000 * 60 * 5;
	constexpr size_t keepAliveInterval = 1000 * 10;

	constexpr size_t bufferSize = 0x4000;

	extern uint16_t serverPort; // = 5500
	extern uint16_t viewerPort; // = 5901
}

