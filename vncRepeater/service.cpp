#include "stdafx.h"
#include "service.h"
#include "vncRepeater.h"



BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType)
{
	switch (dwCtrlType) {
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
	case CTRL_CLOSE_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		StopApplication();
		return TRUE;
	}

	return FALSE;
}



SERVICE_STATUS          _ServiceStatus;
SERVICE_STATUS_HANDLE   _ServiceStatusHandle = 0;

void WINAPI ServiceControlHandler(DWORD fdwControl)
{
	switch (fdwControl) {
	case SERVICE_CONTROL_STOP:
	case SERVICE_CONTROL_SHUTDOWN:
	{
		StopApplication();

		_ServiceStatus.dwWin32ExitCode = 0;
		_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
		_ServiceStatus.dwCheckPoint++;
		_ServiceStatus.dwWaitHint = 10000;
	}
	break;
	default:
		return;
	}

	// Report current status
	SetServiceStatus(_ServiceStatusHandle, &_ServiceStatus);
}


void WINAPI ServiceMain(DWORD dwArgc, LPSTR *lpszArgv)
{
	_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
	_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
	_ServiceStatus.dwWin32ExitCode = 0;
	_ServiceStatus.dwServiceSpecificExitCode = 0;
	_ServiceStatus.dwCheckPoint = 0;
	_ServiceStatus.dwWaitHint = 0;

	_ServiceStatusHandle = RegisterServiceCtrlHandler(L"vncRepeater", (LPHANDLER_FUNCTION)ServiceControlHandler);
	if (_ServiceStatusHandle == (SERVICE_STATUS_HANDLE)0) {
		return;
	}

	int error = InitService();
	if (error) {
		_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
		_ServiceStatus.dwWin32ExitCode = error ? error : -1;
		SetServiceStatus(_ServiceStatusHandle, &_ServiceStatus);
		return;
	}

	_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
	SetServiceStatus(_ServiceStatusHandle, &_ServiceStatus);

	RunApplication();

	{
		_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
		_ServiceStatus.dwWin32ExitCode = 0;
		_ServiceStatus.dwCheckPoint = 0;
		_ServiceStatus.dwWaitHint = 0;
		SetServiceStatus(_ServiceStatusHandle, &_ServiceStatus);
	}
}

int BeginService()
{
	SERVICE_TABLE_ENTRY ServiceTable[] = {
		{ L"vncRepeater", (LPSERVICE_MAIN_FUNCTION)ServiceMain },
		{ NULL, NULL }
	};

	if (!StartServiceCtrlDispatcher(ServiceTable)) {
		return GetLastError();
	}

	return 0;
}

