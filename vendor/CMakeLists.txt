file(GLOB LIBRARIES CONFIGURE_DEPENDS "*")
foreach(LIBRARY IN LISTS LIBRARIES)
	if(IS_DIRECTORY ${LIBRARY})
		file(GLOB_RECURSE LIBRARY_SOURCES CONFIGURE_DEPENDS
			"${LIBRARY}/*.c"
			"${LIBRARY}/*.cc"
			"${LIBRARY}/*.cpp"
		)
		if(LIBRARY_SOURCES)
			# Exclude non-library directories (tests, examples, documentation, etc.).
			# Voice is built from its stub implementation: the enabled variant needs
			# libopus and libsodium, but the stubs still have to be compiled because
			# discordvoiceclient.cpp references them unconditionally.
			list(FILTER LIBRARY_SOURCES EXCLUDE REGEX "/(docpages|mlspp|library|library-vcpkg|coro_loop|davetest|unittest|httpservertest|soaktest|sockettest|userapptest|dave|voice/enabled|win32)/")
			# DPP expects exactly one socket engine to be compiled; each of them
			# defines the same factory. kqueue is BSD/macOS only, epoll is Linux
			# only, and poll is the portable fallback for everything else.
			if(APPLE OR CMAKE_SYSTEM_NAME MATCHES "BSD|DragonFly")
				list(FILTER LIBRARY_SOURCES EXCLUDE REGEX "/socketengines/(epoll|poll)\\.cpp$")
			elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
				list(FILTER LIBRARY_SOURCES EXCLUDE REGEX "/socketengines/(kqueue|poll)\\.cpp$")
			else()
				list(FILTER LIBRARY_SOURCES EXCLUDE REGEX "/socketengines/(epoll|kqueue)\\.cpp$")
			endif()
		endif()
		if(LIBRARY_SOURCES)
			cmake_path(GET LIBRARY FILENAME LIBRARY_NAME)
			set(LIBRARY_TARGET "vendored_${LIBRARY_NAME}")
			add_library(${LIBRARY_TARGET} STATIC ${LIBRARY_SOURCES})
			target_compile_options(${LIBRARY_TARGET} PRIVATE "$<IF:$<BOOL:${MSVC}>,/W0,-w>")
			set_target_properties(${LIBRARY_TARGET} PROPERTIES
				FOLDER "VendoredLibraries"
				PREFIX ""
				UNITY_BUILD OFF
			)
		endif()
	endif()
endforeach()
