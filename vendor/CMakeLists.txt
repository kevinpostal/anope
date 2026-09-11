file(GLOB LIBRARIES CONFIGURE_DEPENDS "*")
foreach(LIBRARY IN LISTS LIBRARIES)
	if(IS_DIRECTORY ${LIBRARY})
		file(GLOB_RECURSE LIBRARY_SOURCES CONFIGURE_DEPENDS
			"${LIBRARY}/*.c"
			"${LIBRARY}/*.cc"
			"${LIBRARY}/*.cpp"
		)
		if(LIBRARY_SOURCES)
			# Exclude non-library directories (tests, examples, documentation, etc.)
			list(FILTER LIBRARY_SOURCES EXCLUDE REGEX "/(docpages|mlspp|library|library-vcpkg|coro_loop|davetest|unittest|httpservertest|soaktest|sockettest|userapptest|dave|voice|win32)/")
			# Exclude socket engines that are not used on this platform (kqueue is BSD/macOS only)
			if(NOT APPLE)
				list(FILTER LIBRARY_SOURCES EXCLUDE REGEX "/socketengines/kqueue\\.cpp$")
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
