# edge_core – cala logika obliczen i pomiarow, bez zaleznosci od Qt.
# Uzywana przez edge_cli (a pozniej edge_bench i GUI).
add_library(edge_core STATIC
    image.h          image.cpp
    masks.h          masks.cpp
    measure.h        measure.cpp
    kernels_ref.h
    kernels.h
    filter.h         filter.cpp
    row_scheduler.h
    thread_pool.h
    edge_detector.h  edge_detector.cpp
    variants.h       variants.cpp
    variants/static_masks.h
    variants/separable.h
    variants/v0_naive.cpp
    variants/v1_borders.cpp
    variants/v2_constmask.cpp
    variants/v3_v4_separable.cpp
    variants/v5_autovec.cpp
    variants/v6_avx2.cpp
    variants/w3_simd.cpp
    variants/omp.cpp
    variants/ai_variant.cpp
    topology.h       topology.cpp
    gpu/gpu_opencl.h gpu/gpu_opencl.cpp
    synthetic.h      synthetic.cpp
    compare.h        compare.cpp
    verify.h         verify.cpp
    harness.h        harness.cpp
    stb_image.h stb_image_write.h stb_impl.cpp
)

target_include_directories(edge_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(edge_core PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(edge_core PUBLIC psapi)
endif()

# V5 / V6: sqrt bez errno (warunek wektoryzacji pierwiastka) i bez FMA
# (wynik identyczny z V0).  Zestaw instrukcji AVX2 wlaczany atrybutem
# target("avx2") tylko w funkcjach tych wariantow.
set_source_files_properties(variants/v5_autovec.cpp variants/v6_avx2.cpp variants/w3_simd.cpp
    PROPERTIES COMPILE_OPTIONS "-fno-math-errno;-ffp-contract=off")
option(EDGE_VEC_REPORT "Raport auto-wektoryzacji dla V5 (-fopt-info-vec)" OFF)
if(EDGE_VEC_REPORT)
    set_property(SOURCE variants/v5_autovec.cpp APPEND PROPERTY COMPILE_OPTIONS "-fopt-info-vec-optimized")
endif()

# OpenMP tylko dla pliku wariantu OMP
find_package(OpenMP COMPONENTS CXX)
if(OpenMP_CXX_FOUND)
    set_property(SOURCE variants/omp.cpp APPEND PROPERTY COMPILE_OPTIONS ${OpenMP_CXX_FLAGS})
    target_link_libraries(edge_core PUBLIC OpenMP::OpenMP_CXX)
endif()

# OpenCL (wariant GPU) – opcjonalny: bez naglowkow wariant jest niedostepny
find_package(OpenCL QUIET)
if(OpenCL_FOUND)
    set_property(SOURCE gpu/gpu_opencl.cpp APPEND PROPERTY COMPILE_DEFINITIONS EDGE_HAVE_OPENCL)
    target_link_libraries(edge_core PUBLIC OpenCL::OpenCL)
    message(STATUS "OpenCL: ${OpenCL_VERSION_STRING} – wariant GPU wlaczony")
else()
    message(STATUS "OpenCL nie znaleziony – wariant GPU niedostepny "
                   "(sudo apt install ocl-icd-opencl-dev opencl-headers)")
endif()

# Biblioteka stb jest zewnetrzna – jej ostrzezenia nas nie dotycza
set_source_files_properties(stb_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")
