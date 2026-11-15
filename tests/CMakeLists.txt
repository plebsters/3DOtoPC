add_executable(make_test_disc make_test_disc.cpp)

add_executable(test_arm test_arm.cpp)
target_link_libraries(test_arm PRIVATE tdocore tdort)

enable_testing()
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/test.iso
    COMMAND make_test_disc ${CMAKE_CURRENT_BINARY_DIR}/test.iso DEPENDS make_test_disc)
add_custom_target(test_disc ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/test.iso)
add_test(NAME arm_semantics COMMAND test_arm ${CMAKE_CURRENT_BINARY_DIR}/test.iso)
add_test(NAME end_to_end COMMAND ${CMAKE_COMMAND}
    "-DCONVERTER_EXE=$<TARGET_FILE:3DOtoPC_cli>" -DISO=${CMAKE_CURRENT_BINARY_DIR}/test.iso
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/e2e -DRUNTIME=${CMAKE_SOURCE_DIR}/runtime
    -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e.cmake)

# ---- Guest C program compiled for the 3DO CPU (big-endian ARM) ----------
find_program(TDO_GUEST_CLANG NAMES clang)
find_program(TDO_GUEST_LLD NAMES ld.lld)
find_program(TDO_GUEST_OBJCOPY NAMES llvm-objcopy)
if(TDO_GUEST_CLANG AND TDO_GUEST_LLD AND TDO_GUEST_OBJCOPY)
    set(G ${CMAKE_CURRENT_BINARY_DIR}/guest)
    set(GFLAGS --target=armeb-none-eabi -march=armv4 -marm -mfloat-abi=soft -ffreestanding
               -fno-builtin -nostdlib -O2 -mno-unaligned-access -fno-exceptions)
    add_custom_command(OUTPUT ${G}/LaunchMe
        COMMAND ${CMAKE_COMMAND} -E make_directory ${G}
        COMMAND ${TDO_GUEST_CLANG} ${GFLAGS} -c ${CMAKE_CURRENT_SOURCE_DIR}/guest/crt0.S -o ${G}/crt0.o
        COMMAND ${TDO_GUEST_CLANG} ${GFLAGS} -c ${CMAKE_CURRENT_SOURCE_DIR}/guest/main.c -o ${G}/main.o
        COMMAND ${TDO_GUEST_LLD} -EB -T ${CMAKE_CURRENT_SOURCE_DIR}/guest/link.ld ${G}/crt0.o ${G}/main.o -o ${G}/guest.elf
        COMMAND ${TDO_GUEST_OBJCOPY} -O binary ${G}/guest.elf ${G}/LaunchMe
        DEPENDS guest/crt0.S guest/main.c guest/pf.h guest/link.ld)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/kernel.iso
        COMMAND make_test_disc ${CMAKE_CURRENT_BINARY_DIR}/kernel.iso ${G}/LaunchMe
        DEPENDS make_test_disc ${G}/LaunchMe)
    add_custom_target(kernel_disc ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/kernel.iso)
    add_test(NAME kernel_file_hle COMMAND ${CMAKE_COMMAND}
        "-DCONVERTER_EXE=$<TARGET_FILE:3DOtoPC_cli>" -DISO=${CMAKE_CURRENT_BINARY_DIR}/kernel.iso
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/kernel_e2e -DRUNTIME=${CMAKE_SOURCE_DIR}/runtime
        -DEXPECT=90 -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e.cmake)

    # Same program with a deliberate bad call: the crash detector must catch it.
    add_custom_command(OUTPUT ${G}/crash/LaunchMe
        COMMAND ${CMAKE_COMMAND} -E make_directory ${G}/crash
        COMMAND ${TDO_GUEST_CLANG} ${GFLAGS} -DCRASH_PROBE -c ${CMAKE_CURRENT_SOURCE_DIR}/guest/main.c -o ${G}/crash/main.o
        COMMAND ${TDO_GUEST_LLD} -EB -T ${CMAKE_CURRENT_SOURCE_DIR}/guest/link.ld ${G}/crt0.o ${G}/crash/main.o -o ${G}/crash/guest.elf
        COMMAND ${TDO_GUEST_OBJCOPY} -O binary ${G}/crash/guest.elf ${G}/crash/LaunchMe
        DEPENDS ${G}/LaunchMe guest/main.c)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/crash.iso
        COMMAND make_test_disc ${CMAKE_CURRENT_BINARY_DIR}/crash.iso ${G}/crash/LaunchMe
        DEPENDS make_test_disc ${G}/crash/LaunchMe)
    add_custom_target(crash_disc ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/crash.iso)
    add_test(NAME crash_detector COMMAND ${CMAKE_COMMAND}
        "-DCONVERTER_EXE=$<TARGET_FILE:3DOtoPC_cli>" -DISO=${CMAKE_CURRENT_BINARY_DIR}/crash.iso
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/crash_e2e -DRUNTIME=${CMAKE_SOURCE_DIR}/runtime
        -DEXPECT_CRASH=1 -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e.cmake)

    # CEL engine, threads/semaphores/timer and event broker
    add_custom_command(OUTPUT ${G}/features/LaunchMe
        COMMAND ${CMAKE_COMMAND} -E make_directory ${G}/features
        COMMAND ${TDO_GUEST_CLANG} ${GFLAGS} -c ${CMAKE_CURRENT_SOURCE_DIR}/guest/features.c -o ${G}/features/features.o
        COMMAND ${TDO_GUEST_LLD} -EB -T ${CMAKE_CURRENT_SOURCE_DIR}/guest/link.ld ${G}/crt0.o ${G}/features/features.o -o ${G}/features/guest.elf
        COMMAND ${TDO_GUEST_OBJCOPY} -O binary ${G}/features/guest.elf ${G}/features/LaunchMe
        DEPENDS ${G}/LaunchMe guest/features.c guest/pf.h)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/features.iso
        COMMAND make_test_disc ${CMAKE_CURRENT_BINARY_DIR}/features.iso ${G}/features/LaunchMe
        DEPENDS make_test_disc ${G}/features/LaunchMe)
    add_custom_target(features_disc ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/features.iso)
    add_test(NAME cel_threads_input COMMAND ${CMAKE_COMMAND}
        "-DCONVERTER_EXE=$<TARGET_FILE:3DOtoPC_cli>" -DISO=${CMAKE_CURRENT_BINARY_DIR}/features.iso
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/features_e2e -DRUNTIME=${CMAKE_SOURCE_DIR}/runtime
        -DEXPECT=90 -DFAKE_INPUT=1 -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e.cmake)

    # Audio folio: instruments, mixer, knobs, samples, cues, linked attachments.
    add_custom_command(OUTPUT ${G}/audio/LaunchMe
        COMMAND ${CMAKE_COMMAND} -E make_directory ${G}/audio
        COMMAND ${TDO_GUEST_CLANG} ${GFLAGS} -c ${CMAKE_CURRENT_SOURCE_DIR}/guest/audio.c -o ${G}/audio/audio.o
        COMMAND ${TDO_GUEST_LLD} -EB -T ${CMAKE_CURRENT_SOURCE_DIR}/guest/link.ld ${G}/crt0.o ${G}/audio/audio.o -o ${G}/audio/guest.elf
        COMMAND ${TDO_GUEST_OBJCOPY} -O binary ${G}/audio/guest.elf ${G}/audio/LaunchMe
        DEPENDS ${G}/LaunchMe guest/audio.c guest/pf.h)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/audio.iso
        COMMAND make_test_disc ${CMAKE_CURRENT_BINARY_DIR}/audio.iso ${G}/audio/LaunchMe
        DEPENDS make_test_disc ${G}/audio/LaunchMe)
    add_custom_target(audio_disc ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/audio.iso)
    add_test(NAME audio_folio COMMAND ${CMAKE_COMMAND}
        "-DCONVERTER_EXE=$<TARGET_FILE:3DOtoPC_cli>" -DISO=${CMAKE_CURRENT_BINARY_DIR}/audio.iso
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/audio_e2e -DRUNTIME=${CMAKE_SOURCE_DIR}/runtime
        -DEXPECT=90 -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e.cmake)
else()
    message(STATUS "clang/ld.lld/llvm-objcopy not found: skipping the guest C test")
endif()
