# SPDX-License-Identifier: MIT
#
# boxwin's one component (u-defeatist-0002). Everything here links into the
# boxed ELF; the feckless components named are headers only, their calls
# imports that the launcher's feckless_box_calls answers. The decoders are
# the player's own files in ../../main, by include path.
idf_component_register(
    SRCS "boxwin_main.c" "boxwin_run.c" "boxwin_arith.c"
    INCLUDE_DIRS "." "../../main"
    REQUIRES esp_timer esp_lcd
             lwip                        # u-defeatist-0003: for elf_loader, see below
             minimp3                     # headers, compiled into boxwin_run.c
             feckless_abi feckless_abi_storage
             feckless_graphics_handler feckless_drivers feckless_storage_handler
             feckless_box_libc           # linked into the ELF
)

# boxwin_run.c as ../../main/CMakeLists.txt builds decoder.c, which is where
# these decoders are compiled in the player: -O3 (5276), minimp3's unused
# functions on RISC-V, and its iscf[40] false positive kept a warning. A
# window measured at another level would not be the player's.
set_source_files_properties(boxwin_run.c PROPERTIES COMPILE_OPTIONS
    "-O3;-Wno-unused-function;-Wno-error=stringop-overflow")

# u-defeatist-0003: lwip is in REQUIRES for a file that is not this
# project's. feckless_abi requires elf_loader, whose esp_elf_symbol.c
# includes <sys/socket.h> and <arpa/inet.h> without requiring lwip, where
# they live. In the player's build lwip is there anyway (../../main names
# it); this project is trimmed to main and what it requires, and nothing
# here needs a socket, so the first build stopped at that file. Nothing of
# lwip goes into the ELF: it is not in ELF_COMPONENTS.
