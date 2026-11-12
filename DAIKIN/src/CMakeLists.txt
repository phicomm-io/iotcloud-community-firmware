file(GLOB sources "*.c" "*.cpp")
idf_component_register(SRCS ${sources})
# Existing shared wire codecs also build under Arduino without this IDF Werror.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=misleading-indentation -Wno-error=parentheses)
