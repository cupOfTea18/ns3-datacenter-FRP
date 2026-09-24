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
  NAME longhaul-proposed-test
  SOURCE_FILES longhaul-proposed-test.cc cdf.c
  LIBRARIES_TO_LINK
    ${libpoint-to-point} ${libtraffic-control} ${libinternet} ${libapplications}
)
