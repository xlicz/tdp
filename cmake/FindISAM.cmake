
FIND_PATH(ISAM_INCLUDE_DIR NAMES isam.h isam/isam.h
  HINTS 
  /usr/include 
  /usr/local/include
  ${LIB_SEARCH_DIR}
  ${LIB_SEARCH_DIR}/include
  )

FIND_LIBRARY(ISAM_LIBRARY NAMES isam libisam.so
  HINTS /usr/lib /usr/local/lib
  ${LIB_SEARCH_DIR}
  ${LIB_SEARCH_DIR}/lib
  ) 

IF (ISAM_INCLUDE_DIR)
   SET(ISAM_FOUND TRUE)
ENDIF (ISAM_INCLUDE_DIR)


IF (ISAM_FOUND)
   IF (NOT ISAM_FIND_QUIETLY)
      MESSAGE(STATUS "Found ISAM: ${ISAM_INCLUDE_DIR}")
   ENDIF (NOT ISAM_FIND_QUIETLY)
ELSE (ISAM_FOUND)
   IF (ISAM_FIND_REQUIRED)
      MESSAGE(FATAL_ERROR "Could not find Asio-lib")
   ENDIF (ISAM_FIND_REQUIRED)
ENDIF (ISAM_FOUND)
