# vncRepeater

Simple, scalable vncRepeater for UltraVNC and others.

## Usage

vncRepeater will listen for incoming server connections on port 5500, and incoming viewer connections on port 5901. 

Logs will be written to common appdata -- usually `C:\ProgramData\vncRepeater`

Access to at least one side of the repeater should be protected by firewall whitelists, or code should be extended to provide your own authentication or filtering. Private customizations with proprietary code are allowed under the LGPL as long as it is not distributed.

## Installation

Can be run as a normal console application, or manually created service via `sc create`

`sc create vncRepeater binpath= "Z:\util\vncRepeater.exe" DisplayName= vncRepeater depend= Tcpip start= auto`

## Technology

Async architecture built around asio which is also proposed for C++ Networking standards in the future. Special handler overloads allow the fast path to run with zero allocations. Many customiztion points can be tweaked within code, such as ports and buffer sizes.

## Debugging

Set up WER to create LocalDumps! eg to C:\ProgramData\vncRepeater\Dumps

Create the registry key `HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\Windows Error Reporting\LocalDumps\vncRepeater.exe` and add the `DumpFolder`, `DumpType`, and `DumpCount` values as specified by Microsoft https://msdn.microsoft.com/en-us/library/windows/desktop/bb787181

## Audience and License

This is a simple implementation of the vncRepeater that can be tweaked or modified as desired, provided the source code is distributed if the resulting binary is also distributed. Running this internally on a server with proprietary changes is allowed, encouraged even. 

GNU LGPL 2.1

