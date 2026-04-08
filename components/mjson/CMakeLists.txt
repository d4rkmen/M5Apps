idf_component_register(
    SRCS "src/mjson.c"
    INCLUDE_DIRS "src"
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    MJSON_ENABLE_PRINT=1
    MJSON_ENABLE_RPC=0
    MJSON_ENABLE_PRETTY=0
    MJSON_ENABLE_MERGE=0
    MJSON_ENABLE_NEXT=1
    MJSON_ENABLE_BASE64=0
)
