build_subdir(${CMAKE_CURRENT_SOURCE_DIR})

# The Discord protocol uses the C++ Discord library (DPP) which is vendored in
# vendor/brainboxdotcc. DPP requires OpenSSL (for its secure websocket
# connections) and zlib (for compressed gateway traffic).
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
target_compile_definitions(vendored_brainboxdotcc PRIVATE DPP_BUILD DPP_NO_CORO)
target_include_directories(vendored_brainboxdotcc PUBLIC ${Anope_SOURCE_DIR}/vendor/brainboxdotcc/dpp/include ${Anope_SOURCE_DIR}/vendor/brainboxdotcc/dpp/include/dpp)
target_link_libraries(vendored_brainboxdotcc PRIVATE OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)
target_link_libraries(${SO} PRIVATE vendored_brainboxdotcc OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)
