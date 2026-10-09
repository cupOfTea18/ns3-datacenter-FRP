build_example(
  NAME longhaul-convergence
  SOURCE_FILES
    longhaul-convergence.cc
    cdf.c
  HEADER_FILES
    cdf.h
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libtraffic-control}
    ${libinternet}
    ${libapplications}
)
set_source_files_properties(cdf.c PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

build_example(
  NAME longhaul-r4-test
  SOURCE_FILES longhaul-r4-test.cc
  LIBRARIES_TO_LINK ${libpoint-to-point} ${libinternet}
)
