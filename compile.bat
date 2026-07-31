del *.obj
del nadb.exe

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

cl /EHsc /std:c++17 /O2 main.cpp adb_bridge.cpp adb_socket_proto.cpp adb_sync_proto.cpp command_resolver.cpp fuzzy.cpp phone_info.cpp sdk_builder.cpp sdk_cache.cpp /Fe:nadb.exe /link ws2_32.lib

del *.obj