cmake_minimum_required(VERSION 3.21)

project(
	NjordlingerCombatGameplayOverhaul
	VERSION 1.0.0
	LANGUAGES CXX ASM_MASM
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in
	${CMAKE_CURRENT_BINARY_DIR}/version.rc
	@ONLY
)

file(GLOB_RECURSE headers CONFIGURE_DEPENDS "include/*.h")
file(GLOB_RECURSE sources CONFIGURE_DEPENDS "source/*.cpp")

# The trampoline stubs are assembly on purpose - see the comment at the top of the file. Keeping
# them in MASM means the assembler checks the encoding at build time, which a hand-written byte
# array never would. They live in their own OBJECT library so the C++ compile options CommonLibSSE
# attaches to the plugin target (which ml64 rejects, and on the NG 7.2 line rejects so hard that
# no object is written) never reach the assembler.
add_library(${PROJECT_NAME}_asm OBJECT "${CMAKE_CURRENT_SOURCE_DIR}/source/SkillCapStub.asm")

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${headers} ${sources})

find_package(CommonLibSSE CONFIG REQUIRED)
find_package(spdlog REQUIRED CONFIG)
find_package(directxtk CONFIG REQUIRED)  # CommonLibSSE-NG links Microsoft::DirectXTK
find_package(nlohmann_json CONFIG REQUIRED)  # the class table and the quest-rewards rows are JSON

add_commonlibsse_plugin(${PROJECT_NAME} SOURCES ${headers} ${sources})
target_sources(${PROJECT_NAME} PRIVATE $<TARGET_OBJECTS:${PROJECT_NAME}_asm>)
add_library("${PROJECT_NAME}::${PROJECT_NAME}" ALIAS "${PROJECT_NAME}")

# Build line: 1 (SE/AE 1.6, CommonLibSSE-NG 3.7) or 17 (Skyrim 1.7.x, CommonLibSSE-NG 7.2).
# The build-relwithdebinfo-17 preset sets this to 17; the source #if-branches the NG 3.7 -> 7.2
# API renames.
set(RUNTIME_LINE "1" CACHE STRING "Build line: 1 (SE/AE 1.6) or 17 (Skyrim 1.7.x)")
target_compile_definitions(${PROJECT_NAME} PRIVATE RUNTIME_LINE=${RUNTIME_LINE})

if (MSVC)
	# Debug symbols (rule 43) and no build-machine paths in the DLL (rule 45).
	target_compile_options(${PROJECT_NAME} PRIVATE /Zi)
	target_link_options(${PROJECT_NAME} PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF)
	file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" PROJECT_NATIVE_DIR)
	target_compile_options(${PROJECT_NAME} PRIVATE "/d1trimfile:${PROJECT_NATIVE_DIR}")
	target_link_options(${PROJECT_NAME} PRIVATE /PDBALTPATH:%_PDB%)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(
	${PROJECT_NAME}
	PRIVATE
		CommonLibSSE::CommonLibSSE
		spdlog::spdlog_header_only
		nlohmann_json::nlohmann_json
)

target_precompile_headers(${PROJECT_NAME} PRIVATE include/PCH.h)

# The generated version resource has to be COMPILED IN, not merely generated. configure_file
# above writes version.rc and nothing else references it, so leaving this out builds a DLL with
# no version resource at all - it reports an empty FileVersion and the packaging checks that read
# the binary have nothing to read. Caught 2026-09-03 on this project's own build; the same
# omission had already shipped once in the AUO fork.
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
