build_subdir(${CMAKE_CURRENT_SOURCE_DIR})

#The Discord protocol uses the C++ Discord library(DPP) which is vendored in
#vendor / brainboxdotcc; its own requirements(OpenSSL, zlib, DPP_NO_CORO) are
#attached to that target.The service itself hashes user ids with libcrypto.
    find_package(OpenSSL REQUIRED) target_link_libraries(${
        SO} PRIVATE vendored_brainboxdotcc OpenSSL::Crypto)
