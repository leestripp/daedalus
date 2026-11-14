cmake_minimum_required(VERSION 3.28)

add_library(monkeybyte_backend_core STATIC
    backend/AgentService.cpp
    backend/LoopbackServer.cpp
    backend/SessionCatalog.cpp
)
target_include_directories(monkeybyte_backend_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/backend)
target_link_libraries(monkeybyte_backend_core PUBLIC daedalus)
target_compile_features(monkeybyte_backend_core PUBLIC cxx_std_23)

add_executable(monkeybyte-electron-backend backend/main.cpp)
target_link_libraries(monkeybyte-electron-backend PRIVATE monkeybyte_backend_core)

add_executable(monkeybyte-backend-catalog-tests tests/backend-catalog-tests.cpp)
target_link_libraries(monkeybyte-backend-catalog-tests PRIVATE monkeybyte_backend_core)
add_test(NAME monkeybyte_backend_catalog_tests COMMAND monkeybyte-backend-catalog-tests)
set_tests_properties(monkeybyte_backend_catalog_tests PROPERTIES LABELS "app")
