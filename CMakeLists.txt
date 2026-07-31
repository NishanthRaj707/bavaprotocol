idf_component_register(
    SRCS "src/bava_crc.c" 
         "src/bava_dict.c" 
         "src/bava_state.c" 
         "src/bava_tx.c"
    INCLUDE_DIRS "include"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -Wstrict-prototypes)