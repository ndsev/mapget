cmake_minimum_required(VERSION 3.15)
project(PackageTest CXX)

find_package(mapget CONFIG
  COMPONENTS model service log http-service http-datasource
  REQUIRED)

add_executable(test_package src/example.cpp)
target_link_libraries(test_package
  mapget::model
  mapget::service
  mapget::log
  mapget::http-service
  mapget::http-datasource)
